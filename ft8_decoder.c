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
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#include <ft8/decode.h>
#include <ft8/message.h>
#include <common/monitor.h>

#include "ft8_decoder.h"

// ---- FT8 timing / audio constants -----------------------------------------
#define FT8_RATE        12000            // decoder sample rate (Hz)
#define FT8_DECIM       4                // 48000 / 12000
#define FT8_SLOT_SEC    15               // FT8 time slot (s)
#define SLOT_SAMPLES    (FT8_RATE * FT8_SLOT_SEC)         // decode window (180000)
#define RING_CAP        (FT8_RATE * (FT8_SLOT_SEC + 1))   // ring: one window + margin
// Decode the most recent window this often.  The Costas search only tolerates a
// transmission starting within the first ~3 s of the window, so a recorded/looped
// I/Q file (not aligned to real UTC) — and, defensively, a slightly-off system
// clock — need overlapping windows rather than a single hard UTC-slot cut.  A hop
// shorter than that tolerance guarantees every complete transmission in the ring
// lands decodably in at least one window.
#define DECODE_HOP      (FT8_RATE * 2)                    // ~2 s between decodes
#define MAX_DECODES     64               // per-window cap we surface to the UI

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
static gboolean work_ready = FALSE;
static gboolean running = FALSE;
static float    work_buf[SLOT_SAMPLES];
static int      work_len = 0;
static time_t   work_slot_time = 0;       // UTC time the window ended
static GThread *worker = NULL;

// ---- decode result list (worker writes, UI reads) --------------------------
static GMutex      list_mutex;
static FT8_DECODE  results[MAX_DECODES];
static int         result_count = 0;
static char        result_utc[8] = "";

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

static void hashtable_init(void) {
  callsign_hashtable_size = 0;
  memset(callsign_hashtable, 0, sizeof(callsign_hashtable));
}

static void hashtable_add(const char *callsign, uint32_t hash) {
  uint16_t hash10 = (hash >> 12) & 0x3FFu;
  int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
  while (callsign_hashtable[idx_hash].callsign[0] != '\0') {
    if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) == hash) &&
        (0 == strcmp(callsign_hashtable[idx_hash].callsign, callsign))) {
      callsign_hashtable[idx_hash].hash &= 0x3FFFFFu; // reset age
      return;
    }
    idx_hash = (idx_hash + 1) % CALLSIGN_HASHTABLE_SIZE;
  }
  callsign_hashtable_size++;
  strncpy(callsign_hashtable[idx_hash].callsign, callsign, 11);
  callsign_hashtable[idx_hash].callsign[11] = '\0';
  callsign_hashtable[idx_hash].hash = hash;
}

static bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign) {
  uint8_t hash_shift = (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 :
                       (hash_type == FTX_CALLSIGN_HASH_12_BITS) ? 10 : 0;
  uint16_t hash10 = (hash >> (12 - hash_shift)) & 0x3FFu;
  int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
  while (callsign_hashtable[idx_hash].callsign[0] != '\0') {
    if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) >> hash_shift) == hash) {
      strcpy(callsign, callsign_hashtable[idx_hash].callsign);
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
// Decode one accumulated slot buffer into the results[] list (worker thread).
// ===========================================================================
static void decode_slot(const float *sig, int len, time_t slot_start) {
  monitor_config_t cfg = {
    .f_min = 100.0f,
    .f_max = 3000.0f,
    .sample_rate = FT8_RATE,
    .time_osr = 2,
    .freq_osr = 2,
    .protocol = FTX_PROTOCOL_FT8
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

  // Duplicate suppression across candidates within this slot.
  ftx_message_t decoded[MAX_DECODES];
  ftx_message_t *decoded_hashtable[MAX_DECODES];
  for (int i = 0; i < MAX_DECODES; i++) decoded_hashtable[i] = NULL;

  FT8_DECODE local[MAX_DECODES];
  int n = 0;

  struct tm tm_slot;
  gmtime_r(&slot_start, &tm_slot);

  for (int idx = 0; idx < num_candidates && n < MAX_DECODES; idx++) {
    const ftx_candidate_t *cand = &candidate_list[idx];
    float freq_hz = (mon.min_bin + cand->freq_offset + (float)cand->freq_sub / wf->freq_osr) / mon.symbol_period;
    float time_sec = (cand->time_offset + (float)cand->time_sub / wf->time_osr) * mon.symbol_period;

    ftx_message_t message;
    ftx_decode_status_t status;
    if (!ftx_decode_candidate(wf, cand, KLDPC_ITERS, &message, &status)) {
      continue;
    }

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

    FT8_DECODE *d = &local[n++];
    snprintf(d->utc, sizeof(d->utc), "%02d%02d%02d",
             tm_slot.tm_hour, tm_slot.tm_min, tm_slot.tm_sec);
    // Approximate report from the Costas sync score (not a true WSJT-X SNR).
    d->snr = cand->score * 0.5f - 20.0f;
    d->dt = time_sec - 0.5f;   // reference the slot's nominal TX start (~0.5 s)
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

  // Publish under the list mutex — but only when this window actually decoded
  // something.  Overlapping windows land on quiet stretches between FT8 slots;
  // keeping the previous non-empty list there means the readout holds the last
  // decodes until the next batch arrives instead of blanking every ~2 s.
  if (n > 0) {
    g_mutex_lock(&list_mutex);
    result_count = n;
    memcpy(results, local, n * sizeof(FT8_DECODE));
    snprintf(result_utc, sizeof(result_utc), "%02d%02d%02d",
             tm_slot.tm_hour, tm_slot.tm_min, tm_slot.tm_sec);
    g_mutex_unlock(&list_mutex);
  }

  if (n > 0) {
    fprintf(stderr, "ft8: %s decoded %d messages (%d candidates)\n",
            result_utc, n, num_candidates);
    for (int i = 0; i < n; i++) {
      fprintf(stderr, "  %s  %+3.0f dB  %4.0f Hz  %s\n",
              local[i].utc, local[i].snr, local[i].freq, local[i].text);
    }
  }
}

// A slot needs enough audio to hold at least the FT8 waveform (~12.6 s); below
// that a decode is pointless.  Guards against tiny partial slots at start-up.
static int mon_min_samples(void) {
  return FT8_RATE * 13;
}

// ---- worker thread ---------------------------------------------------------
static gpointer ft8_worker(gpointer data) {
  g_mutex_lock(&work_mutex);
  while (running) {
    while (running && !work_ready) {
      g_cond_wait(&work_cond, &work_mutex);
    }
    if (!running) break;
    int len = work_len;
    time_t st = work_slot_time;
    // Decode with the lock released so the RX thread can keep filling.
    g_mutex_unlock(&work_mutex);

    if (len > mon_min_samples()) {
      decode_slot(work_buf, len, st);
    }

    g_mutex_lock(&work_mutex);
    work_ready = FALSE;
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
  g_mutex_init(&list_mutex);
  hashtable_init();
  running = TRUE;
  worker = g_thread_new("ft8-decode", ft8_worker, NULL);
}

void ft8_decoder_set_enabled(gboolean en) {
  if (en && !enabled) {
    // Fresh start: clear the decimation phase and the ring.
    dec_count = 0;
    ring_w = 0;
    last_decode_w = 0;
  }
  enabled = en;
}

gboolean ft8_decoder_is_enabled(void) {
  return enabled;
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

  // Every DECODE_HOP samples, hand the most recent 15 s window to the worker.
  if (ring_w >= SLOT_SAMPLES && (ring_w - last_decode_w) >= DECODE_HOP) {
    g_mutex_lock(&work_mutex);
    if (!work_ready) {               // skip if the worker is still busy
      long start = ring_w - SLOT_SAMPLES;
      for (int i = 0; i < SLOT_SAMPLES; i++) {
        work_buf[i] = ring[(start + i) % RING_CAP];
      }
      work_len = SLOT_SAMPLES;
      // Label with the 15 s slot boundary nearest the window end (cosmetic).
      work_slot_time = (time(NULL) / FT8_SLOT_SEC) * FT8_SLOT_SEC;
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
