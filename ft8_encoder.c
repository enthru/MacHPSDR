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
#include <gtk/gtk.h>

#include <ft8/message.h>
#include <ft8/encode.h>
#include <ft8/constants.h>

// Prerequisite types for radio.h (mirrors the include order used elsewhere).
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"       // RADIO, set_mox()
#include "main.h"        // global RADIO *radio
#include "ft8_encoder.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- FT8 waveform constants -----------------------------------------------
#define FT8_TX_RATE     48000                 // mic / TX exchange rate (Hz)
#define FT8_TX_SPS      7680                  // samples per symbol (0.5 + 48000*0.16)
#define FT8_TX_SAMPLES  (FT8_NN * FT8_TX_SPS) // full waveform length (606720, ~12.64 s)
#define GFSK_BT         2.0f                  // Gaussian filter bandwidth-time product
#define GFSK_CONST_K    5.336446f             // pi * sqrt(2 / ln(2))
#define FT8_TX_AMPL     0.9f                  // waveform peak (leaves TX headroom)
#define FT8_SLOT_SEC    15                    // FT8 time slot (s)

// ---- synthesized waveform (GTK thread writes, audio thread reads) ----------
static float            wave[FT8_TX_SAMPLES];
static volatile int     wave_len = 0;
static volatile gboolean have_wave = FALSE;

// ---- transmit / scheduler state --------------------------------------------
static volatile gboolean tx_active = FALSE;   // clocking the waveform out now
static volatile long     tx_idx = 0;          // next sample index (audio thread)
static gboolean          armed = FALSE;       // waiting for a slot boundary
static gboolean          arm_even = FALSE;    // desired slot parity
static long              last_started_slot = -1; // slot we last keyed up in
static gboolean          we_keyed = FALSE;    // did we raise MOX (so we drop it)
static guint             tick_id = 0;         // scheduler g_timeout id
static time_t            key_time = 0;        // when WE last raised MOX (watchdog)

// Hard safety cap on how long our keying may hold MOX.  The GFSK waveform is
// ~12.64 s; if the TX path never clocks it out (e.g. MOX didn't actually engage,
// or a half-duplex/hardware stall), tx_idx never reaches wave_len and MOX would
// stick on forever.  This forces key-down regardless.
#define FT8_TX_MAX_SEC   14

// ===========================================================================
// Callsign hash table — ftx_message_encode() needs it to hash non-standard
// callsigns.  Same scheme as ft8_decoder.c / ft8_lib's demo decoder.
// ===========================================================================
#define CALLSIGN_HASHTABLE_SIZE 256

static struct {
  char     callsign[12];
  uint32_t hash;
} callsign_hashtable[CALLSIGN_HASHTABLE_SIZE];

static void hashtable_add(const char *callsign, uint32_t hash) {
  uint16_t hash10 = (hash >> 12) & 0x3FFu;
  int idx = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
  while (callsign_hashtable[idx].callsign[0] != '\0') {
    if (((callsign_hashtable[idx].hash & 0x3FFFFFu) == hash) &&
        (0 == strcmp(callsign_hashtable[idx].callsign, callsign))) {
      callsign_hashtable[idx].hash &= 0x3FFFFFu;
      return;
    }
    idx = (idx + 1) % CALLSIGN_HASHTABLE_SIZE;
  }
  strncpy(callsign_hashtable[idx].callsign, callsign, 11);
  callsign_hashtable[idx].callsign[11] = '\0';
  callsign_hashtable[idx].hash = hash;
}

static bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign) {
  uint8_t shift = (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 :
                  (hash_type == FTX_CALLSIGN_HASH_12_BITS) ? 10 : 0;
  uint16_t hash10 = (hash >> (12 - shift)) & 0x3FFu;
  int idx = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
  while (callsign_hashtable[idx].callsign[0] != '\0') {
    if (((callsign_hashtable[idx].hash & 0x3FFFFFu) >> shift) == hash) {
      strcpy(callsign, callsign_hashtable[idx].callsign);
      return true;
    }
    idx = (idx + 1) % CALLSIGN_HASHTABLE_SIZE;
  }
  callsign[0] = '\0';
  return false;
}

static ftx_callsign_hash_interface_t hash_if = {
  .lookup_hash = hashtable_lookup,
  .save_hash = hashtable_add
};

// ===========================================================================
// GFSK waveform synthesis (ported from ft8_lib's reference gen_ft8).
// ===========================================================================
static void gfsk_pulse(int n_spsym, float bt, float *pulse) {
  for (int i = 0; i < 3 * n_spsym; i++) {
    float t = i / (float)n_spsym - 1.5f;
    float a1 = GFSK_CONST_K * bt * (t + 0.5f);
    float a2 = GFSK_CONST_K * bt * (t - 0.5f);
    pulse[i] = (erff(a1) - erff(a2)) / 2.0f;
  }
}

static void synth_gfsk(const uint8_t *sym, int n_sym, float f0) {
  const int nsps = FT8_TX_SPS;
  const int n_wave = n_sym * nsps;
  const float dphi_peak = 2.0f * (float)M_PI / nsps;   // hmod = 1

  float *dphi = g_malloc0(sizeof(float) * (n_wave + 2 * nsps));
  float *pulse = g_malloc(sizeof(float) * 3 * nsps);
  gfsk_pulse(nsps, GFSK_BT, pulse);

  // Baseline carrier phase increment for the audio offset f0.
  for (int i = 0; i < n_wave + 2 * nsps; i++) {
    dphi[i] = 2.0f * (float)M_PI * f0 / FT8_TX_RATE;
  }
  // Overlay each symbol's smoothed frequency contribution.
  for (int i = 0; i < n_sym; i++) {
    int ib = i * nsps;
    for (int j = 0; j < 3 * nsps; j++) {
      dphi[j + ib] += dphi_peak * sym[i] * pulse[j];
    }
  }
  // Extend the first and last symbols to seed/settle the filter.
  for (int j = 0; j < 2 * nsps; j++) {
    dphi[j] += dphi_peak * pulse[nsps + j] * sym[0];
    dphi[j + n_sym * nsps] += dphi_peak * pulse[j] * sym[n_sym - 1];
  }

  float phi = 0.0f;
  for (int k = 0; k < n_wave; k++) {
    wave[k] = FT8_TX_AMPL * sinf(phi);
    phi = fmodf(phi + dphi[k + nsps], 2.0f * (float)M_PI);
  }
  // Ramp the leading/trailing edges to suppress key clicks.
  int n_ramp = nsps / 8;
  for (int i = 0; i < n_ramp; i++) {
    float env = (1.0f - cosf(2.0f * (float)M_PI * i / (2 * n_ramp))) / 2.0f;
    wave[i] *= env;
    wave[n_wave - 1 - i] *= env;
  }

  wave_len = n_wave;
  g_free(dphi);
  g_free(pulse);
}

// ===========================================================================
// Slot scheduler (GTK main thread, ~100 ms tick).
// ===========================================================================
static gboolean tx_tick(gpointer data) {
  if (!armed && !tx_active) { tick_id = 0; return G_SOURCE_REMOVE; }

  time_t now = time(NULL);
  long slot = now / FT8_SLOT_SEC;
  int  in_slot = (int)(now % FT8_SLOT_SEC);

  if (tx_active) {
    // Safety watchdog: never let our keying hold MOX past the waveform length +
    // margin, even if the TX path stops pulling samples (tx_idx would stall).
    if (we_keyed && key_time && (now - key_time) > FT8_TX_MAX_SEC) {
      fprintf(stderr, "ft8-tx: WATCHDOG — MOX held >%ds (tx_idx=%ld/%d), forcing key-down\n",
              FT8_TX_MAX_SEC, tx_idx, wave_len);
      tx_active = FALSE;
      armed = FALSE;
      we_keyed = FALSE;
      set_mox(radio, FALSE);
      tick_id = 0;
      return G_SOURCE_REMOVE;
    }
    // Key down once the whole waveform has been clocked into the TX chain.
    if (tx_idx >= wave_len) {
      tx_active = FALSE;
      armed = FALSE;
      if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
      tick_id = 0;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  // Armed and waiting: key up at the top of the next matching-parity slot.
  if (have_wave && in_slot < 2 && slot != last_started_slot &&
      ((slot % 2) == 0) == arm_even) {
    last_started_slot = slot;
    tx_idx = 0;
    tx_active = TRUE;
    fprintf(stderr, "ft8-tx: slot boundary reached, keying up (in_slot=%d even=%d mox=%d)\n",
            in_slot, arm_even, radio->mox);
    key_time = now;                     // arm the MOX safety watchdog
    if (!radio->mox) { we_keyed = TRUE; set_mox(radio, TRUE); }
  }
  return G_SOURCE_CONTINUE;
}

// ===========================================================================
// Public API
// ===========================================================================
gboolean ft8_tx_prepare(const char *text, float offset_hz) {
  ftx_message_t msg;
  ftx_message_init(&msg);
  if (ftx_message_encode(&msg, &hash_if, text) != FTX_MESSAGE_RC_OK) {
    have_wave = FALSE;
    return FALSE;
  }
  uint8_t tones[FT8_NN];
  ft8_encode(msg.payload, tones);
  synth_gfsk(tones, FT8_NN, offset_hz);
  have_wave = TRUE;
  return TRUE;
}

void ft8_tx_arm(gboolean tx_even) {
  if (!have_wave) {                       // nothing valid to send (e.g. encode failed)
    fprintf(stderr, "ft8-tx: arm ignored — no valid waveform (encode failed?)\n");
    return;
  }
  fprintf(stderr, "ft8-tx: armed for %s slots, waiting for boundary\n",
          tx_even ? "even" : "odd");
  arm_even = tx_even;
  // Anchor on the current slot so we never start mid-slot: fire only when the
  // clock advances into a new slot of the desired parity.
  last_started_slot = time(NULL) / FT8_SLOT_SEC;
  armed = TRUE;
  if (tick_id == 0) {
    tick_id = g_timeout_add(100, tx_tick, NULL);
  }
}

void ft8_tx_disarm(void) {
  armed = FALSE;
  tx_active = FALSE;
  if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
  // tx_tick removes itself on the next fire now that armed/tx_active are clear.
}

gboolean ft8_tx_active(void) {
  return tx_active;
}

float ft8_tx_next_sample(void) {
  if (!tx_active) return 0.0f;
  long i = tx_idx;
  if (i >= wave_len) return 0.0f;
  tx_idx = i + 1;
  return wave[i];
}
