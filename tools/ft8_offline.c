/* Copyright (C)
* 2026 - MacHPSDR fork
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

/*
 * Headless FT8/FT4 harness: a synthesised band -- known messages, in known
 * slots, at known offsets and levels -- fed into ft8_decoder.c the way the RX
 * audio thread feeds it, and scored against what was transmitted.
 *
 * What it is FOR.  The decoder does not cut the audio at UTC slot boundaries:
 * it decodes the most recent slot-length window every 2 s (1 s for FT4), so a
 * recording that is not aligned to real UTC still decodes.  Every complete
 * transmission therefore lands in TWO OR THREE consecutive windows, and each of
 * them decodes it again -- which is how the same message came to be listed
 * under two consecutive slot labels, the earlier row carrying a truncated
 * view's wrong dt and a fabricated SNR.  Nothing in the tree could see that:
 * FT8 had no offline test of any kind, and the fault only appears when several
 * windows in a row are decoded against a real clock.
 *
 * So this harness asserts the three properties that overlap makes non-obvious:
 *   - every transmission is reported EXACTLY ONCE, however many windows saw it;
 *   - it is reported under the slot it was SENT in, not the one the decoder
 *     happened to be in when the window closed (the QSO engine answers in the
 *     opposite slot to the one it read, so a label one slot late inverts it);
 *   - dt and SNR are measured against that slot, not against the window.
 * Plus the negative control every decoder here needs: band noise alone must
 * decode to nothing.
 *
 * It carries its own GFSK modulator (the same shape as ft8_encoder.c's, which
 * cannot be linked -- it is wired into the TX chain and the global RADIO), for
 * the reason wefax_offline does: a modulator built from the format's own
 * numbers catches the decoder drifting off the convention it was built to.
 *
 * The decoder is driven through two seams it exposes for this file alone:
 * ft8_decoder_set_clock() (a minute of audio is fed in a fraction of a second,
 * and the wall clock would put every window in the same slot) and
 * ft8_decoder_sync() (feed at full speed without windows being skipped as busy).
 *
 *   make ft8-offline && ./ft8_offline --selftest
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "time_compat.h"   // <time.h> + gmtime_r() on Windows
#include <glib.h>

#include <ft8/message.h>
#include <ft8/encode.h>
#include <ft8/constants.h>

#include "ft8_decoder.h"
#include "ft8_pskreporter.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AUDIO_RATE 48000
#define FEED_BLOCK 1024

// PSK Reporter is a network client wired to the global RADIO; the decoder calls
// it on every publication, so the harness supplies the symbol.  Counted, so the
// "spotted three times per slot" half of the duplicate fault is measured too.
static int reported = 0;
void ft8_pskreporter_report(const FT8_DECODE *decodes, int n, time_t slot_time) {
  (void)decodes; (void)slot_time;
  reported += n;
}

// ---- fake clock ------------------------------------------------------------
// UTC seconds of the first sample fed.  Divisible by 15 (and by 7.5), so scene
// second 0 is a slot boundary and the expected labels are arithmetic.
#define BASE_EPOCH 1788000000.0
static gint64 fake_us = 0;
static gint64 fake_clock(void) { return fake_us; }

// ---- GFSK modulator (same construction as ft8_encoder.c) -------------------
#define GFSK_CONST_K 5.336446f            // pi * sqrt(2 / ln(2))

static void gfsk_pulse(int n_spsym, float bt, float *pulse) {
  for (int i = 0; i < 3 * n_spsym; i++) {
    float t = i / (float)n_spsym - 1.5f;
    pulse[i] = (erff(GFSK_CONST_K * bt * (t + 0.5f)) -
                erff(GFSK_CONST_K * bt * (t - 0.5f))) / 2.0f;
  }
}

// Synthesize n_sym tones at f0 Hz into wave[] (n_sym*nsps samples).
static void synth_gfsk(const uint8_t *sym, int n_sym, int nsps, float bt,
                       float f0, float ampl, float *wave) {
  const int   n_wave = n_sym * nsps;
  const float dphi_peak = 2.0f * (float)M_PI / nsps;      // hmod = 1
  float *dphi  = g_malloc0(sizeof(float) * (n_wave + 2 * nsps));
  float *pulse = g_malloc(sizeof(float) * 3 * nsps);
  gfsk_pulse(nsps, bt, pulse);

  for (int i = 0; i < n_wave + 2 * nsps; i++)
    dphi[i] = 2.0f * (float)M_PI * f0 / AUDIO_RATE;
  for (int i = 0; i < n_sym; i++)
    for (int j = 0; j < 3 * nsps; j++)
      dphi[j + i * nsps] += dphi_peak * sym[i] * pulse[j];
  for (int j = 0; j < 2 * nsps; j++) {
    dphi[j] += dphi_peak * pulse[nsps + j] * sym[0];
    dphi[j + n_sym * nsps] += dphi_peak * pulse[j] * sym[n_sym - 1];
  }

  float phi = 0.0f;
  for (int k = 0; k < n_wave; k++) {
    wave[k] = ampl * sinf(phi);
    phi = fmodf(phi + dphi[k + nsps], 2.0f * (float)M_PI);
  }
  int n_ramp = nsps / 8;
  for (int i = 0; i < n_ramp; i++) {
    float env = (1.0f - cosf(2.0f * (float)M_PI * i / (2 * n_ramp))) / 2.0f;
    wave[i] *= env;
    wave[n_wave - 1 - i] *= env;
  }
  g_free(dphi);
  g_free(pulse);
}

// Add one transmission to the scene at sample position `at`.  Returns FALSE if
// the text cannot be encoded.
static gboolean add_tx(float *scene, long n_scene, long at, const char *text,
                       float f0, float ampl, int ft4) {
  ftx_message_t msg;
  ftx_message_init(&msg);
  if (ftx_message_encode(&msg, NULL, text) != FTX_MESSAGE_RC_OK) {
    fprintf(stderr, "ft8_offline: cannot encode '%s'\n", text);
    return FALSE;
  }
  int   n_sym = ft4 ? FT4_NN : FT8_NN;
  int   nsps  = ft4 ? 2304 : 7680;             // 48000 * symbol period
  float bt    = ft4 ? 1.0f : 2.0f;
  uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN];
  if (ft4) ft4_encode(msg.payload, tones);
  else     ft8_encode(msg.payload, tones);

  long  n_wave = (long)n_sym * nsps;
  float *wave = g_malloc(sizeof(float) * n_wave);
  synth_gfsk(tones, n_sym, nsps, bt, f0, ampl, wave);
  for (long i = 0; i < n_wave && at + i < n_scene; i++) scene[at + i] += wave[i];
  g_free(wave);
  return TRUE;
}

// Reproducible white noise (Box-Muller over a fixed-seed LCG).
static uint32_t rng_state = 12345u;
static float rnd_uniform(void) {
  rng_state = rng_state * 1664525u + 1013904223u;
  return (float)((rng_state >> 8) & 0xFFFFFFu) / 16777216.0f;
}
static void add_noise(float *scene, long n, float sigma) {
  for (long i = 0; i < n; i += 2) {
    float u1 = rnd_uniform(), u2 = rnd_uniform();
    if (u1 < 1e-7f) u1 = 1e-7f;
    float r = sigma * sqrtf(-2.0f * logf(u1));
    scene[i] += r * cosf(2.0f * (float)M_PI * u2);
    if (i + 1 < n) scene[i + 1] += r * sinf(2.0f * (float)M_PI * u2);
  }
}

// ---- what came back --------------------------------------------------------
// The same (utc, count) merge every consumer of the decoder does: the published
// list GROWS within a slot, so what is new is the entries past the count last
// taken.  Collecting it this way is half the point -- a duplicate published
// under a second slot label shows up here as a second row.
typedef struct {
  char  utc[8];
  float snr, dt, freq;
  char  text[40];
} SEEN;
static SEEN seen[512];
static int  n_seen = 0;
static char disp_utc[8] = "";
static int  disp_n = 0;

static void collect(void) {
  FT8_DECODE d[64];
  char utc[8] = "";
  int n = ft8_decoder_get_decodes(d, 64, utc);
  int first = (strcmp(utc, disp_utc) == 0) ? disp_n : 0;
  if (n <= first || !utc[0]) return;
  g_strlcpy(disp_utc, utc, sizeof(disp_utc));
  disp_n = n;
  for (int i = first; i < n && n_seen < (int)(sizeof(seen)/sizeof(seen[0])); i++) {
    SEEN *s = &seen[n_seen++];
    g_strlcpy(s->utc, d[i].utc, sizeof(s->utc));
    g_strlcpy(s->text, d[i].text, sizeof(s->text));
    s->snr = d[i].snr; s->dt = d[i].dt; s->freq = d[i].freq;
  }
}

static void reset_collect(void) {
  n_seen = 0; disp_n = 0; disp_utc[0] = '\0'; reported = 0;
}

// Feed the scene to the decoder in RX-thread-sized blocks, decoding every
// window (no skips) and merging each publication as a consumer would.
static void feed(const float *scene, long n_scene) {
  gdouble blk[FEED_BLOCK * 2];
  for (long pos = 0; pos < n_scene; pos += FEED_BLOCK) {
    int nf = (int)((n_scene - pos < FEED_BLOCK) ? (n_scene - pos) : FEED_BLOCK);
    for (int i = 0; i < nf; i++) { blk[i*2] = scene[pos+i]; blk[i*2+1] = scene[pos+i]; }
    // The window is stamped with the time of its last sample.
    fake_us = (gint64)((BASE_EPOCH + (double)(pos + nf) / AUDIO_RATE) * 1e6);
    ft8_decoder_add_audio(blk, nf);
    ft8_decoder_sync();
    collect();
  }
}

// "hhmmss" of the slot beginning `slot` slots after the scene's first sample.
static void slot_label(char *out, int slot, double slot_len) {
  time_t t = (time_t)llround(BASE_EPOCH + slot * slot_len);
  struct tm tm;
  gmtime_r(&t, &tm);
  snprintf(out, 8, "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int failures = 0;
static guint dup_after_case = 0;   // repeats suppressed during the last case
static void check(gboolean ok, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  char msg[256]; vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
  printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
  if (!ok) failures++;
}

// Count how many collected rows carry this exact message text.
static int count_text(const char *text) {
  int c = 0;
  for (int i = 0; i < n_seen; i++) if (strcmp(seen[i].text, text) == 0) c++;
  return c;
}
// The row for `text` in slot label `utc`, or NULL.
static const SEEN *find(const char *text, const char *utc) {
  for (int i = 0; i < n_seen; i++)
    if (strcmp(seen[i].text, text) == 0 && strcmp(seen[i].utc, utc) == 0) return &seen[i];
  return NULL;
}

static void dump(void) {
  for (int i = 0; i < n_seen; i++)
    printf("      %s  %+5.1f dB  %+5.2f s  %6.1f Hz  %s\n",
           seen[i].utc, seen[i].snr, seen[i].dt, seen[i].freq, seen[i].text);
}

// ===========================================================================
// The tests
// ===========================================================================
// One transmission per slot per station, `n_slots` slots, slot k's waveform
// starting dt0 + k*dt_step seconds after its slot's nominal start (+0.5 s).
// A non-zero dt_step walks the start position across the decode-window grid,
// which is the only way to prove that no phase of it drops a transmission.
static int run_case(const char *name, int ft4, int n_slots,
                    double dt0, double dt_step, float sigma,
                    const char *msg_a, float freq_a, float amp_a,
                    const char *msg_b, float freq_b, float amp_b) {
  const double slot_len = ft4 ? 7.5 : 15.0;
  const double wave_len = ft4 ? FT4_NN * 0.048 : FT8_NN * 0.160;
  // Slot 0 is left empty so the ring is full before the first transmission, and
  // the scene runs a slot past the last one so a whole window can cover it.
  const int    first_slot = 1;
  const long   n_scene = (long)((n_slots + first_slot + 1) * slot_len * AUDIO_RATE);
  float *scene = g_malloc0(sizeof(float) * n_scene);

  printf("\n%s\n", name);
  add_noise(scene, n_scene, sigma);
  for (int k = 0; k < n_slots; k++) {
    long at = (long)(((first_slot + k) * slot_len + 0.5 + dt0 + k * dt_step) * AUDIO_RATE);
    if (!add_tx(scene, n_scene, at, msg_a, freq_a, amp_a, ft4)) return 1;
    if (msg_b && !add_tx(scene, n_scene, at, msg_b, freq_b, amp_b, ft4)) return 1;
  }

  reset_collect();
  ft8_decoder_set_protocol(ft4);
  ft8_decoder_set_enabled(FALSE);
  ft8_decoder_set_enabled(TRUE);       // clears the ring and the duplicate ring
  gint64 t0 = g_get_monotonic_time();
  feed(scene, n_scene);
  double secs = (g_get_monotonic_time() - t0) / 1e6;
  g_free(scene);

  printf("    %d rows from %.0f s of audio in %.1f s\n",
         n_seen, n_scene / (double)AUDIO_RATE, secs);
  dump();

  int want = n_slots * (msg_b ? 2 : 1);
  check(n_seen == want, "%d decodes published, %d transmitted", n_seen, want);
  dup_after_case = ft8_decoder_dup_count();
  printf("    %u repeats suppressed (the same transmission seen by a second window)\n",
         dup_after_case);
  check(reported == n_seen, "%d spots reported, one per decode", reported);
  check(count_text(msg_a) == n_slots,
        "'%s' reported %d times, sent %d", msg_a, count_text(msg_a), n_slots);
  if (msg_b)
    check(count_text(msg_b) == n_slots,
          "'%s' reported %d times, sent %d", msg_b, count_text(msg_b), n_slots);

  for (int k = 0; k < n_slots; k++) {
    char utc[8];
    slot_label(utc, first_slot + k, slot_len);
    const SEEN *a = find(msg_a, utc);
    double dt = dt0 + k * dt_step;
    check(a != NULL, "slot %s carries '%s' (sent %+.2f s into the slot)", utc, msg_a, dt);
    if (a == NULL) continue;
    check(fabsf(a->dt - (float)dt) < 0.25f,
          "slot %s dt %+.2f s (sent %+.2f)", utc, a->dt, dt);
    check(fabsf(a->freq - freq_a) < 6.0f,
          "slot %s freq %.1f Hz (sent %.0f)", utc, a->freq, freq_a);
    if (msg_b) {
      const SEEN *b = find(msg_b, utc);
      check(b != NULL, "slot %s carries '%s'", utc, msg_b);
      if (b) {
        // measure_snr() cuts a DFT per symbol at the decode's own timing, so it
        // reads the -24 dB floor for everything if that timing is out by as
        // little as one symbol -- which is exactly what it did.  Two stations
        // 12 dB apart pin both halves: a real measurement, and one that tracks
        // the level.  The spread is not asserted at 12 dB because the estimator
        // compresses as a signal gets strong (its own splatter inflates the
        // off-tone bins it calls noise), which is a known property of it.
        check(a->snr > -23.0f && b->snr > -23.0f,
              "slot %s SNRs measured (%.1f / %.1f dB), not the -24 dB floor",
              utc, a->snr, b->snr);
        check(a->snr - b->snr > 3.0f && a->snr - b->snr < 17.0f,
              "slot %s SNR spread %.1f dB tracks the 12 dB level difference",
              utc, a->snr - b->snr);
      }
    }
  }
  (void)wave_len;
  return 0;
}

static void run_noise_control(void) {
  printf("\nnegative control: band noise alone\n");
  const long n_scene = (long)(45.0 * AUDIO_RATE);
  float *scene = g_malloc0(sizeof(float) * n_scene);
  add_noise(scene, n_scene, 0.05f);
  reset_collect();
  ft8_decoder_set_protocol(0);
  ft8_decoder_set_enabled(FALSE);
  ft8_decoder_set_enabled(TRUE);
  feed(scene, n_scene);
  g_free(scene);
  dump();
  check(n_seen == 0, "%d decodes from 45 s of noise", n_seen);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--selftest") != 0) {
    fprintf(stderr, "usage: %s --selftest\n", argv[0]);
    return 2;
  }
  ft8_decoder_init();
  ft8_decoder_set_clock(fake_clock);

  // Aligned FT8: two stations in every slot, 12 dB apart.  This is the case the
  // operator photographed -- every transmission decodable in two or three
  // consecutive windows.
  // The noise floor is set so the strong station lands near 0 dB and the weak
  // one 12 dB under it, i.e. both inside the range a real SNR estimate is
  // linear over -- far above it every signal reads the same compressed number
  // and the level check below would pass on an estimator that measured nothing.
  //
  // dt is 0.7 s and not something rounder because it has to put the scene in
  // the overlapping case ON PURPOSE.  An FT8 waveform is 12.64 s of a 15 s
  // window, so the windows that hold it whole span 2.36 s of a 2 s grid: with
  // the wrong phase exactly one window sees each transmission, the duplicate
  // never arises and a run passes while proving nothing.  0.7 s puts two
  // windows over the transmissions in two slots of the three — and the
  // suppressed-repeat count below is what says so, rather than this arithmetic.
  if (run_case("FT8, aligned to UTC, two stations per slot", 0, 3, 0.70, 0.0, 0.12f,
               "CQ TF1A HP94", 759.0f, 0.10f,
               "OH3AD UW2N KN49", 1503.0f, 0.025f)) return 1;
  check(dup_after_case > 0,
        "%u repeats were suppressed, so the overlap this guards was exercised",
        dup_after_case);

  // The same, 3 s late in the slot: a recording is not aligned to UTC, which is
  // the whole reason the windows overlap.  The slot label and dt must still be
  // the transmission's own, not the window's.
  if (run_case("FT8, transmissions 3 s late in their slot", 0, 2, 3.00, 0.0, 0.05f,
               "CQ TF1A HP94", 1200.0f, 0.10f, NULL, 0.0f, 0.0f)) return 1;

  // Walk the start position across a whole period of the 2 s decode grid.  The
  // band of positions a window can find a transmission at is finite (see
  // decode_hop in ft8_decoder.c), so a hop wider than it loses transmissions at
  // some phases and at no others — invisible to any fixed-dt test, and the fault
  // FT4's 1 s hop had against its 0.89 s band.
  if (run_case("FT8, start position swept across the decode grid", 0, 4, 0.0, 0.5, 0.05f,
               "CQ TF1A HP94", 900.0f, 0.10f, NULL, 0.0f, 0.0f)) return 1;

  if (run_case("FT4, start position swept across the decode grid", 1, 4, 0.0, 0.2, 0.05f,
               "CQ TF1A HP94", 1000.0f, 0.12f, NULL, 0.0f, 0.0f)) return 1;

  run_noise_control();

  printf("\n%s\n", failures ? "ft8_offline: FAILED" : "ft8_offline: all checks passed");
  return failures ? 1 : 0;
}
