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
  gtk_stack_add_titled(GTK_STACK(stack),child,title,title);
  pages[n_pages++]=child;
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
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
  gchar *text=NULL;
  gtk_container_child_get(GTK_CONTAINER(stack),child,"title",&text,NULL);
  if(text==NULL) return;
  if(strncmp("RX",text,2)==0) {
    int rx=atoi(&text[3]);
    update_receiver_dialog(radio->receiver[rx]);
  }
  else if(strncmp("TX",text,2)==0) {
    update_transmitter_dialog(radio->transmitter);
  }
  else if(strncmp("DMIX",text,4)==0) {
    update_transmitter_dialog(radio->transmitter);
  }
  else if(strncmp("OC",text,2)==0) {
    update_oc_dialog(radio);
  }
  g_free(text);
}

GtkWidget *create_configure_dialog(RADIO *radio,int tab) {
  int i;
  gchar title[64];

  g_snprintf((gchar *)&title,sizeof(title),"Linux HPSDR: %s %s",radio->discovered->name,inet_ntoa(radio->discovered->info.network.address.sin_addr));

  GtkWidget *dialog=gtk_dialog_new();
  gtk_widget_set_name(dialog,"config-dialog");
  gtk_window_set_transient_for(GTK_WINDOW(dialog),GTK_WINDOW(main_window));
  gtk_window_set_title(GTK_WINDOW(dialog),title);
  g_signal_connect (dialog,"delete_event",G_CALLBACK(delete_event),(gpointer)radio);

  GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  n_pages=0;
  stack=gtk_stack_new();
  gtk_widget_set_name(stack,"config-stack");
  gtk_stack_set_transition_type(GTK_STACK(stack),GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand(stack,TRUE);
  gtk_widget_set_vexpand(stack,TRUE);

  GtkWidget *sidebar=gtk_stack_sidebar_new();
  gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar),GTK_STACK(stack));

  add_page(create_radio_dialog(radio),"Radio");
  add_page(create_oc_dialog(radio),"OC");
  add_page(create_xvtr_dialog(radio),"XVTR");

  for(i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]!=NULL) {
      g_snprintf((gchar *)&title,sizeof(title),"RX-%d",radio->receiver[i]->channel);
      add_page(create_receiver_dialog(radio->receiver[i]),title);
    }
  }

  for(i=0; i < MAX_DIVERSITY_MIXERS; i++) {
    if(radio->divmixer[i]!=NULL) {
      g_snprintf((gchar *)&title,sizeof(title),"DMIX-%d",radio->divmixer[i]->id );
      add_page(create_diversity_dialog(radio->divmixer[i]),title);
    }
  }

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

  add_page(create_about_dialog(radio),"About");

  GtkWidget *hbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_name(hbox,"config-body");
  gtk_box_pack_start(GTK_BOX(hbox),sidebar,FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(hbox),stack,TRUE,TRUE,0);

  gtk_container_add(GTK_CONTAINER(content),hbox);
  if(tab>=0 && tab<n_pages) {
    gtk_stack_set_visible_child(GTK_STACK(stack),pages[tab]);
  }
  gtk_widget_show_all(dialog);

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
