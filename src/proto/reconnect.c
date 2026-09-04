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

// How often the watchdog wakes, and how long a data gap must last before we
// treat the link as lost.  The gap must be comfortably longer than any normal
// stall (buffering, a slow tune) but short enough to react quickly.
#define WATCHDOG_INTERVAL_MS   1000
#define DISCONNECT_TIMEOUT_SEC 3

// Monotonic seconds of the last received data block.  Written from receive
// threads, read from the watchdog; a 32-bit atomic is torn-read-safe and holds
// the monotonic clock for decades.
static volatile gint last_data_sec = 0;

// Have we ever seen data?  The watchdog stays disarmed until the stream has
// actually delivered something, so a slow startup never trips it.
static volatile gboolean seen_data = FALSE;

static gboolean  dialog_open   = FALSE;   // the Reconnect/Exit dialog is up
static gboolean  reconnecting  = FALSE;   // a reconnect attempt is in progress
static guint     watchdog_id   = 0;

// Cancels the dialog currently on screen.  A GtkAlertDialog started with a
// GCancellable can be withdrawn without a click, which is what lets the radio
// coming back by itself take the question away again (see watchdog_cb).
static GCancellable *dialog_cancel = NULL;

// Test hook, set from MACHPSDR_RECONNECT_TEST in radio.c: answer the dialog
// without a click.  RECONNECT_ASK (the default) means "ask the operator".
static int auto_answer = RECONNECT_ASK;

void reconnect_set_auto(int button) {
  auto_answer = button;
}

static gint monotonic_sec(void) {
  return (gint)(g_get_monotonic_time() / G_USEC_PER_SEC);
}

void reconnect_note_data(void) {
  g_atomic_int_set(&last_data_sec, monotonic_sec());
  seen_data = TRUE;
}

// Perform the actual per-protocol hardware re-initialisation.  Runs on the GTK
// main thread (from the dialog response).  Returns TRUE if the link looks alive
// again; a FALSE / still-dead device is caught by the watchdog, which re-shows
// the dialog after the next timeout.
static gboolean do_reconnect(RADIO *r) {
  gboolean ok = FALSE;

  log_info("reconnect: attempting to re-establish the %s link\n",
          r->discovered!=NULL ? r->discovered->name : "device");

  switch(r->discovered->protocol) {
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      {
      RECEIVER *hwrx = radio_soapy_hw_receiver(r);
      ok = (hwrx != NULL) && soapy_protocol_reconnect(hwrx);
      }
      break;
#endif
    case PROTOCOL_1:
      protocol1_reconnect();
      ok = TRUE;   // network restart is fire-and-forget; watchdog re-checks
      break;
    case PROTOCOL_2:
      protocol2_reconnect();
      ok = TRUE;
      break;
    default:
      break;
  }

  // Grant a fresh grace period so the watchdog does not immediately re-fire
  // while the restarted stream spins back up.  seen_data is kept TRUE so that,
  // if data never resumes, the watchdog will pop the dialog again.
  g_atomic_int_set(&last_data_sec, monotonic_sec());
  return ok;
}

// The two branches the dialog's buttons take.  Kept separate from the GTK
// callback so the MACHPSDR_RECONNECT_TEST hook runs *exactly* what a click runs
// and cannot drift into testing something else.
static void reconnect_answer(int btn) {
  if(btn == RECONNECT_BTN_RECONNECT) {
    reconnecting = TRUE;
    do_reconnect(radio);
    reconnecting = FALSE;
    dialog_open = FALSE;
  } else if(btn == RECONNECT_BTN_EXIT) {   // save state, stop cleanly and quit
    main_delete(NULL);
  } else {                  // dismissed — let the watchdog ask again
    dialog_open = FALSE;
    // Re-arm the grace period: without it the watchdog re-raises the dialog on
    // its very next tick, one second later.  If the dialog can never be shown
    // (an unmappable parent), that is a new modal window every second.
    g_atomic_int_set(&last_data_sec, monotonic_sec());
  }
}

// GTK4: GtkMessageDialog is deprecated; GtkAlertDialog is async and returns the
// chosen button index (0 = Reconnect, 1 = Exit, -1 = dismissed/cancelled).
static void on_dialog_response(GObject *src, GAsyncResult *res, gpointer data) {
  GError *error = NULL;
  int btn = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &error);

  // Log which way it went.  A dialog that answers itself — a dismissal, a
  // cancellation, a parent GTK could not present it over — is otherwise
  // indistinguishable in the log from an operator's click, and that ambiguity
  // is what made this path's behaviour look non-deterministic.
  log_info("reconnect: lost-link dialog answered %d (%s)\n", btn,
           error != NULL ? error->message :
           btn == RECONNECT_BTN_RECONNECT ? "Reconnect" :
           btn == RECONNECT_BTN_EXIT      ? "Exit" : "dismissed");
  if(error != NULL) g_error_free(error);

  g_clear_object(&dialog_cancel);
  reconnect_answer(btn);
}

static void show_reconnect_dialog(RADIO *r) {
  const char *name = (r->discovered!=NULL) ? r->discovered->name : "device";

  dialog_open = TRUE;
  g_clear_object(&dialog_cancel);
  dialog_cancel = g_cancellable_new();

  GtkAlertDialog *dialog = gtk_alert_dialog_new("Connection to the SDR was lost");
  char detail[256];
  g_snprintf(detail, sizeof detail,
      "The receiver (%s) stopped sending data.\n\n"
      "Try to reconnect, or exit the application?", name);
  gtk_alert_dialog_set_detail(dialog, detail);
  const char *buttons[] = { "Reconnect", "Exit", NULL };
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_set_modal(dialog, TRUE);
  gtk_alert_dialog_choose(dialog, GTK_WINDOW(main_window), dialog_cancel,
                          on_dialog_response, NULL);
  g_object_unref(dialog);
  // Toplevel count: the one cheap way to see, in a log, whether a dialog really
  // went away again rather than merely stopping talking to us.
  log_info("reconnect: lost-link dialog shown (%u toplevel windows)\n",
            g_list_model_get_n_items(gtk_window_get_toplevels()));
}

static gboolean watchdog_cb(gpointer data) {
  RADIO *r = radio;

  if(r == NULL || r->discovered == NULL) return TRUE;
  if(reconnecting)                       return TRUE;

  // The link can heal on its own — a radio power-cycled, a switch rebooted, a
  // Wi-Fi drop that came back.  Nothing was ever stopped, so data simply
  // resumes; but the dialog is MODAL, so without this the operator is left
  // staring at a question about a link that is already up, with the whole UI
  // locked behind it and nothing to do but quit the application.  Withdraw it.
  if(dialog_open) {
    if(seen_data && monotonic_sec() - g_atomic_int_get(&last_data_sec) < DISCONNECT_TIMEOUT_SEC) {
      log_info("reconnect: data resumed - withdrawing the lost-link dialog\n");
      if(dialog_cancel != NULL) g_cancellable_cancel(dialog_cancel);
    }
    return TRUE;
  }

  if(!seen_data)                         return TRUE;   // not streaming yet

  // The fake test device has no hardware to lose.
  if(r->discovered->protocol == PROTOCOL_FAKE) return TRUE;

  // While transmitting (half-duplex SoapySDR pauses the RX stream) no RX data
  // flows by design - that is not a disconnect.  Keep the clock fresh so the
  // grace period restarts cleanly the moment we return to receive.
  if(isTransmitting(r)) {
    g_atomic_int_set(&last_data_sec, monotonic_sec());
    return TRUE;
  }

  gint gap = monotonic_sec() - g_atomic_int_get(&last_data_sec);
  if(gap >= DISCONNECT_TIMEOUT_SEC) {
    log_info("reconnect: no data for %d s - assuming the link is down\n", gap);
    if(auto_answer != RECONNECT_ASK) {
      log_info("reconnect: test hook answers '%s' (no dialog)\n",
               auto_answer == RECONNECT_BTN_EXIT ? "Exit" : "Reconnect");
      dialog_open = TRUE;          // reconnect_answer() clears it, as a click would
      reconnect_answer(auto_answer);
      return TRUE;
    }
    show_reconnect_dialog(r);
  }
  return TRUE;
}

void reconnect_init(void) {
  // Re-arm the clock so a restart does not immediately look stale.
  g_atomic_int_set(&last_data_sec, monotonic_sec());
  if(watchdog_id == 0) {
    watchdog_id = g_timeout_add(WATCHDOG_INTERVAL_MS, watchdog_cb, NULL);
  }
}
