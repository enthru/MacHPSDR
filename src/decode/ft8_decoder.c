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
#include "log.h"
#include <string.h>
#include "time_compat.h"   // <time.h> + gmtime_r() on Windows
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#include <ft8/decode.h>
#include <ft8/message.h>
#include <ft8/encode.h>
#include <ft8/constants.h>
#include <common/monitor.h>
#include <fft/kiss_fftr.h>

#include "ft8_decoder.h"
#include "ft8_pskreporter.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- FT8 timing / audio constants -----------------------------------------
#define FT8_RATE        12000            // decoder sample rate (Hz)
#define FT8_DECIM       4                // 48000 / 12000
#define FT8_SLOT_SEC    15               // FT8 time slot (s)
#define SLOT_SAMPLES    (FT8_RATE * FT8_SLOT_SEC)         // decode window (180000)
#define RING_CAP        (FT8_RATE * (FT8_SLOT_SEC + 1))   // ring: one window + margin
// Decode the most recent window this often.  A recorded or looped I/Q file is not
// aligned to real UTC, and a system clock can be off, so the windows OVERLAP
// instead of cutting the audio at slot boundaries.
//
// The hop is not a taste: it is bounded by how far into a window a transmission
// may start and still be found.  ft8_lib's candidate search sweeps time_offset
// over −10..+19 SYMBOLS, and a transmission must also fit whole (a truncated one
// is dropped — see decode_slot), which caps it at window − waveform.  So the
// usable band of start positions is 0 .. 2.36 s for FT8 (12.64 s of waveform in
// a 15 s window, the symbol sweep reaching 2.96 s and not binding) and 0 ..
// 0.89 s for FT4, where the sweep DOES bind: 19 symbols of 48 ms.  A hop wider
// than that band leaves phases at which no window sees a transmission whole and
// in range, and the transmission is simply lost — which is what a 1 s FT4 hop
// against a 0.89 s band did, on about one transmission in twenty.  The sweep in
// tools/ft8_offline.c walks a whole grid period of start positions for exactly
// this reason.
#define MAX_DECODES     64               // per-window cap we surface to the UI

// ---- protocol (0 = FT8, 1 = FT4), set by ft8_decoder_set_protocol() ---------
// FT4 has a 7.5 s slot and a ~5 s waveform, so its decode window, decode cadence
// and minimum-audio guard are all shorter than FT8's.  SLOT_SAMPLES/RING_CAP
// above stay sized for the longer FT8 window (the FT4 window fits inside them).
static volatile int proto = 0;
static int slot_samples(void) { return proto ? FT8_RATE * 15 / 2 : FT8_RATE * 15; }  // 90000 / 180000
static int decode_hop(void)   { return proto ? FT8_RATE * 3 / 4 : FT8_RATE * 2; }     // 0.75 s / 2 s

// ft8_lib decode tuning (mirrors the reference decoder in demo/decode_ft8.c)
#define KMIN_SCORE      10
#define KMAX_CANDIDATES 140
#define KLDPC_ITERS     25

// ---- enable flag -----------------------------------------------------------
static volatile gboolean enabled = FALSE;

// ---- 48k->12k decimation state (RX thread) ---------------------------------
// Straight decimate-by-4, no extra anti-alias filter: the DIGU SSB filter
// already band-limits the audio to <=~5 kHz, below the 6 kHz Nyquist of the
// 12 kHz decoder rate, so decimation cannot alias.  An extra low-pass here
// would only risk trimming the top of the FT8 sub-band for no benefit.
static int    dec_count = 0;

// ---- rolling audio ring (RX thread writes) ---------------------------------
static float  ring[RING_CAP];
static long   ring_w = 0;                 // total decimated samples written
static long   last_decode_w = 0;          // ring_w at the previous decode trigger

// ---- hand-off to the worker ------------------------------------------------
static GMutex   work_mutex;
static GCond    work_cond;
static GCond    done_cond;                // worker -> ft8_decoder_sync()
static gboolean work_ready = FALSE;
static gboolean running = FALSE;
static float    work_buf[SLOT_SAMPLES];
static int      work_len = 0;
static gint64   work_end_us = 0;          // UTC time of the window's LAST sample
static GThread *worker = NULL;

// Every decode's slot label and dt are derived from the window's end time, so a
// harness has to be able to drive that clock: tools/ft8_offline.c feeds a minute
// of audio in a fraction of a second, and the wall clock would drop every window
// of it into the same slot.  NULL — the application — means g_get_real_time().
static gint64 (*clock_fn)(void) = NULL;
static gint64 now_us(void) { return clock_fn ? clock_fn() : g_get_real_time(); }

// ---- decode result list (worker writes, UI reads) --------------------------
// The list ACCUMULATES one slot's decodes and is reset when a newer slot
// arrives.  Overlapping windows (see decode_hop) see the same transmission two
// or three times over, and a weak station that only comes through in a later
// window has to be APPENDED to what is already on display rather than replace
// it — so a consumer tracks (utc, count) and takes the entries past the count
// it last saw.
static GMutex      list_mutex;
static FT8_DECODE  results[MAX_DECODES];
static int         result_count = 0;
static char        result_utc[8] = "";
static time_t      result_slot = 0;
static gboolean    have_slot = FALSE;

// Cross-window duplicate suppression.  A transmission decodes in EVERY window
// that holds it whole — two or three of them at a 2 s hop — so without this the
// same message is listed twice under two consecutive slot labels, which is
// exactly what the operator saw.  The key is (payload, slot): the payload
// carries both callsigns, so the same 77 bits in the same slot is the same
// transmission and not a second station.  No ageing — an entry can only
// false-match when its slot comes round again a day later, by which time this
// ring has turned over many times.
#define DUP_RING 256
typedef struct {
  uint8_t  payload[FTX_PAYLOAD_LENGTH_BYTES];
  time_t   slot;
  gboolean used;
} DUP_ENTRY;
static DUP_ENTRY dup_ring[DUP_RING];
static int       dup_w = 0;
static guint     dup_hits = 0;            // suppressed repeats, for the harness

// TRUE if this payload has already been published for this slot; otherwise it is
// recorded and FALSE returned.  Worker thread only.
static gboolean dup_seen(const uint8_t *payload, time_t slot) {
  for (int i = 0; i < DUP_RING; i++) {
    if (!dup_ring[i].used) continue;
    if (dup_ring[i].slot == slot &&
        memcmp(dup_ring[i].payload, payload, FTX_PAYLOAD_LENGTH_BYTES) == 0) {
      dup_hits++;
      return TRUE;
    }
  }
  dup_ring[dup_w].used = TRUE;
  dup_ring[dup_w].slot = slot;
  memcpy(dup_ring[dup_w].payload, payload, FTX_PAYLOAD_LENGTH_BYTES);
  dup_w = (dup_w + 1) % DUP_RING;
  return FALSE;
}

// ===========================================================================
// Callsign hash table — required by ftx_message_decode() to resolve the
// 22/12/10-bit hashed callsigns used by non-standard messages.  Lifted from
// ft8_lib's demo/decode_ft8.c.
// ===========================================================================
#define CALLSIGN_HASHTABLE_SIZE 256

static struct {
  char     callsign[12];
  uint32_t hash;
} callsign_hashtable[CALLSIGN_HASHTABLE_SIZE];
static int callsign_hashtable_size;

// The table AGES OUT, and it has to: ft8_lib saves EVERY decoded callsign here
// (unpack28 does it for standard messages too), so a busy band fills 256 slots
// in well under an hour -- and both probe loops below scan until they find an
// EMPTY slot, i.e. a full table makes them spin for ever on the decode thread.
// Decoding would stop with no error and one core pegged.  Upstream's demo has
// hashtable_cleanup() and calls it once per decode cycle; this port kept the
// "reset age" line but dropped the ageing that gives it meaning (the age is the
// top byte of `hash`, which is why the comparisons mask 0x3FFFFF).  Restored
// with upstream's shape and its max_age of 10 cycles.
#define CALLSIGN_HASH_MAX_AGE 10

static void hashtable_init(void) {
  callsign_hashtable_size = 0;
  memset(callsign_hashtable, 0, sizeof(callsign_hashtable));
}

static void hashtable_cleanup(uint8_t max_age) {
  for (int idx_hash = 0; idx_hash < CALLSIGN_HASHTABLE_SIZE; ++idx_hash) {
    if (callsign_hashtable[idx_hash].callsign[0] == '\0') continue;
    uint8_t age = (uint8_t)(callsign_hashtable[idx_hash].hash >> 24);
    if (age > max_age) {
      callsign_hashtable[idx_hash].callsign[0] = '\0';
      callsign_hashtable[idx_hash].hash = 0;
      callsign_hashtable_size--;
    } else {
      callsign_hashtable[idx_hash].hash =
        (((uint32_t)age + 1u) << 24) | (callsign_hashtable[idx_hash].hash & 0x3FFFFFu);
    }
  }
}

static void hashtable_add(const char *callsign, uint32_t hash) {
  uint16_t hash10 = (hash >> 12) & 0x3FFu;
  int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
  // Bounded probe: the ageing above is what normally keeps a slot free, but a
  // slot count is not something to leave to a policy running on another path.
  // A full table drops the callsign (the hashed-callsign message it would have
  // resolved renders as <...>) instead of hanging the decoder.
  for (int probe = 0; probe < CALLSIGN_HASHTABLE_SIZE; probe++) {
    if (callsign_hashtable[idx_hash].callsign[0] == '\0') {
      callsign_hashtable_size++;
      strncpy(callsign_hashtable[idx_hash].callsign, callsign, 11);
      callsign_hashtable[idx_hash].callsign[11] = '\0';
      callsign_hashtable[idx_hash].hash = hash;
      return;
    }
    if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) == hash) &&
        (0 == strcmp(callsign_hashtable[idx_hash].callsign, callsign))) {
      callsign_hashtable[idx_hash].hash &= 0x3FFFFFu; // reset age
      return;
    }
    idx_hash = (idx_hash + 1) % CALLSIGN_HASHTABLE_SIZE;
  }
}

static bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign) {
  uint8_t hash_shift = (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 :
                       (hash_type == FTX_CALLSIGN_HASH_12_BITS) ? 10 : 0;
  uint16_t hash10 = (hash >> (12 - hash_shift)) & 0x3FFu;
  int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
  for (int probe = 0; probe < CALLSIGN_HASHTABLE_SIZE; probe++) {
    if (callsign_hashtable[idx_hash].callsign[0] == '\0') break;
    if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) >> hash_shift) == hash) {
      strcpy(callsign, callsign_hashtable[idx_hash].callsign); // c11[12] at the call site
      return true;
    }
    idx_hash = (idx_hash + 1) % CALLSIGN_HASHTABLE_SIZE;
  }
  callsign[0] = '\0';
  return false;
}

static ftx_callsign_hash_interface_t hash_if = {
  .lookup_hash = hashtable_lookup,
  .save_hash = hashtable_add
};

// Strip the angle brackets a hashed callsign is rendered with ("<K1ABC>" ->
// "K1ABC") in place, so callsigns compare as plain strings.
static void strip_brackets(char *s) {
  int n = (int)strlen(s);
  if (n >= 2 && s[0] == '<' && s[n-1] == '>') {
    memmove(s, s + 1, n - 2);
    s[n - 2] = '\0';
  }
}

// ===========================================================================
// WSJT-X-style SNR (dB in a 2500 Hz reference bandwidth).
// ===========================================================================
// We know the transmitted tones (re-encoded from the decoded payload), so for
// each symbol we measure the power in the *actual* tone bin (signal) against the
// average of the other tone bins (noise), both via a per-symbol DFT at the
// protocol's tone spacing.  Summed over all symbols the per-symbol normalisation
// cancels, leaving a signal-to-noise ratio per tone-spacing bin; the reference
// term 10log10(2500/tone_hz) rescales it to the 2500 Hz WSJT-X band.
// The estimator is parametric so it serves both FT8 (8 tones, 6.25 Hz, 79 sym)
// and FT4 (4 tones, 20.833 Hz, 105 sym).  SNR_CAL is an empirical trim; a
// synthetic AWGN calibration (scratchpad snr_test.c / ft4_loopback.c) confirmed
// measured==target within ~0.3 dB across the operational -20..-5 dB range with
// SNR_CAL=0, so the physical reference stands on its own.  (Above ~0 dB the
// estimate compresses as the signal's own splatter inflates the off-tone noise
// bins — as WSJT-X also does, outside the range that matters for a QSO.)
#define SNR_MAX_TONES 8                                      // FT8 = 8, FT4 = 4
#define SNR_CAL       0.0f                                   // calibration offset

static float measure_snr(const float *sig, int len, float f0, float t0,
                         const uint8_t *tones, int n_sym, int n_tones,
                         float symbol_period) {
  int nsps = (int)(FT8_RATE * symbol_period);
  float tone_hz = 1.0f / symbol_period;
  float ref_db  = 10.0f * log10f(2500.0f / tone_hz);  // FT8: 26.02, FT4: 20.79
  int start0 = (int)lroundf(t0 * FT8_RATE);
  if (start0 < 0) start0 = 0;
  if (start0 + n_sym * nsps > len) return NAN;   // message runs past the buffer

  // Per-sample complex rotation for each tone bin (e^{-j w}).
  float cw[SNR_MAX_TONES], sw[SNR_MAX_TONES];
  for (int k = 0; k < n_tones; k++) {
    float w = 2.0f * (float)M_PI * (f0 + k * tone_hz) / FT8_RATE;
    cw[k] = cosf(w); sw[k] = sinf(w);
  }

  double Ssig = 0.0, Snoise = 0.0;
  for (int i = 0; i < n_sym; i++) {
    const float *seg = sig + start0 + i * nsps;
    float pw[SNR_MAX_TONES];
    for (int k = 0; k < n_tones; k++) {
      float pr = 1.0f, pi = 0.0f, ar = 0.0f, ai = 0.0f;   // running phasor / accum
      for (int n = 0; n < nsps; n++) {
        ar += seg[n] * pr;
        ai -= seg[n] * pi;
        float npr = pr * cw[k] - pi * sw[k];
        pi = pr * sw[k] + pi * cw[k];
        pr = npr;
      }
      pw[k] = ar * ar + ai * ai;
    }
    int t = tones[i] & (n_tones - 1);
    float nz = 0.0f;
    for (int k = 0; k < n_tones; k++) if (k != t) nz += pw[k];
    Ssig   += pw[t];
    Snoise += nz / (n_tones - 1);
  }
  if (Snoise <= 0.0) return NAN;
  double ratio = (Ssig - Snoise) / Snoise;        // signal / per-bin noise
  if (ratio < 1.0e-3) ratio = 1.0e-3;
  float snr = 10.0f * log10f((float)ratio) - ref_db + SNR_CAL;
  if (snr < -24.0f) snr = -24.0f;
  if (snr >  40.0f) snr =  40.0f;
  return snr;
}

// ===========================================================================
// Decode one accumulated slot buffer into the results[] list (worker thread).
// ===========================================================================
static void decode_slot(const float *sig, int len, gint64 end_us) {
  // Age the callsign table once per decode cycle, where upstream does it.
  hashtable_cleanup(CALLSIGN_HASH_MAX_AGE);

  // Where this window sits in UTC.  Everything below is derived from it: the
  // window slides on a 2 s hop and is NOT slot-aligned, so "the clock at decode
  // time" is not the slot a transmission was sent in.
  const double slot_len   = proto ? 7.5 : 15.0;
  const double tx_nominal = 0.5;          // a transmission starts 0.5 s into its slot
  const double win_end    = (double)end_us / 1.0e6;
  const double win_start  = win_end - (double)len / (double)FT8_RATE;
  const int    n_sym      = proto ? FT4_NN : FT8_NN;
  const int    nsps       = (int)(FT8_RATE * (proto ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD));

  monitor_config_t cfg = {
    .f_min = 100.0f,
    .f_max = 3000.0f,
    .sample_rate = FT8_RATE,
    .time_osr = 2,
    .freq_osr = 2,
    .protocol = proto ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8
  };

  monitor_t mon;
  monitor_init(&mon, &cfg);

  int nblocks = len / mon.block_size;
  if (nblocks > mon.wf.max_blocks) nblocks = mon.wf.max_blocks;
  for (int i = 0; i < nblocks; i++) {
    monitor_process(&mon, sig + i * mon.block_size);
  }

  const ftx_waterfall_t *wf = &mon.wf;
  ftx_candidate_t candidate_list[KMAX_CANDIDATES];
  int num_candidates = ftx_find_candidates(wf, KMAX_CANDIDATES, candidate_list, KMIN_SCORE);

  // Duplicate suppression across the candidates of THIS window (the cross-window
  // ring below is the one that spans several).
  ftx_message_t decoded[MAX_DECODES];
  ftx_message_t *decoded_hashtable[MAX_DECODES];
  for (int i = 0; i < MAX_DECODES; i++) decoded_hashtable[i] = NULL;

  FT8_DECODE local[MAX_DECODES];
  time_t     local_slot[MAX_DECODES];
  int n = 0;

  for (int idx = 0; idx < num_candidates && n < MAX_DECODES; idx++) {
    const ftx_candidate_t *cand = &candidate_list[idx];
    float freq_hz = (mon.min_bin + cand->freq_offset + (float)cand->freq_sub / wf->freq_osr) / mon.symbol_period;
    // ONE SYMBOL PERIOD is subtracted here, and it is not a fudge: the monitor's
    // analysis frame is nfft = 2 symbols long and ENDS at its block boundary, so
    // the block a symbol scores best in is the one whose window is centred on it
    // — a whole symbol after the symbol itself starts.  ft8_lib's own demo omits
    // the term because it only prints the number; here it is the anchor
    // measure_snr() cuts its per-symbol DFTs at, so being 0.16 s (FT8) / 0.048 s
    // (FT4) late made every one of them straddle two symbols and read the signal
    // bin as noise: EVERY measured SNR came out at the −24 dB clamp, and the
    // only rows that did not were the ones falling back to the Costas score.
    // Measured with tools/ft8_offline.c, which transmits at a known dt.
    float time_sec = (cand->time_offset + (float)cand->time_sub / wf->time_osr - 1.0f)
                     * mon.symbol_period;

    ftx_message_t message;
    ftx_decode_status_t status;
    if (!ftx_decode_candidate(wf, cand, KLDPC_ITERS, &message, &status)) {
      continue;
    }

    // Only a transmission lying WHOLE inside this window is published.  A
    // truncated one decodes too — ft8_lib simply skips the symbols it does not
    // have and the LDPC fills them in — but its SNR cannot be measured (the
    // fallback then reports the Costas score as decibels, which read ~14 dB
    // optimistic) and its dt is off by whatever is missing.  With a hop shorter
    // than the slack between the waveform and the window (2.36 s for FT8,
    // 2.46 s for FT4) every complete transmission lands whole in at least one
    // window, so a truncated view is never the only view.  The exception is
    // audio at the very end of a recording, which no later window can cover.
    int start0 = (int)lroundf(time_sec * FT8_RATE);
    if (start0 < 0 || start0 + n_sym * nsps > len) continue;

    // The slot a transmission belongs to comes from WHERE IT SITS IN THE AUDIO,
    // never from the clock at decode time.  The window that holds a transmission
    // whole usually ends after the NEXT slot boundary, so labelling by "now" put
    // every decode one slot late — and with it inverted the QSO engine's slot
    // parity, which answers in the opposite slot to the one it read.
    double tx_start = win_start + time_sec;
    double slot_beg = floor((tx_start - tx_nominal) / slot_len + 0.5) * slot_len;
    time_t slot_t   = (time_t)llround(slot_beg);

    // Check the per-slot duplicate hash table.
    int idx_hash = message.hash % MAX_DECODES;
    gboolean found_empty = FALSE, found_dup = FALSE;
    do {
      if (decoded_hashtable[idx_hash] == NULL) {
        found_empty = TRUE;
      } else if ((decoded_hashtable[idx_hash]->hash == message.hash) &&
                 (0 == memcmp(decoded_hashtable[idx_hash]->payload, message.payload, sizeof(message.payload)))) {
        found_dup = TRUE;
      } else {
        idx_hash = (idx_hash + 1) % MAX_DECODES;
      }
    } while (!found_empty && !found_dup);
    if (found_dup) continue;

    memcpy(&decoded[idx_hash], &message, sizeof(message));
    decoded_hashtable[idx_hash] = &decoded[idx_hash];

    char text[FTX_MAX_MESSAGE_LENGTH];
    ftx_message_offsets_t offsets;
    if (ftx_message_decode(&message, &hash_if, text, &offsets) != FTX_MESSAGE_RC_OK) {
      continue;
    }

    // ...and against every window that already published this transmission.
    if (dup_seen(message.payload, slot_t)) continue;

    struct tm tm_slot;
    gmtime_r(&slot_t, &tm_slot);
    local_slot[n] = slot_t;
    FT8_DECODE *d = &local[n++];
    snprintf(d->utc, sizeof(d->utc), "%02d%02d%02d",
             tm_slot.tm_hour, tm_slot.tm_min, tm_slot.tm_sec);
    // Measure a real WSJT-X-style SNR from the known tone sequence (FT8 8-GFSK or
    // FT4 4-GFSK).  The Costas sync-score fallback is now only reachable on a
    // degenerate window with no measurable noise: the case it was written for —
    // a message running off the end of the buffer — is dropped above, because
    // that number is not a signal report and read ~14 dB optimistic.
    float snr;
    if (proto) {
      uint8_t tones[FT4_NN];
      ft4_encode(message.payload, tones);
      snr = measure_snr(sig, len, freq_hz, time_sec, tones, FT4_NN, 4, FT4_SYMBOL_PERIOD);
    } else {
      uint8_t tones[FT8_NN];
      ft8_encode(message.payload, tones);
      snr = measure_snr(sig, len, freq_hz, time_sec, tones, FT8_NN, 8, FT8_SYMBOL_PERIOD);
    }
    d->snr = isnan(snr) ? (cand->score * 0.5f - 20.0f) : snr;
    d->dt = (float)(tx_start - tx_nominal - slot_beg);   // vs the slot's nominal TX start
    d->freq = freq_hz;
    snprintf(d->text, sizeof(d->text), "%s", text);

    // Structured fields for the QSO engine: split into recipient / sender /
    // extra.  Dispatch on the message type — the non-standard-call parser
    // (Type 4: /P, /R, compound and hashed calls) and the standard parser
    // interpret the same bits differently, so picking by type avoids one
    // "succeeding" with garbage.  Blank for free-text/telemetry/etc.
    d->call_to[0] = d->call_de[0] = d->extra[0] = '\0';
    char to[FTX_MAX_MESSAGE_LENGTH], de[FTX_MAX_MESSAGE_LENGTH], ex[FTX_MAX_MESSAGE_LENGTH];
    ftx_field_t ftypes[FTX_MAX_MESSAGE_FIELDS];
    ftx_message_type_t mtype = ftx_message_get_type(&message);
    ftx_message_rc_t frc = FTX_MESSAGE_RC_ERROR_TYPE;
    if (mtype == FTX_MESSAGE_TYPE_NONSTD_CALL)
      frc = ftx_message_decode_nonstd(&message, &hash_if, to, de, ex, ftypes);
    else if (mtype == FTX_MESSAGE_TYPE_STANDARD || mtype == FTX_MESSAGE_TYPE_EU_VHF)
      frc = ftx_message_decode_std(&message, &hash_if, to, de, ex, ftypes);
    if (frc == FTX_MESSAGE_RC_OK) {
      // Drop the <> that mark a hashed callsign so the QSO engine can string-
      // compare against the plain callsign it knows.
      strip_brackets(to);
      strip_brackets(de);
      snprintf(d->call_to, sizeof(d->call_to), "%s", to);
      snprintf(d->call_de, sizeof(d->call_de), "%s", de);
      snprintf(d->extra, sizeof(d->extra), "%s", ex);
    }
  }

  monitor_free(&mon);

  // Publish what is new, grouped by the slot each decode belongs to.  A window
  // holds one slot's transmissions in practice (two whole waveforms do not fit),
  // but the grouping does not assume it.  Nothing is published for a window that
  // decoded nothing new: overlapping windows land on the quiet stretches between
  // transmissions, and the readout has to hold the last decodes rather than
  // blank every 2 s.
  for (int i = 0; i < n; ) {
    int j = i;
    while (j < n && local_slot[j] == local_slot[i]) j++;
    time_t slot_t = local_slot[i];
    int added = 0;

    g_mutex_lock(&list_mutex);
    if (!have_slot || slot_t > result_slot) {   // a newer slot replaces the list
      result_count = 0;
      result_slot  = slot_t;
      have_slot    = TRUE;
      snprintf(result_utc, sizeof(result_utc), "%s", local[i].utc);
    }
    if (slot_t == result_slot) {                // same slot: append to it
      for (int k = i; k < j && result_count < MAX_DECODES; k++) {
        results[result_count++] = local[k];
        added++;
      }
    }
    g_mutex_unlock(&list_mutex);

    if (added > 0) {
      log_info("ft8: %s decoded %d messages (%d candidates)\n",
              local[i].utc, added, num_candidates);
      for (int k = i; k < i + added; k++) {
        log_info("  %s  %+3.0f dB  %4.0f Hz  %s\n",
                local[k].utc, local[k].snr, local[k].freq, local[k].text);
      }
      // Feed the decoded spots to the PSK Reporter network (no-op unless enabled
      // and the station call/grid are set).  Only the new ones: before the
      // cross-window duplicate check every station was spotted two or three
      // times per slot.
      ft8_pskreporter_report(&local[i], added, slot_t);
    }
    i = j;
  }
}

// A slot needs enough audio to hold at least the full waveform (FT8 ~12.6 s,
// FT4 ~5 s); below that a decode is pointless.  Guards against tiny partial slots
// at start-up.
static int mon_min_samples(void) {
  return proto ? FT8_RATE * 5 : FT8_RATE * 13;
}

// ---- worker thread ---------------------------------------------------------
static gpointer ft8_worker(gpointer data) {
  g_mutex_lock(&work_mutex);
  while (running) {
    while (running && !work_ready) {
      g_cond_wait(&work_cond, &work_mutex);
    }
    if (!running) break;
    int    len = work_len;
    gint64 end = work_end_us;
    // Decode with the lock released so the RX thread can keep filling.
    g_mutex_unlock(&work_mutex);

    if (len > mon_min_samples()) {
      decode_slot(work_buf, len, end);
    }

    g_mutex_lock(&work_mutex);
    work_ready = FALSE;
    g_cond_signal(&done_cond);
  }
  g_mutex_unlock(&work_mutex);
  return NULL;
}

// ===========================================================================
// Public API
// ===========================================================================
void ft8_decoder_init(void) {
  g_mutex_init(&work_mutex);
  g_cond_init(&work_cond);
  g_cond_init(&done_cond);
  g_mutex_init(&list_mutex);
  hashtable_init();
  running = TRUE;
  worker = g_thread_new("ft8-decode", ft8_worker, NULL);
}

void ft8_decoder_set_enabled(gboolean en) {
  if (en && !enabled) {
    // Fresh start: clear the decimation phase, the ring, and everything that
    // remembers what has already been decoded and published.
    dec_count = 0;
    ring_w = 0;
    last_decode_w = 0;
    memset(dup_ring, 0, sizeof(dup_ring));
    dup_w = 0;
    dup_hits = 0;
    g_mutex_lock(&list_mutex);
    result_count = 0;
    result_utc[0] = '\0';
    have_slot = FALSE;
    g_mutex_unlock(&list_mutex);
  }
  enabled = en;
}

// ---- harness seams (tools/ft8_offline.c) -----------------------------------
void ft8_decoder_set_clock(gint64 (*fn)(void)) {
  clock_fn = fn;
}

guint ft8_decoder_dup_count(void) {
  return dup_hits;
}

void ft8_decoder_sync(void) {
  g_mutex_lock(&work_mutex);
  while (running && work_ready) g_cond_wait(&done_cond, &work_mutex);
  g_mutex_unlock(&work_mutex);
}

gboolean ft8_decoder_is_enabled(void) {
  return enabled;
}

void ft8_decoder_set_protocol(int ft4) {
  proto = ft4 ? 1 : 0;
}

void ft8_decoder_add_audio(const gdouble *samples, int nframes) {
  if (!enabled) return;

  // Decimate the left channel 48k -> 12k into the rolling ring (take every 4th).
  for (int i = 0; i < nframes; i++) {
    if (++dec_count >= FT8_DECIM) {
      dec_count = 0;
      ring[ring_w % RING_CAP] = (float)samples[i * 2];   // left channel
      ring_w++;
    }
  }

  // Every decode_hop() samples, hand the most recent slot-length window to the
  // worker.  Both lengths are protocol-dependent (FT4 is half of FT8).
  int ss = slot_samples();
  if (ring_w >= ss && (ring_w - last_decode_w) >= decode_hop()) {
    g_mutex_lock(&work_mutex);
    if (!work_ready) {               // skip if the worker is still busy
      long start = ring_w - ss;
      for (int i = 0; i < ss; i++) {
        work_buf[i] = ring[(start + i) % RING_CAP];
      }
      work_len = ss;
      // Stamp the window with the UTC time of its LAST sample; decode_slot()
      // works every decode's slot and dt back from there.  It is "now" plus the
      // receive chain's own latency (~0.1-0.2 s of WDSP ring and audio path),
      // which biases dt late by that much — the same bias WSJT-X carries, and
      // three orders below the slot rounding.
      work_end_us = now_us();
      work_ready = TRUE;
      g_cond_signal(&work_cond);
      last_decode_w = ring_w;
    }
    g_mutex_unlock(&work_mutex);
  }
}

int ft8_decoder_get_decodes(FT8_DECODE *out, int max, char *utc7) {
  g_mutex_lock(&list_mutex);
  int n = result_count;
  if (n > max) n = max;
  memcpy(out, results, n * sizeof(FT8_DECODE));
  if (utc7 != NULL) {
    memcpy(utc7, result_utc, sizeof(result_utc));
  }
  g_mutex_unlock(&list_mutex);
  return n;
}

// ---- live spectrum for the FT8 waterfall -----------------------------------
// A windowed real FFT of the newest WF_FFT samples of the 12 kHz ring.  At 12
// kHz a 4096-point FFT gives ~2.93 Hz/bin — fine enough to separate individual
// FT8 tones (6.25 Hz apart), JTDX-like.  Called on the GTK thread ~15x/s; the
// ring is written by the RX thread, but an occasional torn sample only smears a
// single waterfall pixel, so no lock is taken.
#define WF_FFT 4096

int ft8_decoder_get_spectrum(float *out, int max_bins, float *hz_per_bin) {
  static kiss_fftr_cfg cfg = NULL;
  static float          window[WF_FFT];
  static kiss_fft_scalar tin[WF_FFT];
  static kiss_fft_cpx    fout[WF_FFT/2 + 1];

  if (ring_w < WF_FFT) return 0;                 // not enough audio buffered yet
  if (cfg == NULL) {
    cfg = kiss_fftr_alloc(WF_FFT, 0, NULL, NULL);
    for (int i = 0; i < WF_FFT; i++)
      window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (WF_FFT - 1)));
  }
  if (cfg == NULL) return 0;

  long start = ring_w - WF_FFT;
  for (int i = 0; i < WF_FFT; i++)
    tin[i] = ring[(start + i) % RING_CAP] * window[i];
  kiss_fftr(cfg, tin, fout);

  float hpb = (float)FT8_RATE / (float)WF_FFT;
  if (hz_per_bin != NULL) *hz_per_bin = hpb;

  int nb = (int)(5000.0f / hpb);                 // up to 5 kHz; caller shows a sub-span
  if (nb > max_bins)   nb = max_bins;
  if (nb > WF_FFT / 2) nb = WF_FFT / 2;
  for (int i = 0; i < nb; i++) {
    float re = fout[i].r, im = fout[i].i;
    out[i] = 10.0f * log10f(re * re + im * im + 1.0e-12f);
  }
  return nb;
}
