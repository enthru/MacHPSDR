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

#ifndef _RECONNECT_H
#define _RECONNECT_H

#include <gtk/gtk.h>

// Link watchdog.  Each protocol receive path calls reconnect_note_data()
// whenever a valid data block arrives from the hardware; a GTK-main-thread
// timer watches the stream and drives the link through the states below,
// retrying by itself on a widening interval and reporting through a banner in
// the main window rather than a modal dialog.

typedef enum {
  LINK_IDLE = 0,     // no radio, or the watchdog has not been armed
  LINK_STARTING,     // armed or just reconnected, waiting for the FIRST block
  LINK_STREAMING,    // data is arriving
  LINK_LOST,         // it stopped, and the next attempt is not due yet
  LINK_RETRYING      // an attempt is running right now
} LINK_STATE;

// --- the decision, separated from everything that acts on it ---------------
//
// Everything below this line is pure: no radio, no GTK, no clock of its own.
// That is what makes it testable, and it is the half that was wrong -- the old
// watchdog only ever armed itself AFTER the first block arrived, so "the device
// opened but the stream never started" was the one failure it could not see.
// tools/reconnect_offline.c drives it on a mock clock.

typedef struct {
  LINK_STATE state;
  gint64 state_since;      // monotonic seconds the current state was entered
  gint64 last_data;        // monotonic seconds of the last block
  gint64 data_seq;         // blocks seen, ever -- "since when" without a clock
  gint64 seq_at_entry;     // data_seq when LINK_STARTING was entered
  gint64 next_attempt;     // monotonic seconds the next retry is due
  int    backoff;          // seconds until the attempt after this one
  int    attempt;          // attempts since the link was last healthy
} LINK;

typedef enum {
  LINK_ACT_NOTHING = 0,
  LINK_ACT_RECONNECT       // run the per-protocol re-initialisation now
} LINK_ACTION;

// A gap in an ESTABLISHED stream.  Comfortably longer than any normal stall
// (buffering, a slow tune), short enough to react.
#define DISCONNECT_TIMEOUT_SEC 3
// A stream that never started.  Longer, because this covers opening a device
// as well as reopening one: a networked PlutoSDR takes seconds to answer.
#define FIRST_DATA_TIMEOUT_SEC 10
// Retries widen 1, 2, 4 ... up to this.  A radio that is off, or a cable that
// is out, can be that way for hours -- and a retry is not free (it tears the
// device down and re-makes it), so the interval has to grow.
#define RECONNECT_BACKOFF_MIN_SEC 1
#define RECONNECT_BACKOFF_MAX_SEC 60

// Arm the state machine at `now`.
extern void link_reset(LINK *l, gint64 now);
// One watchdog tick.  `transmitting` suppresses the gap test (a half-duplex
// SoapySDR device delivers no RX while keyed, which is not a disconnect).
// Returns what the caller must do; the caller reports the outcome with
// link_attempted().
extern LINK_ACTION link_tick(LINK *l, gint64 now, gboolean transmitting);
// Tell the state machine an attempt has just been made, and when it ran.
extern void link_attempted(LINK *l, gint64 now);
// The operator pressed "Retry now": bring the next attempt forward.
extern void link_retry_now(LINK *l, gint64 now);
// What to put in front of the operator, or NULL while the link is healthy.
extern const char *link_state_name(LINK_STATE s);

// --- the live watchdog ------------------------------------------------------

// Arm it.  Call once, on the GTK main thread, after the radio has been started.
// Safe to call again after a protocol restart.  Unlike the old watchdog this
// starts counting IMMEDIATELY, so a device that opens and never streams is
// caught by FIRST_DATA_TIMEOUT_SEC instead of leaving the watchdog dormant for
// the rest of the session.
extern void reconnect_init(void);

// Called from protocol/receive threads on every valid data block received.
// Thread-safe and cheap.
extern void reconnect_note_data(void);

// The status strip the main window puts across the top of the radio window.
// Built once, hidden while the link is healthy.
extern GtkWidget *reconnect_banner(void);

// Test hook: with RECONNECT_BTN_EXIT, quit the moment the link is declared
// lost instead of retrying.  Set from MACHPSDR_RECONNECT_TEST in radio.c.
#define RECONNECT_ASK            (-1)
#define RECONNECT_BTN_RECONNECT    0
#define RECONNECT_BTN_EXIT         1
extern void reconnect_set_auto(int button);

#endif
