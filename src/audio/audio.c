/* Copyright (C)
* 2018 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/


#include <gtk/gtk.h>
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>

#include <soundio/soundio.h>
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <Availability.h>
// macOS 12 renamed kAudioObjectPropertyElementMaster -> …ElementMain (same
// value).  It is an *enum* constant, not a macro, so #ifndef can't detect it —
// gate the fallback on the SDK version instead: only pre-12.0 SDKs lack the new
// spelling.  (The old #ifndef guard was always true and wrongly mapped Main back
// onto the deprecated Master on modern SDKs.)
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif
#endif
#ifndef __APPLE__
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <pulse/simple.h>
#include <alsa/asoundlib.h>
#endif

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "adc.h"
#include "dac.h"
#include "discovered.h"
#include "wideband.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "audio.h"

int n_input_devices;
AUDIO_DEVICE input_devices[MAX_AUDIO_DEVICES];
int n_output_devices;
AUDIO_DEVICE output_devices[MAX_AUDIO_DEVICES];



static int running=FALSE;

#ifndef __APPLE__
static snd_pcm_format_t record_audio_format;

#define FORMATS 3
static snd_pcm_format_t formats[3]={
  SND_PCM_FORMAT_FLOAT_LE,
  SND_PCM_FORMAT_S32_LE,
  SND_PCM_FORMAT_S16_LE};
#endif

static GThread *mic_read_thread_id;
static void *mic_read_thread(void *arg);


struct SoundIo *soundio;

#ifndef __APPLE__
//static pa_buffer_attr bufattr;
static pa_glib_mainloop *main_loop;
static pa_mainloop_api *main_loop_api;
static pa_operation *op;
static pa_context *pa_ctx;
#endif

static int ready=0;
static int sample_rate=48000;

// Sentinel device name for the synthetic "System Default" output entry.  When a
// receiver's audio_name is this string, the output stream is (re)opened on
// whatever device macOS currently considers the default output, resolved fresh
// every time — so the user can pick it once and never have to change devices in
// the app when they connect e.g. Bluetooth headphones.
#define AUDIO_SYSTEM_DEFAULT_NAME "__system_default__"


static int underflow_count=0;

static void underflow_callback(struct SoundIoOutStream *outstream) {
  underflow_count++;
  //g_print("audio_write: underflow %d\n", underflow_count);
}

// ---------------------------------------------------------------------------
// Audio flow diagnostics — MACHPSDR_AUDIO_DEBUG=1
//
// Periodic dropouts have three quite different causes and they cannot be told
// apart by ear (each backend just fails with its own buffer depth, which is why
// the same fault sounds like a different period on ALSA, Pulse and JACK).  Once
// every 5 s this prints, per receiver:
//
//   frames/s   what the RX thread actually produced.  Must be ~48000.  A steady
//              47xxx/48xxx means the radio and the sound card disagree on the
//              rate, and the sink drains (or overflows) until it gaps —
//              period = buffer_depth / mismatch, i.e. backend-dependent.
//   maxgap     longest interval between two consecutive 1024-sample handovers.
//              ~21 ms is normal; a big value means the producing thread was
//              stalled by something else (this is the "sharing a thread"
//              hypothesis, and it is either true or it is not).
//   fill       how much audio is queued in the sink, in frames: the soundio
//              ring fill, or snd_pcm_delay() for direct ALSA.  fill hitting 0
//              is starvation (no cushion); fill pinned at the maximum means we
//              are pushing into a full device and dropping.
//   dropped / xrun / wait / underflow
//              frames the sink refused, ALSA underruns recovered, snd_pcm_wait
//              rounds, and libsoundio underflow callbacks.
//
// The check runs on a 1024-sample boundary, so the per-sample cost when the
// flag is off is one int compare.
// ---------------------------------------------------------------------------
static int audio_debug=-1;

typedef struct {
  gint64 t0;              // start of the current report window
  gint64 last_us;         // time of the last 1024-sample checkpoint
  gint64 max_gap_us;      // longest producer stall in this window
  long   produced;        // frames handed to the sink
  long   dropped;         // frames the sink refused
  long   xruns;           // ALSA underruns recovered
  long   waits;           // ALSA snd_pcm_wait rounds
  int    fill_min;
  int    fill_max;
} AUDIO_STATS;

static AUDIO_STATS astats[MAX_RECEIVERS];

// Consumer side of the soundio path (write_callback runs on libsoundio's own
// thread).  Aggregated over all streams — with one local-audio receiver, which
// is the normal case, that is exactly the one we report next to.
static volatile long sio_consumed=0;    // frames taken from the ring
static volatile long sio_zerofill=0;    // frames of silence emitted instead

// Sink fill for the Pulse/ALSA paths.  Those only talk to the device once per
// local_audio_buffer_size block, so the queue depth is sampled there and reused
// for the frames in between.
static int pa_alsa_dbg_fill=-1;

static inline gboolean audio_debug_on(void) {
  if(audio_debug<0) {
    const char *e=getenv("MACHPSDR_AUDIO_DEBUG");
    audio_debug=(e!=NULL && *e!='0')?1:0;
  }
  return audio_debug==1;
}

// ---------------------------------------------------------------------------
// "The sound device is not taking the audio" warning.
//
// A sink that accepts far less than 48000 frames per second of wall clock is
// not a thing this application can fix — the surplus has to go somewhere and
// the only honest place is the bin (resampling to the sink's apparent rate
// would play everything slowed and pitched down, and fall further behind for
// ever).  What we CAN stop doing is failing in silence: measured on a Linux VM
// with an emulated virtio card under PipeWire, the sink took ~20500 frames/s
// against 48000 produced, so 57 % of the audio was discarded and the operator
// got no sound and not one word of explanation.  Say it once, with the numbers.
//
// Deliberately once per audio_open_output(): the condition persists for as long
// as the device does, and a dialog every five seconds would be worse than the
// bug.  Re-armed on the next open, so trying another device gets its own
// verdict.
// ---------------------------------------------------------------------------
static gint audio_sink_warned=0;              // re-armed by audio_open_output()

typedef struct {
  int  channel;
  long produced;
  long dropped;
  double seconds;
} AUDIO_SINK_WARNING;

static gboolean audio_sink_warning_idle(gpointer data) {
  AUDIO_SINK_WARNING *w=(AUDIO_SINK_WARNING *)data;
  if(main_window!=NULL) {
    double taken=(double)(w->produced-w->dropped)/w->seconds;
    char detail[640];
    g_snprintf(detail,sizeof detail,
      "The audio device is accepting only about %.0f of the 48000 frames per "
      "second that receiver %d produces, so %.0f%% of the audio is being "
      "discarded.  Playback will be broken up or silent.\n\n"
      "This is a fault in the audio path outside MacHPSDR, not a setting in it "
      "— most often an emulated sound device in a virtual machine, or a "
      "PulseAudio/PipeWire server running with too little headroom.\n\n"
      "Worth trying, in this order: pick a different output device, or another "
      "backend in Configure → Audio (the native PulseAudio backend is usually "
      "the most reliable one on Linux); check whether other applications play "
      "cleanly on this machine at all; raise the sound server's buffer "
      "headroom.\n\n"
      "Run with MACHPSDR_AUDIO_DEBUG=1 for per-window figures.",
      taken, w->channel, 100.0*(double)w->dropped/(double)w->produced);

    GtkAlertDialog *dialog=gtk_alert_dialog_new("Audio output is not keeping up");
    gtk_alert_dialog_set_detail(dialog,detail);
    const char *buttons[]={ "OK", NULL };
    gtk_alert_dialog_set_buttons(dialog,buttons);
    gtk_alert_dialog_set_default_button(dialog,0);
    gtk_alert_dialog_show(dialog,GTK_WINDOW(main_window));
    g_object_unref(dialog);
  }
  g_free(w);
  return G_SOURCE_REMOVE;
}

// Runs on the RX audio thread, so the dialog is handed to the GTK thread.
static void audio_sink_warn(int channel,long produced,long dropped,double seconds) {
  if(!g_atomic_int_compare_and_exchange(&audio_sink_warned,0,1)) return;
  log_error("audio: sink took %ld of %ld frames in %.1f s (%.0f%% discarded) "
            "— see the warning dialog\n",
            produced-dropped,produced,seconds,
            100.0*(double)dropped/(double)produced);
  AUDIO_SINK_WARNING *w=g_new0(AUDIO_SINK_WARNING,1);
  w->channel=channel;
  w->produced=produced;
  w->dropped=dropped;
  w->seconds=seconds;
  g_idle_add(audio_sink_warning_idle,w);
}

// Called once per produced frame.  fill_frames is what is currently queued in
// the sink, or -1 when the caller cannot cheaply tell (measuring it is only
// worth doing on a block boundary).
//
// The accounting runs ALWAYS, not only under MACHPSDR_AUDIO_DEBUG: the drop
// ratio is what raises the warning above, and a fault nobody can see is the
// thing being fixed here.  Only the printing, and the sink-fill measurement at
// the call sites (which costs a real query on Pulse/ALSA), stay behind the flag.
static void audio_stats_frame(RECEIVER *rx,int fill_frames,int dropped,int xruns,int waits) {
  if(rx->channel<0 || rx->channel>=MAX_RECEIVERS) return;
  AUDIO_STATS *s=&astats[rx->channel];

  s->produced++;
  s->dropped+=dropped;
  s->xruns+=xruns;
  s->waits+=waits;
  if(fill_frames>=0) {
    if(fill_frames<s->fill_min) s->fill_min=fill_frames;
    if(fill_frames>s->fill_max) s->fill_max=fill_frames;
  }
  if((s->produced & 1023)!=0) return;

  gint64 now=g_get_monotonic_time();
  if(s->t0==0) {                      // first checkpoint: just start the window
    s->t0=now;
    s->last_us=now;
    s->fill_min=G_MAXINT;
    s->fill_max=0;
    s->produced=0;
    s->dropped=s->xruns=s->waits=0;
    return;
  }
  gint64 gap=now-s->last_us;
  s->last_us=now;
  if(gap>s->max_gap_us) s->max_gap_us=gap;

  gint64 window=now-s->t0;
  if(window<5000000) return;

  // A quarter of the audio thrown away is not jitter, it is a sink that cannot
  // take the stream.  (Ordinary hiccups on a healthy device measured 144-288
  // frames per 5 s window here, i.e. about a tenth of one per cent.)
  if(s->produced>0 && s->dropped*4>=s->produced)
    audio_sink_warn(rx->channel,s->produced,s->dropped,(double)window/1e6);

  if(audio_debug_on()) {
  log_info("audio dbg RX%d: %ld frames in %.2f s = %.0f/s  fill %d..%d  "
           "maxgap %.1f ms  dropped %ld  xrun %ld  wait %ld  "
           "underflow %d  sio out %ld zero %ld\n",
           rx->channel,
           s->produced, (double)window/1e6,
           (double)s->produced*1e6/(double)window,
           s->fill_min==G_MAXINT?-1:s->fill_min, s->fill_max,
           (double)s->max_gap_us/1000.0,
           s->dropped, s->xruns, s->waits,
           underflow_count, sio_consumed, sio_zerofill);
  }

  s->t0=now;
  s->produced=0;
  s->dropped=s->xruns=s->waits=0;
  s->max_gap_us=0;
  s->fill_min=G_MAXINT;
  s->fill_max=0;
  sio_consumed=0;
  sio_zerofill=0;
}

// Output device runs at a rate other than the 48 kHz DSP rate (e.g. a Bluetooth
// headset locked to 44.1 kHz): linear-resample the 48 kHz interleaved stereo
// audio held in the ring buffer to the device rate on the fly.  The ring buffer
// still stores 48 kHz frames; only whole input frames actually consumed are
// released, and the fractional read position is carried across callbacks in
// rx->audio_resample_phase so pitch stays exact and continuous.
static void write_callback_resample(struct SoundIoOutStream *outstream,
                                    int frame_count_min, int frame_count_max) {
  RECEIVER *rx=(RECEIVER *)outstream->userdata;
  struct SoundIoChannelArea *areas;
  int err;
  const int chans=outstream->layout.channel_count;
  const double ratio=(double)sample_rate/(double)outstream->sample_rate; // input frames per output frame

  char *read_ptr=soundio_ring_buffer_read_ptr(rx->ring_buffer);
  int in_avail=soundio_ring_buffer_fill_count(rx->ring_buffer)/(int)(sizeof(float)*2);
  const float (*in)[2]=(const float (*)[2])read_ptr;

  double phase=rx->audio_resample_phase;

  // How many output frames can we produce?  Output frame j reads input frames
  // floor(phase+j*ratio) and +1, so we need phase+j*ratio <= in_avail-1.
  int producible=0;
  if(in_avail>=2 && phase<(double)(in_avail-1))
    producible=(int)(((double)(in_avail-1)-phase)/ratio)+1;

  // Not enough data to satisfy the callback minimum: emit silence and wait
  // (the producer keeps filling the ring); do not consume any input.
  if(producible<frame_count_min) {
    int frames_left=frame_count_min;
    while(frames_left>0) {
      int fc=frames_left;
      if((err=soundio_outstream_begin_write(outstream,&areas,&fc)) || fc<=0) return;
      for(int f=0;f<fc;f++)
        for(int ch=0;ch<chans;ch++) {
          memset(areas[ch].ptr,0,outstream->bytes_per_sample);
          areas[ch].ptr+=areas[ch].step;
        }
      if((err=soundio_outstream_end_write(outstream))) return;
      frames_left-=fc;
    }
    return;
  }

  int want=producible;
  if(want>frame_count_max) want=frame_count_max;

  int produced=0;
  double pos=phase;
  while(produced<want) {
    int fc=want-produced;
    if((err=soundio_outstream_begin_write(outstream,&areas,&fc))) {
      log_info("write_callback_resample: begin write error: %s\n",soundio_strerror(err));
      return;
    }
    if(fc<=0) break;
    for(int f=0;f<fc;f++) {
      int i=(int)pos;
      int i1=(i+1<in_avail)?i+1:i;
      double frac=pos-(double)i;
      for(int ch=0;ch<chans;ch++) {
        float s0=in[i][ch&1];
        float s1=in[i1][ch&1];
        float v=(float)(s0+(s1-s0)*frac);
        memcpy(areas[ch].ptr,&v,sizeof(float));
        areas[ch].ptr+=areas[ch].step;
      }
      pos+=ratio;
    }
    if((err=soundio_outstream_end_write(outstream))) return;
    produced+=fc;
  }

  // Release the whole input frames consumed; keep the fraction for next time.
  int consumed=(int)pos;
  if(consumed>in_avail) consumed=in_avail;
  soundio_ring_buffer_advance_read_ptr(rx->ring_buffer, consumed*(int)(sizeof(float)*2));
  rx->audio_resample_phase=pos-(double)consumed;
}

static void write_callback(struct SoundIoOutStream *outstream, int frame_count_min, int frame_count_max) {
  RECEIVER *rx=(RECEIVER *)outstream->userdata;
  struct SoundIoChannelArea *areas;
  int frames_left;
  int frame_count;
  int err;

  // Device not running at the 48 kHz DSP rate: hand off to the resampling path.
  if(outstream->sample_rate!=sample_rate) {
    write_callback_resample(outstream,frame_count_min,frame_count_max);
    return;
  }

  // The ring buffer ALWAYS holds interleaved stereo float frames (audio_write
  // pushes exactly two floats per frame), whatever the device's channel count
  // is.  outstream->bytes_per_frame is the *device's* frame size, so it must
  // never be used to measure the ring: libsoundio leaves outstream->layout at
  // the device's own layout, and a device whose layout is not stereo (a JACK
  // client with 4/6/8 ports, a surround ALSA device) then drained the ring
  // channel_count/2 times too fast — the start-up cushion played for a fraction
  // of a second and every callback after it underran into silence.
  const int chans = outstream->layout.channel_count;
  const int ring_bytes_per_frame = (int)sizeof(float) * 2;

  char *read_ptr = soundio_ring_buffer_read_ptr(rx->ring_buffer);
  int fill_bytes = soundio_ring_buffer_fill_count(rx->ring_buffer);
  int fill_count = fill_bytes / ring_bytes_per_frame;

  if (frame_count_min > fill_count) {
    // Ring buffer does not have enough data, fill with zeroes.
    frames_left = frame_count_min;
    while (frames_left > 0) {
      frame_count = frames_left;
      if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count)))
        return;
      if (frame_count <= 0)
        return;
      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < chans; ch += 1) {
          memset(areas[ch].ptr, 0, outstream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
        }
      }
      if ((err = soundio_outstream_end_write(outstream)))
        return;
      // NB: this decrement used to be the body of the `if` above (missing
      // braces), so on a *successful* end_write the loop never advanced and
      // kept zero-filling until the device buffer was full — one small underrun
      // then flushed the whole ALSA/Pulse buffer to silence.
      frames_left -= frame_count;
      sio_zerofill += frame_count;
    }
    return;
  }

  int read_count;
  if(frame_count_max<fill_count) read_count=frame_count_max; else read_count=fill_count;
  frames_left = read_count;

  while (frames_left > 0) {
    frame_count = frames_left;

    if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
      log_info("begin write error: %s", soundio_strerror(err));
      break;
    }

    if (frame_count <= 0)
      break;

    for (int frame = 0; frame < frame_count; frame += 1) {
      const float *src = (const float *)read_ptr;
      for (int ch = 0; ch < chans; ch += 1) {
        // Map the stereo ring onto however many channels the device has
        // (mono takes the left, >2 duplicates the pair), same as the
        // resampling path does.
        memcpy(areas[ch].ptr, &src[ch & 1], sizeof(float));
        areas[ch].ptr += areas[ch].step;
      }
      read_ptr += ring_bytes_per_frame;
    }

    if ((err = soundio_outstream_end_write(outstream)))
      break;

    frames_left -= frame_count;
  }

  // Release only what was actually consumed (frames_left is what is left over
  // if a begin/end_write failed part way through).
  soundio_ring_buffer_advance_read_ptr(rx->ring_buffer,
                                       (read_count - frames_left) * ring_bytes_per_frame);
  sio_consumed += (read_count - frames_left);
}

static void read_callback(struct SoundIoInStream *instream, int frame_count_min, int frame_count_max) {
    RADIO *r=(RADIO *)instream->userdata;
    struct SoundIoChannelArea *areas;
    int err;

    //if(frame_count_min!=frame_count_max) {
    if(frame_count_min<=0) {
      //g_print("read_callback: frame_counts differ min=%d max=%d\n",frame_count_min,frame_count_max);
      return;
    }

    if(r->local_microphone_buffer!=NULL) {
      if(r->local_microphone_buffer_size!=frame_count_min) {
        free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
      }
    }

    if(r->local_microphone_buffer==NULL) {
      r->local_microphone_buffer_size=frame_count_min;
      r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
log_info("read_callback: create microphone buffer: %p length=%d (%d bytes)\n",r->local_microphone_buffer,r->local_microphone_buffer_size,instream->bytes_per_sample*r->local_microphone_buffer_size);
    }

    int frame_count=frame_count_min;

    if((err = soundio_instream_begin_read(instream, &areas, &frame_count))) {
      log_info("read_callback: begin read error: %s\n", soundio_strerror(err));
      return;
    }

    g_mutex_lock(&r->ring_buffer_mutex);

    if(r->mic_resample_rate!=0) {
      // Capture device is not at 48 kHz (e.g. a 16 kHz Bluetooth headset mic):
      // linear-resample the captured mono audio up to 48 kHz before it enters
      // the ring buffer, so the rest of the mic path sees a plain 48 kHz stream.
      // ratio = input frames consumed per output frame (<1 when upsampling).
      double ratio=(double)r->mic_resample_rate/(double)sample_rate;
      float *out=(float *)soundio_ring_buffer_write_ptr(r->ring_buffer);
      int free_count=soundio_ring_buffer_free_count(r->ring_buffer)/(int)sizeof(float);
      char *base=(areas!=NULL)?areas[0].ptr:NULL;
      int step=(areas!=NULL)?areas[0].step:0;
      double pos=r->mic_resample_phase;
      float prev=r->mic_resample_prev;
      int produced=0;

      // Virtual input sequence c[0]=prev, c[k]=s[k-1] for k>=1 (s = this
      // callback's frames). Output sample at pos interpolates c[floor(pos)] and
      // c[floor(pos)+1]; valid while pos < frame_count.
      while(pos<(double)frame_count && produced<free_count) {
        int i=(int)pos;
        double frac=pos-(double)i;
        float a=(i==0)?prev:(base?*(float *)(base+(i-1)*step):0.0f);
        float b=base?*(float *)(base+i*step):0.0f;   // c[i+1] == s[i]
        out[produced++]=(float)(a+(b-a)*frac);
        pos+=ratio;
      }

      if(frame_count>0 && base)
        r->mic_resample_prev=*(float *)(base+(frame_count-1)*step);
      r->mic_resample_phase=pos-(double)frame_count;
      if(r->mic_resample_phase<0.0) r->mic_resample_phase=0.0;

      if((err = soundio_instream_end_read(instream)))
        log_info("read_callback: end read error: %s", soundio_strerror(err));

      if(produced>0) {
        soundio_ring_buffer_advance_write_ptr(r->ring_buffer, produced*(int)sizeof(float));
        g_cond_signal (&r->ring_buffer_cond);
      }
      g_mutex_unlock (&r->ring_buffer_mutex);
      return;
    }

    char *write_ptr = soundio_ring_buffer_write_ptr(r->ring_buffer);
    int free_bytes = soundio_ring_buffer_free_count(r->ring_buffer);
    int free_count = free_bytes / instream->bytes_per_frame;
    if(frame_count!=0 && free_count>=frame_count) {
      if(areas==NULL) {
        log_info("read_callback: areas is NULL\n");
        memset(write_ptr,0,frame_count*instream->bytes_per_sample);
      } else {
        memcpy(write_ptr,areas[0].ptr,frame_count*instream->bytes_per_sample);
      }

      if((err = soundio_instream_end_read(instream))) {
        log_info("read_callback: end read error: %s", soundio_strerror(err));
      }

      soundio_ring_buffer_advance_write_ptr(r->ring_buffer, frame_count*instream->bytes_per_frame);
      g_cond_signal (&r->ring_buffer_cond);
    } else {
      log_info("read_callback: frame_count is 0 or free_count too small\n");
    }
    g_mutex_unlock (&r->ring_buffer_mutex);

}

int audio_open_output(RECEIVER *rx) {
  int result=0;
  int err;
#ifndef __APPLE__
  pa_sample_spec sample_spec;
#endif
  // A different device deserves its own verdict, so re-arm the "sink is not
  // taking the audio" warning and restart the measurement window — otherwise
  // the outgoing device's drops would be charged to the incoming one.
  g_atomic_int_set(&audio_sink_warned,0);
  if(rx->channel>=0 && rx->channel<MAX_RECEIVERS) {
    astats[rx->channel].t0=0;
    astats[rx->channel].produced=0;
    astats[rx->channel].dropped=0;
  }
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
log_info("audio_open_output: SOUNDIO: %s\n",rx->audio_name);
      if(soundio==NULL) {
        log_info("audio_open_output: no soundio backend connected\n");
        return -1;
      }
      // Idempotent: drop any stream already open for this receiver so callers
      // (normal startup and the system-default monitor) can never race into
      // leaking a stream/device.
      if(rx->output_stream!=NULL) audio_close_output(rx);
      g_mutex_lock(&rx->local_audio_mutex);

      // find the device
      rx->output_index=-1;
      if(rx->audio_name!=NULL) {
        for(int i=0;i<n_output_devices;i++) {
          if(strcmp(rx->audio_name,output_devices[i].name)==0) {
            rx->output_index=output_devices[i].index;
            break;
          }
        }
      }

      // output_index==-1 here means either the user picked the synthetic
      // "System Default" entry (index -1 by design), or the configured device
      // was not found (stale "Dummy Output Device" name, or a device that has
      // since been unplugged).  In both cases follow the current system default
      // output.  Re-flush events first so a device connected after launch (e.g.
      // Bluetooth headphones macOS just made the default) is picked up.
      if(rx->output_index==-1) {
        soundio_flush_events(soundio);
        int def=soundio_default_output_device_index(soundio);
        if(def>=0) {
          log_info("audio_open_output: '%s' -> using current default output device\n",
                  rx->audio_name?rx->audio_name:"(none)");
          rx->output_index=def;
        }
      }

      if(rx->output_index==-1) {
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }
  
      rx->output_device = soundio_get_output_device(soundio, rx->output_index);
      if(!rx->output_device) {
        log_info("audio_open_output: could not get output device: out of memory");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      if(!soundio_device_supports_format(rx->output_device, SoundIoFormatFloat32NE)) {
        log_info("audio_open_output: device does not support SoundIoFormatFloat32NE");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      // Pick the stream rate: prefer the 48 kHz DSP rate, but if the device does
      // not accept it (e.g. a Bluetooth headset locked to 44.1 kHz) fall back to
      // the nearest rate the device supports.  When they differ, write_callback
      // resamples the 48 kHz audio to the device rate on the fly.
      int device_rate=sample_rate;
      if(!soundio_device_supports_sample_rate(rx->output_device, sample_rate)) {
        device_rate=soundio_device_nearest_sample_rate(rx->output_device, sample_rate);
        if(device_rate<=0) {
          log_info("audio_open_output: device has no usable sample rate\n");
          g_mutex_unlock(&rx->local_audio_mutex);
          return -1;
        }
        log_info("audio_open_output: device does not support %d Hz; opening at %d Hz with resampling\n",
                sample_rate, device_rate);
      }
      rx->audio_resample_phase=0.0;

      // Size the ring buffer for a fixed ~200 ms of output audio so there is
      // room for a start-up cushion at any DSP rate.  At high DSP rates
      // output_samples is small (e.g. 128 at 384 kHz), so the old "8 buffers"
      // guess collapsed to the 32 kB floor with almost no slack.
      int size=(int)(0.200 * (double)sample_rate) * (int)sizeof(float) * 2;
      if(size<32768) size=32768;
      rx->ring_buffer = soundio_ring_buffer_create(soundio, size);
      if(!rx->ring_buffer) {
        log_info("audio_open_output: soundio_ring_buffer_create failed");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      rx->output_stream = soundio_outstream_create(rx->output_device);
      if(!rx->output_stream) {
        log_info("audio_open_output: could not open output device: out of memory");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }
      rx->output_stream->format = SoundIoFormatFloat32NE;
      rx->output_stream->sample_rate = device_rate;
      rx->output_stream->write_callback = write_callback;
      rx->output_stream->underflow_callback = underflow_callback;
      // Device buffer depth.  This was hardwired to 10 ms, which asks the
      // backend to wake us ~700 times a second; measured against a PipeWire
      // sink, that only sustained ~20-44k frames/s instead of 48000, so the ring
      // buffer stayed pinned full and audio_write threw away every frame that
      // did not fit — heard as sound that starts and then breaks up.  The same
      // measurement at 50 ms runs at the full rate with ~15x fewer wakeups.
      // rx->local_audio_latency is the per-receiver value (ms, default 50) that
      // was already being saved and restored but never actually applied.
      double latency=(double)rx->local_audio_latency/1000.0;
      if(latency<0.005 || latency>1.0) latency=0.05;
      rx->output_stream->software_latency = latency;
      rx->output_stream->userdata=(void *)rx;

      if((err = soundio_outstream_open(rx->output_stream))) {
        log_info("audio_open_output: unable to open output stream: %s", soundio_strerror(err));
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      // What the backend actually gave us.  The numbers in the audio-flow
      // diagnostics below are meaningless without these: a device running at a
      // rate other than 48 kHz goes through the resampler, and a layout that is
      // not stereo is what broke the JACK path before.
      log_info("audio_open_output: SOUNDIO stream: backend=%s rate=%d channels=%d "
               "latency=%.1f ms ring=%d frames\n",
               soundio_backend_name(soundio->current_backend),
               rx->output_stream->sample_rate,
               rx->output_stream->layout.channel_count,
               rx->output_stream->software_latency*1000.0,
               size/(int)(sizeof(float)*2));

      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
log_info("audio_open_output: PULSEAUDIO: %s\n",rx->audio_name);

      {
        // "System Default" (and an unconfigured receiver) means: give
        // pa_simple_new no device at all.  A stream with no explicit target is
        // attached to the server's default sink AND is moved by the server when
        // the user changes it — so this follows the system default for free,
        // without the poll-and-reopen timer the SoundIo path needs.
        const char *dev=rx->audio_name;
        if(dev!=NULL && strcmp(dev,AUDIO_SYSTEM_DEFAULT_NAME)==0) dev=NULL;

        g_mutex_lock(&rx->local_audio_mutex);
        sample_spec.rate=48000;
        sample_spec.channels=2;
        sample_spec.format=PA_SAMPLE_FLOAT32NE;

        char stream_id[16];
        sprintf(stream_id,"RX-%d",rx->channel);

        rx->playstream=pa_simple_new(NULL,               // Use the default server.
                        "MacHPSDR",           // Our application's name.
                        PA_STREAM_PLAYBACK,
                        dev,
                        stream_id,            // Description of our stream.
                        &sample_spec,                // Our sample format.
                        NULL,               // Use default channel map
                        NULL,               // Use default buffering attributes.
                        &err               // error code if returns NULL
                        );
    
        if(rx->playstream!=NULL) {
          rx->local_audio_buffer_offset=0;
          rx->local_audio_buffer=g_new0(float,2*rx->local_audio_buffer_size);
          log_info("audio_open_output: allocated local_audio_buffer %p size %ld bytes\n",rx->local_audio_buffer,2*rx->local_audio_buffer_size*sizeof(float));
        } else {
          result=-1;
          log_error("pa-simple_new failed: err=%d\n",err);
        }
        g_mutex_unlock(&rx->local_audio_mutex);
      }
      break;
    }
    case USE_ALSA: {
log_info("audio_open_output: ALSA: %s\n",rx->audio_name);
      int err;
      unsigned int rate = 48000;
      unsigned int channels=2;
      int soft_resample=1;
      unsigned int latency=125000;      
      
      if(rx->audio_name==NULL) {
        rx->local_audio=0;
        return -1;
      }

      int i;
      char hw[128];

      // The list entries carry the card name after the PCM name ("plughw:0,0
      // VirtIO SoundCard"), so the device is everything up to the first space —
      // but the string may have no space at all, and the loop must then stop at
      // the terminator rather than run on into whatever follows in memory.
      if(strcmp(rx->audio_name,AUDIO_SYSTEM_DEFAULT_NAME)==0) {
        // "default" is ALSA's own default PCM: the raw card on a bare-ALSA
        // machine, and the sound server's plugin where one is running.
        snprintf(hw,sizeof(hw),"default");
      } else {
        i=0;
        while(i<(int)sizeof(hw)-1 && rx->audio_name[i]!=' ' && rx->audio_name[i]!='\0') {
          hw[i]=rx->audio_name[i];
          i++;
        }
        hw[i]='\0';
      }

    log_info("audio_open_output: hw=%s\n",hw);

      for(i=0;i<FORMATS;i++) {
        g_mutex_lock(&rx->local_audio_mutex);
        if ((err = snd_pcm_open (&rx->playback_handle, hw, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
          log_info("audio_open_output: cannot open audio device %s (%s)\n", 
                  hw,
                  snd_strerror (err));
          g_mutex_unlock(&rx->local_audio_mutex);
          return err;
        }
    log_info("audio_open_output: handle=%p\n",rx->playback_handle);

    log_info("audio_open_output: trying format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
        if ((err = snd_pcm_set_params (rx->playback_handle,formats[i],SND_PCM_ACCESS_RW_INTERLEAVED,channels,rate,soft_resample,latency)) < 0) {
          log_info("audio_open_output: snd_pcm_set_params failed: %s\n",snd_strerror(err));
          g_mutex_unlock(&rx->local_audio_mutex);
          audio_close_output(rx);
          continue;
        } else {
    log_info("audio_open_output: using format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
          rx->local_audio_format=formats[i];
          break;
        }
      }

      if(i>=FORMATS) {
        log_info("audio_open_output: cannot find usable format\n");
        return err;
      }

      rx->local_audio_buffer_offset=0;
      switch(rx->local_audio_format) {
        case SND_PCM_FORMAT_S16_LE:
    log_info("audio_open_output: local_audio_buffer: size=%d sample=%ld\n",rx->local_audio_buffer_size,sizeof(gint16));
          rx->local_audio_buffer=g_new(gint16,2*rx->local_audio_buffer_size);
            break;
        case SND_PCM_FORMAT_S32_LE:
    log_info("audio_open_output: local_audio_buffer: size=%d sample=%ld\n",rx->local_audio_buffer_size,sizeof(gint32));
          rx->local_audio_buffer=g_new(gint32,2*rx->local_audio_buffer_size);
          break;
        case SND_PCM_FORMAT_FLOAT_LE:
    log_info("audio_open_output: local_audio_buffer: size=%d sample=%ld\n",rx->local_audio_buffer_size,sizeof(gfloat));
          rx->local_audio_buffer=g_new(gfloat,2*rx->local_audio_buffer_size);
          break;

        default: return -1;
      }
      
      log_info("audio_open_output: rx=%d handle=%p buffer=%p size=%d\n",rx->channel,rx->playback_handle,rx->local_audio_buffer,rx->local_audio_buffer_size);

      // What ALSA actually negotiated.  We ask for 125 ms of latency; the card
      // decides the real buffer and period, and those two numbers set how much
      // producer jitter the path can absorb before it gaps.
      {
        snd_pcm_uframes_t abuf=0,aper=0;
        if(snd_pcm_get_params(rx->playback_handle,&abuf,&aper)==0)
          log_info("audio_open_output: ALSA params: buffer=%lu frames period=%lu frames "
                   "(block=%d frames)\n",
                   (unsigned long)abuf,(unsigned long)aper,rx->local_audio_buffer_size);
      }


      g_mutex_unlock(&rx->local_audio_mutex);          
      break;
    }
#endif
  }
  return result;
}
	
int audio_open_input(RADIO *r) {
  int result=0;
  int err;
#ifndef __APPLE__
  pa_sample_spec sample_spec;
#endif

  log_info("%s\n",__FUNCTION__);
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      if(soundio==NULL) {
        log_info("audio_open_input: no soundio backend connected\n");
        return -1;
      }
      if(r->microphone_name==NULL) {
        log_info("audio_open_input: microphone name is NULL\n");
        return -1;
      }
      // Idempotent: drop any mic stream already open (normal open path and the
      // system-default input monitor must not race into two read threads).
      if(r->input_stream!=NULL) audio_close_input(r);

      log_info("audio_open_input: %s\n",r->microphone_name);
      // find the device
      int input_index=-1;
      for(int i=0;i<n_input_devices;i++) {
        if(strcmp(r->microphone_name,input_devices[i].name)==0) {
          input_index=input_devices[i].index;
          break;
        }
      }

      // input_index==-1 means the user picked the synthetic "System Default"
      // entry (index -1 by design), or the configured mic was not found.  In
      // both cases follow the current system default input; re-flush events so
      // a device connected after launch is picked up.
      if(input_index==-1) {
        soundio_flush_events(soundio);
        int def=soundio_default_input_device_index(soundio);
        if(def>=0) {
          log_info("audio_open_input: '%s' -> using current default input device\n",r->microphone_name);
          input_index=def;
        }
      }

      if(input_index==-1) {
        log_info("audio_open_input: did not find %s\n",r->microphone_name);
        return -1;
      }


      r->input_device = soundio_get_input_device(soundio, input_index);
      if(!r->input_device) {
        log_info("audio_open_input: could not get input device: out of memory");
        return -1;
      }

      if(!soundio_device_supports_format(r->input_device, SoundIoFormatFloat32NE)) {
        log_info("audio_open_input: device does not support SoundIoFormatFloat32NE");
        return -1;
      }

      // As for output: if the capture device does not run at 48 kHz (e.g. a
      // Bluetooth headset mic at 16 kHz) open it at its nearest rate and let
      // read_callback resample the captured audio up to 48 kHz.
      int in_device_rate=sample_rate;
      if(!soundio_device_supports_sample_rate(r->input_device, sample_rate)) {
        in_device_rate=soundio_device_nearest_sample_rate(r->input_device, sample_rate);
        if(in_device_rate<=0) {
          log_info("audio_open_input: device has no usable sample rate\n");
          return -1;
        }
        log_info("audio_open_input: device does not support %d Hz; opening at %d Hz with resampling\n",
                sample_rate, in_device_rate);
      }
      r->mic_resample_rate=(in_device_rate==sample_rate)?0:in_device_rate;
      r->mic_resample_phase=0.0;
      r->mic_resample_prev=0.0f;

      r->input_stream = soundio_instream_create(r->input_device);
      if(!r->input_stream) {
        log_info("audio_open_input: could not open input device: out of memory");
        return -1;
      }
      r->input_stream->format = SoundIoFormatFloat32NE;
      r->input_stream->sample_rate = in_device_rate;
      r->input_stream->read_callback = read_callback;
      r->input_stream->userdata=(void *)r;

      if((err = soundio_instream_open(r->input_stream))) {
        log_info("audio_open_input: unable to open input stream: %s", soundio_strerror(err));
        return -1;
      }

      // guess that 8 input buffers should be enough
      int size=8*512*sizeof(float);
      r->ring_buffer = soundio_ring_buffer_create(soundio, size);
      if(!r->ring_buffer) {
        log_info("audio_open_input: soundio_ring_buffer_create failed");
        return -1;
      }

      if((err = soundio_instream_start(r->input_stream))) {
        log_info("unable to start input device: %s", soundio_strerror(err));
        return -1;
      }
      r->input_started=TRUE;
      running=TRUE;
      log_info("%s: SOUNDIO mic_read_thread\n",__FUNCTION__);
      mic_read_thread_id = g_thread_new( "mic_thread", mic_read_thread, r);
      if(!mic_read_thread_id ) {
        log_error("g_thread_new failed on mic_read_thread\n");
        soundio_instream_destroy(r->input_stream);
        soundio_device_unref(r->input_device);
        soundio_ring_buffer_destroy(r->ring_buffer);
        if(r->local_microphone_buffer!=NULL) {
          g_free(r->local_microphone_buffer);
          r->local_microphone_buffer=NULL;
        }
        running=FALSE;
        result=-1;
      }
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      if(r->microphone_name==NULL) {
        return -1;
      }

      g_mutex_lock(&r->local_microphone_mutex);
      
      
      pa_buffer_attr attr;
      attr.maxlength = (uint32_t) -1;
      attr.tlength = (uint32_t) -1;
      attr.prebuf = (uint32_t) -1;
      attr.minreq = (uint32_t) -1;
      attr.fragsize = 512;    
      
      
      sample_spec.rate=48000;
      sample_spec.channels=1;
      sample_spec.format=PA_SAMPLE_FLOAT32NE;

      // Same rule as the output: no device given == the server's default source,
      // which the server also keeps following if the user changes it.
      const char *mic_dev=r->microphone_name;
      if(mic_dev!=NULL && strcmp(mic_dev,AUDIO_SYSTEM_DEFAULT_NAME)==0) mic_dev=NULL;

      r->microphone_stream=pa_simple_new(NULL,               // Use the default server.
                      "MacHPSDR",           // Our application's name.
                      PA_STREAM_RECORD,
                      mic_dev,
                      "TX",            // Description of our stream.
                      &sample_spec,                // Our sample format.
                      NULL,               // Use default channel map
                      //NULL,
                      &attr,               // Use default buffering attributes.
                      NULL               // Ignore error code.
                      );

      if(r->microphone_stream!=NULL) {
        r->local_microphone_buffer_offset=0;
        r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
        running=TRUE;
        log_info("%s: PULSEAUDIO mic_read_thread\n",__FUNCTION__);
        mic_read_thread_id = g_thread_new( "mic_thread", mic_read_thread, r);
        if(!mic_read_thread_id ) {
          log_error("g_thread_new failed on mic_read_thread\n");
          g_free(r->local_microphone_buffer);
          r->local_microphone_buffer=NULL;
          running=FALSE;
          result=-1;
        }
      } else {
        result=-1;
      }
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
    case USE_ALSA: {
      int err;
      unsigned int rate=48000;
      unsigned int channels=1;
      int soft_resample=1;
      unsigned int latency=125000;
      
      char hw[64];
      int i = 0;
      
      if(r->microphone_name==NULL) {
        r->local_microphone=0;
        return -1;
      }
        
      // Same as the output side — and this loop was additionally unbounded on
      // BOTH ends: it stopped only at a space, so a name without one (every
      // dmix: entry, and AUDIO_SYSTEM_DEFAULT_NAME) ran off the end of the
      // string and off the end of a 64-byte stack buffer.
      if(strcmp(r->microphone_name,AUDIO_SYSTEM_DEFAULT_NAME)==0) {
        snprintf(hw,sizeof(hw),"default");
      } else {
        i=0;
        while(i<(int)sizeof(hw)-1 && r->microphone_name[i]!=' ' && r->microphone_name[i]!='\0') {
          hw[i]=r->microphone_name[i];
          i++;
        }
        hw[i]='\0';
      }


      log_info("audio_open_input: hw=%s\n",hw);

      for(i=0;i<FORMATS;i++) {
        if ((err = snd_pcm_open (&r->record_handle, hw, SND_PCM_STREAM_CAPTURE, SND_PCM_ASYNC)) < 0) {
          log_info("audio_open_input: cannot open audio device %s (%s)\n",
                  hw,
                  snd_strerror (err));
          return err;
        }
log_info("audio_open_input: handle=%p\n",r->record_handle);

log_info("audio_open_input: trying format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
        if ((err = snd_pcm_set_params (r->record_handle,formats[i],SND_PCM_ACCESS_RW_INTERLEAVED,channels,rate,soft_resample,latency)) < 0) {
          log_info("audio_open_input: snd_pcm_set_params failed: %s\n",snd_strerror(err));
          audio_close_input(r);
          continue;
        } else {
log_info("audio_open_input: using format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
          record_audio_format=formats[i];
          break;
        }
      }

      if(i>=FORMATS) {
log_info("audio_open_input: cannot find usable format\n");
        return err;
      }

log_info("audio_open_input: format=%d\n",record_audio_format);

      switch(record_audio_format) {
        case SND_PCM_FORMAT_S16_LE:
log_info("audio_open_input: mic_buffer: size=%d channels=%d sample=%ld bytes\n",r->local_microphone_buffer_size,channels,sizeof(gint16));
          r->local_microphone_buffer = g_new(float, r->local_microphone_buffer_size);
          break;
        case SND_PCM_FORMAT_S32_LE:
log_info("audio_open_input: mic_buffer: size=%d channels=%d sample=%ld bytes\n",r->local_microphone_buffer_size,channels,sizeof(gint32));
          r->local_microphone_buffer = g_new(float, r->local_microphone_buffer_size);
          break;
        case SND_PCM_FORMAT_FLOAT_LE:
log_info("audio_open_input: mic_buffer: size=%d channels=%d sample=%ld bytes\n",r->local_microphone_buffer_size,channels,sizeof(gfloat));
          r->local_microphone_buffer=g_new(gfloat, r->local_microphone_buffer_size);
          break;
          
        default: return -1;          
      }

      //r->local_microphone_buffer_offset=0;
      //r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
      running=TRUE;

      log_info("%s: ALSA mic_read_thread\n",__FUNCTION__);
      GError *error;
      mic_read_thread_id = g_thread_try_new("microphone",mic_read_thread,r,&error);
      if(!mic_read_thread_id ) {
        log_info("g_thread_new failed on mic_read_thread: %s\n",error->message);
        g_free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
        running=FALSE;
        result=-1;        
      }
      break;
  }
#endif  
  }
  return result;
}

void audio_close_output(RECEIVER *rx) {
 log_info("audio_close_output\n");
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->output_stream!=NULL) {
        soundio_outstream_destroy(rx->output_stream);
        rx->output_stream=NULL;
      }
      if(rx->output_device!=NULL) {
        soundio_device_unref(rx->output_device);
        rx->output_device=NULL;
      } 
      if(rx->ring_buffer!=NULL) {
        soundio_ring_buffer_destroy(rx->ring_buffer);
        rx->ring_buffer=NULL;
      }
      rx->output_started=FALSE;
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->playstream!=NULL) {
        pa_simple_free(rx->playstream);
        rx->playstream=NULL;
      }
      if(rx->local_audio_buffer!=NULL) {
        g_free(rx->local_audio_buffer);
        rx->local_audio_buffer=NULL;
      }
      rx->output_started=FALSE;
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
    case USE_ALSA: {    
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->playback_handle!=NULL) {
        snd_pcm_close (rx->playback_handle);
        rx->playback_handle=NULL;
      }
      if(rx->local_audio_buffer!=NULL) {
        g_free(rx->local_audio_buffer);
        rx->local_audio_buffer=NULL;
      }
      rx->output_started=FALSE;
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#endif
  }
}

void audio_close_input(RADIO *r) {
  log_info("Close audio input\n");
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      // Stop the mic read thread first so it can't touch the ring buffer/stream
      // we are about to destroy.  It may be blocked waiting for data, so clear
      // `running` and wake it, then join.
      running=FALSE;
      g_mutex_lock(&r->ring_buffer_mutex);
      g_cond_signal(&r->ring_buffer_cond);
      g_mutex_unlock(&r->ring_buffer_mutex);
      if(mic_read_thread_id!=NULL) {
        g_thread_join(mic_read_thread_id);
        mic_read_thread_id=NULL;
      }
      g_mutex_lock(&r->local_microphone_mutex);
      // Destroying the stream stops its read_callback before we free the ring
      // buffer it writes into.
      if(r->input_stream!=NULL) { soundio_instream_destroy(r->input_stream); r->input_stream=NULL; }
      if(r->input_device!=NULL) { soundio_device_unref(r->input_device); r->input_device=NULL; }
      if(r->ring_buffer!=NULL) { soundio_ring_buffer_destroy(r->ring_buffer); r->ring_buffer=NULL; }
      r->input_started=FALSE;
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      g_mutex_lock(&r->local_microphone_mutex);
      if(r->microphone_stream!=NULL) {
        pa_simple_free(r->microphone_stream);
        r->microphone_stream=NULL;
        g_free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
      }
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
    case USE_ALSA: {
      g_mutex_lock(&r->local_microphone_mutex);
      running=FALSE;
      if(mic_read_thread_id!=NULL) {
log_info("audio_close_input: wait for thread to complete\n");
        g_thread_join(mic_read_thread_id);
        mic_read_thread_id=NULL;
      }      
      if(r->record_handle!=NULL) {
log_info("audio_close_input: snd_pcm_close\n");        
        snd_pcm_close (r->record_handle);
        r->record_handle=NULL;
      }
      if(r->local_microphone_buffer!=NULL) {
log_info("audio_close_input: free mic buffer\n");
        g_free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
      }      
      
      g_free(r->local_microphone_buffer);
      r->local_microphone_buffer=NULL;
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
#endif
  }
}

void audio_start_output(RECEIVER *rx) {
  int err;
  switch(radio->which_audio) {
    case USE_SOUNDIO:
      if(rx->output_stream!=NULL) {
        if(!rx->output_started) {
          // Don't start playback until the ring buffer holds a cushion (~60 ms):
          // starting on a near-empty buffer makes the consumer callback drain to
          // empty and underrun on the slightest producer jitter, heard as
          // periodic "buffer gaps" (worst at high DSP rates where each feed is
          // only a few ms of audio).  audio_write keeps filling the ring while
          // we wait; this is retried every buffer until the cushion is reached.
          int frames = soundio_ring_buffer_fill_count(rx->ring_buffer) / (int)(sizeof(float)*2);
          if(frames < rx->output_stream->sample_rate / 16) break;
          underflow_count=0;
          if((err = soundio_outstream_start(rx->output_stream))) {
              log_info("audio_start_output: unable to start output device: %s", soundio_strerror(err));
          } else {
            rx->output_started=TRUE;
          }
        }
      }
      break;
#ifndef __APPLE__
    case USE_PULSEAUDIO:
      rx->output_started=TRUE;
      break;
    case USE_ALSA:
      rx->output_started=TRUE;
      break;
#endif
  }
}

int audio_write(RECEIVER *rx,float left_sample,float right_sample) {
  int result=0;
  int rc;
  int err;
  float *float_buffer;  
  
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      float samples[2];
      samples[0]=left_sample;
      samples[1]=right_sample;
      if(rx->ring_buffer!=NULL) {
        char *buf = soundio_ring_buffer_write_ptr(rx->ring_buffer);
        int fill_count = sizeof(float)*2;
        int free=soundio_ring_buffer_free_count(rx->ring_buffer);
        int refused=0;
        if(free<fill_count) {
          //g_print("audio_write: ring buffer full: need %d free %d\n",fill_count,free);
          refused=1;
        } else {
          memcpy(buf, &samples[0], fill_count);
          soundio_ring_buffer_advance_write_ptr(rx->ring_buffer, fill_count);
        }
        audio_stats_frame(rx,
                          audio_debug_on()
                            ? soundio_ring_buffer_fill_count(rx->ring_buffer)/(int)(sizeof(float)*2)
                            : -1,
                          refused,0,0);
      }
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->local_audio_buffer==NULL) {
        rx->local_audio_buffer_offset=0;
        rx->local_audio_buffer=g_new0(float,2*rx->local_audio_buffer_size);
      }
      
      float_buffer=(float *)rx->local_audio_buffer;
      float_buffer[rx->local_audio_buffer_offset*2]=left_sample;
      float_buffer[(rx->local_audio_buffer_offset*2)+1]=right_sample;
      //rx->local_audio_buffer[rx->local_audio_buffer_offset*2]=left_sample;
      //rx->local_audio_buffer[(rx->local_audio_buffer_offset*2)+1]=right_sample;
      
      rx->local_audio_buffer_offset++;
      if(rx->local_audio_buffer_offset>=rx->local_audio_buffer_size) {
        rc=pa_simple_write(rx->playstream,
                           rx->local_audio_buffer,
                           rx->local_audio_buffer_size*sizeof(float)*2,
                           &err);
        if(rc!=0) {
          log_error("audio_write failed err=%d\n",err);
        }
        rx->local_audio_buffer_offset=0;
        if(audio_debug_on()) {
          // What the server still has queued for us, in frames.
          pa_usec_t lat=pa_simple_get_latency(rx->playstream,&err);
          pa_alsa_dbg_fill=(lat==(pa_usec_t)-1)?-1:(int)(lat*48ULL/1000ULL);
        }
      }
      audio_stats_frame(rx,audio_debug_on()?pa_alsa_dbg_fill:-1,0,0,0);
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
    case USE_ALSA: {
      snd_pcm_sframes_t delay;
      long trim;
      

      gint32 *long_buffer;
      gint16 *short_buffer;      

      g_mutex_lock(&rx->local_audio_mutex);

      if(rx->playback_handle!=NULL && rx->local_audio_buffer!=NULL) {
        switch(rx->local_audio_format) {
          case SND_PCM_FORMAT_S16_LE:
            short_buffer=(gint16 *)rx->local_audio_buffer;
            short_buffer[rx->local_audio_buffer_offset*2]=(gint16)(left_sample*32767.0F);
            short_buffer[(rx->local_audio_buffer_offset*2)+1]=(gint16)(right_sample*32767.0F);
            break;
          case SND_PCM_FORMAT_S32_LE:
            long_buffer=(gint32 *)rx->local_audio_buffer;
            // Full-scale for signed 32-bit is INT32_MAX (2147483647), not
            // 4294967295 (that is UINT32_MAX — twice too large, so anything past
            // half scale overflowed and wrapped into loud distortion).
            long_buffer[rx->local_audio_buffer_offset*2]=(gint32)(left_sample*2147483647.0F);
            long_buffer[(rx->local_audio_buffer_offset*2)+1]=(gint32)(right_sample*2147483647.0F);
            break;
          case SND_PCM_FORMAT_FLOAT_LE:
            float_buffer=(float *)rx->local_audio_buffer;
            float_buffer[rx->local_audio_buffer_offset*2]=left_sample;
            float_buffer[(rx->local_audio_buffer_offset*2)+1]=right_sample;
            break;
            
          default:
            // Unknown format: bail out, but release the lock first (the bare
            // `return -1` here used to strand rx->local_audio_mutex held).
            g_mutex_unlock(&rx->local_audio_mutex);
            return -1;
        }
        rx->local_audio_buffer_offset++;

        if(rx->local_audio_buffer_offset>=rx->local_audio_buffer_size) {

          trim=0;

/*
          int max_delay=rx->local_audio_buffer_size*6;
          if(snd_pcm_delay(rx->playback_handle,&delay)==0) {
            if(delay>max_delay) {
              trim=delay-max_delay;
log_info("audio delay=%ld trim=%ld audio_buffer_size=%d\n",delay,trim,rx->local_audio_buffer_size);
            }
          }
*/
          if(trim<rx->local_audio_buffer_size) {
            // The PCM is opened SND_PCM_NONBLOCK, so a momentarily full device
            // buffer returns -EAGAIN and a partial write returns fewer frames
            // than asked.  Both used to discard the rest of the block ("ignore
            // short write"), which is heard as periodic dropouts even though
            // the card was perfectly healthy — mere producer jitter was enough.
            // Write the remainder instead, waiting for space and recovering
            // from an underrun.  The wait is bounded (~50 ms total) so a device
            // that is genuinely slower than the radio's clock still drops
            // rather than back-pressuring — and eventually stalling — the RX
            // thread that calls us.
            snd_pcm_uframes_t frames = rx->local_audio_buffer_size-trim;
            const char *p = (const char *)rx->local_audio_buffer;
            ssize_t frame_bytes = snd_pcm_frames_to_bytes(rx->playback_handle, 1);
            int waits = 0;
            int xruns = 0;
            int guard = 0;
            // Budget: two PERIODS, not a whole block.  A healthy card frees
            // space one period at a time, so one period plus a margin is all a
            // transient can need — while a block's worth (43 ms) is long enough
            // to be felt upstream: measured on the reporter's box, waiting that
            // long per block dragged the RX thread's own output from 48000 down
            // to 30000-43000 frames/s (maxgap 80 ms).  This loop runs ON the RX
            // thread, so a stall here is not merely late audio, it is the whole
            // DSP/decoder chain missing its deadline.
            snd_pcm_uframes_t abuf=0,aper=0;
            if(snd_pcm_get_params(rx->playback_handle,&abuf,&aper)!=0 || aper==0)
              aper=1024;
            const gint64 deadline =
              g_get_monotonic_time() + 2*(gint64)aper*1000000/48000;
            while(frames > 0 && ++guard <= 64) {
              rc = snd_pcm_writei(rx->playback_handle, p, frames);
              if(rc > 0) {
                p += (size_t)rc*(size_t)frame_bytes;
                frames -= (snd_pcm_uframes_t)rc;
                continue;
              }
              if(rc == -EAGAIN) {
                // A full device buffer on a stream that was never started stays
                // full for ever.  snd_pcm_set_params leaves the start threshold
                // to alsa-lib, so make it explicit rather than depend on it.
                if(snd_pcm_state(rx->playback_handle)==SND_PCM_STATE_PREPARED)
                  snd_pcm_start(rx->playback_handle);
                // A device that is full and STILL has no space after a wait is
                // not jittering, it is slower than the radio (or, as seen on a
                // virtio card under a sound server, RUNNING and draining
                // nothing at all — delay pinned at the full buffer, every block
                // dropped).  Waiting longer cannot conjure space and only holds
                // up the RX thread, so drop the rest of the block now.
                if(waits>0 && snd_pcm_avail_update(rx->playback_handle)<=0) break;
                if(g_get_monotonic_time() >= deadline) break;  // give up, drop the rest
                waits++;
                // The card frees space one PERIOD at a time (1024 frames =
                // 21 ms here), so a timeout is the ordinary outcome and must
                // not end the write: the previous 5 ms wait timed out on nearly
                // every block and dropped it whole, losing 97 % of the audio
                // with the device perfectly healthy.  Only an error ends it.
                if(snd_pcm_wait(rx->playback_handle, 10) < 0) break;
                continue;
              }
              if(rc < 0) {
                xruns++;
                if(snd_pcm_recover(rx->playback_handle, rc, 1) < 0) {
                  log_info("audio_write: ALSA write failed: %s\n", snd_strerror(rc));
                  break;
                }
                continue;   // recovered (underrun/suspend) — retry the remainder
              }
              break;        // rc == 0 and no error: nothing accepted, avoid spinning
            }
            if(audio_debug_on()) {
              // How much audio the card still has to play, in frames: 0 means we
              // are running on empty (every hiccup is then an xrun), a value
              // pinned at the buffer size means we are pushing into a full
              // device and the tail of each block is being dropped.
              if(snd_pcm_delay(rx->playback_handle,&delay)==0)
                pa_alsa_dbg_fill=(int)delay;
              // A dropped block is worth a line of its own, rate-limited: the
              // PCM state is what separates "the card is slower than the radio"
              // (RUNNING) from "the stream never started" (PREPARED), and the
              // aggregate counters cannot tell those apart.
              if(frames>0) {
                static gint64 last_drop_log=0;
                gint64 now=g_get_monotonic_time();
                if(now-last_drop_log>1000000) {
                  last_drop_log=now;
                  log_info("audio_write: ALSA dropped %lu of %d frames "
                           "(state=%s delay=%ld waits=%d xruns=%d)\n",
                           (unsigned long)frames,rx->local_audio_buffer_size,
                           snd_pcm_state_name(snd_pcm_state(rx->playback_handle)),
                           (long)delay,waits,xruns);
                }
              }
            }
            // Outside the debug block: the drop count feeds the warning.
            audio_stats_frame(rx,audio_debug_on()?pa_alsa_dbg_fill:-1,
                              (int)frames,xruns,waits);
          }
          rx->local_audio_buffer_offset=0;
        }
        if(rx->local_audio_buffer_offset!=0)
          audio_stats_frame(rx,audio_debug_on()?pa_alsa_dbg_fill:-1,0,0,0);
      }

      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }      
#endif
  }
  return result;
}

static void *mic_read_thread(gpointer arg) {
  RADIO *r=(RADIO *)arg;
  int rc;
  int err;
  log_info("mic_read_thread: ENTRY\n");
  switch(radio->which_audio) {
    case USE_SOUNDIO:
      while(running) {
        g_mutex_lock (&r->ring_buffer_mutex);
        // Also watch `running` so audio_close_input can wake and stop us even
        // when no more mic data is arriving (needed to join cleanly on a live
        // input-device switch or shutdown).
        while(running && soundio_ring_buffer_fill_count(r->ring_buffer)==0)
          g_cond_wait (&r->ring_buffer_cond, &r->ring_buffer_mutex);
        if(!running) { g_mutex_unlock (&r->ring_buffer_mutex); break; }
        char *read_ptr = soundio_ring_buffer_read_ptr(r->ring_buffer);
        int fill_bytes = soundio_ring_buffer_fill_count(r->ring_buffer);
        if(fill_bytes>(r->local_microphone_buffer_size*sizeof(float))) {
          fill_bytes=r->local_microphone_buffer_size*sizeof(float);
        }
        memcpy(r->local_microphone_buffer,read_ptr,fill_bytes);
        soundio_ring_buffer_advance_read_ptr(r->ring_buffer, fill_bytes);
        g_mutex_unlock (&r->ring_buffer_mutex);
        switch(r->discovered->protocol) {
          case PROTOCOL_1:
            protocol1_process_local_mic(r);
            break;
          case PROTOCOL_2:
            protocol2_process_local_mic(r);
            break;
#ifdef SOAPYSDR
          case PROTOCOL_SOAPYSDR:
            soapy_protocol_process_local_mic(r);
            break;
#endif
        }
      }
      break;
#ifndef __APPLE__
    case USE_PULSEAUDIO:
      while(running) {
        g_mutex_lock(&r->local_microphone_mutex);
        if(r->local_microphone_buffer==NULL) {
          running=0;
        } else {
          rc=pa_simple_read(r->microphone_stream,
      		r->local_microphone_buffer,
      		r->local_microphone_buffer_size*sizeof(float),
      		&err); 
          if(rc<0) {
            running=0;
            log_info("mic_read_thread: returned %d error=%d (%s)\n",rc,err,pa_strerror(err));
          } else {
            switch(r->discovered->protocol) {
              case PROTOCOL_1:
                protocol1_process_local_mic(r);
                break;
              case PROTOCOL_2:
                protocol2_process_local_mic(r);
                break;
#ifdef SOAPYSDR
              case PROTOCOL_SOAPYSDR:
                soapy_protocol_process_local_mic(r);
                break;
#endif
            }
          }
        }
        g_mutex_unlock(&r->local_microphone_mutex);
      }
      break;

    case USE_ALSA:
      {
      int rc;
      if ((rc = snd_pcm_start (r->record_handle)) < 0) {
    log_info("mic_read_thread: ALSA: cannot start audio interface for use (%s)\n",
            snd_strerror (rc));

        return NULL;
      }
log_info("mic_read_thread: ALSA: mic_buffer_size=%d\n",radio->local_microphone_buffer_size);
      running=TRUE;
      while(running) {
        if ((rc = snd_pcm_readi (r->record_handle, r->local_microphone_buffer, r->local_microphone_buffer_size)) != r->local_microphone_buffer_size) {
          if(running) {
            if(rc<0) {
              if(rc==-EPIPE) {
                //g_print("mic_read_thread: -EPIPE: snd_pcm_prepare\n");
                if ((rc = snd_pcm_prepare (r->record_handle)) < 0) {
                    log_info("mic_read_thread: ALSA: cannot prepare audio interface for use %d (%s)\n", rc, snd_strerror (rc));
                    //return rc;
                }
              } else {
                log_error("mic_read_thread: ALSA: read from audio interface failed (%s)\n",
                      snd_strerror (rc));
                running=FALSE;
              }
            } else {
              log_info("mic_read_thread: ALSA: read %d\n",rc);
            }
          }
        } else {
          // process the mic input
          switch(r->discovered->protocol) {
            case PROTOCOL_1:        
              protocol1_process_local_mic(r);
              break;
            case PROTOCOL_2:
              protocol2_process_local_mic(r);
              break;
#ifdef SOAPYSDR
            case PROTOCOL_SOAPYSDR:
              soapy_protocol_process_local_mic(r);
              break;
#endif
          }
        }
      }

      }
      break;
#endif
  }
log_info("mic_read_thread: EXIT\n");
  return NULL;
}

void audio_get_cards(void) {
}

#ifndef __APPLE__
static void source_list_cb(pa_context *context,const pa_source_info *s,int eol,void *data) {
  int i;
  if(eol>0) {
    for(i=0;i<n_input_devices;i++) {
      log_info("Input: %d: %s (%s)\n",input_devices[i].index,input_devices[i].name,input_devices[i].description);
    }
  } else if(n_input_devices<MAX_AUDIO_DEVICES) {
    input_devices[n_input_devices].name=g_new0(char,strlen(s->name)+1);
    memcpy(input_devices[n_input_devices].name, s->name, strlen(s->name));
    input_devices[n_input_devices].description=g_new0(char,strlen(s->description)+1);
    memcpy(input_devices[n_input_devices].description,s->description, strlen(s->description));
    input_devices[n_input_devices].index=s->index;
    
    n_input_devices++;
  }
}

static void sink_list_cb(pa_context *context,const pa_sink_info *s,int eol,void *data) {
  int i;
  if(eol>0) {
    for(i=0;i<n_output_devices;i++) {
      log_info("Output: %d: %s (%s)\n",output_devices[i].index,output_devices[i].name,output_devices[i].description);
    }
    op=pa_context_get_source_info_list(pa_ctx,source_list_cb,NULL);
  } else if(n_output_devices<MAX_AUDIO_DEVICES) {
    output_devices[n_output_devices].name=g_new0(char,strlen(s->name)+1);
    memcpy(output_devices[n_output_devices].name,s->name,strlen(s->name));
    output_devices[n_output_devices].description=g_new0(char,strlen(s->description)+1);
    memcpy(output_devices[n_output_devices].description,s->description,strlen(s->description));
    output_devices[n_output_devices].index=s->index;
    n_output_devices++;
  }
}

static void state_cb(pa_context *c, void *userdata) {
        pa_context_state_t state;
        int *ready = userdata;

        state = pa_context_get_state(c);

log_info("%s: %d\n",__FUNCTION__,state);
        switch  (state) {
                // There are just here for reference
                case PA_CONTEXT_UNCONNECTED:
log_info("audio: state_cb: PA_CONTEXT_UNCONNECTED\n");
                        break;
                case PA_CONTEXT_CONNECTING:
log_info("audio: state_cb: PA_CONTEXT_CONNECTING\n");
                        break;
                case PA_CONTEXT_AUTHORIZING:
log_info("audio: state_cb: PA_CONTEXT_AUTHORIZING\n");
                        break;
                case PA_CONTEXT_SETTING_NAME:
log_info("audio: state_cb: PA_CONTEXT_SETTING_NAME\n");
                        break;
                case PA_CONTEXT_FAILED:
log_info("audio: state_cb: PA_CONTEXT_FAILED\n");
                        *ready = 2;
                        break;
                case PA_CONTEXT_TERMINATED:
log_info("audio: state_cb: PA_CONTEXT_TERMINATED\n");
                        *ready = 2;
                        break;
                case PA_CONTEXT_READY:
log_info("audio: state_cb: PA_CONTEXT_READY\n");
                        *ready = 1;
// get a list of the output devices.  Seed both lists with the same synthetic
// "System Default" entry the SoundIo path offers, so the choice is in the menu
// whichever audio system is selected; audio_open_output/-input turn it into "no
// device given", which is how PulseAudio spells "the default, and keep following
// it".  index -1 marks it as not being a real sink/source index.
                        n_input_devices=0;
                        n_output_devices=0;
                        output_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
                        output_devices[0].description=g_strdup("System Default");
                        output_devices[0].index=-1;
                        n_output_devices=1;
                        input_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
                        input_devices[0].description=g_strdup("System Default");
                        input_devices[0].index=-1;
                        n_input_devices=1;
                        op = pa_context_get_sink_info_list(pa_ctx,sink_list_cb,NULL);
                        break;
                default:
                        log_info("audio: state_cb: unknown state %d\n",state);
                        break;
        }
}
#endif

// A backend enumerating a device is not a promise that the device can be used.
// The ALSA backend in particular lists every PCM name in the system, and on any
// machine running a sound server (PipeWire/PulseAudio/JACK) the server holds the
// card, so all the direct entries for it — hw:, plughw:, sysdefault:, dmix:, and
// the raw hw:C,D — fail at snd_pcm_open with EBUSY.  They used to be offered in
// the receiver's output menu anyway: picking one silently did nothing, which is
// what made the menu look full of devices "that aren't there".
//
// There is no property that predicts this; the only honest test is to open a
// stream the same way audio_open_output does and see whether it succeeds.  The
// stream is never started, so nothing is audible and the device is held for the
// few milliseconds the open takes (~6 ms per device here, and the failures are
// the fast ones).  Returns TRUE when the device is really usable.
//
// Linux only.  CoreAudio does not enumerate devices it cannot open, so there is
// nothing to filter on macOS, and probing there would cost real time on
// Bluetooth and pop the microphone-permission prompt during a device scan.
#ifndef __APPLE__
// The USE_ALSA backend's equivalent of the SoundIo probe below.  With a sound
// server running, every raw plughw:/dmix: entry is refused with EBUSY, so the
// list was all decoration bar "System Default"; with no sound server they open
// and are all listed, which is why they are still enumerated.
//
// History worth keeping: this was added, blamed for a "half a second of audio
// then silence" report the same day and reverted, then restored once that
// failure was actually measured and found to be on the SOUNDIO path — a sink
// accepting ~20500 of 48000 frames/s — which this function cannot reach, since
// it only ever runs for USE_ALSA.  If a similar report appears again, that is
// the fact to check first: which backend, from the `create_audio:` line.
static gboolean alsa_pcm_opens(const char *name,snd_pcm_stream_t stream) {
  snd_pcm_t *t=NULL;
  int e=snd_pcm_open(&t,name,stream,SND_PCM_NONBLOCK);
  if(e>=0) { snd_pcm_close(t); return TRUE; }
  log_info("audio: ALSA %s '%s' not usable: %s\n",
           stream==SND_PCM_STREAM_PLAYBACK?"output":"input",name,snd_strerror(e));
  return FALSE;
}

static gboolean soundio_output_device_opens(struct SoundIoDevice *device,int rate) {
  struct SoundIoOutStream *test=soundio_outstream_create(device);
  if(test==NULL) return FALSE;
  test->format=SoundIoFormatFloat32NE;
  test->sample_rate=rate;
  test->software_latency=0.01;
  int err=soundio_outstream_open(test);
  if(err) log_info("audio: output '%s' not usable: %s\n",device->name,soundio_strerror(err));
  soundio_outstream_destroy(test);
  return err==0;
}

static gboolean soundio_input_device_opens(struct SoundIoDevice *device,int rate) {
  struct SoundIoInStream *test=soundio_instream_create(device);
  if(test==NULL) return FALSE;
  test->format=SoundIoFormatFloat32NE;
  test->sample_rate=rate;
  test->software_latency=0.01;
  int err=soundio_instream_open(test);
  if(err) log_info("audio: input '%s' not usable: %s\n",device->name,soundio_strerror(err));
  soundio_instream_destroy(test);
  return err==0;
}
#else
#define soundio_output_device_opens(d,r) TRUE
#define soundio_input_device_opens(d,r)  TRUE
#endif

// Common tests that need no open: the backend failed to probe the device, it is
// a raw (exclusive-access) device, or we already listed the same id.  Raw
// devices bypass the sound server and lock the card away from everything else on
// the machine, which is never what a receiver's "Local Audio" menu should do.
static gboolean soundio_device_is_listable(struct SoundIoDevice *device,
                                           AUDIO_DEVICE *list,int n) {
  if(device->probe_error) {
    log_info("audio: skipping '%s': probe error: %s\n",
             device->name,soundio_strerror(device->probe_error));
    return FALSE;
  }
  if(device->is_raw) return FALSE;
  for(int i=0;i<n;i++)
    if(list[i].name!=NULL && strcmp(list[i].name,device->name)==0) return FALSE;
  return TRUE;
}

#ifndef __APPLE__
// libasound prints its own diagnostics straight to stderr, so probing the dead
// ALSA entries above would spray "unable to open slave" over the console every
// time the audio page is opened.  Silence it for the duration of the scan.
static void alsa_quiet_handler(const char *f,int l,const char *fn,int e,const char *fmt,...) {
  (void)f; (void)l; (void)fn; (void)e; (void)fmt;
}
#endif

// (Re)build the SoundIo output/input device lists from the current backend
// state.  Called at startup and again every time the receiver audio page is
// opened, so devices that appear after launch (Bluetooth headphones the user
// just connected) show up without restarting the app.  A synthetic "System
// Default" entry is always placed first (see AUDIO_SYSTEM_DEFAULT_NAME).
static void soundio_build_device_lists(void) {
  // free strings from a previous scan
  for(int i=0;i<n_output_devices;i++) {
    if(output_devices[i].name) { g_free(output_devices[i].name); output_devices[i].name=NULL; }
    if(output_devices[i].description) { g_free(output_devices[i].description); output_devices[i].description=NULL; }
  }
  for(int i=0;i<n_input_devices;i++) {
    if(input_devices[i].name) { g_free(input_devices[i].name); input_devices[i].name=NULL; }
    if(input_devices[i].description) { g_free(input_devices[i].description); input_devices[i].description=NULL; }
  }
  n_output_devices=0;
  n_input_devices=0;

  // refresh soundio's cached device list from the backend
  soundio_flush_events(soundio);

#ifndef __APPLE__
  snd_lib_error_set_handler(alsa_quiet_handler);
#endif

  // synthetic "System Default" output at index 0; index -1 is resolved to the
  // current system default output device each time the stream is (re)opened
  output_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
  output_devices[0].description=g_strdup("System Default");
  output_devices[0].index=-1;
  n_output_devices=1;

  int output_count=soundio_output_device_count(soundio);
  for(int i=0;i<output_count;i++) {
    if(n_output_devices>=MAX_AUDIO_DEVICES) break;
    struct SoundIoDevice *device=soundio_get_output_device(soundio,i);
    if(!device) continue;

    int ok_fmt=soundio_device_supports_format(device, SoundIoFormatFloat32NE);
    int nearest=soundio_device_supports_sample_rate(device, sample_rate)
                  ? sample_rate
                  : soundio_device_nearest_sample_rate(device, sample_rate);

    // Only Float32 output is required.  Devices that don't accept the 48 kHz
    // DSP rate (e.g. Bluetooth headsets locked to 44.1 kHz) are still listed
    // and usable: audio_open_output opens them at their nearest rate and the
    // write callback resamples on the fly.  A device with no usable rate at all
    // (nearest<=0) is skipped.
    if(!ok_fmt || nearest<=0) {
      soundio_device_unref(device);
      continue;
    }

    // ...and it has to actually open.  See soundio_output_device_opens().
    if(!soundio_device_is_listable(device,output_devices,n_output_devices) ||
       !soundio_output_device_opens(device,nearest)) {
      soundio_device_unref(device);
      continue;
    }

    output_devices[n_output_devices].name=g_strdup(device->name);
    output_devices[n_output_devices].description=g_strdup(device->name);
    output_devices[n_output_devices].index=i;
    soundio_device_unref(device);
    n_output_devices++;
  }

  // synthetic "System Default" input at index 0 (mirrors the output list);
  // index -1 is resolved to the current system default input each time the
  // mic stream is opened
  input_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
  input_devices[0].description=g_strdup("System Default");
  input_devices[0].index=-1;
  n_input_devices=1;

  int input_count=soundio_input_device_count(soundio);
  for(int i=0;i<input_count;i++) {
    if(n_input_devices>=MAX_AUDIO_DEVICES) break;
    struct SoundIoDevice *device=soundio_get_input_device(soundio,i);
    if(!device) continue;

    // Same treatment as the output list, which the input list never got: a mic
    // entry that cannot be opened is worse than useless, because selecting it
    // leaves TX with no audio and nothing on screen says why.
    int ok_fmt=soundio_device_supports_format(device, SoundIoFormatFloat32NE);
    int nearest=soundio_device_supports_sample_rate(device, sample_rate)
                  ? sample_rate
                  : soundio_device_nearest_sample_rate(device, sample_rate);
    if(!ok_fmt || nearest<=0 ||
       !soundio_device_is_listable(device,input_devices,n_input_devices) ||
       !soundio_input_device_opens(device,nearest)) {
      soundio_device_unref(device);
      continue;
    }

    input_devices[n_input_devices].name=g_strdup(device->name);
    input_devices[n_input_devices].description=g_strdup(device->name);
    input_devices[n_input_devices].index=i;
    soundio_device_unref(device);
    n_input_devices++;
  }

#ifndef __APPLE__
  snd_lib_error_set_handler(NULL);
#endif

  log_info("audio: %s backend: %d usable output device(s), %d input device(s)\n",
           soundio_backend_name(soundio->current_backend),
           n_output_devices-1, n_input_devices-1);
}

// Public: re-scan audio devices (macOS/SoundIo only).  Safe to call while audio
// is streaming — it only rebuilds the UI-facing device metadata, not the open
// stream, which holds its own device reference.
void audio_refresh_devices(void) {
  if(radio!=NULL && radio->which_audio==USE_SOUNDIO && soundio!=NULL) {
    soundio_build_device_lists();
  }
}

// A receiver "follows the system default" output when the user picked the
// synthetic "System Default" entry, or left the output unconfigured.  A receiver
// pinned to a specific named device is intentionally left where the user put it.
static gboolean rx_follows_system_default(RECEIVER *rx) {
  return rx->audio_name==NULL ||
         strcmp(rx->audio_name,AUDIO_SYSTEM_DEFAULT_NAME)==0;
}

// libsoundio binds an output stream to a *concrete* device resolved when the
// stream is opened; it does not track later changes to the macOS default output.
// So when a "System Default" receiver is playing and the user switches the
// system output device (System Settings > Sound, or (un)plugging headphones),
// audio keeps going to the now-stale device.  This timer polls the current
// default output and, when it differs from the device a System Default receiver
// is bound to, re-opens that receiver's stream onto the new device.  Playback
// restarts on its own once the receiver feed refills the ring buffer
// (see the audio_start_output call at the end of the receiver audio loop).
static guint default_output_monitor_id=0;

// Core of the follow-the-default logic: resolve the current system default
// output and (re)open every System Default receiver that isn't already on it.
// Runs on the GTK main thread (from the CoreAudio-listener idle handler and
// from the fallback timer).  Returns TRUE if at least one receiver still wants
// to follow the default (used to decide whether the fallback timer keeps
// ticking).
static gboolean default_output_apply(void) {
  if(radio==NULL || radio->which_audio!=USE_SOUNDIO || soundio==NULL)
    return FALSE;

  // Anything that should be following the system default?  Note we do NOT
  // require an open stream here: a previous transition may have left a receiver
  // with output_stream==NULL, and we want to recover it, not abandon it (that
  // was the "switched once, then never again" bug — the receiver got dropped
  // from the poll forever).
  int any=0;
  for(int i=0;i<radio->receivers;i++) {
    RECEIVER *rx=radio->receiver[i];
    if(rx!=NULL && rx->local_audio && rx_follows_system_default(rx)) { any=1; break; }
  }
  if(!any) return FALSE;

  // Apply whatever device change CoreAudio has already signalled.
  soundio_flush_events(soundio);
  int def=soundio_default_output_device_index(soundio);
  if(def<0) return TRUE;
  struct SoundIoDevice *ddev=soundio_get_output_device(soundio,def);
  if(ddev==NULL) return TRUE;

  // Log default transitions so a terminal run shows exactly what happened.
  static char last_default[256]="";
  const char *did = ddev->id ? ddev->id : "";
  if(strncmp(last_default, did, sizeof(last_default)-1)!=0) {
    log_info("audio: system default output = '%s' (%s)\n", ddev->name, did);
    snprintf(last_default,sizeof(last_default),"%s",did);
  }

  for(int i=0;i<radio->receivers;i++) {
    RECEIVER *rx=radio->receiver[i];
    if(rx==NULL || !rx->local_audio || !rx_follows_system_default(rx)) continue;

    // Already streaming to the current default device *at the right rate*?
    // NB: a Bluetooth headset keeps the same device id but silently changes its
    // sample rate when it flips A2DP<->HFP (its mic starting/stopping being
    // used forces the "hands-free" 16 kHz profile).  Our open stream does not
    // follow that and it plays as garbage, so we must compare the rate too, not
    // just the device identity.
    if(rx->output_stream!=NULL && rx->output_device!=NULL &&
       rx->output_device->id!=NULL &&
       strcmp(rx->output_device->id, ddev->id)==0 &&
       rx->output_device->is_raw==ddev->is_raw) {
      int want = soundio_device_supports_sample_rate(ddev, sample_rate)
                   ? sample_rate
                   : soundio_device_nearest_sample_rate(ddev, sample_rate);
      if(want>0 && rx->output_stream->sample_rate==want)
        continue;   // same device, same rate — nothing to do
      log_info("audio: RX%d output '%s' changed rate %d -> %d; re-opening\n",
              rx->channel, ddev->name, rx->output_stream->sample_rate, want);
    }

    // Either the default moved, its rate changed, or a previous (re)open left
    // this receiver with no stream.  (Re)open onto the current default.
    //
    // Debounce: re-opening a soundio output is slow (tens of ms of
    // soundio_outstream_destroy/open, worse on Bluetooth) and it happens under
    // rx->local_audio_mutex, which the RX receive thread also takes in
    // audio_write.  A Bluetooth A2DP<->HFP transition makes the device's
    // reported rate flip-flop, so without a guard we would re-open again and
    // again, stalling the receive thread each time until the WDSP RX ring
    // starves (endless "fexchange0: error=-2").  Re-open at most once every few
    // seconds so the transition can settle and the RX feed is not starved.
    static gint64 last_reopen=0;
    gint64 now=g_get_monotonic_time();
    if(now-last_reopen < 3000000) {
      log_info("audio: RX%d output change to '%s' debounced (recent re-open)\n",
              rx->channel, ddev->name);
      continue;
    }
    last_reopen=now;
    log_info("audio: (re)opening RX%d output on system default '%s'\n",
            rx->channel, ddev->name);
    audio_close_output(rx);
    if(audio_open_output(rx)<0)
      log_info("audio: RX%d re-open on '%s' failed; will retry\n",
              rx->channel, ddev->name);
  }

  soundio_device_unref(ddev);
  return TRUE;
}

// The microphone "follows the system default" input under the same rule as the
// output: the user picked the synthetic "System Default" entry (or left it
// unconfigured).  A mic pinned to a specific device is left alone.
static gboolean mic_follows_system_default(RADIO *r) {
  return r->microphone_name==NULL ||
         strcmp(r->microphone_name,AUDIO_SYSTEM_DEFAULT_NAME)==0;
}

// Input counterpart of default_output_apply().  Unlike the output, the mic is
// only open while the protocol/TX path wants it, so we act ONLY when a stream
// is already open (input_stream!=NULL) and re-open it onto the new default.  A
// failed re-open just leaves the mic closed until the next TX re-opens it — no
// "permanently muted" hazard, so no recovery loop is needed here.
static gboolean default_input_apply(void) {
  if(radio==NULL || radio->which_audio!=USE_SOUNDIO || soundio==NULL)
    return FALSE;
  RADIO *r=radio;
  if(r->input_stream==NULL || !mic_follows_system_default(r)) return FALSE;

  soundio_flush_events(soundio);
  int def=soundio_default_input_device_index(soundio);
  if(def<0) return TRUE;
  struct SoundIoDevice *ddev=soundio_get_input_device(soundio,def);
  if(ddev==NULL) return TRUE;

  static char last_input[256]="";
  const char *did = ddev->id ? ddev->id : "";
  if(strncmp(last_input, did, sizeof(last_input)-1)!=0) {
    log_info("audio: system default input = '%s' (%s)\n", ddev->name, did);
    snprintf(last_input,sizeof(last_input),"%s",did);
  }

  // Already capturing from the current default device?  Nothing to do.
  if(!(r->input_device!=NULL && r->input_device->id!=NULL &&
       strcmp(r->input_device->id, ddev->id)==0 &&
       r->input_device->is_raw==ddev->is_raw)) {
    // Debounce: opening a mic on a Bluetooth headset forces it into the HFP
    // "hands-free" profile, which itself fires a burst of CoreAudio device
    // events (and can briefly flip the system default between the headset and
    // the built-in mic).  Without a guard each event would re-open the mic,
    // re-trigger the profile negotiation, and spiral until it wedges.  Re-open
    // at most once every few seconds so the negotiation can settle.
    static gint64 last_reopen=0;
    gint64 now=g_get_monotonic_time();
    if(now-last_reopen < 3000000) {
      log_info("audio: mic default changed to '%s' but debouncing (recent re-open)\n", ddev->name);
    } else {
      last_reopen=now;
      log_info("audio: re-opening microphone on system default '%s'\n", ddev->name);
      audio_close_input(r);
      if(audio_open_input(r)<0)
        log_info("audio: microphone re-open on '%s' failed\n", ddev->name);
    }
  }

  soundio_device_unref(ddev);
  return TRUE;
}

// Fallback poll: a slow safety net that also handles retrying a failed re-open
// (no CoreAudio event fires for that).  The instant path is the CoreAudio
// listener below; on macOS this timer rarely does more than a cheap
// flush_events + early-out, so it costs next to nothing.
static gboolean default_output_monitor_cb(gpointer data) {
  default_output_apply();
  default_input_apply();
  return TRUE;
}

#ifdef __APPLE__
// Event-driven fast path.  CoreAudio calls this (on one of its own threads) the
// instant the user changes the system default output device — no polling, zero
// cost until it actually fires.  We must not touch soundio/GTK from this thread,
// so we force a device rescan and bounce the real work onto the GTK main thread.
// A short delay lets soundio's async rescan land before we read the new default.
static gboolean default_output_apply_idle(gpointer data) {
  default_output_apply();
  return FALSE;   // one-shot
}
static gboolean default_output_changed_idle(gpointer data) {
  if(soundio!=NULL) soundio_force_device_scan(soundio);
  g_timeout_add(120, default_output_apply_idle, NULL);
  return FALSE;   // one-shot
}
static OSStatus default_output_listener(AudioObjectID objectID, UInt32 n,
                                        const AudioObjectPropertyAddress *addr,
                                        void *client) {
  g_idle_add(default_output_changed_idle, NULL);
  return noErr;
}
static void install_default_output_listener(void) {
  static gboolean installed=FALSE;
  if(installed) return;
  AudioObjectPropertyAddress addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };
  OSStatus st=AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                             default_output_listener, NULL);
  if(st==noErr) {
    installed=TRUE;
    log_info("audio: installed CoreAudio default-output listener (instant switching)\n");
  } else {
    log_info("audio: AudioObjectAddPropertyListener failed (%d); using timer fallback only\n",(int)st);
  }
}

// Same event-driven fast path for the microphone / default *input* device.
static gboolean default_input_apply_idle(gpointer data) {
  default_input_apply();
  return FALSE;   // one-shot
}
static gboolean default_input_changed_idle(gpointer data) {
  if(soundio!=NULL) soundio_force_device_scan(soundio);
  g_timeout_add(120, default_input_apply_idle, NULL);
  return FALSE;   // one-shot
}
static OSStatus default_input_listener(AudioObjectID objectID, UInt32 n,
                                       const AudioObjectPropertyAddress *addr,
                                       void *client) {
  g_idle_add(default_input_changed_idle, NULL);
  return noErr;
}
static void install_default_input_listener(void) {
  static gboolean installed=FALSE;
  if(installed) return;
  AudioObjectPropertyAddress addr = {
    kAudioHardwarePropertyDefaultInputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };
  OSStatus st=AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                             default_input_listener, NULL);
  if(st==noErr) {
    installed=TRUE;
    log_info("audio: installed CoreAudio default-input listener (instant switching)\n");
  } else {
    log_info("audio: AudioObjectAddPropertyListener (input) failed (%d); using timer fallback only\n",(int)st);
  }
}
#endif

void create_audio(int backend_index,const char *backend) {
  int rc;

  n_output_devices=0;
  n_input_devices=0;

  switch(radio->which_audio) {
    case USE_SOUNDIO:
      soundio=soundio_create();
      if(!soundio) {
        log_info("create_audio: soundio_create failed\n");
        return;
      }
      // Resolve the requested backend, but NEVER use the Dummy backend: it
      // enumerates a fake "Dummy Output Device" and silently discards all audio
      // (this is what left a Fake-SDR config with no real output devices). If the
      // saved index points at Dummy/None, fall back to the first real backend and
      // heal the persisted selection so it doesn't recur.
      {
        enum SoundIoBackend want=soundio_get_backend(soundio,backend_index);
        if(want==SoundIoBackendDummy || want==SoundIoBackendNone) {
          int nb=soundio_backend_count(soundio);
          want=SoundIoBackendNone;
          for(int b=0;b<nb;b++) {
            enum SoundIoBackend cand=soundio_get_backend(soundio,b);
            if(cand!=SoundIoBackendDummy && cand!=SoundIoBackendNone) {
              want=cand;
              radio->which_audio_backend=b;
              break;
            }
          }
        }
        log_info("audio: create_audio: USE_SOUNDIO backend=%s\n",soundio_backend_name(want));
        if(want==SoundIoBackendNone)
          rc=soundio_connect(soundio);            // let soundio auto-pick a real backend
        else {
          rc=soundio_connect_backend(soundio,want);
          // The saved backend (index 0 is often JACK on Linux) may be compiled
          // into libsoundio but not actually usable (no server running), so
          // connect_backend fails. Rather than bail out — which used to leave
          // the global `soundio` created but unconnected, so the very first
          // audio_open_output crashed on soundio_flush_events'
          // `current_backend != SoundIoBackendNone` assertion — fall back to
          // letting libsoundio auto-pick a working backend (PulseAudio/ALSA/…).
          if(rc) {
            log_info("create_audio: backend %s failed (%s); auto-selecting\n",
                    soundio_backend_name(want),soundio_strerror(rc));
            rc=soundio_connect(soundio);
          }
        }
      }
      if(rc) {
        // No backend could be connected at all: destroy and NULL the object so
        // the audio_open_*/monitor paths skip it instead of dereferencing an
        // unconnected soundio (which asserts in libsoundio).
        log_info("create_audio: soundio_connect: %s\n",soundio_strerror(rc));
        soundio_destroy(soundio);
        soundio=NULL;
        return;
      }

      // What actually connected may not be what was asked for (auto-selection
      // above).  Note it, but do NOT write it into radio->which_audio_backend:
      // that field is the operator's REQUEST and is persisted, so overwriting it
      // silently discards a choice of JACK the moment JACK's server happens to
      // be down, and the next run never tries it again.  The request and the
      // reality are two different things; audio_get_current_backend() reports
      // the second, and Configure → Audio shows both.
      {
        int cur=audio_get_current_backend();
        if(cur>=0 && cur!=radio->which_audio_backend)
          log_info("audio: %s was requested but %s is what connected\n",
                   audio_get_backend_name(radio->which_audio_backend),
                   soundio_backend_name(soundio->current_backend));
      }

      soundio_build_device_lists();
      // Follow the system default output live when the user switches the macOS
      // output device.  On macOS this is event-driven (instant, no polling) via
      // a CoreAudio property listener; the timer below is just a slow safety net
      // that also retries a failed re-open.
#ifdef __APPLE__
      install_default_output_listener();
      install_default_input_listener();
#endif
      if(default_output_monitor_id==0)
        default_output_monitor_id=g_timeout_add(2000, default_output_monitor_cb, NULL);
      break;

#ifndef __APPLE__
    case USE_PULSEAUDIO:
log_info("audio: create_audio: USE_PULSEAUDIO\n");
      main_loop=pa_glib_mainloop_new(NULL);
      main_loop_api=pa_glib_mainloop_get_api(main_loop);
      pa_ctx=pa_context_new(main_loop_api,"machpsdr");
      pa_context_connect(pa_ctx,NULL,0,NULL);
      pa_context_set_state_callback(pa_ctx, state_cb, &ready);
      break;

    case USE_ALSA:
      {

        snd_ctl_card_info_t *info;
        snd_pcm_info_t *pcminfo;
        snd_ctl_card_info_alloca(&info);
        snd_pcm_info_alloca(&pcminfo);
        int i;
        char *device_id;
        int card = -1;

        n_input_devices=0;
        n_output_devices=0;

        // Synthetic "System Default" first, exactly as the SoundIo and Pulse
        // lists do (audio_open_*/USE_ALSA maps it to the ALSA "default" PCM).
        // Without it this backend was unusable: a receiver's audio_name starts
        // life as AUDIO_SYSTEM_DEFAULT_NAME and survives a backend switch, so
        // selecting ALSA tried to open a PCM literally called
        // "__system_default__" and every open failed with "Unknown PCM" — the
        // operator had to know to also re-pick a device by hand.  "default" is
        // additionally the only entry that works on a desktop running
        // PulseAudio/PipeWire, where the raw card is already claimed by the
        // sound server and plughw: returns EBUSY.
        output_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
        output_devices[0].description=g_strdup("System Default");
        output_devices[0].index=-1;
        n_output_devices=1;
        input_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
        input_devices[0].description=g_strdup("System Default");
        input_devices[0].index=-1;
        n_input_devices=1;

        snd_ctl_card_info_alloca(&info);
        snd_pcm_info_alloca(&pcminfo);
        while (snd_card_next(&card) >= 0 && card >= 0) {
          int err = 0;
          snd_ctl_t *handle;
          char name[20];
          snprintf(name, sizeof(name), "hw:%d", card);
          if ((err = snd_ctl_open(&handle, name, 0)) < 0) {
            continue;
          }

          if ((err = snd_ctl_card_info(handle, info)) < 0) {
            snd_ctl_close(handle);
            continue;
          }

          int dev = -1;

          while (snd_ctl_pcm_next_device(handle, &dev) >= 0 && dev >= 0) {
            snd_pcm_info_set_device(pcminfo, dev);
            snd_pcm_info_set_subdevice(pcminfo, 0);

            char pcm[32];
            snprintf(pcm, sizeof(pcm), "plughw:%d,%d", card, dev);

            // input devices
            snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);
            if ((err = snd_ctl_pcm_info(handle, pcminfo)) == 0 &&
                alsa_pcm_opens(pcm, SND_PCM_STREAM_CAPTURE)) {
              device_id=g_new(char,128);
              snprintf(device_id, 128, "plughw:%d,%d %s", card, dev, snd_ctl_card_info_get_name(info));
              if(n_input_devices<MAX_AUDIO_DEVICES) {
                input_devices[n_input_devices].name=g_new0(char,strlen(device_id)+1);
                strcpy(input_devices[n_input_devices].name,device_id);
                input_devices[n_input_devices].description=g_new0(char,strlen(device_id)+1);
                strcpy(input_devices[n_input_devices].description,device_id);
                input_devices[n_input_devices].index=0; // not used
                n_input_devices++;
log_info("input_device: %s\n",device_id);
              }
	      g_free(device_id);
            }

            // ouput devices
            snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
            if ((err = snd_ctl_pcm_info(handle, pcminfo)) == 0 &&
                alsa_pcm_opens(pcm, SND_PCM_STREAM_PLAYBACK)) {
              device_id=g_new(char,128);
              snprintf(device_id, 128, "plughw:%d,%d %s", card, dev, snd_ctl_card_info_get_name(info));
              if(n_output_devices<MAX_AUDIO_DEVICES) {
                output_devices[n_output_devices].name=g_new0(char,strlen(device_id)+1);
                strcpy(output_devices[n_output_devices].name,device_id);
                output_devices[n_output_devices].description=g_new0(char,strlen(device_id)+1);
                strcpy(output_devices[n_output_devices].description,device_id);
                output_devices[n_output_devices].index=0; // not used
                n_output_devices++;
log_info("output_device: %s\n",device_id);
              }
	    g_free(device_id);
            }
          }
          snd_ctl_close(handle);
        }
	
        // look for dmix
        void **hints, **n;
        char *name, *descr, *io;

        // No dmix hints is not a reason to abandon the scan: the cards found
        // above (and "System Default") are already listed.
        if (snd_device_name_hint(-1, "pcm", &hints) < 0)
          break;
        n = hints;
        while (*n != NULL) {
          name = snd_device_name_get_hint(*n, "NAME");
          descr = snd_device_name_get_hint(*n, "DESC");
          io = snd_device_name_get_hint(*n, "IOID");

          // snd_device_name_get_hint returns NULL for a hint the PCM does not
          // carry (DESC is routinely absent), so neither pointer may be walked
          // before it is checked.
          if(name!=NULL && descr!=NULL && strncmp("dmix:", name, 5)==0 &&
             alsa_pcm_opens(name, SND_PCM_STREAM_PLAYBACK)) {
            if(n_output_devices<MAX_AUDIO_DEVICES) {
              output_devices[n_output_devices].name=g_new0(char,strlen(name)+1);
              strcpy(output_devices[n_output_devices].name,name);
              // Lead with the PCM name, as the plughw: entries do.  ALSA's DESC
              // hint alone ("VirtIO SoundCard, VirtIO PCM 0") names the card,
              // not the device, so the row read as a duplicate of the plughw:
              // one with no way to tell what it actually was.
              char first[128];
              i=0;
              while(i<(int)sizeof(first)-1 && descr[i]!='\0' && descr[i]!='\n') {
                first[i]=descr[i];
                i++;
              }
              first[i]='\0';
              output_devices[n_output_devices].description=
                g_strdup_printf("%s %s",name,first);
              // was input_devices[n_output_devices]: this is an output entry,
              // and writing it into the input array clobbered an unrelated mic.
              output_devices[n_output_devices].index=0;  // not used
              n_output_devices++;
log_info("output_device: name=%s descr=%s\n",name,descr);
            }
          }

          if (name != NULL)
            free(name);
          if (descr != NULL)
            free(descr);
          if (io != NULL)
            free(io);
          n++;
        }
        snd_device_name_free_hint(hints);
      }
      break;
#endif
  }
  log_info("n_input_devices=%d\n", n_input_devices);
}

// Is this soundio backend one an operator may actually choose?  Dummy is not:
// it enumerates a fake "Dummy Output Device" and silently discards everything
// written to it, and create_audio() refuses it and substitutes a real backend —
// so offering it in the menu was a lie in both directions.  Picking it appeared
// to work (real device names, real audio) because a different backend had
// quietly been used instead.
gboolean audio_backend_is_usable(int backend_index) {
  if(soundio==NULL) return FALSE;
  if(backend_index<0 || backend_index>=soundio_backend_count(soundio)) return FALSE;
  enum SoundIoBackend b=soundio_get_backend(soundio,backend_index);
  return b!=SoundIoBackendDummy && b!=SoundIoBackendNone;
}

// Index of the backend that is actually connected right now, or -1.  The
// selection an operator made is only a request: a backend can be compiled into
// libsoundio and still fail to connect (JACK with no server running is the
// everyday case), and create_audio() then auto-picks a working one.  Without
// this the menu went on showing "JACK" while PulseAudio was playing.
int audio_get_current_backend(void) {
  if(soundio==NULL) return -1;
  int n=soundio_backend_count(soundio);
  for(int i=0;i<n;i++)
    if(soundio_get_backend(soundio,i)==soundio->current_backend) return i;
  return -1;
}

int audio_get_backends(RADIO *r) {
  int count=0;
  switch(r->which_audio) {
    case USE_SOUNDIO:
      count=soundio_backend_count(soundio);
      break;
#ifndef __APPLE__
    case USE_PULSEAUDIO:
      count=0;
      break;
#endif
  }
log_info("audio_get_backends: %d\n",count);
  return count;
}

const char *audio_get_backend_name(int backend_index) {
  const char *name=soundio_backend_name(soundio_get_backend(soundio,backend_index));
log_info("audio_get_backend_name: %s\n",name);
  return name;
}
