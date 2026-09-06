/*
 * FreeDV 2020 receive path.
 *
 * libcodec2's FreeDV API consumes a variable number of 8 kHz modem samples
 * and produces 16 kHz LPCNet speech.  MacHPSDR's WDSP path is fixed at 48 kHz,
 * so the RX thread FIR-decimates 48k->8k, a worker runs the comparatively
 * expensive neural decoder, and its speech is interpolated 16k->48k into a
 * second lock-protected FIFO.  The RX thread never waits for the modem.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <freedv_api.h>

#include "freedv_decoder.h"
#include "log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define INPUT_RATE       48000
#define MODEM_RATE        8000
#define SPEECH_RATE      16000
#define DECIM                6
#define INTERP               3
#define FIR_TAPS             49
#define INPUT_CAP        (MODEM_RATE * 12)
#define OUTPUT_CAP       (INPUT_RATE * 6)
#define START_FRAMES     (INPUT_RATE / 10)

static GMutex mutex;
static GCond cond;
static GThread *worker;
static gboolean wanted;
static guint generation;

static short input_fifo[INPUT_CAP];
static int input_r, input_w, input_count;
static float output_fifo[OUTPUT_CAP];
static int output_r, output_w, output_count;
static gboolean play_ready;

/* RX-thread-only decimator state. */
static double fir[FIR_TAPS];
static double fir_state[FIR_TAPS];
static int fir_pos, decim_phase;

static char status_text[96] = "Off";

static void set_status_locked(const char *s) {
  g_strlcpy(status_text, s, sizeof(status_text));
}

static void reset_stream_locked(void) {
  input_r=input_w=input_count=0;
  output_r=output_w=output_count=0;
  play_ready=FALSE;
  memset(fir_state,0,sizeof(fir_state));
  fir_pos=decim_phase=0;
}

static void design_decimator(void) {
  /* Blackman-windowed sinc.  3 kHz keeps the 2020 waveform and rejects the
     part of a wide DIGU passband that would alias below 4 kHz. */
  const double fc=3000.0/INPUT_RATE;
  double sum=0.0;
  for(int i=0;i<FIR_TAPS;i++) {
    double x=(double)i-(FIR_TAPS-1)*0.5;
    double sinc=(x==0.0)?2.0*fc:sin(2.0*M_PI*fc*x)/(M_PI*x);
    double w=0.42-0.5*cos(2.0*M_PI*i/(FIR_TAPS-1))
                 +0.08*cos(4.0*M_PI*i/(FIR_TAPS-1));
    fir[i]=sinc*w;
    sum+=fir[i];
  }
  for(int i=0;i<FIR_TAPS;i++) fir[i]/=sum;
}

static gpointer decoder_worker(gpointer unused) {
  struct freedv *fdv=NULL;
  short *modem=NULL, *speech=NULL;
  int max_modem=0, max_speech=0, nin=0;
  guint local_generation=0;
  float previous=0.0f;
  gboolean have_previous=FALSE;

  for(;;) {
    g_mutex_lock(&mutex);
    while(generation==local_generation &&
          (!wanted || fdv==NULL || input_count<nin))
      g_cond_wait(&cond,&mutex);

    if(generation!=local_generation) {
      gboolean open_now=wanted;
      local_generation=generation;
      g_mutex_unlock(&mutex);

      if(fdv) freedv_close(fdv);
      fdv=NULL;
      g_free(modem); modem=NULL;
      g_free(speech); speech=NULL;
      have_previous=FALSE;

      if(open_now) {
        fdv=freedv_open(FREEDV_MODE_2020);
        if(fdv) {
          int mr=freedv_get_modem_sample_rate(fdv);
          int sr=freedv_get_speech_sample_rate(fdv);
          max_modem=freedv_get_n_max_modem_samples(fdv);
          max_speech=freedv_get_n_max_speech_samples(fdv);
          if(mr!=MODEM_RATE || sr!=SPEECH_RATE || max_modem<=0 || max_speech<=0) {
            log_error("FreeDV 2020: unsupported rates modem=%d speech=%d\n",mr,sr);
            freedv_close(fdv); fdv=NULL;
          } else {
            modem=g_new(short,max_modem);
            speech=g_new(short,max_speech);
            nin=freedv_nin(fdv);
            freedv_set_squelch_en(fdv,true);
            freedv_set_snr_squelch_thresh(fdv,0.0f);
          }
        }
        g_mutex_lock(&mutex);
        if(local_generation==generation) {
          set_status_locked(fdv?"Searching":"FreeDV 2020 unavailable");
          g_cond_signal(&cond);
        }
        g_mutex_unlock(&mutex);
      }
      continue;
    }

    if(nin>max_modem) {
      set_status_locked("FreeDV input size error");
      g_mutex_unlock(&mutex);
      continue;
    }
    for(int i=0;i<nin;i++) {
      modem[i]=input_fifo[input_r];
      input_r=(input_r+1)%INPUT_CAP;
    }
    input_count-=nin;
    g_mutex_unlock(&mutex);

    int nout=freedv_rx(fdv,speech,modem);
    int next_nin=freedv_nin(fdv);
    int sync=0;
    float snr=0.0f;
    freedv_get_modem_stats(fdv,&sync,&snr);

    g_mutex_lock(&mutex);
    if(local_generation==generation && wanted) {
      char s[64];
      if(sync) g_snprintf(s,sizeof(s),"Sync %.1f dB",snr);
      else g_strlcpy(s,"Searching",sizeof(s));
      set_status_locked(s);

      for(int i=0;i<nout;i++) {
        float current=(float)speech[i]/32768.0f;
        if(!have_previous) { previous=current; have_previous=TRUE; }
        for(int k=1;k<=INTERP;k++) {
          float v=previous+(current-previous)*(float)k/INTERP;
          if(output_count==OUTPUT_CAP) {
            output_r=(output_r+1)%OUTPUT_CAP;
            output_count--;
          }
          output_fifo[output_w]=v;
          output_w=(output_w+1)%OUTPUT_CAP;
          output_count++;
        }
        previous=current;
      }
      nin=next_nin;
    }
    g_mutex_unlock(&mutex);
  }
  return NULL;
}

void freedv_decoder_init(void) {
  if(worker) return;
  g_mutex_init(&mutex);
  g_cond_init(&cond);
  design_decimator();
  worker=g_thread_new("freedv-2020",decoder_worker,NULL);
}

void freedv_decoder_set_enabled(gboolean enabled) {
  if(!worker) freedv_decoder_init();
  g_mutex_lock(&mutex);
  if(wanted!=enabled) {
    wanted=enabled;
    generation++;
    reset_stream_locked();
    set_status_locked(enabled?"Starting":"Off");
    g_cond_signal(&cond);
  }
  g_mutex_unlock(&mutex);
}

void freedv_decoder_add_audio(const gdouble *samples, int nframes) {
  if(!samples || nframes<=0) return;
  g_mutex_lock(&mutex);
  if(!wanted) { g_mutex_unlock(&mutex); return; }
  for(int n=0;n<nframes;n++) {
    double v=samples[2*n];
    if(v>1.0) v=1.0; else if(v< -1.0) v= -1.0;
    fir_state[fir_pos]=v;
    fir_pos=(fir_pos+1)%FIR_TAPS;
    if(++decim_phase<DECIM) continue;
    decim_phase=0;
    double y=0.0;
    int p=fir_pos;
    for(int k=0;k<FIR_TAPS;k++) {
      if(--p<0) p=FIR_TAPS-1;
      y+=fir[k]*fir_state[p];
    }
    if(input_count==INPUT_CAP) {
      /* Never stall the real-time RX thread.  Lose the oldest modem sample;
         the OFDM synchronizer will reacquire after an overloaded worker. */
      input_r=(input_r+1)%INPUT_CAP;
      input_count--;
    }
    long q=lround(y*32767.0);
    if(q>32767) q=32767; else if(q< -32768) q= -32768;
    input_fifo[input_w]=(short)q;
    input_w=(input_w+1)%INPUT_CAP;
    input_count++;
  }
  g_cond_signal(&cond);
  g_mutex_unlock(&mutex);
}

void freedv_decoder_get_audio(gdouble *speech, int nframes) {
  if(!speech || nframes<=0) return;
  memset(speech,0,(size_t)nframes*sizeof(*speech));
  g_mutex_lock(&mutex);
  if(!wanted) { g_mutex_unlock(&mutex); return; }
  if(!play_ready && output_count>=START_FRAMES) play_ready=TRUE;
  if(play_ready) {
    int n=output_count<nframes?output_count:nframes;
    for(int i=0;i<n;i++) {
      speech[i]=output_fifo[output_r];
      output_r=(output_r+1)%OUTPUT_CAP;
    }
    output_count-=n;
    if(n<nframes) play_ready=FALSE;
  }
  g_mutex_unlock(&mutex);
}

void freedv_decoder_get_status(char *out, size_t size) {
  if(!out || size==0) return;
  g_mutex_lock(&mutex);
  g_strlcpy(out,status_text,size);
  g_mutex_unlock(&mutex);
}
