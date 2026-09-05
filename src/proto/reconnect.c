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

/* reconnect.c -- the link watchdog.
 *
 * Three things were wrong with the shape this replaces, and they compound.
 *
 *   - It only armed itself AFTER the first block arrived ("so a slow startup
 *     never trips it"), which means the one failure it could not see is the
 *     device that opens and never streams -- a Pluto whose context came up but
 *     whose stream did not, a HackRF that enumerated and then wedged. That is
 *     not a slow start, it is a dead radio, and it presented as an application
 *     sitting at a flat waterfall for ever with nothing in the log.
 *   - It asked. A MODAL dialog over a radio that has just gone away takes the
 *     whole UI with it: the operator cannot look at Configure, cannot read the
 *     log, cannot change anything, and has one thing to do -- answer a question
 *     about a link neither of you can do anything about yet.
 *   - It asked ONCE per timeout and then waited to be answered, so a radio that
 *     came back two minutes later came back to a dialog nobody had clicked.
 *
 * So: it counts from the moment it is armed, it retries BY ITSELF on a widening
 * interval, and it reports through a status strip across the top of the window
 * that the operator can ignore. The decision half is pure and lives above the
 * line -- no radio, no GTK, no clock of its own -- because it is the half that
 * was wrong and the half nothing could test. tools/reconnect_offline.c drives
 * it on a mock clock.
 *
 * WHAT THIS DOES NOT FIX: the reconnection itself still runs on the GTK thread.
 * soapy_protocol_reconnect() re-makes the device, rebuilds both streams and
 * re-applies every setting, calling into create_transmitter, audio_open_input
 * and frequency_changed on the way -- none of which may be touched from another
 * thread -- and on a networked device SoapySDRDevice_make alone can take
 * seconds. The window is therefore still unresponsive FOR THE DURATION OF AN
 * ATTEMPT. What has changed is that the operator is no longer blocked BETWEEN
 * attempts, which is where all the waiting is. Moving the attempt itself off
 * the GTK thread is a protocol-layer change, not a watchdog one.
 */
#include <gtk/gtk.h>
#include "log.h"

#include "discovered.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "reconnect.h"

// ===========================================================================
// The live watchdog: the clock, the radio, and the banner.
// ===========================================================================

#define WATCHDOG_INTERVAL_MS 1000

// NOT `link`: that is POSIX link(2), declared by <unistd.h> through the GTK
// headers, and shadowing it makes every member access a compile error.
static LINK  the_link;
static guint watchdog_id = 0;

// Blocks seen, bumped from receive threads. The state machine reads it through
// its own copy in the_link.data_seq, which only the GTK thread writes.
static volatile gint data_seq = 0;
static volatile gint last_data_sec = 0;

static int auto_answer = RECONNECT_ASK;

static GtkWidget *banner = NULL;
static GtkWidget *banner_label = NULL;
static gboolean   banner_shown = FALSE;

// Test hook, MACHPSDR_LINK_TEST=1: watch the FAKE device too. It never calls
// reconnect_note_data(), so it looks exactly like a radio that opened and never
// streamed -- which is the whole lost-link path, banner included, reachable
// with no hardware and nothing unplugged. Read once when the watchdog is armed.
static gboolean watch_fake = FALSE;

void reconnect_set_auto(int button) { auto_answer = button; }

static gint64 monotonic_sec(void) {
  return g_get_monotonic_time() / G_USEC_PER_SEC;
}

void reconnect_note_data(void) {
  g_atomic_int_inc(&data_seq);
  g_atomic_int_set(&last_data_sec, (gint)monotonic_sec());
}

// ---- the banner -----------------------------------------------------------

static void banner_retry_cb(GtkWidget *w, gpointer data) {
  log_info("reconnect: operator asked to retry now\n");
  link_retry_now(&the_link, monotonic_sec());
}

static void banner_exit_cb(GtkWidget *w, gpointer data) {
  main_delete(NULL);            // saves state, stops cleanly and quits
}

GtkWidget *reconnect_banner(void) {
  if(banner != NULL) return banner;
  banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_name(banner, "link-banner");
  gtk_widget_set_margin_start(banner, 6);
  gtk_widget_set_margin_end(banner, 6);
  gtk_widget_set_margin_top(banner, 3);
  gtk_widget_set_margin_bottom(banner, 3);

  banner_label = gtk_label_new("");
  gtk_widget_set_halign(banner_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(banner_label, TRUE);
  gtk_box_append(GTK_BOX(banner), banner_label);

  GtkWidget *retry = gtk_button_new_with_label("Retry now");
  gtk_widget_set_name(retry, "toolbar-button");
  gtk_widget_set_tooltip_text(retry,
      "Try to re-establish the link immediately instead of waiting for the "
      "next automatic attempt.");
  g_signal_connect(retry, "clicked", G_CALLBACK(banner_retry_cb), NULL);
  gtk_box_append(GTK_BOX(banner), retry);

  GtkWidget *quit = gtk_button_new_with_label("Exit");
  gtk_widget_set_name(quit, "toolbar-button");
  gtk_widget_set_tooltip_text(quit,
      "Save the settings and close the application. Retrying continues in the "
      "background until you do.");
  g_signal_connect(quit, "clicked", G_CALLBACK(banner_exit_cb), NULL);
  gtk_box_append(GTK_BOX(banner), quit);

  gtk_widget_set_visible(banner, FALSE);
  return banner;
}

static void banner_update(gint64 now) {
  if(banner == NULL) return;
  const char *name = (radio != NULL && radio->discovered != NULL)
                       ? radio->discovered->name : "the device";
  gboolean show = FALSE;
  char text[320];

  switch(the_link.state) {
    case LINK_STARTING:
      // Only after the link has been up once: at a cold start this is simply
      // what the first second looks like.
      if(the_link.attempt > 0) {
        g_snprintf(text, sizeof text,
                   "Reconnecting to %s — attempt %d, waiting for data…",
                   name, the_link.attempt);
        // No BANNER_AFTER_SEC delay here: an attempt has already been made, so
        // the operator knows there is a problem, and holding the strip back for
        // two seconds after each one makes it FLICKER off and on for the whole
        // outage -- which reads as the link coming and going.
        show = TRUE;
      } else if(now - the_link.state_since >= FIRST_DATA_TIMEOUT_SEC/2) {
        g_snprintf(text, sizeof text,
                   "%s has not sent any data yet…", name);
        show = TRUE;
      }
      break;
    case LINK_LOST: {
      gint64 in = the_link.next_attempt - now;
      if(in < 0) in = 0;
      if(the_link.attempt == 0)
        g_snprintf(text, sizeof text,
                   "Connection to %s was lost — retrying in %llds", name,
                   (long long)in);
      else
        g_snprintf(text, sizeof text,
                   "Connection to %s was lost — %d attempt%s so far, next in %llds",
                   name, the_link.attempt, the_link.attempt==1?"":"s", (long long)in);
      show = TRUE;
      break;
    }
    case LINK_RETRYING:
      g_snprintf(text, sizeof text, "Reconnecting to %s…", name);
      show = TRUE;
      break;
    default:
      break;
  }

  if(show && banner_label != NULL) gtk_label_set_text(GTK_LABEL(banner_label), text);
  // Logged on the EDGE, and with its text: a banner is the one part of this
  // that no headless run can look at, so the log is the only place its
  // appearance can be observed at all.
  if(show != banner_shown) {
    banner_shown = show;
    if(show) log_info("reconnect: banner shown -- \"%s\"\n", text);
    else     log_info("reconnect: banner hidden (link %s)\n",
                      link_state_name(the_link.state));
  } else if(show) {
    log_debug("reconnect: banner -- \"%s\"\n", text);
  }
  gtk_widget_set_visible(banner, show);
}

// ---- the per-protocol re-initialisation -----------------------------------

// Runs on the GTK main thread. Whether the link is really back is NOT decided
// here: the protocol restarts are fire-and-forget and even a device that
// re-opens may not stream, so the state machine goes back to LINK_STARTING and
// lets the first-data timeout answer it.
static void do_reconnect(RADIO *r) {
  log_info("reconnect: attempt %d -- re-establishing the %s link\n",
          the_link.attempt+1, r->discovered!=NULL ? r->discovered->name : "device");

  switch(r->discovered->protocol) {
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      {
      RECEIVER *hwrx = radio_soapy_hw_receiver(r);
      if(hwrx != NULL) soapy_protocol_reconnect(hwrx);
      }
      break;
#endif
    case PROTOCOL_1:
      protocol1_reconnect();
      break;
    case PROTOCOL_2:
      protocol2_reconnect();
      break;
    default:
      break;
  }
}

static gboolean watchdog_cb(gpointer data) {
  RADIO *r = radio;
  gint64 now = monotonic_sec();

  if(r == NULL || r->discovered == NULL) return TRUE;
  // The fake test device has no hardware to lose -- unless it is being used to
  // stand in for one that has (MACHPSDR_LINK_TEST).
  if(r->discovered->protocol == PROTOCOL_FAKE && !watch_fake) return TRUE;

  the_link.data_seq = g_atomic_int_get(&data_seq);
  the_link.last_data = g_atomic_int_get(&last_data_sec);

  LINK_STATE was = the_link.state;
  LINK_ACTION act = link_tick(&the_link, now, isTransmitting(r));

  if(was == LINK_STREAMING && the_link.state == LINK_LOST)
    log_info("reconnect: no data for %llds - assuming the link is down\n",
             (long long)(now - the_link.last_data));
  if(was == LINK_STARTING && the_link.state == LINK_LOST && the_link.attempt == 0)
    log_error("reconnect: %s was opened but has not delivered a single block in "
              "%d s - treating it as a dead link\n",
              r->discovered->name, FIRST_DATA_TIMEOUT_SEC);

  if(act == LINK_ACT_RECONNECT) {
    if(auto_answer == RECONNECT_BTN_EXIT) {
      log_info("reconnect: test hook answers 'Exit' (no retry)\n");
      main_delete(NULL);
      return TRUE;
    }
    // Paint "Reconnecting…" BEFORE the attempt: it runs on this thread and can
    // take seconds, so without this the operator watches a frozen window still
    // showing the previous state.
    banner_update(now);
    while(g_main_context_iteration(NULL, FALSE)) { }
    do_reconnect(r);
    link_attempted(&the_link, monotonic_sec());
    // The attempt itself is a fresh grace period for the receive threads.
    g_atomic_int_set(&last_data_sec, (gint)monotonic_sec());
  }

  banner_update(now);
  return TRUE;
}

void reconnect_init(void) {
  gint64 now = monotonic_sec();
  watch_fake = (g_getenv("MACHPSDR_LINK_TEST") != NULL);
  if(watch_fake)
    log_info("reconnect: TEST HOOK -- watching the fake device as if it were "
             "hardware, so the lost-link path runs\n");
  g_atomic_int_set(&last_data_sec, (gint)now);
  the_link.data_seq = g_atomic_int_get(&data_seq);
  link_reset(&the_link, now);
  if(watchdog_id == 0)
    watchdog_id = g_timeout_add(WATCHDOG_INTERVAL_MS, watchdog_cb, NULL);
}
