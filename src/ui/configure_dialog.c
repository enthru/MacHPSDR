/* Copyright (C)
* 2018 - John Melton, G0ORX/N6LYT
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
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "radio_dialog.h"
#include "cw_dialog.h"
#include "transmitter_dialog.h"
#include "puresignal_dialog.h"
#include "pa_dialog.h"
#include "eer_dialog.h"
#include "oc_dialog.h"
#include "xvtr_dialog.h"
#include "receiver_dialog.h"
#include "about_dialog.h"
#include "wideband_dialog.h"
#include "labels_dialog.h"
#ifdef MIDI
#include "midi.h"
#include "midi_dialog.h"
#endif
#include "diversity_dialog.h"
#include "recorder.h"
#ifdef FT8
#include "ft8_dialog.h"
#endif
#include "cluster_dialog.h"

int rx_base=3; // number of tabs before receivers

// The dialog now uses a GtkStack + GtkStackSidebar (vertical navigation) instead
// of a GtkNotebook, but the rest of the app still selects pages by integer index
// (rx_base + receiver count, etc.), so we keep an ordered child list to map an
// index onto its stack child.
static GtkWidget *stack;
static GtkWidget *pages[64];
static int n_pages;

static void add_page(GtkWidget *child, const char *title) {
  if(n_pages>=(int)(sizeof(pages)/sizeof(pages[0]))) return;
  // Wrap every settings page in a scroller so tall/wide pages (e.g. the TX page
  // with the CFC + Phase-Rotator + 10-band EQ frames) stay inside the window
  // instead of running off the screen edge. The scroller does not propagate its
  // child's natural size (GTK default), so the window honours its default size
  // and scrolls the overflow. Store the scroller in pages[] since that is what
  // gtk_stack_set_visible_child() must target (the child is no longer the direct
  // stack child).
  GtkWidget *scroller=gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
  // Let the scroller request its child's full natural size so the window opens
  // large enough to show the whole page. GTK caps the window to the monitor
  // work-area, and the AUTOMATIC scrollbars only appear as a fallback when the
  // page is bigger than the screen (so nothing is ever pushed off-screen).
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller),TRUE);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller),TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),child);
  gtk_widget_set_hexpand(scroller,TRUE);
  gtk_widget_set_vexpand(scroller,TRUE);
  gtk_stack_add_titled(GTK_STACK(stack),scroller,title,title);
  pages[n_pages++]=scroller;
}

// GTK4: GtkWindow emits "close-request" (delete-event was removed). Returning
// FALSE lets the default handler destroy the window; we must clear the cached
// radio->dialog (and per-receiver widget pointers) here or configure_cb's
// "dialog==NULL" guard would block reopening after the first close.
static gboolean close_request(GtkWindow *self, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int i;

  save_xvtr();
  configure_midi_device(false);
  radio->dialog=NULL;
  for(i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]!=NULL) {
      radio->receiver[i]->dialog=NULL;
      radio->receiver[i]->band_grid=NULL;
      radio->receiver[i]->mode_grid=NULL;
      radio->receiver[i]->filter_frame=NULL;
      radio->receiver[i]->filter_grid=NULL;
    }
  }
  return FALSE;
}

static void visible_child_changed(GObject *object,GParamSpec *pspec,gpointer data) {
  RADIO *radio=(RADIO *)data;
  GtkWidget *child=gtk_stack_get_visible_child(GTK_STACK(stack));
  if(child==NULL) return;
  // GTK4: child properties are gone — query the GtkStackPage for its title.
  const gchar *text=NULL;
  GtkStackPage *page=gtk_stack_get_page(GTK_STACK(stack),child);
  if(page!=NULL) text=gtk_stack_page_get_title(page);
  if(text==NULL) return;
  if(strncmp("RX",text,2)==0) {
    int rx=atoi(&text[3]);
    update_receiver_dialog(radio->receiver[rx]);
  }
  else if(strncmp("TX",text,2)==0) {
    update_transmitter_dialog(radio->transmitter);
  }
  else if(strncmp("OC",text,2)==0) {
    update_oc_dialog(radio);
  }
  // gtk_stack_page_get_title() returns a GTK-owned (transfer-none) string —
  // do NOT free it (freeing it crashed with "pointer being freed was not
  // allocated" on every page navigation).
}

GtkWidget *create_configure_dialog(RADIO *radio,int tab) {
  int i;
  gchar title[64];

  g_snprintf((gchar *)&title,sizeof(title),"Linux HPSDR: %s %s",radio->discovered->name,inet_ntoa(radio->discovered->info.network.address.sin_addr));

  // GTK4: GtkDialog is deprecated; use a plain GtkWindow with a content box.
  GtkWidget *dialog=gtk_window_new();
  gtk_widget_set_name(dialog,"config-dialog");
  gtk_window_set_transient_for(GTK_WINDOW(dialog),GTK_WINDOW(main_window));
  gtk_window_set_title(GTK_WINDOW(dialog),title);
  // Bound the window so oversized pages scroll (see the scroller in add_page)
  // rather than forcing the window past the screen edge. User-resizable from here.
  // Let both dimensions follow the page's natural size (-1,-1) so the window
  // opens exactly big enough for the page, without scrollbars; GTK still caps it
  // to the monitor work-area, and the scroller (AUTOMATIC) scrolls only when a
  // page is bigger than the screen (see add_page).
  gtk_window_set_default_size(GTK_WINDOW(dialog),-1,-1);
  g_signal_connect (dialog,"close-request",G_CALLBACK(close_request),(gpointer)radio);

  GtkWidget *content=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  gtk_window_set_child(GTK_WINDOW(dialog),content);

  n_pages=0;
  stack=gtk_stack_new();
  gtk_widget_set_name(stack,"config-stack");
  gtk_stack_set_transition_type(GTK_STACK(stack),GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand(stack,TRUE);
  gtk_widget_set_vexpand(stack,TRUE);

  GtkWidget *sidebar=gtk_stack_sidebar_new();
  gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar),GTK_STACK(stack));

  add_page(create_radio_dialog(radio),"Radio");
  add_page(create_cw_dialog(radio),"CW");
  add_page(create_oc_dialog(radio),"OC");
  add_page(create_xvtr_dialog(radio),"XVTR");

  for(i=0;i<radio->discovered->supported_receivers;i++) {
    // Skip hidden receivers (show_rx==FALSE): a diversity hidden RX or a
    // PureSignal feedback RX has no visual and must not appear as a settings
    // page — it isn't a receiver the operator tunes.
    if(radio->receiver[i]!=NULL && radio->receiver[i]->show_rx) {
      g_snprintf((gchar *)&title,sizeof(title),"RX-%d",radio->receiver[i]->channel);
      add_page(create_receiver_dialog(radio->receiver[i]),title);
    }
  }

  // The Diversity page is always present; it carries its own on/off checkbox
  // (greyed when the device can't do diversity) and the gain/phase controls.
  add_page(create_diversity_dialog(radio),"Diversity");

  if(radio->can_transmit) {
    add_page(create_transmitter_dialog(radio->transmitter),"TX");
    add_page(create_puresignal_dialog(radio->transmitter),"Pure Signal");
    add_page(create_pa_dialog(radio),"PA");
    add_page(create_eer_dialog(radio),"EER");
  }

  if(radio->wideband) {
    add_page(create_wideband_dialog(radio->wideband),"Wideband");
  }

#ifdef MIDI
  add_page(create_midi_dialog(radio),"MIDI");
#endif

  add_page(create_labels_dialog(radio),"Misc");

  add_page(create_recording_dialog(radio),"Recording");

#ifdef FT8
  add_page(create_ft8_dialog(radio),"FT8");
#endif

  add_page(create_cluster_dialog(radio),"Cluster");

  add_page(create_about_dialog(radio),"About");

  GtkWidget *hbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_name(hbox,"config-body");
  gtk_box_append(GTK_BOX(hbox),sidebar);
  gtk_box_append(GTK_BOX(hbox),stack); gtk_widget_set_hexpand(stack,TRUE); gtk_widget_set_vexpand(stack,TRUE);

  gtk_box_append(GTK_BOX(content),hbox);
  if(tab>=0 && tab<n_pages) {
    gtk_stack_set_visible_child(GTK_STACK(stack),pages[tab]);
  }
  gtk_widget_set_visible(dialog, TRUE);

  g_signal_connect(stack,"notify::visible-child",G_CALLBACK(visible_child_changed),(gpointer)radio);

  return dialog;

}

void configure_dialog_set_tab(int tab) {
  if(tab>=0 && tab<n_pages) {
    gtk_stack_set_visible_child(GTK_STACK(stack),pages[tab]);
  }
}

// Select a page by its title/name (add_page registers title as the stack child
// name), so callers don't have to compute a fragile integer index.
void configure_dialog_set_page(const char *name) {
  if(stack!=NULL && name!=NULL) {
    gtk_stack_set_visible_child_name(GTK_STACK(stack),name);
  }
}
