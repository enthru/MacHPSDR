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

/* link_state.c -- the link watchdog's DECISION, with nothing attached to it.
 *
 * Split out of reconnect.c so it can be tested. It is handed the time and told
 * whether the transmitter is keyed, and it answers with a state and at most one
 * action: no radio, no socket, no GTK and no clock of its own. That matters
 * because this is the half that was wrong -- the old watchdog only armed itself
 * AFTER the first block arrived, so "the device opened but the stream never
 * started" was the one failure it could not see -- and the half that nothing
 * could exercise, since every emulator in this tree answers and the whole
 * subject here is what happens when nothing does.
 *
 * tools/reconnect_offline.c drives it on a mock clock. reconnect.c is the other
 * half: the timer, the radio, the per-protocol re-initialisation and the banner.
 */
#include <glib.h>
#include "log.h"
#include "reconnect.h"

// ===========================================================================
// The decision. Pure: it is handed the time and told whether the transmitter
// is keyed, and it answers with a state and at most one action.
// ===========================================================================

const char *link_state_name(LINK_STATE s) {
  switch(s) {
    case LINK_IDLE:      return "idle";
    case LINK_STARTING:  return "starting";
    case LINK_STREAMING: return "streaming";
    case LINK_LOST:      return "lost";
    case LINK_RETRYING:  return "retrying";
  }
  return "?";
}

static void link_enter(LINK *l, LINK_STATE s, gint64 now) {
  if(l->state != s)
    log_info("reconnect: link %s -> %s\n", link_state_name(l->state), link_state_name(s));
  l->state = s;
  l->state_since = now;
  // "Has anything arrived since we entered this state?" without a second
  // clock: the sequence number is what the receive threads bump, and comparing
  // it against the value at entry cannot be fooled by a coarse second counter
  // or by a block that arrived in the same second as the transition.
  l->seq_at_entry = l->data_seq;
}

void link_reset(LINK *l, gint64 now) {
  l->last_data    = now;
  l->next_attempt = 0;
  l->backoff      = RECONNECT_BACKOFF_MIN_SEC;
  l->attempt      = 0;
  l->state        = LINK_IDLE;      // so link_enter logs the transition
  link_enter(l, LINK_STARTING, now);
}

void link_retry_now(LINK *l, gint64 now) {
  l->next_attempt = now;
  // LINK_STARTING is waiting out the first-data timeout and does not look at
  // next_attempt at all, so asking from there did nothing for up to
  // FIRST_DATA_TIMEOUT_SEC -- a button that appears not to work, which is worse
  // than no button. Drop into LINK_LOST, where the next tick acts on it.
  if(l->state == LINK_STARTING) link_enter(l, LINK_LOST, now);
}

// An attempt has just been run. Whether it WORKED is not knowable here and is
// deliberately not guessed at: the protocol restarts are fire-and-forget and
// even a device that re-opens may not stream. So the machine goes back to
// LINK_STARTING and lets FIRST_DATA_TIMEOUT_SEC answer the question, which is
// the same test a cold start uses.
void link_attempted(LINK *l, gint64 now) {
  l->attempt++;
  l->last_data = now;               // fresh grace period while it spins up
  // Clamp the DOUBLED value, not the one before it. Written the other way
  // round (double unless already at the cap) the ladder overshoots once -- 32
  // becomes 64 -- and the step after that clamps back to 60, so the interval
  // between attempts SHRINKS, which is the one thing a backoff must never do.
  l->backoff = (l->backoff * 2 > RECONNECT_BACKOFF_MAX_SEC)
                 ? RECONNECT_BACKOFF_MAX_SEC : l->backoff * 2;
  // The wait for the FIRST block is part of the attempt, not part of the pause
  // after it -- otherwise the whole ladder is swallowed by it. Written the
  // obvious way (next_attempt = now + backoff) the first-data timeout expires
  // with the attempt already overdue, so every retry fired at
  // FIRST_DATA_TIMEOUT_SEC + 1 no matter what the backoff had climbed to:
  // measured by reconnect_offline as gaps of 11, 11, 11 ... where they should
  // have been 12, 14, 18. That is the entire point of a backoff, silently
  // absent.
  l->next_attempt = now + FIRST_DATA_TIMEOUT_SEC + l->backoff;
  link_enter(l, LINK_STARTING, now);
}

LINK_ACTION link_tick(LINK *l, gint64 now, gboolean transmitting) {
  if(l->state == LINK_IDLE) return LINK_ACT_NOTHING;

  gboolean fresh = (l->data_seq != l->seq_at_entry);

  // While keyed, a half-duplex device delivers no RX by design. Keep the clock
  // fresh so the grace period restarts cleanly on return to receive -- but only
  // while the link is believed up, or a radio that vanished during a
  // transmission would never be noticed.
  if(transmitting && (l->state == LINK_STREAMING || l->state == LINK_STARTING)) {
    l->last_data = now;
    l->state_since = now;
    return LINK_ACT_NOTHING;
  }

  switch(l->state) {
    case LINK_STARTING:
      if(fresh) {
        // A block arrived: the link is up, and the backoff ladder resets so the
        // next unrelated outage is answered in one second again rather than in
        // whatever interval the last one had climbed to.
        l->backoff = RECONNECT_BACKOFF_MIN_SEC;
        l->attempt = 0;
        link_enter(l, LINK_STREAMING, now);
      } else if(now - l->state_since >= FIRST_DATA_TIMEOUT_SEC) {
        link_enter(l, LINK_LOST, now);
        // The FIRST loss is worth trying immediately -- it is as likely to be a
        // cable pushed back in as a radio that is off. Every loss after one
        // waits out the ladder that link_attempted() set.
        if(l->attempt == 0) l->next_attempt = now;
      }
      break;

    case LINK_STREAMING:
      if(now - l->last_data >= DISCONNECT_TIMEOUT_SEC) {
        link_enter(l, LINK_LOST, now);
        l->next_attempt = now;
      }
      break;

    case LINK_LOST:
      // The link can heal on its own -- a radio power-cycled, a switch
      // rebooted, a Wi-Fi drop that came back. Nothing was ever stopped, so
      // data simply resumes and there is nothing to reconnect.
      if(fresh) {
        l->backoff = RECONNECT_BACKOFF_MIN_SEC;
        l->attempt = 0;
        link_enter(l, LINK_STREAMING, now);
      } else if(now >= l->next_attempt) {
        link_enter(l, LINK_RETRYING, now);
        return LINK_ACT_RECONNECT;
      }
      break;

    case LINK_RETRYING:
      // The caller runs the attempt synchronously and reports with
      // link_attempted(), so this is only reached if it did not.
      break;

    case LINK_IDLE:
      break;
  }
  return LINK_ACT_NOTHING;
}

