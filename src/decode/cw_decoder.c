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

#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>

#include "cw_decoder.h"
#include "log.h"

#define SR         48000.0   // demod audio sample rate (Hz)

// ---- Goertzel tone-tracking bank -------------------------------------------
// A bank of short-block Goertzel filters spanning the usual CW pitch range;
// the bin with the highest slow-EMA energy is taken as the operator's keyed
// tone, so the decoder auto-tracks whatever pitch is being sent (no manual
// pitch setting). FFT-free, O(N_BINS) per sample.
#define N_BINS      16
#define F_LO        400.0
#define F_HI        1000.0
#define BLOCK_LEN   128                     // samples/block (~2.667 ms @ 48 kHz)
#define BLOCK_MS    (BLOCK_LEN * 1000.0 / SR)

static double bin_freq[N_BINS];
static double bin_coeff[N_BINS];
static gboolean bins_ready = FALSE;

static void init_bins(void) {
  for (int i = 0; i < N_BINS; i++) {
    double f = F_LO + (F_HI - F_LO) * i / (double)(N_BINS - 1);
    bin_freq[i] = f;
    double k = f * BLOCK_LEN / SR;             // need not be an integer bin
    bin_coeff[i] = 2.0 * cos(2.0 * M_PI * k / BLOCK_LEN);
  }
  bins_ready = TRUE;
}

// ---- adaptive envelope / Schmitt trigger -----------------------------------
// The floor/peak followers must move much slower than a single Morse element
// (even the longest dash at the slowest sane WPM) or the on/off threshold
// collapses mid-element and every mark misclassifies. Time constants here are
// several seconds; the *snap* side (floor down, peak up) is still immediate.
#define SQ_OPEN             10.0    // open the squelch when the keying modulation depth (sig_hi/floor) exceeds this
#define SQ_CLOSE            3.0     // ...and re-squelch below this (hysteresis) — CW keying swing vs flat noise
#define SIG_HI_DECAY        (BLOCK_MS / 1500.0)   // ~1.5 s: sig_hi holds the mark level across inter-element gaps
#define TONE_EMA_ALPHA      0.02    // slow: which bin is "the" CW tone
#define FLOOR_RISE_ALPHA    (BLOCK_MS / 4000.0)   // ~4 s time constant
#define PEAK_DECAY_ALPHA    (BLOCK_MS / 2500.0)   // ~2.5 s time constant
#define THRESH_ON_K         0.6
#define THRESH_OFF_K        0.4
#define DEBOUNCE_BLOCKS     4       // candidate must persist this many blocks

// ---- Morse timing ------------------------------------------------------
#define DOT_MS_SEED         60.0   // ~20 WPM
#define DOT_EMA_ALPHA        0.2
#define DOT_MS_MIN          20.0
#define DOT_MS_MAX         300.0
#define WPM_MIN              5
#define WPM_MAX             60
#define MAX_SYMBOL_LEN       8
#define LOCK_ELEMENTS        6     // elements observed before status.locked
// Morse-timing plausibility gate. Real CW marks land on 1 or 3 dot-units and
// gaps on 1/3/7; a strong NON-CW signal (SSB carrier, birdie, AGC-pumped noise)
// that gets past the squelch has chaotic durations. We EMA each interval's
// distance to its nearest ideal multiple and only emit while that error stays
// small — so garbage from non-CW signals is suppressed even when they modulate
// enough to open the squelch.
#define TIMING_ALPHA        0.15   // responsiveness of the timing-error EMA
#define TIMING_TOL          0.55   // emit only while EMA'd timing error is below this (dot-units)
#define TIMING_ERR_CAP      2.0    // clamp a single wild interval's contribution

// ---- Morse table ------------------------------------------------------
static const struct { const char *code; char ch; } MORSE_TABLE[] = {
  { ".-",    'A' }, { "-...",  'B' }, { "-.-.",  'C' }, { "-..",   'D' },
  { ".",     'E' }, { "..-.",  'F' }, { "--.",   'G' }, { "....",  'H' },
  { "..",    'I' }, { ".---",  'J' }, { "-.-",   'K' }, { ".-..",  'L' },
  { "--",    'M' }, { "-.",    'N' }, { "---",   'O' }, { ".--.",  'P' },
  { "--.-",  'Q' }, { ".-.",   'R' }, { "...",   'S' }, { "-",     'T' },
  { "..-",   'U' }, { "...-",  'V' }, { ".--",   'W' }, { "-..-",  'X' },
  { "-.--",  'Y' }, { "--..",  'Z' },
  { "-----", '0' }, { ".----", '1' }, { "..---", '2' }, { "...--", '3' },
  { "....-", '4' }, { ".....", '5' }, { "-....", '6' }, { "--...", '7' },
  { "---..", '8' }, { "----.", '9' },
  { ".-.-.-",'.' }, { "--..--",',' }, { "..--..",'?' }, { "-..-.", '/' },
  { "-...-", '=' }, { ".-.-.", '+' }, { "-....-",'-' }, { ".--.-.",'@' },
  { "-.--.", '(' }, { "-.--.-",')' },
};
#define N_MORSE (int)(sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

static char morse_lookup(const char *code) {
  for (int i = 0; i < N_MORSE; i++)
    if (strcmp(MORSE_TABLE[i].code, code) == 0) return MORSE_TABLE[i].ch;
  return 0;   // unknown code: emit nothing
}

// ---- decoder state (audio thread) ------------------------------------------
static gboolean enabled = FALSE;
static gboolean reset_req = FALSE;

static double slow_energy[N_BINS];
static int    dominant_bin = 0;
static double tracked_freq = 600.0;

static double floor_env = 0.0;
static double peak_env  = 0.0;
static double sig_hi    = 0.0;    // fast-attack/slow-decay envelope peak, for the modulation-depth squelch
static gboolean env_seeded = FALSE;
static gboolean peak_locked = FALSE;   // TRUE once a real mark has ever been confirmed
#define BOOTSTRAP_MULT 6.0             // pre-lock squelch margin over the floor

// A short warm-up period (no Schmitt trigger, no state transitions) that
// simply averages the incoming power into floor_env. Without this, floor/peak
// bootstrap directly off whatever the first few blocks happen to see — if
// that is noise (the usual case: the decoder is enabled before the operator
// keys anything), the degenerate near-zero span between them lets pure noise
// free-run the trigger and corrupt the dot-length estimate before any real
// signal ever arrives. Forcing a quiet calibration window first establishes a
// genuine noise-floor baseline so real CW reliably clears the on-threshold.
#define WARMUP_BLOCKS 60   // ~160 ms @ BLOCK_MS
static long warmup_left = WARMUP_BLOCKS;
static long warmup_count = 0;

#define P_SMOOTH_ALPHA 0.3   // ~3-block (~8 ms) smoothing of the raw block power
static double p_smooth = 0.0;

static gboolean squelch_open = FALSE; // envelope modulation-depth gate (keyed tone present vs noise)
static gboolean tone_state = FALSE;   // debounced mark(TRUE)/space(FALSE)
static gboolean candidate  = FALSE;
static int      debounce_cnt = 0;
static long     run_blocks = 0;

static double dot_ms = DOT_MS_SEED;
static int    elements_seen = 0;
static double timing_err = TIMING_ERR_CAP;   // EMA of interval-vs-ideal error; starts pessimistic (suppress)

static char current_symbol[MAX_SYMBOL_LEN + 1];
static int  current_symbol_len = 0;
static gboolean last_emitted_was_space = TRUE;

// current-block Goertzel accumulators
static double gz_s1[N_BINS], gz_s2[N_BINS];
static int    gz_count = 0;

// ---- pending decoded text + published status (mutex-guarded) ---------------
// text_mutex guards BOTH the pending-text ring AND the published status trio
// (pub_*). The status numbers are mutated on the audio thread but read by the
// GTK panel/readout; publishing them under the same lock the text already uses
// avoids a torn double read yielding a bogus WPM/Hz flicker.
#define PENDING_CAP 2048
static GMutex text_mutex;
static char   pending[PENDING_CAP];
static int    pending_len = 0;
// Non-draining rolling tail of the most-recent decoded text, for the always-on
// bottom Decode-block readout (the panel drains `pending`; the block peeks this).
#define HIST_CAP  256
#define HIST_DROP 64            // how much to shed when the ring fills
static char   history[HIST_CAP];
static int    hist_len = 0;
static double pub_dot_ms = DOT_MS_SEED;
static double pub_tone   = 600.0;
static int    pub_elements = 0;

static int    echo_env = -1;   // -1 = not checked yet, 0 = off, 1 = on

static void echo_char(char c) {
  if (echo_env < 0) echo_env = getenv("MACHPSDR_CW_ECHO") ? 1 : 0;
  if (echo_env) { fputc(c, stderr); fflush(stderr); }
}

static void push_pending(char c) {
  g_mutex_lock(&text_mutex);
  if (pending_len < PENDING_CAP - 1) pending[pending_len++] = c;
  // Also feed the non-draining rolling tail; shed the oldest chunk when full.
  if (hist_len >= HIST_CAP - 1) {
    memmove(history, history + HIST_DROP, hist_len - HIST_DROP);
    hist_len -= HIST_DROP;
  }
  history[hist_len++] = c;
  g_mutex_unlock(&text_mutex);
  echo_char(c);
}

static void emit_char(char c) {
  push_pending(c);
  last_emitted_was_space = FALSE;
}

static void emit_space(void) {
  if (last_emitted_was_space) return;
  push_pending(' ');
  last_emitted_was_space = TRUE;
}

// Feed one interval's fit to the timing-error EMA. `units` = interval/dot_ms;
// `ideals` are the allowed dot-unit multiples (1,3 for marks; 1,3,7 for gaps).
static void feed_timing(double units, const double *ideals, int n) {
  double err = fabs(units - ideals[0]);
  for (int i = 1; i < n; i++) { double e = fabs(units - ideals[i]); if (e < err) err = e; }
  if (err > TIMING_ERR_CAP) err = TIMING_ERR_CAP;
  timing_err += TIMING_ALPHA * (err - timing_err);
}

static gboolean timing_ok(void) { return timing_err < TIMING_TOL; }

static void emit_char_from_symbol(void) {
  if (current_symbol_len == 0) return;
  current_symbol[current_symbol_len] = '\0';
  char c = morse_lookup(current_symbol);
  // Only emit when the recent timing looks like real Morse; otherwise the
  // symbol is discarded (a non-CW signal that opened the squelch).
  if (c && timing_ok()) emit_char(c);
  else if (!c)          log_debug("cw_decoder: unknown code '%s'\n", current_symbol);
  current_symbol_len = 0;
  current_symbol[0] = '\0';
}

// A completed gap (space run) of gap_ms. Idempotent: safe to call repeatedly
// while a long silence continues (used both for the confirmed space->mark
// transition and for a flush during an ongoing word gap with no mark yet).
static void handle_gap(double gap_ms) {
  // Feed the timing plausibility gate for every gap (element/char/word ideals
  // 1/3/7 dot-units) — element gaps count too, so a signal whose gaps don't fit
  // pulls timing_err up and suppresses emission.
  static const double gap_ideals[3] = { 1.0, 3.0, 7.0 };
  feed_timing(gap_ms / dot_ms, gap_ideals, 3);

  if (gap_ms < 2.0 * dot_ms) return;              // element gap: ignore
  emit_char_from_symbol();                        // char gap: flush the symbol
  if (gap_ms >= 5.0 * dot_ms && timing_ok()) emit_space();  // word gap: also a space
}

// Adaptive dot length: track the shortest mark seen in a short rolling window
// and EMA the running estimate toward it. This is deliberately *not* "only
// learn from marks already classified as dots" — that scheme is a trap: a bad
// early estimate (e.g. corrupted by pre-signal noise transients) makes every
// later mark classify as a dash too, so it would never see another "dot" to
// correct itself from. Feeding every mark's duration through a rolling
// minimum means one genuinely short element — dot or not — is enough to pull
// the estimate back to reality within a window's worth of marks.
#define MARK_HISTORY 16
static double mark_history[MARK_HISTORY];
static int    mark_hist_len = 0, mark_hist_pos = 0;

static void classify_mark(double mark_ms) {
  mark_history[mark_hist_pos] = mark_ms;
  mark_hist_pos = (mark_hist_pos + 1) % MARK_HISTORY;
  if (mark_hist_len < MARK_HISTORY) mark_hist_len++;
  double min_recent = mark_history[0];
  for (int i = 1; i < mark_hist_len; i++)
    if (mark_history[i] < min_recent) min_recent = mark_history[i];

  dot_ms = dot_ms * (1.0 - DOT_EMA_ALPHA) + min_recent * DOT_EMA_ALPHA;
  if (dot_ms < DOT_MS_MIN) dot_ms = DOT_MS_MIN;
  if (dot_ms > DOT_MS_MAX) dot_ms = DOT_MS_MAX;

  static const double mark_ideals[2] = { 1.0, 3.0 };   // dot, dash
  feed_timing(mark_ms / dot_ms, mark_ideals, 2);

  gboolean is_dot = mark_ms < 2.0 * dot_ms;
  if (current_symbol_len < MAX_SYMBOL_LEN) {
    current_symbol[current_symbol_len++] = is_dot ? '.' : '-';
    current_symbol[current_symbol_len] = '\0';
  }
  elements_seen++;
}

static void do_reset(void) {
  for (int i = 0; i < N_BINS; i++) slow_energy[i] = 0.0;
  dominant_bin = 0;
  tracked_freq = (F_LO + F_HI) * 0.5;
  floor_env = peak_env = sig_hi = 0.0;
  env_seeded = FALSE;
  peak_locked = FALSE;
  warmup_left = WARMUP_BLOCKS;
  warmup_count = 0;
  p_smooth = 0.0;
  squelch_open = FALSE;
  tone_state = candidate = FALSE;
  debounce_cnt = 0;
  run_blocks = 0;
  dot_ms = DOT_MS_SEED;
  mark_hist_len = 0;
  mark_hist_pos = 0;
  elements_seen = 0;
  timing_err = TIMING_ERR_CAP;
  current_symbol_len = 0;
  current_symbol[0] = '\0';
  last_emitted_was_space = TRUE;
  gz_count = 0;
  for (int i = 0; i < N_BINS; i++) gz_s1[i] = gz_s2[i] = 0.0;

  g_mutex_lock(&text_mutex);
  pending_len = 0;
  hist_len = 0;
  pub_dot_ms = DOT_MS_SEED;
  pub_tone = (F_LO + F_HI) * 0.5;
  pub_elements = 0;
  g_mutex_unlock(&text_mutex);
}

// Process one completed Goertzel block: tone tracking, envelope, Schmitt
// trigger + debounce, mark/space classification.
static void process_block(void) {
  double power[N_BINS];
  for (int i = 0; i < N_BINS; i++) {
    double s1 = gz_s1[i], s2 = gz_s2[i];
    power[i] = s1 * s1 + s2 * s2 - bin_coeff[i] * s1 * s2;
    if (power[i] < 0.0) power[i] = 0.0;
    slow_energy[i] += TONE_EMA_ALPHA * (power[i] - slow_energy[i]);
  }
  gz_count = 0;
  for (int i = 0; i < N_BINS; i++) gz_s1[i] = gz_s2[i] = 0.0;

  // Reported tone frequency: the bin with the highest *slow* EMA energy, so
  // the readout doesn't jump around on individual noise blocks.
  int best = 0;
  for (int i = 1; i < N_BINS; i++) if (slow_energy[i] > slow_energy[best]) best = i;
  dominant_bin = best;
  tracked_freq = bin_freq[best];

  // On/off envelope = TOTAL in-band energy (sum of all bins). The Goertzel bank
  // is deliberately oversampled (16 bins / 40 Hz over a ~375 Hz per-filter
  // resolution), so it can't resolve spectral tonality — a single tone lights up
  // the whole bank. But total in-band energy is a clean keying envelope: on a
  // keyed CW signal it swings between mark (noise+tone) and gap (noise); on
  // broadband noise the sum over 16 bins is statistically near-constant. That
  // difference — envelope MODULATION DEPTH, not spectral shape — is what tells
  // CW from noise here (see the sig_hi/floor squelch below).
  double p_raw = 0.0;
  for (int i = 0; i < N_BINS; i++) p_raw += power[i];

  // Smooth over a few blocks (~8 ms) so a single noisy 2.667 ms block can't
  // masquerade as a keying edge (still well under the shortest real element).
  p_smooth += P_SMOOTH_ALPHA * (p_raw - p_smooth);
  double p = p_smooth;

  // Quiet calibration window: just average p into floor_env, no trigger, no
  // state transitions (run_blocks still accumulates so the very first mark
  // after warm-up gets a correct off-duration). See WARMUP_BLOCKS above.
  if (warmup_left > 0) {
    warmup_count++;
    floor_env += (p - floor_env) / (double)warmup_count;
    peak_env = sig_hi = floor_env;
    warmup_left--;
    run_blocks++;
    return;
  }
  if (!env_seeded) { peak_env = floor_env; env_seeded = TRUE; }
  // The noise floor tracks every block (attack down fast, slow rise) — this is
  // safe even while a mark is on, since the rise time constant (seconds) is
  // far slower than any Morse element. peak_env is deliberately NOT updated
  // here from every raw block: a fast "snap up to any new max" follower is
  // exactly what let isolated noise blocks masquerade as signal (a single
  // loud block, or noise's own slowly-creeping running maximum, inflates it
  // with nothing to distinguish "one noisy block" from "an actual tone").
  // Instead peak_env is only (re)seeded from p once a mark is actually
  // CONFIRMED below (i.e. survived the debounce) — see the transition block.
  if (p < floor_env) floor_env = p;
  else floor_env += FLOOR_RISE_ALPHA * (p - floor_env);

  // Envelope MODULATION-DEPTH squelch. sig_hi is a fast-attack / slow-decay
  // follower of the total-energy envelope, so it sits at the mark level while
  // floor_env sits at the gap/noise level. Their ratio is the keying depth:
  // a real keyed signal swings it high; broadband noise (near-constant total
  // energy) keeps sig_hi≈floor, so the ratio stays low and the squelch shut.
  // Only while open does the Schmitt trigger form marks/spaces (below), so
  // noise emits nothing. Hysteresis: SQ_OPEN to open, SQ_CLOSE to re-squelch.
  if (p > sig_hi) sig_hi = p;
  else sig_hi += SIG_HI_DECAY * (p - sig_hi);
  double depth = sig_hi / (floor_env + 1e-9);
  if (!squelch_open && depth >= SQ_OPEN)      squelch_open = TRUE;
  else if (squelch_open && depth <  SQ_CLOSE) squelch_open = FALSE;

  // Before any mark has ever been confirmed, require a strict margin over the
  // floor (a real keyed tone comfortably clears the noise floor by several
  // times; noise alone essentially never does for DEBOUNCE_BLOCKS in a row).
  // This is the squelch that keeps pre-signal noise from ever free-running
  // the trigger in the first place.
  double span = peak_env - floor_env;
  double min_span = 1e-3;
  if (floor_env * 0.5 > min_span) min_span = floor_env * 0.5;
  if (!peak_locked) {
    double boot = floor_env * BOOTSTRAP_MULT;
    if (boot > min_span) min_span = boot;
  }
  if (span < min_span) span = min_span;
  double thr_on  = floor_env + THRESH_ON_K  * span;
  double thr_off = floor_env + THRESH_OFF_K * span;

  // The Schmitt trigger + element classification only run while the squelch is
  // open (a sustained tone is present). Noise keeps the squelch closed, so it
  // forms no marks and emits nothing.
  if (squelch_open) {
    gboolean want = candidate;
    if (!tone_state && p > thr_on)       want = TRUE;
    else if (tone_state && p < thr_off)  want = FALSE;

    if (want != candidate) { candidate = want; debounce_cnt = 0; }
    else debounce_cnt++;

    if (candidate != tone_state && debounce_cnt >= DEBOUNCE_BLOCKS) {
      double duration_ms = run_blocks * BLOCK_MS;
      if (tone_state) {
        classify_mark(duration_ms);   // mark just ended
      } else {
        handle_gap(duration_ms);       // space just ended
        // A mark just got confirmed (we were OFF, now going ON): seed peak_env
        // from the tone power we're actually seeing right now — the one and
        // only place peak_env moves, so it only ever reflects a real,
        // debounce-confirmed element.
        peak_env = p;
        peak_locked = TRUE;
      }
      tone_state = candidate;
      run_blocks = 0;
    }
    run_blocks++;

    // Flush a completed word/char before the next mark ever arrives (covers the
    // tail of a transmission / trailing silence between words).
    if (!tone_state) {
      double off_ms = run_blocks * BLOCK_MS;
      if (off_ms >= 2.0 * dot_ms) handle_gap(off_ms);
    }
  } else {
    // Squelched: close an in-progress mark and flush the symbol accumulated
    // while a real tone was present (end-of-transmission tail), then hold off.
    if (tone_state) classify_mark(run_blocks * BLOCK_MS);
    emit_char_from_symbol();
    tone_state = candidate = FALSE;
    debounce_cnt = 0;
    run_blocks = 0;
  }

  // Publish the status trio under the text lock so the GTK reader never sees a
  // torn double.
  g_mutex_lock(&text_mutex);
  pub_dot_ms = dot_ms;
  pub_tone = tracked_freq;
  pub_elements = elements_seen;
  g_mutex_unlock(&text_mutex);
}

void cw_decoder_set_enabled(gboolean on) {
  if (on == enabled) return;
  enabled = on;
  if (on) reset_req = TRUE;   // fresh start each time the decoder is selected
}

void cw_decoder_add_audio(const double *samples, int nframes) {
  if (!enabled) return;
  if (!bins_ready) init_bins();
  if (reset_req) { reset_req = FALSE; do_reset(); }

  for (int i = 0; i < nframes; i++) {
    double x = samples[i * 2];
    for (int b = 0; b < N_BINS; b++) {
      double s0 = bin_coeff[b] * gz_s1[b] - gz_s2[b] + x;
      gz_s2[b] = gz_s1[b];
      gz_s1[b] = s0;
    }
    if (++gz_count >= BLOCK_LEN) process_block();
  }
}

int cw_decoder_get_text(char *buf, int buflen) {
  if (buf == NULL || buflen <= 0) return 0;
  g_mutex_lock(&text_mutex);
  int n = pending_len;
  if (n > buflen - 1) n = buflen - 1;
  if (n > 0) memcpy(buf, pending, n);
  buf[n] = '\0';
  // Keep any leftover (buf was too small) for the next poll.
  if (n < pending_len) {
    memmove(pending, pending + n, pending_len - n);
    pending_len -= n;
  } else {
    pending_len = 0;
  }
  g_mutex_unlock(&text_mutex);
  return n;
}

int cw_decoder_get_recent(char *buf, int buflen) {
  if (buf == NULL || buflen <= 0) return 0;
  g_mutex_lock(&text_mutex);
  int take = hist_len;
  if (take > buflen - 1) take = buflen - 1;
  memcpy(buf, history + hist_len - take, take);   // the newest `take` chars
  buf[take] = '\0';
  g_mutex_unlock(&text_mutex);
  return take;
}

void cw_decoder_get_status(int *wpm, double *tone_hz, gboolean *locked) {
  g_mutex_lock(&text_mutex);
  double d = pub_dot_ms, t = pub_tone;
  int el = pub_elements;
  g_mutex_unlock(&text_mutex);

  int w = (int)lround(1200.0 / d);
  if (w < WPM_MIN) w = WPM_MIN;
  if (w > WPM_MAX) w = WPM_MAX;
  if (wpm)     *wpm = w;
  if (tone_hz) *tone_hz = t;
  if (locked)  *locked = enabled && (el >= LOCK_ELEMENTS);
}

void cw_decoder_reset(void) {
  reset_req = TRUE;
  // Also clear pending + rolling text immediately so both the panel's "Clear"
  // and the bottom-block readout blank at once, even if the audio thread is
  // momentarily idle (do_reset() will re-clear them too).
  g_mutex_lock(&text_mutex);
  pending_len = 0;
  hist_len = 0;
  g_mutex_unlock(&text_mutex);
}
