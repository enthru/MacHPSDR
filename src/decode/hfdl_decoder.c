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
#include <math.h>
#include <liquid/liquid.h>

#include "hfdl_decoder.h"
#include "hfdl_demod.h"
#include "hfdl_fec.h"
#include "hfdl_frame.h"
#include "hfdl_arinc.h"
#include "hfdl_cpdlc.h"
#include "hfdl_miam.h"
#include "hfdl_ohma.h"
#include "hfdl_msg.h"
#include "hfdl_pdu.h"
#include "log.h"

// Scratch for one block of conditioned baseband output (interleaved float I/Q).
// Real off-air rates (≥48 kHz) decimate to far fewer than this per block; the
// front-end caps its write at HFDL_OUT_MAX so this can never overflow.
#define HFDL_OUT_MAX 8192

// --- state (all audio-thread-owned unless noted) ---------------------------

static volatile gint enabled = 0;          // atomic on/off gate
static volatile gint rebuild_req = 0;      // GTK asks the audio thread to rebuild channels

// --- channels ---------------------------------------------------------------
//
// One front-end + framer per HFDL channel being decoded. An HF band holds a
// dozen HFDL channels inside ~100 kHz (the 11 MHz band runs 11306…11387), and a
// 192 kHz receiver already has all of them in its passband — so decoding only
// the one under the dial throws most of the traffic away. Each channel is just
// another NCO offset into the same I/Q block; measured cost is ~0.5 % of a core
// per channel at 192 kHz, so a bandful is affordable.
//
// Channel 0 is always the channel the operator has TUNED — the CTUN/freetune
// cursor when either is on, otherwise the receiver centre. It is not fixed to
// the centre: with freetune the operator points the cursor at a channel tens of
// kHz off centre, and a decoder listening at the centre would find nothing
// there. Scanning adds the other known channels around it.

typedef struct {
  double       offset_hz;   // channel centre relative to the receiver centre
  uint32_t     khz;         // channel frequency, 0 when it is just "the dial"
  hfdl_demod  *demod;
  hfdl_framer *framer;
  double       trim_hz;     // search offset applied on top (0 for the exact one)
  gboolean     is_trim;     // one of the tuned channel's search siblings
  gboolean     is_tuned;    // follows the cursor (vs a fixed scan channel)
} HFDL_CHANNEL;

// Coarse carrier search.
//
// MEASURED on a real recording: the chain decodes with the channel frequency out
// by 100 Hz, and decodes nothing at 200 Hz. That is far finer than an operator
// can point — one pixel of an unzoomed panadapter is already ~100 Hz — and finer
// than an uncalibrated dial. dumphfdl never meets this because it is handed exact
// channel frequencies and a calibrated SDR.
//
// So the tuned channel is run at several offsets AT ONCE, and whichever decodes
// a frame wins; the losers are then dropped, leaving exactly the single
// front-end that was there before. They must run in parallel rather than be
// tried in turn: HFDL is bursty (this recording carries ~14 frames in 174 s), so
// a search that walks the offset on a timer is almost never on the right one
// when a burst finally arrives — measured, it decoded nothing at all.
static const double HFDL_TRIM_HZ[] = { 0.0, 150.0, -150.0, 300.0, -300.0,
                                       450.0, -450.0 };
#define HFDL_TRIM_CNT ((int)(sizeof(HFDL_TRIM_HZ)/sizeof(HFDL_TRIM_HZ[0])))

// A cursor move RETUNES the front-end's NCO in place — no rebuild, so the
// resampler, AGC and symbol recovery keep their state and the decoder is always
// exactly where the VFO says it is. (It used to ignore moves under 200 Hz to
// avoid the rebuild, which meant small steps of the dial did nothing at all
// while the readout kept showing the old frequency: fine-tuning by ear or by eye
// was impossible.) Only a jump bigger than this re-arms the coarse search, on
// the grounds that the operator has gone to a different channel rather than
// nudged this one.
#define HFDL_RESEARCH_HZ 400.0

static HFDL_CHANNEL  chans[HFDL_MAX_CHANNELS];
static int           nchans = 0;
static double        demod_rate = 0.0;      // input rate the channels were built for
static long long     demod_center = 0;      // receiver centre they were built for
static long long     demod_cursor = 0;      // tuned channel they were built for
static gboolean      scan_band = FALSE;     // decode every known channel in the passband
static float         out_buf[2 * HFDL_OUT_MAX];   // conditioned baseband
static glong         status_frames = 0;     // frames decoded since reset

static GMutex        lock;                  // guards the published fields below
static GString      *pending = NULL;        // decoded text awaiting the panel drain
// Separate, NON-draining copy of the same text for the bottom Decode block.
// The panel's drain must not steal what the readout shows (and vice versa), so
// the readout peeks at this history instead — same split as cw_decoder.c.
// Both buffers are capped: with the panel closed nothing ever drains `pending`,
// so an unbounded GString would grow for the whole session.
#define HFDL_HISTORY_MAX 4096
#define HFDL_PENDING_MAX 65536
static GString      *history = NULL;
static gboolean      reset_pending = FALSE; // requested from GTK, applied on feed
static int           status_rate = 0;       // last off-air sample rate seen (Hz)
static glong         status_syms = 0;       // symbol-domain samples since reset
static double        status_level = -160.0; // AGC RSSI (dB)
static double        status_peak_hz = 0.0;  // strongest bin in the passband, Hz from centre
static gboolean      status_peak_ok = FALSE;
static gboolean      status_fed = FALSE;    // fed at least one block since enable

static gboolean      echo = FALSE;          // MACHPSDR_HFDL_ECHO -> stderr
static gboolean      env_read = FALSE;

// Coarse "where is the signal, really" probe.
//
// When nothing decodes, the question is always whether the front-end is looking
// where the operator thinks. This measures it inside the decoder, in the
// decoder's OWN view of the I/Q (same (Q, I) order as the demod), and publishes
// the strongest bin's offset from the receiver centre for the readout. If that
// disagrees with the panadapter, the two are not seeing the same spectrum and
// nothing about the tuning matters until that is resolved.
#define HFDL_PROBE_N 4096
static float complex probe_in[HFDL_PROBE_N];
static float complex probe_out[HFDL_PROBE_N];
static int           probe_fill = 0;
static long          probe_hold = 0;   // input samples until the next probe

static void probe_feed(const double *iq, int nframes, int sample_rate) {
  // One probe per second of input is plenty and costs a single 4096-pt FFT.
  if (probe_hold > 0) { probe_hold -= nframes; return; }
  for (int i = 0; i < nframes && probe_fill < HFDL_PROBE_N; i++)
    probe_in[probe_fill++] = (float)iq[2*i+1] + (float)iq[2*i] * I;
  if (probe_fill < HFDL_PROBE_N) return;
  probe_fill = 0;
  probe_hold = sample_rate;
  fft_run(HFDL_PROBE_N, probe_in, probe_out, LIQUID_FFT_FORWARD, 0);
  double best = 0.0; int bestk = 0;
  for (int k = 0; k < HFDL_PROBE_N; k++) {
    double m = crealf(probe_out[k])*crealf(probe_out[k]) +
               cimagf(probe_out[k])*cimagf(probe_out[k]);
    if (m > best) { best = m; bestk = k; }
  }
  int kk = (bestk < HFDL_PROBE_N/2) ? bestk : bestk - HFDL_PROBE_N;
  g_mutex_lock(&lock);
  status_peak_hz = (double)kk * (double)sample_rate / (double)HFDL_PROBE_N;
  status_peak_ok = TRUE;
  g_mutex_unlock(&lock);
}

static void ensure_init(void) {
  if (pending == NULL) pending = g_string_new(NULL);
  if (history == NULL) history = g_string_new(NULL);
  if (!env_read) {
    echo = (g_getenv("MACHPSDR_HFDL_ECHO") != NULL);
    env_read = TRUE;
  }
}

// Audio thread. Free every channel (on disable / rate or centre change / reset).
static void demod_free(void) {
  for (int i = 0; i < nchans; i++) {
    if (chans[i].demod)  hfdl_demod_destroy(chans[i].demod);
    if (chans[i].framer) hfdl_framer_destroy(chans[i].framer);
  }
  memset(chans, 0, sizeof(chans));
  nchans = 0;
  demod_rate = 0.0;
  demod_center = 0;
  demod_cursor = 0;
}

static void chan_add_trim(double offset_hz, uint32_t khz, double rate,
                          double trim_hz, gboolean is_trim) {
  if (nchans >= HFDL_MAX_CHANNELS) return;
  hfdl_demod *d = hfdl_demod_create_at(rate, offset_hz + trim_hz);
  if (d == NULL) return;
  chans[nchans].offset_hz = offset_hz;
  chans[nchans].khz       = khz;
  chans[nchans].demod     = d;
  chans[nchans].framer    = hfdl_framer_create();
  chans[nchans].trim_hz   = trim_hz;
  chans[nchans].is_trim   = is_trim;
  nchans++;
}

static void chan_add(double offset_hz, uint32_t khz, double rate) {
  chan_add_trim(offset_hz, khz, rate, 0.0, FALSE);
}

// A frame decoded on channel `winner`: if the coarse search is still running,
// keep that front-end and drop its siblings, so the steady state is the single
// channel that existed before the search was added.
static void chan_search_done(int winner) {
  if (winner < 0 || winner >= nchans) return;
  if (!chans[winner].is_trim && chans[winner].trim_hz == 0.0) {
    // The exact offset won and no sibling is worth keeping either.
  }
  gboolean any = FALSE;
  for (int i = 0; i < nchans; i++)
    if (i != winner && (chans[i].is_trim || chans[i].trim_hz != 0.0) &&
        chans[i].khz == chans[winner].khz) any = TRUE;
  if (!any) return;
  if (chans[winner].trim_hz != 0.0)
    log_debug_area(LOG_SYNC, "hfdl: carrier found %+.0f Hz from the requested channel\n",
             chans[winner].trim_hz);
  int n = 0;
  for (int i = 0; i < nchans; i++) {
    gboolean sibling = (i != winner) && (chans[i].is_trim || chans[i].trim_hz != 0.0) &&
                       (chans[i].khz == chans[winner].khz);
    if (sibling) {
      if (chans[i].demod)  hfdl_demod_destroy(chans[i].demod);
      if (chans[i].framer) hfdl_framer_destroy(chans[i].framer);
      continue;
    }
    if (n != i) chans[n] = chans[i];
    n++;
  }
  nchans = n;
  for (int i = 0; i < nchans; i++) chans[i].is_trim = FALSE;
}

// Audio thread. Build the channel set for this receiver centre / tuned channel
// / sample rate.
static void chans_build(double rate, long long center_hz, long long cursor_hz) {
  demod_free();
  if (cursor_hz <= 0) cursor_hz = center_hz;

  // ONE channel: the one the operator is pointing at. Under CTUN/freetune that
  // is the cursor, otherwise the centre — the same thing the rest of the
  // receiver calls "tuned". Decoding anywhere else on the quiet is wrong: it
  // spends a front-end on spectrum nobody asked about and hides a mistuned
  // cursor behind lines from somewhere else. Decoding more than one channel is
  // what "Scan band" is for, and it is deliberate.
  double cursor_off = (center_hz > 0 && cursor_hz > 0)
                      ? (double)(cursor_hz - center_hz) : 0.0;
  // Outside the passband there is nothing to decode; fall back to the centre
  // rather than pointing a front-end into empty spectrum.
  double usable = rate * 0.5 * 0.9;
  if (fabs(cursor_off) > usable) {
    cursor_off = 0.0;
    cursor_hz  = center_hz;
  }
  uint32_t cursor_khz = cursor_hz > 0 ? (uint32_t)(cursor_hz / 1000) : 0;
  chan_add_trim(cursor_off, cursor_khz, rate, 0.0, FALSE);
  chans[nchans-1].is_tuned = TRUE;
  // Search siblings at the same channel, a few hundred Hz either side. Only when
  // not scanning: a bandful of channels is already the CPU budget, and those
  // come from the station table at exact frequencies rather than from a mouse.
  if (!scan_band) {
    for (int t = 1; t < HFDL_TRIM_CNT; t++) {
      chan_add_trim(cursor_off, cursor_khz, rate, HFDL_TRIM_HZ[t], TRUE);
      chans[nchans-1].is_tuned = TRUE;
    }
    // NOTE: an inverted sideband ("Swap I & Q" in the wrong position, or a
    // recording made with the other convention) needs nothing here. It mirrors
    // the panadapter and the decoder together — both read the same buffer the
    // same way — so the signal simply appears on the other side of the centre
    // and the operator points at what they see. Verified on a deliberately
    // conjugated copy of a real recording: decodes normally.
  }

  demod_rate   = rate;
  demod_center = center_hz;
  demod_cursor = cursor_hz;
  // Say which channel is actually being decoded: "no frames" is nearly always
  // the front-end pointed somewhere other than the operator thinks, and that is
  // invisible without this line.
  log_info("hfdl: tuned channel %lld Hz (%+.0f Hz from centre %lld), rate %.0f\n",
           cursor_hz, cursor_off, center_hz, rate);
  if (!scan_band || center_hz <= 0) return;

  // Every known HFDL channel that falls inside the usable part of the passband.
  // 80 % of it: the outer edges are where the receiver's own filtering rolls off,
  // and a channel there would decode badly and just waste a front-end.
  double half = rate * 0.5 * 0.8;
  HFDL_GS_INFO gs[64];
  int ng = hfdl_msg_gs_list(gs, 64);
  for (int i = 0; i < ng && nchans < HFDL_MAX_CHANNELS; i++) {
    for (int f = 0; f < gs[i].freq_cnt && nchans < HFDL_MAX_CHANNELS; f++) {
      if (gs[i].freqs[f] == 0) continue;
      double off = (double)((long long)gs[i].freqs[f] * 1000LL - center_hz);
      if (off < -half || off > half) continue;
      if (fabs(off - cursor_off) < 1.0) continue;       // that is the tuned channel
      gboolean dup = FALSE;                             // stations share channels
      for (int c = 0; c < nchans; c++)
        if (chans[c].khz == gs[i].freqs[f]) { dup = TRUE; break; }
      if (dup) continue;
      chan_add(off, gs[i].freqs[f], rate);
    }
  }
  log_info("hfdl: decoding %d channel(s) around %lld Hz\n", nchans, center_hz);
}

void hfdl_decoder_set_scan(gboolean on) {
  if (scan_band == (on != FALSE)) return;
  scan_band = (on != FALSE);
  // The channel set is only ever built or freed on the audio thread; this asks
  // for a rebuild there rather than touching the channels from the GTK thread.
  g_atomic_int_set(&rebuild_req, 1);
}

gboolean hfdl_decoder_get_scan(void) { return scan_band; }

int hfdl_decoder_channel_count(void) { return nchans; }

// --- API -------------------------------------------------------------------

// Every layer's own test, in the order the signal passes through them, so a
// failure names the first stage that broke rather than "HFDL is broken".  Kept
// in one place because there are two callers: MACHPSDR_HFDL_SELFTEST inside the
// running app, and `hfdl_offline --selftest`, which needs no recording at all.
gboolean hfdl_decoder_selftest(void) {
  static const struct { const char *name; gboolean (*run)(void); } layers[] = {
    { "demod", hfdl_demod_selftest },
    { "fec",   hfdl_fec_selftest   },
    { "frame", hfdl_frame_selftest },
    { "pdu",   hfdl_pdu_selftest   },
    { "msg",   hfdl_msg_selftest   },
    { "arinc", hfdl_arinc_selftest },
    { "miam",  hfdl_miam_selftest  },
    { "ohma",  hfdl_ohma_selftest  },
    { "cpdlc", hfdl_cpdlc_selftest },
  };
  gboolean all = TRUE;
  for (unsigned i = 0; i < G_N_ELEMENTS(layers); i++) {
    gboolean ok = layers[i].run();
    g_printerr("[HFDL] %-5s selftest: %s\n", layers[i].name, ok ? "PASS" : "FAIL");
    if (!ok) all = FALSE;
  }
  return all;
}

void hfdl_decoder_set_enabled(gboolean on) {
  gint was = g_atomic_int_get(&enabled);
  if (on && !was) {
    // off -> on: fresh start.
    g_mutex_lock(&lock);
    ensure_init();
    g_string_truncate(pending, 0);
    g_string_truncate(history, 0);
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
      if (g_getenv("MACHPSDR_HFDL_SELFTEST")) hfdl_decoder_selftest();
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
  hfdl_demod  *demod;     // the channel's own front-end (for the slicer)
  uint32_t     khz;       // channel frequency, 0 = the dial
  int          frames;    // frames completed in this block
  GString     *text;      // decoded messages (allocated lazily)
} frame_sink;

static void on_symbol(void *ctx, float re, float im) {
  frame_sink *s = ctx;
  if (s->framer == NULL) return;
  int nb = hfdl_framer_push(s->framer, re, im);
  // The framer has now advanced; slice the next symbol against the modulation
  // it says is coming (BPSK for sync/training, the frame's own for data).
  hfdl_demod_set_slicer(s->demod, hfdl_framer_arity(s->framer));
  if (nb <= 0) return;

  const uint8_t *fb = hfdl_framer_bytes(s->framer, &nb);
  s->frames++;
  // Full application-layer decode: MPDU/SPDU header -> LPDU -> HFNPDU -> ACARS
  // message text (hfdl_msg.c). Frames whose FCS fails are shown as a hex dump
  // instead, so a marginal decode is still visible.
  if (s->text == NULL) s->text = g_string_new(NULL);
  // Only label the frame when more than one channel is being decoded, so the
  // single-channel output stays exactly as it was.
  if (s->khz) g_string_append_printf(s->text, "[%u kHz] ", s->khz);
  if (!hfdl_msg_decode(fb, nb, s->text)) {
    g_string_append(s->text, "  ");
    int show = nb < 32 ? nb : 32;   // trim the hex dump for the panel
    for (int k = 0; k < show; k++) g_string_append_printf(s->text, "%02x ", fb[k]);
    if (show < nb) g_string_append(s->text, "\xE2\x80\xA6");
    g_string_append_c(s->text, '\n');
  }
}

void hfdl_decoder_add_iq_at(const double *iq, int nframes, int sample_rate,
                            long long center_hz, long long cursor_hz) {
  if (!g_atomic_int_get(&enabled) || iq == NULL || nframes <= 0 || sample_rate <= 0)
    return;
  if (cursor_hz <= 0) cursor_hz = center_hz;

  // (Re)build the channels when the rate, the centre or the tuned channel moved,
  // or when the scan toggle asked for it. Always on the audio thread, which owns
  // them. The cursor gets a dead band: rebuilding drops the framer's sync, and a
  // one-step nudge of the dial must not cost the frame being received.
  double moved = fabs((double)(cursor_hz - demod_cursor));
  if (nchans == 0 || demod_rate != (double)sample_rate ||
      demod_center != center_hz || moved > HFDL_RESEARCH_HZ ||
      g_atomic_int_get(&rebuild_req)) {
    g_atomic_int_set(&rebuild_req, 0);
    chans_build((double)sample_rate, center_hz, cursor_hz);
  } else if (moved > 0.0) {
    // Small move: follow it by retuning, keeping every filter's state. The
    // decoder must sit exactly where the operator put the cursor, or fine
    // tuning does nothing and the readout lies about where it is listening.
    double off = (double)(cursor_hz - center_hz);
    for (int c = 0; c < nchans; c++) {
      if (!chans[c].is_tuned) continue;
      chans[c].offset_hz = off;
      chans[c].khz       = (uint32_t)(cursor_hz / 1000);
      hfdl_demod_set_channel_offset(chans[c].demod, off + chans[c].trim_hz);
    }
    demod_cursor = cursor_hz;
  }

  probe_feed(iq, nframes, sample_rate);

  int nsym = 0, frames = 0, winner = -1;
  double level = -160.0;
  GString *frametext = NULL;      // decoded messages from this block (may be NULL)
  for (int c = 0; c < nchans; c++) {
    // Front-end: condition the raw I/Q into the 5400 S/s symbol domain.
    int nbb = hfdl_demod_process(chans[c].demod, iq, nframes, out_buf, HFDL_OUT_MAX);
    // The dial channel drives the level readout — it is the one the operator is
    // pointing at, and averaging over channels would say nothing about either.
    if (c == 0) level = hfdl_demod_level_db(chans[c].demod);
    // Symbol recovery + framing, interleaved symbol by symbol: each recovered
    // symbol goes straight into the framer, and the framer's resulting state
    // picks the modulation the carrier loop slices the NEXT symbol against.
    // (A block holds tens of symbols and spans several framer states, so doing
    // this a block at a time would be far too coarse — PSK4/PSK8 data would be
    // sliced as BPSK for most of the frame.)
    // Label lines with their channel only when genuinely decoding several
    // DIFFERENT channels (scan band) — the search siblings are all the same
    // channel, and prefixing them would invent a distinction that is not there.
    frame_sink sink = { chans[c].framer, chans[c].demod,
                        (scan_band && nchans > 1) ? chans[c].khz : 0, 0, frametext };
    int n = hfdl_demod_symbols_cb(chans[c].demod, out_buf, nbb, on_symbol, &sink);
    if (c == 0) nsym = n;         // throughput readout follows the dial channel
    frames += sink.frames;
    frametext = sink.text;        // shared, allocated lazily by the first frame

    if (sink.frames > 0) winner = c;
  }
  // Collapse the coarse search onto whichever offset actually decoded. Done
  // after the loop so the channel array is not rearranged while iterating it.
  if (winner >= 0) chan_search_done(winner);

  g_mutex_lock(&lock);
  ensure_init();
  if (reset_pending) {
    g_string_truncate(pending, 0);
    g_string_truncate(history, 0);
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
  if (frametext != NULL) {
    g_string_append(pending, frametext->str);
    if (pending->len > HFDL_PENDING_MAX)
      g_string_erase(pending, 0, pending->len - HFDL_PENDING_MAX);
    g_string_append(history, frametext->str);
    if (history->len > HFDL_HISTORY_MAX)
      g_string_erase(history, 0, history->len - HFDL_HISTORY_MAX);
  }
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

// Single-channel entry point: the caller does not know the dial frequency, so
// scanning cannot be offered (there is nothing to place the other channels
// against). Used by the offline harness and the self-tests.
void hfdl_decoder_add_iq(const double *iq, int nframes, int sample_rate) {
  hfdl_decoder_add_iq_at(iq, nframes, sample_rate, 0, 0);
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

// Newest decoded text WITHOUT consuming it — for the bottom Decode readout, which
// must not steal messages from the panel's drain.
int hfdl_decoder_get_recent(char *buf, int buflen) {
  if (buf == NULL || buflen <= 0) return 0;
  g_mutex_lock(&lock);
  ensure_init();
  int take = (int)MIN((gsize)(buflen - 1), history->len);
  memcpy(buf, history->str + history->len - take, take);   // the newest `take` chars
  buf[take] = '\0';
  g_mutex_unlock(&lock);
  return take;
}

void hfdl_decoder_get_status(gboolean *listening, int *sample_rate, glong *blocks) {
  g_mutex_lock(&lock);
  ensure_init();
  if (listening)   *listening   = g_atomic_int_get(&enabled) && status_fed;
  if (sample_rate) *sample_rate = status_rate;
  if (blocks)      *blocks      = status_syms;   // recovered symbols since reset
  g_mutex_unlock(&lock);
}

void hfdl_decoder_get_peak(double *peak_hz, gboolean *valid) {
  g_mutex_lock(&lock);
  if (peak_hz) *peak_hz = status_peak_hz;
  if (valid)   *valid   = status_peak_ok;
  g_mutex_unlock(&lock);
}

void hfdl_decoder_get_tuned(long long *cursor_hz, double *offset_hz) {
  // Audio-thread-owned scalars; a torn read here would only misprint a readout.
  if (cursor_hz) *cursor_hz = demod_cursor;
  if (offset_hz) *offset_hz = (nchans > 0) ? chans[0].offset_hz : 0.0;
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
