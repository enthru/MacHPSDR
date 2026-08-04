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
 * Owns the whole RX chain: the front-end DSP + symbol recovery (hfdl_demod.c) that
 * turn raw off-air I/Q into carrier-locked symbols, the frame state machine
 * (hfdl_frame.c: preamble sync → mode select → equalizer → deinterleave → Viterbi →
 * frame bytes) and the application layer (hfdl_msg.c: MPDU/LPDU/HFNPDU → ACARS
 * message text). It publishes a signal-level / symbol-throughput status and the
 * decoded messages for the panel.
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
#include "hfdl_frame.h"
#include "hfdl_msg.h"
#include "hfdl_pdu.h"
#include "log.h"

// Scratch for one block of conditioned baseband output (interleaved float I/Q).
// Real off-air rates (≥48 kHz) decimate to far fewer than this per block; the
// front-end caps its write at HFDL_OUT_MAX so this can never overflow.
#define HFDL_OUT_MAX 8192

// --- state (all audio-thread-owned unless noted) ---------------------------

static volatile gint enabled = 0;          // atomic on/off gate
static hfdl_demod   *demod = NULL;          // front-end DSP (audio thread only)
static hfdl_framer  *framer = NULL;         // frame state machine (audio thread only)
static double        demod_rate = 0.0;      // input rate demod was built for
static float         out_buf[2 * HFDL_OUT_MAX];   // conditioned baseband
static glong         status_frames = 0;     // frames decoded since reset

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
  if (framer) { hfdl_framer_destroy(framer); framer = NULL; }
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
    log_info("hfdl: decoder enabled (liquid-dsp %s) — full RX chain incl. message decode\n",
             liquid_libversion());
    if (do_echo) {
      g_printerr("[HFDL] enabled (liquid-dsp %s)\n", liquid_libversion());
      if (g_getenv("MACHPSDR_HFDL_SELFTEST")) {
        g_printerr("[HFDL] demod selftest: %s\n", hfdl_demod_selftest() ? "PASS" : "FAIL");
        g_printerr("[HFDL] fec selftest:   %s\n", hfdl_fec_selftest() ? "PASS" : "FAIL");
        g_printerr("[HFDL] frame selftest: %s\n", hfdl_frame_selftest() ? "PASS" : "FAIL");
        g_printerr("[HFDL] pdu selftest:   %s\n", hfdl_pdu_selftest() ? "PASS" : "FAIL");
        g_printerr("[HFDL] msg selftest:   %s\n", hfdl_msg_selftest() ? "PASS" : "FAIL");
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

// Per-symbol sink: push into the framer, decode a completed frame, and keep the
// carrier loop's slicer aligned with what the frame is carrying.
typedef struct {
  hfdl_framer *framer;
  int          frames;    // frames completed in this block
  GString     *text;      // decoded messages (allocated lazily)
} frame_sink;

static void on_symbol(void *ctx, float re, float im) {
  frame_sink *s = ctx;
  if (s->framer == NULL) return;
  int nb = hfdl_framer_push(s->framer, re, im);
  // The framer has now advanced; slice the next symbol against the modulation
  // it says is coming (BPSK for sync/training, the frame's own for data).
  hfdl_demod_set_slicer(demod, hfdl_framer_arity(s->framer));
  if (nb <= 0) return;

  const uint8_t *fb = hfdl_framer_bytes(s->framer, &nb);
  s->frames++;
  // Full application-layer decode: MPDU/SPDU header -> LPDU -> HFNPDU -> ACARS
  // message text (hfdl_msg.c). Frames whose FCS fails are shown as a hex dump
  // instead, so a marginal decode is still visible.
  if (s->text == NULL) s->text = g_string_new(NULL);
  if (!hfdl_msg_decode(fb, nb, s->text)) {
    g_string_append(s->text, "  ");
    int show = nb < 32 ? nb : 32;   // trim the hex dump for the panel
    for (int k = 0; k < show; k++) g_string_append_printf(s->text, "%02x ", fb[k]);
    if (show < nb) g_string_append(s->text, "\xE2\x80\xA6");
    g_string_append_c(s->text, '\n');
  }
}

void hfdl_decoder_add_iq(const double *iq, int nframes, int sample_rate) {
  if (!g_atomic_int_get(&enabled) || iq == NULL || nframes <= 0 || sample_rate <= 0)
    return;

  // (Re)build the front-end + framer if the rate changed (audio thread — safe).
  if (demod == NULL || demod_rate != (double)sample_rate) {
    demod_free();
    demod = hfdl_demod_create((double)sample_rate);
    framer = hfdl_framer_create();
    demod_rate = (double)sample_rate;
  }
  int nsym = 0, frames = 0;
  double level = -160.0;
  GString *frametext = NULL;      // decoded messages from this block (may be NULL)
  if (demod) {
    // Front-end: condition the raw I/Q into the 5400 S/s symbol domain.
    int nbb = hfdl_demod_process(demod, iq, nframes, out_buf, HFDL_OUT_MAX);
    level = hfdl_demod_level_db(demod);
    // Symbol recovery + framing, interleaved symbol by symbol: each recovered
    // symbol goes straight into the framer, and the framer's resulting state
    // picks the modulation the carrier loop slices the NEXT symbol against.
    // (A block holds tens of symbols and spans several framer states, so doing
    // this a block at a time would be far too coarse — PSK4/PSK8 data would be
    // sliced as BPSK for most of the frame.)
    frame_sink sink = { framer, 0, NULL };
    nsym = hfdl_demod_symbols_cb(demod, out_buf, nbb, on_symbol, &sink);
    frames = sink.frames;
    frametext = sink.text;
  }

  g_mutex_lock(&lock);
  ensure_init();
  if (reset_pending) {
    g_string_truncate(pending, 0);
    status_syms = 0;
    status_frames = 0;
    reset_pending = FALSE;
    hfdl_msg_reset();     // audio thread — same thread the cache is filled on
  }
  status_rate = sample_rate;
  status_syms += nsym;
  status_frames += frames;
  status_level = level;
  status_fed = TRUE;
  if (frametext != NULL) g_string_append(pending, frametext->str);
  glong syms = status_syms;
  gboolean do_echo = echo;
  g_mutex_unlock(&lock);

  if (do_echo && frames > 0)
    g_printerr("[HFDL] decoded %d frame(s) @ %.0f dB\n%s", frames, level,
               frametext ? frametext->str : "");
  else if (do_echo && nsym > 0 && (syms - nsym) / 20000 != syms / 20000)
    g_printerr("[HFDL] %ld symbols recovered @ %.0f dB (searching for frames)\n",
               syms, level);
  if (frametext != NULL) g_string_free(frametext, TRUE);
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

glong hfdl_decoder_get_frames(void) {
  g_mutex_lock(&lock);
  glong v = status_frames;
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
