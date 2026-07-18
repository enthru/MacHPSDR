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
static GtkWidget *reconnect_dialog = NULL;

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

  g_print("reconnect: attempting to re-establish the %s link\n",
          r->discovered!=NULL ? r->discovered->name : "device");

  switch(r->discovered->protocol) {
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      ok = soapy_protocol_reconnect(r->receiver[0]);
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

static void on_dialog_response(GtkDialog *dialog, gint response, gpointer data) {
  gtk_widget_destroy(GTK_WIDGET(dialog));
  reconnect_dialog = NULL;

  if(response == GTK_RESPONSE_ACCEPT) {
    reconnecting = TRUE;
    do_reconnect(radio);
    reconnecting = FALSE;
    dialog_open = FALSE;
  } else {
    // Exit: save state, stop the protocol cleanly and quit (same as the
    // window-close / Quit path).
    main_delete(NULL);
  }
}

static void show_reconnect_dialog(RADIO *r) {
  const char *name = (r->discovered!=NULL) ? r->discovered->name : "device";

  dialog_open = TRUE;

  reconnect_dialog = gtk_message_dialog_new(GTK_WINDOW(main_window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_ERROR, GTK_BUTTONS_NONE,
      "Connection to the SDR was lost");
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(reconnect_dialog),
      "The receiver (%s) stopped sending data.\n\n"
      "Try to reconnect, or exit the application?", name);
  gtk_dialog_add_button(GTK_DIALOG(reconnect_dialog), "Reconnect", GTK_RESPONSE_ACCEPT);
  gtk_dialog_add_button(GTK_DIALOG(reconnect_dialog), "Exit", GTK_RESPONSE_CLOSE);
  gtk_dialog_set_default_response(GTK_DIALOG(reconnect_dialog), GTK_RESPONSE_ACCEPT);

  g_signal_connect(reconnect_dialog, "response", G_CALLBACK(on_dialog_response), NULL);
  gtk_widget_show_all(reconnect_dialog);
}

static gboolean watchdog_cb(gpointer data) {
  RADIO *r = radio;

  if(r == NULL || r->discovered == NULL) return TRUE;
  if(dialog_open || reconnecting)        return TRUE;
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
    g_print("reconnect: no data for %d s - assuming the link is down\n", gap);
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
