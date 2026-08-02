/* Fake SDR protocol - synthetic noise+tones IQ generator for UI/design testing.
 *
 * No hardware is involved. fake_discovery() appends a synthetic device to the
 * global discovered[] list. fake_protocol_init() starts a background thread that
 * continuously feeds each active receiver a synthetic IQ stream (Gaussian-ish
 * white noise plus a handful of fixed baseband tones) via add_iq_samples(), so
 * the panadapter/waterfall have something to render.
 *
 * IQ amplitude levels are tunable via the #defines below. If the panadapter
 * looks flat, raise FAKE_NOISE_AMP / the tone amplitudes; if it clips, lower them.
 */

#include <gtk/gtk.h>
#include "log.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "discovered.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "fake_protocol.h"

#define FAKE_NOISE_AMP  0.0009    // noise floor level
#define FAKE_TONE_COUNT 4

// Synthetic TX: a quiet audio test tone fed to the transmitter's mic input
// while keyed (MOX/Tune), so the tx_panadapter shows a signal without hardware.
#define FAKE_TX_TONE_AMP  0.10     // mic tone amplitude (0..1); lower if it clips
#define FAKE_TX_TONE_FREQ 1000.0   // mic tone frequency (Hz)

static const double fake_tone_offset[FAKE_TONE_COUNT] = { 10000.0, -25000.0, 40000.0, -5000.0 };
static const double fake_tone_amp[FAKE_TONE_COUNT]    = {    0.02,     0.008,    0.03,   0.015 };

// Optional I/Q recording playback. If an "iq.wav" (16-bit stereo I/Q, e.g. an
// SDR# recording) is found at startup, the fake device streams that real RF
// instead of synthetic noise/tones, looping forever and resampling on the fly to
// the receiver's sample rate. Great for verifying real WFM/AM/SSB demodulation
// with no hardware. FAKE_IQ_GAIN scales the (normalised +-1) file level; lower it
// if the S-meter pins, raise it if the signal is buried.
#define FAKE_IQ_FILE  "iq.wav"
#define FAKE_IQ_GAIN  0.2

static float  *iq_data   = NULL;   // interleaved I,Q, normalised to ~[-1,1]
static long    iq_frames = 0;      // number of I/Q sample pairs
static double  iq_rate   = 0.0;    // file sample rate (Hz)
static double  iq_offset = 0.0;    // recorded carrier offset from centre (Hz)
static double  iq_pos[8] = {0};    // per-receiver fractional read cursor (loops)
static double  mix_phase[8] = {0}; // per-receiver de-rotation phase (centres station)

// Playback position for the on-panadapter readout (elapsed time), published by
// the feed thread from the file-reading channel's cursor and read on the GTK
// draw thread. A torn double read is a harmless cosmetic glitch for one frame,
// so no lock is taken (mirrors how the DX-spot overlay is a display-only read).
static volatile double fake_play_pos_frames = 0.0;

// Diversity replay: a diversity hidden RX is fed the SAME I/Q as its visual
// partner (so the two streams are coherent/identical, not two independent reads
// of the file). The most-recently-fed channel's samples are stashed here and
// replayed for a hidden RX whose visual partner is that channel.
static double *fake_replay = NULL;
static int     fake_replay_cap = 0;   // capacity in complex samples
static int     fake_replay_n = 0;     // valid complex samples stashed
static int     fake_replay_ch = -1;   // WDSP channel that produced them

static volatile int fake_running = 0;
static GThread *fake_thread_id = NULL;

// ---- anti-imaging / anti-aliasing low-pass -------------------------------
// Resampling the file_rate I/Q to the (usually much higher) receiver rate by
// per-sample interpolation leaves spectral IMAGES of the recording spaced at
// +-file_rate across the panadapter — they look like a "mirror". A 6th-order
// Butterworth low-pass at the file's Nyquist removes them, so the panadapter
// shows only the recording's own bandwidth (and, when downsampling, it doubles
// as the anti-alias filter). The real signal near baseband is untouched, so
// demod/decode is unaffected. Applied to I and Q independently — a real filter
// band-limits the complex spectrum symmetrically, exactly what we want.
static double  aa_bq[3][5];            // 3 biquads: {b0,b1,b2,a1,a2}, a0=1
static double  aa_z[8][3][2][2];       // state [ch][stage][I/Q][z1,z2]
static double  aa_fs = 0.0;            // sample rate the coeffs were designed for

// RBJ cookbook low-pass biquads for a 6th-order Butterworth (three stages).
static void aa_design(double fc, double fs) {
  static const double Q[3] = { 0.51763809, 0.70710678, 1.93185165 };
  double w0 = 2.0*M_PI*fc/fs, cw = cos(w0), sw = sin(w0);
  for(int s=0;s<3;s++) {
    double alpha = sw/(2.0*Q[s]);
    double a0 = 1.0+alpha;
    aa_bq[s][0] = ((1.0-cw)/2.0)/a0;   // b0
    aa_bq[s][1] = (1.0-cw)/a0;         // b1
    aa_bq[s][2] = ((1.0-cw)/2.0)/a0;   // b2
    aa_bq[s][3] = (-2.0*cw)/a0;        // a1
    aa_bq[s][4] = (1.0-alpha)/a0;      // a2
  }
  memset(aa_z, 0, sizeof(aa_z));
  aa_fs = fs;
}

// Run one sample of channel ch (iq: 0=I,1=Q) through the 3-biquad cascade.
static inline double aa_filter(int ch, int iq, double x) {
  for(int s=0;s<3;s++) {
    double *z = aa_z[ch][s][iq];
    double y = aa_bq[s][0]*x + z[0];
    z[0] = aa_bq[s][1]*x - aa_bq[s][3]*y + z[1];
    z[1] = aa_bq[s][2]*x - aa_bq[s][4]*y;
    x = y;
  }
  return x;
}

// 4-point Catmull-Rom cubic interpolation. Far cleaner than linear when
// resampling a wideband I/Q signal (linear interp adds audible demod noise).
static inline double cubic4(double ym1, double y0, double y1, double y2, double t) {
  double a = -0.5*ym1 + 1.5*y0 - 1.5*y1 + 0.5*y2;
  double b =       ym1 - 2.5*y0 + 2.0*y1 - 0.5*y2;
  double c = -0.5*ym1           + 0.5*y1;
  return ((a*t + b)*t + c)*t + y0;
}

// Minimal RIFF/WAVE reader: 16-bit PCM, 2 channels (I,Q). Loads the whole file
// into memory as a freshly-malloc'd float array (interleaved I,Q, ~[-1,1]) and
// returns it via the out-params WITHOUT touching any global playback state, so
// it is safe to call while the feed thread is streaming. Returns 1 on success.
static int fake_read_iq(const char *path, float **out_data, long *out_frames, double *out_rate) {
  FILE *f = fopen(path, "rb");
  if(!f) return 0;
  unsigned char hdr[12];
  if(fread(hdr,1,12,f)!=12 || memcmp(hdr,"RIFF",4)!=0 || memcmp(hdr+8,"WAVE",4)!=0) {
    fclose(f); return 0;
  }
  int channels=0, bits=0;
  unsigned int rate=0, data_len=0;
  long data_off=0;
  unsigned char c[8];
  while(fread(c,1,8,f)==8) {
    unsigned int csize = c[4] | (c[5]<<8) | (c[6]<<16) | ((unsigned int)c[7]<<24);
    if(memcmp(c,"fmt ",4)==0) {
      unsigned char fmt[16];
      unsigned int toread = csize<16 ? csize : 16;
      if(fread(fmt,1,toread,f)!=toread) { fclose(f); return 0; }
      channels = fmt[2] | (fmt[3]<<8);
      rate     = fmt[4] | (fmt[5]<<8) | (fmt[6]<<16) | ((unsigned int)fmt[7]<<24);
      bits     = fmt[14] | (fmt[15]<<8);
      if(csize>toread) fseek(f, csize-toread, SEEK_CUR);
      if(csize & 1) fseek(f, 1, SEEK_CUR);         // chunks are word-aligned
    } else if(memcmp(c,"data",4)==0) {
      data_off = ftell(f);
      data_len = csize;
      break;
    } else {
      fseek(f, csize + (csize & 1), SEEK_CUR);      // skip aux chunks (e.g. "auxi")
    }
  }
  if(channels!=2 || bits!=16 || rate==0 || data_len==0 || data_off==0) {
    fclose(f); return 0;
  }
  long frames = data_len / 4;                       // 2 ch * 2 bytes
  short *raw = (short *)malloc((size_t)frames * 4);
  if(!raw) { fclose(f); return 0; }
  fseek(f, data_off, SEEK_SET);
  if(fread(raw,1,(size_t)frames*4,f)!=(size_t)frames*4) { free(raw); fclose(f); return 0; }
  fclose(f);
  float *data = (float *)malloc((size_t)frames * 2 * sizeof(float));
  if(!data) { free(raw); return 0; }
  for(long i=0;i<frames*2;i++) data[i] = (float)raw[i] / 32768.0f;
  free(raw);
  *out_data = data;
  *out_frames = frames;
  *out_rate = (double)rate;
  return 1;
}

// Init-time loader: read the file and commit it straight into the global
// playback state (feed thread not yet running, so no locking needed).
static int fake_load_iq(const char *path) {
  float *data; long frames; double rate;
  if(!fake_read_iq(path, &data, &frames, &rate)) return 0;
  free(iq_data);
  iq_data = data;
  iq_frames = frames;
  iq_rate = rate;
  for(int i=0;i<8;i++) { iq_pos[i] = 0.0; mix_phase[i] = 0.0; }
  fake_play_pos_frames = 0.0;
  aa_fs = 0.0;                                      // force AA-filter redesign

  // Carrier de-rotation offset. By default we do NOT shift the recording: an
  // SDR I/Q capture already has the signal of interest at (or very near) DC,
  // where the radio was tuned, so playback shows it centred — which is what you
  // want. An earlier version tried to auto-centre by estimating the recording's
  // mean/centroid frequency and de-rotating to null it, but that centres the
  // *energy centroid*, not your signal: on a wide multi-signal capture (this
  // 1.024 MHz SSTV file has energy spread across the band) the centroid sits
  // hundreds of kHz off the DC signal, so the "fix" shoved the already-centred
  // station right off to the side. Opt in with MACHPSDR_FAKE_OFFSET=<Hz> to
  // manually de-rotate a genuinely off-centre single-signal recording.
  iq_offset = 0.0;
  {
    const char *env = getenv("MACHPSDR_FAKE_OFFSET");
    if(env && env[0]) iq_offset = atof(env);
  }
  if(iq_offset != 0.0)
    log_info("fake: iq.wav manual de-rotation %.0f Hz (MACHPSDR_FAKE_OFFSET)\n", iq_offset);
  return 1;
}

/* Only the --faker command-line flag (parsed in main()) turns this on. */
int enable_fake = 0;
const char *fake_iq_file = NULL;

void fake_discovery(void) {
  // The "I/Q Player" pseudo-device is always offered in the device-selection
  // list (appended last, after any real radios — see discovery.c), so the user
  // can play back an I/Q recording without the --faker command-line flag. With
  // --faker it is still the only device discovered.
  if(devices >= MAX_DEVICES) return;

  DISCOVERED *d = &discovered[devices];
  memset(d, 0, sizeof(DISCOVERED));

  d->protocol = PROTOCOL_FAKE;
  d->device = DEVICE_HERMES;
  strcpy(d->name, "I/Q Player");
  strcpy(d->software_version, "1.0");
  d->status = STATE_AVAILABLE;
  d->supported_receivers = 2;
  d->supported_transmitters = 1;
  d->adcs = 1;
  // The fake device's "frequency" is purely cosmetic — the recorded I/Q is
  // streamed at baseband regardless of the dial — so give it a wide, VHF-capable
  // range (0..200 MHz) instead of the classic-HPSDR 61.44 MHz ceiling. That lets
  // the dial sit where the content actually lives (e.g. 145.8 MHz for an ISS
  // SSTV recording) rather than being clamped down into HF.
  d->frequency_min = 0.0;
  d->frequency_max = 200000000.0;

  unsigned char mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  memcpy(d->info.network.mac_address, mac, 6);
  d->info.network.address_length = 6;
  strcpy(d->info.network.interface_name, "fake");

  devices++;
}

static gpointer fake_thread_fn(gpointer data) {
  RADIO *r = (RADIO *)data;
  static double phase[FAKE_TONE_COUNT] = {0.0};
  static double mic_phase = 0.0;

  gint64 next = g_get_monotonic_time();

  while(fake_running) {
    int any = 0;
    int n_ref = 0;
    double sr_ref = 48000.0;

    for(int ch = 0; ch < r->discovered->supported_receivers && ch < 8; ch++) {
      g_mutex_lock(&r->delete_rx_mutex);
      RECEIVER *rx = r->receiver[ch];
      // Normally we only feed receivers that have a visual panel. A diversity
      // *hidden* receiver has show_rx=FALSE but must still be fed, otherwise its
      // buffer never fills, the diversity mix (triggered when the hidden buffer
      // completes) never runs, and the visible RX freezes. So also feed a
      // receiver that is participating in a diversity mixer.
      gboolean is_div_hidden = (rx != NULL &&
                                rx->dmix_id >= 0 && rx->dmix_id < MAX_DIVERSITY_MIXERS &&
                                r->divmixer[rx->dmix_id] != NULL);
      if(rx == NULL || (!rx->show_rx && !is_div_hidden)) {
        g_mutex_unlock(&r->delete_rx_mutex);
        continue;
      }
      any = 1;

      int n = rx->buffer_size;
      double sr = (double)rx->sample_rate;
      n_ref = n;
      sr_ref = sr;

      // Replay applies ONLY to the diversity *hidden* partner: feed it the
      // visual partner's just-fed block verbatim so both see identical I/Q.
      // NB: is_div_hidden is true for BOTH participants (they share dmix_id), so
      // this must additionally check rx == dm->rx_hidden — otherwise the VISUAL
      // receiver also matches and, after storing its first block, keeps replaying
      // that same stale block instead of streaming fresh samples (the whole mix
      // then freezes on the first buffer -> waterfall repeats the last values).
      gboolean replay = FALSE;
      if(is_div_hidden) {
        DIVMIXER *dm = r->divmixer[rx->dmix_id];
        if(dm != NULL && rx == dm->rx_hidden && dm->rx_visual != NULL &&
           fake_replay != NULL && fake_replay_n == n &&
           fake_replay_ch == dm->rx_visual->channel) {
          replay = TRUE;
        }
      }
      // Otherwise stash this channel's samples so a later hidden partner in the
      // same pass can replay them.
      gboolean store = !replay;
      if(store && fake_replay_cap < n) {
        fake_replay = g_realloc(fake_replay, 2*n*sizeof(double));
        fake_replay_cap = n;
      }

      for(int s = 0; s < n; s++) {
        double i_sample, q_sample;

        if(replay) {
          i_sample = fake_replay[s*2];
          q_sample = fake_replay[s*2+1];
        } else if(iq_data) {
          // Stream the recorded I/Q, resampling file_rate -> sr by cubic
          // interpolation and looping back to the start at end-of-file. Each
          // receiver keeps its own cursor so they don't consume the file twice.
          double step = iq_rate / sr;
          double pos = iq_pos[ch];
          long i0 = (long)pos;
          double t = pos - (double)i0;
          long im1 = i0 - 1; if(im1 < 0)          im1 += iq_frames;   // wrap
          long ip1 = i0 + 1; if(ip1 >= iq_frames) ip1 -= iq_frames;
          long ip2 = i0 + 2; if(ip2 >= iq_frames) ip2 -= iq_frames;
          double ii = cubic4(iq_data[im1*2],   iq_data[i0*2],   iq_data[ip1*2],   iq_data[ip2*2],   t);
          double qq = cubic4(iq_data[im1*2+1], iq_data[i0*2+1], iq_data[ip1*2+1], iq_data[ip2*2+1], t);
          // Live "Swap I & Q" (mirrors the spectrum) for inverted-sideband
          // recordings — driven by the radio-dialog checkbox (radio->iqswap),
          // so it can be toggled while playing.
          if(r->iqswap) { double tmp = ii; ii = qq; qq = tmp; }
          // Band-limit to the recording's own bandwidth: kills the resampling
          // images (the "mirror") so the panadapter shows the file's spectrum.
          if(aa_fs != sr) aa_design(0.49*(iq_rate<sr?iq_rate:sr), sr);
          ii = aa_filter(ch, 0, ii);
          qq = aa_filter(ch, 1, qq);
          // de-rotate by the carrier offset to move the station to baseband 0
          double th = mix_phase[ch];
          double cc = cos(th), ss = sin(th);
          double ri = ii*cc + qq*ss;
          double rq = qq*cc - ii*ss;
          mix_phase[ch] += 2.0*M_PI*iq_offset/sr;
          if(mix_phase[ch] >  M_PI) mix_phase[ch] -= 2.0*M_PI;
          if(mix_phase[ch] < -M_PI) mix_phase[ch] += 2.0*M_PI;
          i_sample = FAKE_IQ_GAIN * ri;
          q_sample = FAKE_IQ_GAIN * rq;
          pos += step;
          if(pos >= (double)iq_frames) pos -= (double)iq_frames;   // loop
          iq_pos[ch] = pos;
        } else {
          // Gaussian-ish white noise via sum of 12 uniforms (mean 0)
          double ni = 0.0;
          double nq = 0.0;
          for(int k = 0; k < 12; k++) {
            ni += (double)rand() / (double)RAND_MAX;
            nq += (double)rand() / (double)RAND_MAX;
          }
          ni = (ni - 6.0) * FAKE_NOISE_AMP;
          nq = (nq - 6.0) * FAKE_NOISE_AMP;
          i_sample = ni;
          q_sample = nq;
          // Fixed baseband tones -> distinct peaks on the panadapter
          for(int t = 0; t < FAKE_TONE_COUNT; t++) {
            i_sample += fake_tone_amp[t] * cos(phase[t]);
            q_sample += fake_tone_amp[t] * sin(phase[t]);
            phase[t] += 2.0 * M_PI * fake_tone_offset[t] / sr;
            if(phase[t] > 2.0 * M_PI) phase[t] -= 2.0 * M_PI;
            if(phase[t] < -2.0 * M_PI) phase[t] += 2.0 * M_PI;
          }
        }

        if(store) { fake_replay[s*2] = i_sample; fake_replay[s*2+1] = q_sample; }
        add_iq_samples(rx, i_sample, q_sample);
      }
      if(store) { fake_replay_ch = rx->channel; fake_replay_n = n; }
      // Publish the elapsed-time cursor from the channel that actually read the
      // file (the file-reading path advances iq_pos[ch]; a diversity replay
      // channel does not, so guard on `store` to avoid reporting its stale 0).
      if(iq_data && store) fake_play_pos_frames = iq_pos[ch];

      // While keyed, feed the TX DSP a synthetic mic tone so tx_panadapter
      // shows a signal. Only for the receiver bound to the transmitter, paced
      // with this receiver's real-time block (num_mic scales rx rate -> mic rate).
      if(r->can_transmit && r->transmitter != NULL &&
         r->transmitter->rx == rx && isTransmitting(r)) {
        TRANSMITTER *tx = r->transmitter;
        int num_mic = (int)((double)tx->mic_sample_rate * (double)n / sr);
        for(int m = 0; m < num_mic; m++) {
          float mic = (float)(FAKE_TX_TONE_AMP * sin(mic_phase));
          mic_phase += 2.0 * M_PI * FAKE_TX_TONE_FREQ / (double)tx->mic_sample_rate;
          if(mic_phase > 2.0 * M_PI) mic_phase -= 2.0 * M_PI;
          add_mic_sample(tx, mic);
        }
      }

      g_mutex_unlock(&r->delete_rx_mutex);
    }

    if(!any) {
      g_usleep(10000);
      next = g_get_monotonic_time();
      continue;
    }

    // Pace ONE block to real time against a monotonic deadline. Sleeping per
    // receiver (as before) under-fed each RX when several were shown, and plain
    // g_usleep drifts and starves the audio ring buffer -> crackle/dropouts.
    next += (gint64)(1000000.0 * (double)n_ref / sr_ref);
    gint64 now = g_get_monotonic_time();
    if(next > now) g_usleep((gulong)(next - now));
    else           next = now;   // fell behind; resync without accumulating lag
  }

  return NULL;
}

void fake_protocol_init(RADIO *r) {
  // Which I/Q recording to loop.  Precedence: the `--faker <file>` argument,
  // then the file chosen in Configure -> Radio (persisted on RADIO), then the
  // MACHPSDR_FAKE_IQ env var, then the default iq.wav.  Any 16-bit stereo I/Q
  // WAV works, e.g. `--faker ft8.wav` to drive the FT8 decoder.
  const char *iq_file = fake_iq_file;
  if(iq_file==NULL || iq_file[0]=='\0') iq_file = r->iq_player_file[0] ? r->iq_player_file : NULL;
  if(iq_file==NULL || iq_file[0]=='\0') iq_file = getenv("MACHPSDR_FAKE_IQ");
  if(iq_file==NULL || iq_file[0]=='\0') iq_file = FAKE_IQ_FILE;

  // Look for the recording in the current dir, then the home dir.
  if(!fake_load_iq(iq_file)) {
    char *home_path = g_build_filename(g_get_home_dir(), iq_file, NULL);
    fake_load_iq(home_path);
    g_free(home_path);
  }
  if(iq_data) {
    log_info("fake: playing I/Q file '%s' (%.0f Hz, %ld frames, %.1f s), looping\n",
            iq_file, iq_rate, iq_frames, (double)iq_frames/iq_rate);
  } else {
    log_info("fake: no '%s' found; using synthetic noise+tones\n", iq_file);
  }
  fake_running = 1;
  fake_thread_id = g_thread_new("fake_iq", fake_thread_fn, r);
}

void fake_protocol_stop(void) {
  fake_running = 0;
}

int fake_protocol_set_iq_file(RADIO *r, const char *path) {
  // Empty/NULL path => go back to the synthetic noise+tones generator.
  if(path==NULL || path[0]=='\0') {
    g_mutex_lock(&r->delete_rx_mutex);
    free(iq_data);
    iq_data = NULL; iq_frames = 0; iq_rate = 0.0;
    for(int i=0;i<8;i++) { iq_pos[i] = 0.0; mix_phase[i] = 0.0; }
    fake_play_pos_frames = 0.0;
    aa_fs = 0.0;
    g_mutex_unlock(&r->delete_rx_mutex);
    log_info("fake: I/Q playback stopped, using synthetic noise+tones\n");
    return 1;
  }

  // Read the whole file into a fresh buffer OUTSIDE the lock (the feed thread
  // keeps streaming the old buffer meanwhile), then swap it in under the same
  // mutex the feed thread holds around each block. If the read fails, the
  // current playback is left running untouched.
  float *data; long frames; double rate;
  if(!fake_read_iq(path, &data, &frames, &rate)) {
    log_error("fake: could not load I/Q file '%s' (need 16-bit stereo WAV)\n", path);
    return 0;
  }

  g_mutex_lock(&r->delete_rx_mutex);
  free(iq_data);
  iq_data = data; iq_frames = frames; iq_rate = rate;
  for(int i=0;i<8;i++) { iq_pos[i] = 0.0; mix_phase[i] = 0.0; }
  fake_play_pos_frames = 0.0;
  aa_fs = 0.0;                                      // force AA-filter redesign
  g_mutex_unlock(&r->delete_rx_mutex);

  log_info("fake: now playing I/Q file '%s' (%.0f Hz, %ld frames, %.1f s), looping\n",
           path, rate, frames, rate>0.0 ? (double)frames/rate : 0.0);
  return 1;
}

int fake_protocol_playback(double *elapsed_s, double *total_s, double *bw_hz) {
  // iq_data/iq_frames/iq_rate are only ever written on the GTK thread
  // (fake_protocol_init before the feed thread starts, or set_iq_file), which is
  // also the thread that calls this, so reading them here is race-free; only the
  // position cursor is cross-thread (benign, see fake_play_pos_frames).
  if(iq_data == NULL || iq_frames <= 0 || iq_rate <= 0.0) return 0;
  double pos = fake_play_pos_frames;
  if(pos < 0.0) pos = 0.0;
  if(pos > (double)iq_frames) pos = (double)iq_frames;
  if(elapsed_s) *elapsed_s = pos / iq_rate;
  if(total_s)   *total_s   = (double)iq_frames / iq_rate;
  if(bw_hz)     *bw_hz     = iq_rate;               // complex sample rate == span
  return 1;
}

int fake_protocol_seek(RADIO *r, double fraction) {
  // Jump the file read cursor to `fraction` (0..1) of the recording. Same
  // locking discipline as set_iq_file: mutate the per-receiver cursors under
  // r->delete_rx_mutex (the lock the feed thread holds around each block), so
  // no torn read / mid-block jump. iq_data/iq_frames are GTK-thread-owned.
  if(iq_data == NULL || iq_frames <= 0) return 0;
  if(fraction < 0.0) fraction = 0.0;
  if(fraction > 1.0) fraction = 1.0;
  double target = fraction * (double)iq_frames;
  if(target >= (double)iq_frames) target = (double)iq_frames - 1.0;
  if(target < 0.0) target = 0.0;
  g_mutex_lock(&r->delete_rx_mutex);
  for(int i=0;i<8;i++) { iq_pos[i] = target; mix_phase[i] = 0.0; }
  fake_play_pos_frames = target;
  memset(aa_z, 0, sizeof(aa_z));   // clear AA-filter history so the jump doesn't click
  g_mutex_unlock(&r->delete_rx_mutex);
  return 1;
}

int fake_protocol_is_running(void) {
  return fake_running;
}
