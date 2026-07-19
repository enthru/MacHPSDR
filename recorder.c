/* recorder.c -- see recorder.h.
 *
 * A minimal streaming RIFF/WAVE writer: the 44-byte header is written up front
 * with placeholder sizes and patched with the real byte counts on stop. Both
 * files are 16-bit little-endian PCM, 2 channels. The I/Q file mirrors the
 * format the --faker replay path expects (fake_protocol.c), so a capture can be
 * looped straight back through the RX/decoder chain.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <gtk/gtk.h>

#include "recorder.h"

static GMutex rec_mutex;
static RECEIVER *rec_rx = NULL;      /* receiver being recorded, NULL when idle */
static FILE *rec_iq = NULL;
static FILE *rec_af = NULL;
static guint32 iq_bytes = 0;         /* PCM data bytes written to each file */
static guint32 af_bytes = 0;

static void put_u32(unsigned char *p, guint32 v) {
  p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}
static void put_u16(unsigned char *p, guint16 v) {
  p[0]=v&0xff; p[1]=(v>>8)&0xff;
}

/* Write a 44-byte canonical WAV header with data size 0 (patched on close). */
static void write_wav_header(FILE *f, guint32 rate) {
  unsigned char h[44];
  const guint16 channels=2, bits=16;
  const guint32 byte_rate=rate*channels*(bits/8);
  memcpy(h, "RIFF", 4);
  put_u32(h+4, 36);                 /* RIFF chunk size (patched)   */
  memcpy(h+8, "WAVE", 4);
  memcpy(h+12, "fmt ", 4);
  put_u32(h+16, 16);                /* fmt chunk size              */
  put_u16(h+20, 1);                 /* PCM                         */
  put_u16(h+22, channels);
  put_u32(h+24, rate);
  put_u32(h+28, byte_rate);
  put_u16(h+32, channels*(bits/8)); /* block align                 */
  put_u16(h+34, bits);
  memcpy(h+36, "data", 4);
  put_u32(h+40, 0);                 /* data chunk size (patched)   */
  fwrite(h, 1, 44, f);
}

/* Patch RIFF size (offset 4) and data size (offset 40), then close. */
static void close_wav(FILE *f, guint32 data_bytes) {
  unsigned char v[4];
  if(!f) return;
  put_u32(v, 36+data_bytes);
  fseek(f, 4, SEEK_SET);  fwrite(v, 1, 4, f);
  put_u32(v, data_bytes);
  fseek(f, 40, SEEK_SET); fwrite(v, 1, 4, f);
  fclose(f);
}

static short clamp16(double x) {
  if(x >  1.0) x =  1.0;
  if(x < -1.0) x = -1.0;
  return (short)(x*32767.0);
}

gboolean recorder_active(void) {
  return rec_rx != NULL;
}

gboolean recorder_toggle(RECEIVER *rx) {
  g_mutex_lock(&rec_mutex);
  if(rec_rx) {
    /* stop */
    close_wav(rec_iq, iq_bytes);
    close_wav(rec_af, af_bytes);
    rec_iq=NULL; rec_af=NULL; rec_rx=NULL;
    g_print("recorder: stopped\n");
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }

  /* start */
  char dir[512], iqp[600], afp[600], stamp[32];
  time_t t=time(NULL);
  struct tm tm; gmtime_r(&t, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
  snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  snprintf(iqp, sizeof(iqp), "%s/rec_%s_iq.wav", dir, stamp);
  snprintf(afp, sizeof(afp), "%s/rec_%s_af.wav", dir, stamp);

  rec_iq=fopen(iqp, "wb");
  rec_af=fopen(afp, "wb");
  if(!rec_iq || !rec_af) {
    if(rec_iq) { fclose(rec_iq); rec_iq=NULL; }
    if(rec_af) { fclose(rec_af); rec_af=NULL; }
    g_print("recorder: cannot open output files in %s\n", dir);
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }
  write_wav_header(rec_iq, (guint32)rx->sample_rate);
  write_wav_header(rec_af, 48000);
  iq_bytes=0; af_bytes=0;
  rec_rx=rx;
  g_print("recorder: I/Q -> %s (%d Hz)\nrecorder: AF  -> %s (48000 Hz)\n",
          iqp, rx->sample_rate, afp);
  g_mutex_unlock(&rec_mutex);
  return TRUE;
}

void recorder_iq(RECEIVER *rx, double *iq, int nsamples) {
  if(rec_rx != rx) return;               /* fast unlocked reject */
  g_mutex_lock(&rec_mutex);
  if(rec_rx==rx && rec_iq) {
    for(int i=0;i<nsamples;i++) {
      short s[2] = { clamp16(iq[i*2]), clamp16(iq[i*2+1]) };
      fwrite(s, sizeof(short), 2, rec_iq);
    }
    iq_bytes += (guint32)nsamples*4;
  }
  g_mutex_unlock(&rec_mutex);
}

void recorder_audio(RECEIVER *rx, double *audio, int nstereo) {
  if(rec_rx != rx) return;
  g_mutex_lock(&rec_mutex);
  if(rec_rx==rx && rec_af) {
    for(int i=0;i<nstereo;i++) {
      short s[2] = { clamp16(audio[i*2]), clamp16(audio[i*2+1]) };
      fwrite(s, sizeof(short), 2, rec_af);
    }
    af_bytes += (guint32)nstereo*4;
  }
  g_mutex_unlock(&rec_mutex);
}
