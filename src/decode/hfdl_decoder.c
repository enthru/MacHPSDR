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
 * HFDL receive decoder — orchestrator. See hfdl_decoder.h for the design.
 *
 * Phase 2a: owns the front-end DSP (hfdl_demod.c) that conditions the raw off-air
 * I/Q into the HFDL symbol domain, and publishes a signal-level / symbol-throughput
 * status for the panel. The demod CORE (symbol/carrier recovery, equalizer, M-PSK
 * modem, preamble→framing→FEC) is the next phase.
 *
 * Threading: hfdl_decoder_set_enabled() and hfdl_decoder_add_iq() both run on the
 * RX audio thread (from the tap in receiver.c:full_rx_buffer), so the front-end
 * (hfdl_demod, not thread-safe) is created/fed/destroyed entirely there — no lock
 * needed around it. Only the published status trio and the message ring are
 * touched cross-thread (the GTK panel poll), guarded by a single GMutex (mirrors
 * cw_decoder.c). The enable flag is an atomic so the tap's fast-path is lock-free.
 */

#include <glib.h>
#include <string.h>
#include <liquid/liquid.h>

#include "hfdl_decoder.h"
#include "hfdl_demod.h"
#include "hfdl_fec.h"
#include "log.h"

// Scratch for one block of conditioned baseband output (interleaved float I/Q).
// Real off-air rates (≥48 kHz) decimate to far fewer than this per block; the
// front-end caps its write at HFDL_OUT_MAX so this can never overflow.
#define HFDL_OUT_MAX 8192
// Recovered symbols per block ≤ baseband/SPS; the same cap is more than enough.
#define HFDL_SYM_MAX 8192

// --- state (all audio-thread-owned unless noted) ---------------------------

static volatile gint enabled = 0;          // atomic on/off gate
static hfdl_demod   *demod = NULL;          // front-end DSP (audio thread only)
static double        demod_rate = 0.0;      // input rate demod was built for
static float         out_buf[2 * HFDL_OUT_MAX];   // conditioned baseband
static float         sym_buf[2 * HFDL_SYM_MAX];   // recovered symbols

static GMutex        lock;                  // guards the published fields below
static GString      *pending = NULL;        // decoded text awaiting the panel drain
static gboolean      reset_pending = FALSE; // requested from GTK, applied on feed
static int           status_rate = 0;       // last off-air sample rate seen (Hz)
static glong         status_syms = 0;       // symbol-domain samples since reset
static double        status_level = -160.0; // AGC RSSI (dB)
static gboolean      status_fed = FALSE;    // fed at least one block since enable

static gboolean      echo = FALSE;          // MACHPSDR_HFDL_ECHO -> stderr
static gboolean      env_read = FALSE;

static void ensure_init(void) {
  if (pending == NULL) pending = g_string_new(NULL);
  if (!env_read) {
    echo = (g_getenv("MACHPSDR_HFDL_ECHO") != NULL);
    env_read = TRUE;
  }
}

// Audio thread. Free the front-end (on disable / rate change / re-enable reset).
static void demod_free(void) {
  if (demod) { hfdl_demod_destroy(demod); demod = NULL; }
  demod_rate = 0.0;
}

// --- API -------------------------------------------------------------------

void hfdl_decoder_set_enabled(gboolean on) {
  gint was = g_atomic_int_get(&enabled);
  if (on && !was) {
    // off -> on: fresh start.
    g_mutex_lock(&lock);
    ensure_init();
    g_string_truncate(pending, 0);
    status_syms = 0;
    status_rate = 0;
    status_level = -160.0;
    status_fed = FALSE;
    reset_pending = FALSE;
    gboolean do_echo = echo;
    g_mutex_unlock(&lock);
    demod_free();   // rebuilt lazily on the first feed at the live rate
    log_info("hfdl: decoder enabled (liquid-dsp %s) — front-end (phase 2a), no framing yet\n",
             liquid_libversion());
    if (do_echo) {
      g_printerr("[HFDL] enabled (liquid-dsp %s)\n", liquid_libversion());
      if (g_getenv("MACHPSDR_HFDL_SELFTEST")) {
        g_printerr("[HFDL] demod selftest: %s\n", hfdl_demod_selftest() ? "PASS" : "FAIL");
        g_printerr("[HFDL] fec selftest:   %s\n", hfdl_fec_selftest() ? "PASS" : "FAIL");
      }
    }
  } else if (!on && was) {
    demod_free();
    log_info("hfdl: decoder disabled\n");
    g_mutex_lock(&lock);
    if (echo) g_printerr("[HFDL] disabled\n");
    g_mutex_unlock(&lock);
  }
  g_atomic_int_set(&enabled, on ? 1 : 0);
}

void hfdl_decoder_add_iq(const double *iq, int nframes, int sample_rate) {
  if (!g_atomic_int_get(&enabled) || iq == NULL || nframes <= 0 || sample_rate <= 0)
    return;

  // (Re)build the front-end if the rate changed (audio thread — safe).
  if (demod == NULL || demod_rate != (double)sample_rate) {
    demod_free();
    demod = hfdl_demod_create((double)sample_rate);
    demod_rate = (double)sample_rate;
  }
  int nsym = 0;
  double level = -160.0;
  if (demod) {
    // Phase 2a front-end: condition the raw I/Q into the 5400 S/s symbol domain.
    int nbb = hfdl_demod_process(demod, iq, nframes, out_buf, HFDL_OUT_MAX);
    level = hfdl_demod_level_db(demod);
    // Phase 2b: recover carrier-locked BPSK symbols (symsync + Costas + modem).
    nsym = hfdl_demod_symbols(demod, out_buf, nbb, sym_buf, HFDL_SYM_MAX);
    // The recovered symbols in sym_buf are not yet framed: preamble correlation,
    // the LMS equalizer, adaptive M-PSK selection and framing→FEC→message text
    // are the next phase (only meaningfully verifiable against a real recording).
  }

  g_mutex_lock(&lock);
  ensure_init();
  if (reset_pending) {
    g_string_truncate(pending, 0);
    status_syms = 0;
    reset_pending = FALSE;
  }
  status_rate = sample_rate;
  status_syms += nsym;
  status_level = level;
  status_fed = TRUE;
  glong syms = status_syms;
  gboolean do_echo = echo;
  g_mutex_unlock(&lock);

  if (do_echo && nsym > 0 && (syms - nsym) / 20000 != syms / 20000)
    g_printerr("[HFDL] %ld symbols recovered @ %.0f dB (no framing yet)\n",
               syms, level);
}

int hfdl_decoder_get_messages(char *buf, int buflen) {
  if (buf == NULL || buflen <= 0) return 0;
  g_mutex_lock(&lock);
  ensure_init();
  int n = (int)MIN((gsize)(buflen - 1), pending->len);
  if (n > 0) {
    memcpy(buf, pending->str, n);
    g_string_erase(pending, 0, n);
  }
  buf[n] = '\0';
  g_mutex_unlock(&lock);
  return n;
}

void hfdl_decoder_get_status(gboolean *listening, int *sample_rate, glong *blocks) {
  g_mutex_lock(&lock);
  ensure_init();
  if (listening)   *listening   = g_atomic_int_get(&enabled) && status_fed;
  if (sample_rate) *sample_rate = status_rate;
  if (blocks)      *blocks      = status_syms;   // recovered symbols since reset
  g_mutex_unlock(&lock);
}

double hfdl_decoder_get_level_db(void) {
  g_mutex_lock(&lock);
  double v = status_level;
  g_mutex_unlock(&lock);
  return v;
}

void hfdl_decoder_reset(void) {
  g_mutex_lock(&lock);
  ensure_init();
  g_string_truncate(pending, 0);
  reset_pending = TRUE;   // zero the counters on the next audio-thread feed
  status_fed = FALSE;
  g_mutex_unlock(&lock);
}
