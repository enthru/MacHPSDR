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
 * HFDL receive decoder — phase-1 scaffold. See hfdl_decoder.h for the design.
 *
 * Threading: hfdl_decoder_add_iq() runs on the RX audio thread; the panel drains
 * text + reads status from the GTK thread. A single GMutex guards the pending
 * message ring and the published status trio (same model as cw_decoder.c). The
 * enable flag is an atomic so the tap's fast-path check is lock-free.
 */

#include <glib.h>
#include <string.h>
#include <liquid/liquid.h>

#include "hfdl_decoder.h"
#include "log.h"

// --- state -----------------------------------------------------------------

static volatile gint enabled = 0;          // atomic on/off gate
static gboolean      reset_pending = FALSE; // requested from GTK, applied on feed

static GMutex        lock;                  // guards everything below
static GString      *pending = NULL;        // decoded text awaiting the panel drain
static int           status_rate = 0;       // last off-air sample rate seen (Hz)
static glong         status_blocks = 0;     // I/Q blocks fed since reset
static gboolean      status_fed = FALSE;    // fed at least one block since enable

static gboolean      echo = FALSE;          // MACHPSDR_HFDL_ECHO -> stderr
static gboolean      echo_read = FALSE;

static void ensure_init(void) {
  if (pending == NULL) pending = g_string_new(NULL);
  if (!echo_read) { echo = (g_getenv("MACHPSDR_HFDL_ECHO") != NULL); echo_read = TRUE; }
}

// --- API -------------------------------------------------------------------

void hfdl_decoder_set_enabled(gboolean on) {
  gint was = g_atomic_int_get(&enabled);
  if (on && !was) {
    // off -> on: fresh start. Log the liquid-dsp version to prove the phase-0
    // link and mark the demod core still to come.
    g_mutex_lock(&lock);
    ensure_init();
    g_string_truncate(pending, 0);
    status_blocks = 0;
    status_rate = 0;
    status_fed = FALSE;
    reset_pending = FALSE;
    g_mutex_unlock(&lock);
    log_info("hfdl: decoder enabled (liquid-dsp %s) — scaffold, demod not yet implemented\n",
             liquid_libversion());
    if (echo) g_printerr("[HFDL] enabled (liquid-dsp %s)\n", liquid_libversion());
  } else if (!on && was) {
    log_info("hfdl: decoder disabled\n");
    if (echo) g_printerr("[HFDL] disabled\n");
  }
  g_atomic_int_set(&enabled, on ? 1 : 0);
}

void hfdl_decoder_add_iq(const double *iq, int nframes, int sample_rate) {
  if (!g_atomic_int_get(&enabled) || iq == NULL || nframes <= 0) return;

  g_mutex_lock(&lock);
  ensure_init();
  if (reset_pending) {
    g_string_truncate(pending, 0);
    status_blocks = 0;
    reset_pending = FALSE;
  }
  status_rate = sample_rate;
  status_blocks++;
  status_fed = TRUE;
  glong blk = status_blocks;
  g_mutex_unlock(&lock);

  // Phase 1: no demodulation yet. Later phases NCO-shift the 1800 Hz carrier to
  // DC, LP-filter + decimate to ~3600 complex S/s, then run the liquid M-PSK
  // demod + equalizer + framing on a worker thread from here.
  if (echo && (blk % 200) == 0)
    g_printerr("[HFDL] %ld blocks fed @ %d Hz (no demod yet)\n", blk, sample_rate);

  (void)nframes;
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
  if (blocks)      *blocks      = status_blocks;
  g_mutex_unlock(&lock);
}

void hfdl_decoder_reset(void) {
  g_mutex_lock(&lock);
  ensure_init();
  g_string_truncate(pending, 0);
  reset_pending = TRUE;   // zero the counters on the next audio-thread feed
  status_fed = FALSE;
  g_mutex_unlock(&lock);
}
