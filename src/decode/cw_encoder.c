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
#include <gtk/gtk.h>

// Prerequisite types for radio.h (mirrors the include order used elsewhere).
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "mode.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"       // RADIO, set_mox()
#include "main.h"        // global RADIO *radio
#include "log.h"
#include "cw_encoder.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===========================================================================
// Char -> Morse code table (the inverse of cw_decoder.c's MORSE_TABLE; kept as
// an independent copy here rather than sharing/modifying the decoder file).
// ===========================================================================
static const struct { char ch; const char *code; } CW_CODE_TABLE[] = {
  { 'A', ".-" },     { 'B', "-..." },   { 'C', "-.-." },   { 'D', "-.." },
  { 'E', "." },      { 'F', "..-." },   { 'G', "--." },    { 'H', "...." },
  { 'I', ".." },     { 'J', ".---" },   { 'K', "-.-" },    { 'L', ".-.." },
  { 'M', "--" },     { 'N', "-." },     { 'O', "---" },    { 'P', ".--." },
  { 'Q', "--.-" },   { 'R', ".-." },    { 'S', "..." },    { 'T', "-" },
  { 'U', "..-" },    { 'V', "...-" },   { 'W', ".--" },    { 'X', "-..-" },
  { 'Y', "-.--" },   { 'Z', "--.." },
  { '0', "-----" },  { '1', ".----" },  { '2', "..---" },  { '3', "...--" },
  { '4', "....-" },  { '5', "....." },  { '6', "-...." },  { '7', "--..." },
  { '8', "---.." },  { '9', "----." },
  { '.', ".-.-.-" }, { ',', "--..--" }, { '?', "..--.." }, { '/', "-..-." },
  { '=', "-...-" },  { '+', ".-.-." },  { '-', "-....-" }, { '@', ".--.-." },
  { '(', "-.--." },  { ')', "-.--.-" },
};
#define N_CW_CODES (int)(sizeof(CW_CODE_TABLE) / sizeof(CW_CODE_TABLE[0]))

static const char *char_to_code(char c) {
  for (int i = 0; i < N_CW_CODES; i++)
    if (CW_CODE_TABLE[i].ch == c) return CW_CODE_TABLE[i].code;
  return NULL;   // unencodable: caller skips it
}

// Expand the (currently one) macro token: %C -> station callsign, %% -> %.
// Unknown/unterminated tokens are copied through literally.
static void cw_expand_macros(const char *in, char *out, size_t outlen) {
  if (outlen == 0) return;
  size_t oi = 0;
  for (size_t ii = 0; in[ii] != '\0' && oi + 1 < outlen; ii++) {
    if (in[ii] == '%' && in[ii + 1] == 'C') {
      const char *call = (radio != NULL) ? radio->station_call : "";
      for (size_t k = 0; call[k] != '\0' && oi + 1 < outlen; k++) out[oi++] = call[k];
      ii++;   // consume the 'C'
    } else if (in[ii] == '%' && in[ii + 1] == '%') {
      out[oi++] = '%';
      ii++;
    } else {
      out[oi++] = in[ii];
    }
  }
  out[oi] = '\0';
}

// ===========================================================================
// PURE encoder: text -> keyed-sidetone audio. No RADIO/GTK/MOX dependency, so
// it can be linked into a standalone headless test harness against
// cw_decoder.c (see the round-trip test used to verify this feature).
// ===========================================================================
typedef struct { gboolean on; double dur_ms; } cw_seg_t;

static void seg_push(cw_seg_t **segs, int *n, int *cap, gboolean on, double dur_ms) {
  if (dur_ms <= 0.0) return;
  if (*n >= *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *segs = g_realloc(*segs, (gsize)(*cap) * sizeof(cw_seg_t));
  }
  (*segs)[*n].on = on;
  (*segs)[*n].dur_ms = dur_ms;
  (*n)++;
}

float *cw_encode_to_audio(const char *text, int wpm, int weight,
                          double sidetone_hz, int sample_rate,
                          double amplitude, int *n_samples) {
  if (n_samples) *n_samples = 0;
  if (text == NULL || text[0] == '\0') return NULL;
  if (wpm < 1) wpm = 1;
  double dot_ms = 1200.0 / (double)wpm;   // PARIS-standard dot length

  // weight==50 => exact standard timing (dot=1, dash=3, gaps=1/3/7 dot-units).
  // Otherwise the mark is stretched (or shortened) and the ELEMENT gap takes the
  // difference, which is the WinKeyer convention: weight is the share of a
  // dot-plus-gap period the key is down, so at 65 the dot is 1.3 units and the
  // element gap 0.7. Dash stays 3x the dot and the letter/word gaps stay at
  // 3/7 units, so heavy weighting costs a little speed rather than legibility.
  //
  // The compensation is per-gap and uniform — NOT proportional to the mark it
  // follows, and never accumulated. Accumulating a whole character's worth onto
  // the one inter-character gap (what this did before) leaves the elements at
  // standard spacing and pays for all of it in a single place: at weight 65 that
  // gap collapsed to the anti-click ramp and the letters ran together into one
  // unbroken stream. Guarded at both extremes by tools/cw_offline.c.
  double wscale = (double)weight / 50.0;
  if (wscale < 0.3) wscale = 0.3;
  if (wscale > 2.5) wscale = 2.5;
  // A gap has to survive an extreme setting: the UI spinner goes to 100, where
  // 2 - wscale is negative and mark and gap would fuse into one longer mark.
  double elem_gap_ms = (2.0 - wscale) * dot_ms;
  if (elem_gap_ms < 0.25 * dot_ms) elem_gap_ms = 0.25 * dot_ms;

  cw_seg_t *segs = NULL;
  int n_segs = 0, cap_segs = 0;
  double total_ms = 0.0;
  double next_gap_dots = 0.0;   // dot-units owed before the NEXT character
  gboolean first_char = TRUE;
  gboolean have_output = FALSE;

  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    char c = (char)toupper(*p);
    if (c == ' ') {
      if (!first_char && next_gap_dots < 7.0) next_gap_dots = 7.0;
      continue;
    }
    const char *code = char_to_code(c);
    if (code == NULL) {
      log_debug_area(LOG_TX, "cw_encoder: skipping unencodable char '%c'\n", c);
      continue;
    }
    if (!first_char) {
      double gap_ms = next_gap_dots * dot_ms;
      seg_push(&segs, &n_segs, &cap_segs, FALSE, gap_ms);
      total_ms += gap_ms;
    }
    for (int k = 0; code[k] != '\0'; k++) {
      if (k > 0) {   // intra-character (element) gap — the one weight acts on
        seg_push(&segs, &n_segs, &cap_segs, FALSE, elem_gap_ms);
        total_ms += elem_gap_ms;
      }
      double nominal_dots = (code[k] == '-') ? 3.0 : 1.0;
      double mark_ms = nominal_dots * dot_ms * wscale;
      seg_push(&segs, &n_segs, &cap_segs, TRUE, mark_ms);
      total_ms += mark_ms;
    }
    next_gap_dots = 3.0;   // default: a letter gap before the next character
    first_char = FALSE;
    have_output = TRUE;
  }

  if (!have_output || n_segs == 0) {
    g_free(segs);
    return NULL;
  }

  long total_samples = (long)(total_ms * sample_rate / 1000.0 + 0.5);
  if (total_samples <= 0) { g_free(segs); return NULL; }

  float *buf = g_malloc0(sizeof(float) * (gsize)total_samples);
  double dphi = 2.0 * M_PI * sidetone_hz / (double)sample_rate;
  double phase = 0.0;

  // Raised-cosine on/off ramp (~5 ms) at every mark's edges to suppress key
  // clicks; phase is kept continuous across the whole message (including
  // silent gaps) so re-keying never introduces a phase discontinuity.
  const double RAMP_MS = 5.0;
  long ramp_nom = (long)(RAMP_MS * sample_rate / 1000.0 + 0.5);
  if (ramp_nom < 1) ramp_nom = 1;

  double cum_ms = 0.0;
  long start_idx = 0;
  for (int i = 0; i < n_segs; i++) {
    cum_ms += segs[i].dur_ms;
    long end_idx = (long)(cum_ms * sample_rate / 1000.0 + 0.5);
    if (end_idx > total_samples) end_idx = total_samples;
    long seg_len = end_idx - start_idx;
    if (seg_len <= 0) { start_idx = end_idx; continue; }

    long rs = ramp_nom;
    if (rs * 2 > seg_len) rs = seg_len / 2;
    if (rs < 1) rs = 1;

    for (long j = 0; j < seg_len; j++) {
      double s = 0.0;
      if (segs[i].on) {
        double env = 1.0;
        if (j < rs)                  env = 0.5 * (1.0 - cos(M_PI * (double)j / (double)rs));
        else if (j >= seg_len - rs)  env = 0.5 * (1.0 - cos(M_PI * (double)(seg_len - 1 - j) / (double)rs));
        s = sin(phase) * env;
      }
      buf[start_idx + j] = (float)(amplitude * s);
      phase += dphi;
      if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
    start_idx = end_idx;
  }

  g_free(segs);
  if (n_samples) *n_samples = (int)total_samples;
  return buf;
}

// ===========================================================================
// Live TX side (mirrors ft8_encoder.c / sstv_encoder.c): MOX keying, a
// lock-free sample feeder for the audio thread, and a GTK-timer watchdog.
//
// The waveform lives in a fixed static buffer that is NEVER freed, so the audio
// thread can read it with no lock and no risk of use-after-free: a send memcpy's
// the freshly-encoded waveform in, then flips `active` to 1 with a release
// barrier (g_atomic) once the buffer is fully published; cw_tx_next_sample()
// acquires `active` before touching the buffer. (An earlier version g_free()d a
// heap wave from the GTK thread while the audio thread was mid-read — a
// use-after-free on weakly-ordered CPUs; the static buffer removes it entirely.)
//
// Phase 4.4b adds a second TX mode alongside the memory-message wave[] replay:
// a real-time KEYER tone gated by cw_tx_key(), driven by the iambic keyer
// (cw_keyer.c). `active` (and MOX) is shared by both modes -- `tx_mode` picks
// which one cw_tx_next_sample() renders, and cw_tx_send_text()'s pre-existing
// `if (active) return FALSE` guard already keeps a memory send from fighting a
// paddle that is mid-KEYER (see the mode==CW_TX_MESSAGE check in cw_tx_key()
// for the reverse direction: a paddle event can't hijack an in-flight send).
// ===========================================================================
#define CW_TX_RATE            48000
#define CW_TX_AMPL            0.6f
#define CW_TX_MAX_MARGIN_MS   2000               // safety margin over waveform length
#define CW_TX_MAX_SAMPLES     (CW_TX_RATE * 60)  // hard cap: 60 s of keyed CW
#define CW_KEYER_RAMP_SAMPLES ((int)(CW_TX_RATE * 0.005))  // ~5 ms raised-cosine edge
#define CW_KEYER_HANG_MS      250                // default hang time if unset/<=0

enum { CW_TX_IDLE = 0, CW_TX_MESSAGE, CW_TX_KEYER };

static float          wave[CW_TX_MAX_SAMPLES];   // static: never freed (no UAF)
static int            wave_len = 0;              // valid once published; set before `active`
static volatile int   wave_pos = 0;             // next sample index (audio thread)
static volatile gint  active = 0;               // g_atomic: 1 while clocking out (either mode)
static gboolean       we_keyed = FALSE;         // did WE raise MOX (GTK thread only)
static guint          tick_id = 0;              // watchdog g_timeout id (GTK thread)
static gint64         key_time_ms = 0;          // when the MESSAGE send started (watchdog, ms)

static volatile gint  tx_mode = CW_TX_IDLE;     // g_atomic: which engine cw_tx_next_sample() renders
static volatile gint  key_on = 0;               // g_atomic: KEYER paddle state (audio thread reads)
static gint64         last_key_ms = 0;          // GTK thread: last cw_tx_key() edge, for hang timing
static int            keyer_ramp_pos = 0;       // audio thread: 0..CW_KEYER_RAMP_SAMPLES envelope
static double         keyer_phase = 0.0;        // audio thread: phase-continuous across gaps

// ~100 ms watchdog tick (GTK thread): drops MOX when the waveform finishes /
// the paddle hangs, and enforces the MESSAGE-mode safety watchdog, like
// ft8_encoder.c's tx_tick.
static gboolean tx_tick(gpointer data) {
  if (!g_atomic_int_get(&active)) { tick_id = 0; return G_SOURCE_REMOVE; }
  gint64 now_ms = g_get_real_time() / 1000;

  if (g_atomic_int_get(&tx_mode) == CW_TX_KEYER) {
    if (!g_atomic_int_get(&key_on)) {
      // Hang floored at ~2 dot-units so it always spans a 1-unit inter-element
      // gap: at slow WPM (or a short configured hang) MOX would otherwise drop
      // and re-key between every element, chopping the character.
      gint64 cfg_hang = (radio != NULL && radio->cw_keyer_hang_time > 0) ?
                         radio->cw_keyer_hang_time : CW_KEYER_HANG_MS;
      gint64 min_hang = (radio != NULL && radio->cw_keyer_speed > 0) ?
                         (gint64)(2.0 * 1200.0 / radio->cw_keyer_speed) : 120;
      gint64 hang_ms = cfg_hang > min_hang ? cfg_hang : min_hang;
      if (now_ms - last_key_ms > hang_ms) {
        if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
        g_atomic_int_set(&active, 0);
        g_atomic_int_set(&tx_mode, CW_TX_IDLE);
        tick_id = 0;
        return G_SOURCE_REMOVE;
      }
    }
    return G_SOURCE_CONTINUE;
  }

  // CW_TX_MESSAGE: unchanged completion / stall-watchdog logic.
  if (wave_pos >= wave_len) {
    g_atomic_int_set(&active, 0);
    g_atomic_int_set(&tx_mode, CW_TX_IDLE);
    if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
    log_info("cw-tx: complete\n");
    tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  // Safety watchdog: fire regardless of who raised MOX, so a stalled feed (a mode
  // mismatch, or MOX dropped elsewhere) can never leave `active` stuck at 1 and
  // wedge every future send behind the `if (active) return FALSE` guard.
  gint64 max_ms = (gint64)wave_len * 1000 / CW_TX_RATE + CW_TX_MAX_MARGIN_MS;
  if (key_time_ms && (now_ms - key_time_ms) > max_ms) {
    log_error("cw-tx: WATCHDOG - waveform not drained in %lldms (wave_pos=%d/%d)\n",
              (long long)max_ms, wave_pos, wave_len);
    g_atomic_int_set(&active, 0);
    g_atomic_int_set(&tx_mode, CW_TX_IDLE);
    if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
    tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

gboolean cw_tx_send_text(const char *text) {
  if (text == NULL || text[0] == '\0') return FALSE;
  // Also true while the KEYER is mid-paddle (active==1 covers both modes), so
  // a memory send can't fight the paddle.
  if (g_atomic_int_get(&active)) return FALSE;
  if (radio == NULL || !radio->can_transmit) {
    log_error("cw-tx: radio cannot transmit\n");
    return FALSE;
  }
  if (radio->tune) {
    log_error("cw-tx: cannot send CW while tuning\n");
    return FALSE;
  }
  // Gate on the *transmitter's* mode -- the same mode add_mic_sample() uses to
  // decide whether to substitute our samples -- so we never key MOX in a mode
  // where the CW waveform would not actually be injected (e.g. split with a
  // non-CW transmit VFO while the active RX is on a CW VFO).
  int txmode = (radio->transmitter != NULL) ? transmitter_get_mode(radio->transmitter) : -1;
  if (txmode != CWL && txmode != CWU) {
    log_error("cw-tx: transmit mode must be CWL/CWU\n");
    return FALSE;
  }

  char expanded[256];
  cw_expand_macros(text, expanded, sizeof(expanded));

  int n = 0;
  float *tmp = cw_encode_to_audio(expanded, radio->cw_keyer_speed, radio->cw_keyer_weight,
                                  (double)radio->cw_keyer_sidetone_frequency,
                                  CW_TX_RATE, CW_TX_AMPL, &n);
  if (tmp == NULL || n <= 0) {
    g_free(tmp);
    log_error("cw-tx: nothing to send (empty/unencodable text)\n");
    return FALSE;
  }
  if (n > CW_TX_MAX_SAMPLES) {
    log_error("cw-tx: message longer than %d s, truncating\n", CW_TX_MAX_SAMPLES / CW_TX_RATE);
    n = CW_TX_MAX_SAMPLES;
  }
  memcpy(wave, tmp, (size_t)n * sizeof(float));
  g_free(tmp);   // GTK thread, buffer not yet published to the audio thread -> safe

  wave_len = n;
  wave_pos = 0;
  key_time_ms = g_get_real_time() / 1000;
  g_atomic_int_set(&tx_mode, CW_TX_MESSAGE);
  g_atomic_int_set(&active, 1);   // release: publishes wave[]/wave_len + tx_mode to the audio thread

  we_keyed = FALSE;
  if (!radio->mox) { we_keyed = TRUE; set_mox(radio, TRUE); }
  if (tick_id == 0) tick_id = g_timeout_add(100, tx_tick, NULL);
  log_info("cw-tx: sending \"%s\" (%.1f s)\n", expanded, (double)wave_len / (double)CW_TX_RATE);
  return TRUE;
}

void cw_tx_abort(void) {
  g_atomic_int_set(&active, 0);
  g_atomic_int_set(&tx_mode, CW_TX_IDLE);
  if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
  // tx_tick removes itself on its next fire now that active is clear.
}

gboolean cw_tx_active(void) {
  return g_atomic_int_get(&active) ? TRUE : FALSE;
}

// Real-time keyer keying (Phase B): the iambic keyer (cw_keyer.c) calls this at
// every mark's start/end. MOX is owned here exactly like a MESSAGE send -- raised
// on the first key-down if we weren't already keyed, dropped by tx_tick() once
// the paddle has been up for cw_keyer_hang_time ms. A MESSAGE send in progress
// owns the TX chain, so paddle edges are ignored while one is active.
void cw_tx_key(gboolean down) {
  if (radio == NULL) return;
  if (g_atomic_int_get(&tx_mode) == CW_TX_MESSAGE) return;   // a memory send owns the TX

  if (down) {
    // Safety gate on the DOWN edge only (an UP must always be honoured so the
    // key can't stick): never raise MOX unless the radio can transmit and the
    // transmit mode is CW. This covers BOTH the keyboard paddle (which also
    // gates in receiver.c) and the MIDI paddle (which has no gate of its own),
    // and guards the RX-only case where radio->transmitter is NULL (set_mox ->
    // rxtx() would deref it).
    if (!radio->can_transmit || radio->transmitter == NULL) return;
    int m = transmitter_get_mode(radio->transmitter);
    if (m != CWL && m != CWU) return;

    if (g_atomic_int_get(&tx_mode) != CW_TX_KEYER) {
      // Start of a keying session: raise MOX ONCE here (not per key-down). If
      // we set we_keyed on every element's key-down, the 2nd element would find
      // MOX already up -> we_keyed reset to FALSE -> tx_tick would never drop
      // the MOX we raised, leaving it stuck on after the paddle is released.
      g_atomic_int_set(&tx_mode, CW_TX_KEYER);
      g_atomic_int_set(&active, 1);   // release: publishes tx_mode to the audio thread
      we_keyed = FALSE;
      if (!radio->mox) { we_keyed = TRUE; set_mox(radio, TRUE); }
      if (tick_id == 0) tick_id = g_timeout_add(100, tx_tick, NULL);
    }
    g_atomic_int_set(&key_on, 1);
  } else {
    g_atomic_int_set(&key_on, 0);
  }
  last_key_ms = g_get_real_time() / 1000;
}

float cw_tx_next_sample(void) {
  if (!g_atomic_int_get(&active)) return 0.0f;   // acquire: pairs with the publishing release
  gint mode = g_atomic_int_get(&tx_mode);

  if (mode == CW_TX_KEYER) {
    gboolean on = g_atomic_int_get(&key_on) ? TRUE : FALSE;
    if (on && keyer_ramp_pos < CW_KEYER_RAMP_SAMPLES) keyer_ramp_pos++;
    else if (!on && keyer_ramp_pos > 0) keyer_ramp_pos--;
    double env = 0.5 * (1.0 - cos(M_PI * (double)keyer_ramp_pos / (double)CW_KEYER_RAMP_SAMPLES));

    double hz = (radio != NULL) ? (double)radio->cw_keyer_sidetone_frequency : 600.0;
    double dphi = 2.0 * M_PI * hz / (double)CW_TX_RATE;
    float s = (float)(CW_TX_AMPL * env * sin(keyer_phase));
    keyer_phase += dphi;   // keeps advancing even while silent -> phase-continuous, no click on re-key
    if (keyer_phase >= 2.0 * M_PI) keyer_phase -= 2.0 * M_PI;
    return s;
  }

  if (mode == CW_TX_MESSAGE) {   // wave[] replay
    int i = wave_pos;
    if (i >= wave_len) return 0.0f;
    wave_pos = i + 1;
    return wave[i];
  }

  return 0.0f;   // CW_TX_IDLE seen transiently on a mode change: no stale wave[] replay
}
