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

#include "net_compat.h"   // inet_ntoa(); before gtk.h on Windows
#include <gtk/gtk.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

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
#include "qo100_dialog.h"
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
#include "keybind_dialog.h"
#include "cluster_dialog.h"
#include "tci_dialog.h"

int rx_base=3; // number of tabs before receivers

// The dialog now uses a GtkStack + GtkStackSidebar (vertical navigation) instead
// of a GtkNotebook, but the rest of the app still selects pages by integer index
// (rx_base + receiver count, etc.), so we keep an ordered child list to map an
// index onto its stack child.
static GtkWidget *stack;
static GtkWidget *pages[64];
static gchar *page_search_text[64];
static int n_pages;

static GtkWidget *search_entry;
static GtkWidget *search_empty;

// Build one case-folded search document per page.  Walking the widget tree
// catches frame titles, field captions, checkbox/button labels and tooltips,
// so an operator can search for the setting itself ("sample rate", "AGC",
// "beacon"), not merely guess which section contains it.
static void append_search_text(GtkWidget *widget,GString *text) {
  GString *own=g_string_new(NULL);
  if(GTK_IS_LABEL(widget)) {
    const char *label=gtk_label_get_text(GTK_LABEL(widget));
    if(label!=NULL && *label!='\0') g_string_append_printf(own," %s",label);
  }

  const char *tooltip=gtk_widget_get_tooltip_text(widget);
  if(tooltip!=NULL) {
    g_string_append_printf(own," %s",tooltip);
  }

  if(own->len>0) {
    g_string_append(text,own->str);
    // Keep the widget's own text separately from the page-wide document: it
    // lets search_changed mark the exact captions/help-bearing controls that
    // contributed a hit.  The data dies with the widget.
    g_object_set_data_full(G_OBJECT(widget),"settings-search-text",
                           g_utf8_casefold(own->str,-1),g_free);
  }
  g_string_free(own,TRUE);

  for(GtkWidget *child=gtk_widget_get_first_child(widget);
      child!=NULL;child=gtk_widget_get_next_sibling(child)) {
    append_search_text(child,text);
  }
}

static gchar *make_search_text(GtkWidget *child,const char *title) {
  GString *text=g_string_new(title);
  append_search_text(child,text);
  gchar *folded=g_utf8_casefold(text->str,-1);
  g_string_free(text,TRUE);
  return folded;
}

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
  // Pin the page content to the top-left at its NATURAL size. The GtkStack is
  // homogeneous (every page gets the size of the biggest page — RX/TX/MIDI), so
  // without this a small page's frames hexpand/vexpand to fill that whole area
  // and look grotesquely stretched (full-width Recording box, etc.). halign/
  // valign START + explicit hexpand/vexpand FALSE stops the fill and stops the
  // upward expand-propagation from greedy descendants (entries, column views),
  // so each frame renders at its content width with empty margin to the right.
  gtk_widget_set_halign(child,GTK_ALIGN_START);
  gtk_widget_set_valign(child,GTK_ALIGN_START);
  gtk_widget_set_hexpand(child,FALSE);
  gtk_widget_set_vexpand(child,FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),child);
  gtk_widget_set_hexpand(scroller,TRUE);
  gtk_widget_set_vexpand(scroller,TRUE);
  gtk_stack_add_titled(GTK_STACK(stack),scroller,title,title);
  pages[n_pages]=scroller;
  page_search_text[n_pages]=make_search_text(child,title);
  n_pages++;
}

// Every non-empty word must occur somewhere on the page.  This makes searches
// such as "audio rate" useful without requiring the exact phrase or order.
static gboolean text_matches_all_words(const char *text,gchar **words) {
  for(int i=0;words[i]!=NULL;i++) {
    if(*words[i]!='\0' && strstr(text,words[i])==NULL) return FALSE;
  }
  return TRUE;
}

static void clear_search_highlights(GtkWidget *widget) {
  gtk_widget_remove_css_class(widget,"search-match");
  gtk_widget_remove_css_class(widget,"search-match-related");
  for(GtkWidget *child=gtk_widget_get_first_child(widget);
      child!=NULL;child=gtk_widget_get_next_sibling(child)) {
    clear_search_highlights(child);
  }
}

// Mark the field(s) which share a GtkGrid row with a matching caption.  Most
// settings pages use exactly that label/control layout.  Horizontal boxes are
// the other common row container. A matching frame title marks the frame,
// making a group-name result visible as more than a coloured word.
static void mark_related_setting(GtkWidget *match) {
  GtkWidget *node=match;
  GtkWidget *parent=gtk_widget_get_parent(node);

  while(parent!=NULL) {
    if(GTK_IS_FRAME(parent) && gtk_frame_get_label_widget(GTK_FRAME(parent))==node) {
      gtk_widget_add_css_class(parent,"search-match-related");
      break;
    }
    if(GTK_IS_GRID(parent)) {
      int column,row,width,height;
      gtk_grid_query_child(GTK_GRID(parent),node,&column,&row,&width,&height);
      gtk_widget_add_css_class(node,"search-match-related");

      // A check/radio/button carries its own caption, so it is already the
      // complete setting. In a two-column grid, treating every widget on its
      // row as related made an FPS search also light up "Panadapter Filled".
      if(!GTK_IS_CHECK_BUTTON(node) && !GTK_IS_BUTTON(node)) {
        GtkWidget *nearest=NULL;
        int nearest_column=GTK_IS_LABEL(node) ? G_MAXINT : G_MININT;

        for(GtkWidget *sibling=gtk_widget_get_first_child(parent);
            sibling!=NULL;sibling=gtk_widget_get_next_sibling(sibling)) {
          if(sibling==node) continue;
          int sibling_column,sibling_row,sibling_width,sibling_height;
          gtk_grid_query_child(GTK_GRID(parent),sibling,
                               &sibling_column,&sibling_row,
                               &sibling_width,&sibling_height);
          if(!(sibling_row<row+height && row<sibling_row+sibling_height)) continue;

          if(GTK_IS_LABEL(node)) {
            // Caption -> closest control to its right. A deliberate blank grid
            // column separates independent setting columns.
            if(sibling_column>=column+width && sibling_column<nearest_column) {
              nearest=sibling;
              nearest_column=sibling_column;
            }
          } else if(GTK_IS_LABEL(sibling) &&
                    sibling_column+sibling_width<=column &&
                    sibling_column+sibling_width>nearest_column) {
            // Tooltip-bearing control -> closest caption to its left.
            nearest=sibling;
            nearest_column=sibling_column+sibling_width;
          }
        }
        if(nearest!=NULL) gtk_widget_add_css_class(nearest,"search-match-related");
      }
      break;
    }
    if(GTK_IS_BOX(parent) &&
       !GTK_IS_BOX(node) && !GTK_IS_GRID(node) && !GTK_IS_FRAME(node) &&
       gtk_orientable_get_orientation(GTK_ORIENTABLE(parent))==GTK_ORIENTATION_HORIZONTAL) {
      for(GtkWidget *sibling=gtk_widget_get_first_child(parent);
          sibling!=NULL;sibling=gtk_widget_get_next_sibling(sibling))
        gtk_widget_add_css_class(sibling,"search-match-related");
      break;
    }
    node=parent;
    parent=gtk_widget_get_parent(node);
  }
  gtk_widget_add_css_class(match,"search-match");
}

// A multi-word page match can come from several different captions. Highlight
// each caption which supplied at least one word, plus its associated control,
// giving the eye a useful trail through a large page.
static void highlight_search_matches(GtkWidget *widget,gchar **words,gboolean active) {
  const char *text=g_object_get_data(G_OBJECT(widget),"settings-search-text");
  if(active && text!=NULL) {
    for(int i=0;words[i]!=NULL;i++) {
      if(*words[i]!='\0' && strstr(text,words[i])!=NULL) {
        mark_related_setting(widget);
        break;
      }
    }
  }

  for(GtkWidget *child=gtk_widget_get_first_child(widget);
      child!=NULL;child=gtk_widget_get_next_sibling(child)) {
    highlight_search_matches(child,words,active);
  }
}

static void search_changed(GtkSearchEntry *entry,gpointer data) {
  (void)data;
  const char *query=gtk_editable_get_text(GTK_EDITABLE(entry));
  gchar *folded=g_utf8_casefold(query,-1);
  gchar **words=g_strsplit_set(folded," \t\r\n",-1);
  gboolean active=folded[strspn(folded," \t\r\n")]!='\0';
  GtkWidget *current=gtk_stack_get_visible_child(GTK_STACK(stack));
  GtkWidget *first_match=NULL;
  gboolean current_matches=FALSE;
  int matches=0;

  for(int i=0;i<n_pages;i++) {
    gboolean match=text_matches_all_words(page_search_text[i],words);
    GtkStackPage *page=gtk_stack_get_page(GTK_STACK(stack),pages[i]);
    gtk_stack_page_set_visible(page,match);
    clear_search_highlights(pages[i]);
    highlight_search_matches(pages[i],words,active);
    if(match) {
      matches++;
      if(first_match==NULL) first_match=pages[i];
      if(pages[i]==current) current_matches=TRUE;
    }
  }

  gtk_widget_set_visible(search_empty,matches==0);
  if(first_match!=NULL && !current_matches)
    gtk_stack_set_visible_child(GTK_STACK(stack),first_match);
  g_strfreev(words);
  g_free(folded);
}

// Compose several page builders onto one tab. Each create_*_dialog() returns a
// self-contained styled root (an sui_style_page grid of frames); stacking them
// in a vertical box makes a merged tab read as several groups on one page. The
// outer page padding comes from CSS on #config-stack, so the box needs none of
// its own — only an inter-group gap matching the page rhythm. NULL children are
// skipped (a builder may legitimately return NULL when its feature is absent).
// Wrap a builder's bare page-grid in a titled frame so a merged tab labels each
// section (OC/XVTR return unframed grids — the Bands tab needs the group names).
static GtkWidget *titled(GtkWidget *child,const char *title) {
  GtkWidget *frame=gtk_frame_new(title);
  gtk_widget_set_halign(frame,GTK_ALIGN_START);
  gtk_frame_set_child(GTK_FRAME(frame),child);
  return frame;
}

static GtkWidget *merge_pages(int n,...) {
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
  va_list ap;
  va_start(ap,n);
  for(int i=0;i<n;i++) {
    GtkWidget *w=va_arg(ap,GtkWidget *);
    if(w!=NULL) gtk_box_append(GTK_BOX(box),w);
  }
  va_end(ap);
  return box;
}

// GTK4: GtkWindow emits "close-request" (delete-event was removed). Returning
// FALSE lets the default handler destroy the window; we must clear the cached
// radio->dialog (and per-receiver widget pointers) here or configure_cb's
// "dialog==NULL" guard would block reopening after the first close.
// Everything that has to happen when this window goes away, wherever the close
// came from.  It used to live inside the "close-request" handler alone, which
// GTK4 emits for the close button and for gtk_window_close() -- but NOT for
// gtk_window_destroy(), and five places in the tree close this dialog that way
// (adding or closing a receiver, adding or closing the wideband window).  Down
// those paths the transverter edits were never saved, MIDI stayed in learn mode
// with every control dead, and each receiver was left holding pointers to five
// destroyed widgets.
static void configure_dialog_cleanup(RADIO *radio) {
  int i;

  save_xvtr();
#ifdef MIDI
  configure_midi_device(false);
#endif
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
  for(i=0;i<n_pages;i++) {
    g_clear_pointer(&page_search_text[i],g_free);
    pages[i]=NULL;
  }
  n_pages=0;
  search_entry=NULL;
  search_empty=NULL;
  stack=NULL;
}

// The one way for code outside this file to close the settings window.
void configure_dialog_close(RADIO *radio) {
  GtkWidget *dialog=radio->dialog;
  if(dialog==NULL) return;
  configure_dialog_cleanup(radio);
  gtk_window_destroy(GTK_WINDOW(dialog));
}

static gboolean close_request(GtkWindow *self, gpointer data) {
  RADIO *radio=(RADIO *)data;

  configure_dialog_cleanup(radio);
  return FALSE;
}

// Layout-independent test for a physical Latin key (mirrors key_is_q() in
// receiver.c): GTK reports keyval after the active keyboard layout, so on a
// Russian layout Cmd-W arrives as Cyrillic "ц" and a plain keyval==GDK_KEY_w
// test misses it. Look up every keyval the hardware keycode produces across
// layout groups; there is normally a Latin group containing the requested key.
static gboolean key_is_latin(guint keyval,guint keycode,guint lower,guint upper) {
  if(keyval==lower || keyval==upper) return TRUE;
  GdkDisplay *display=gdk_display_get_default();
  if(!display || keycode==0) return FALSE;
  GdkKeymapKey *keys=NULL;
  guint *keyvals=NULL;
  int n=0;
  gboolean found=FALSE;
  if(gdk_display_map_keycode(display,keycode,&keys,&keyvals,&n)) {
    for(int i=0;i<n;i++) {
      if(keyvals[i]==lower || keyvals[i]==upper) { found=TRUE; break; }
    }
  }
  g_free(keys);
  g_free(keyvals);
  return found;
}

// Cmd-W (macOS) / Ctrl-W closes the Configure dialog regardless of keyboard
// layout, mirroring the main window's Cmd-Q handling. gtk_window_close() emits
// "close-request" so close_request() still runs its cleanup.
static gboolean configure_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
  (void)controller;
  if(key_is_latin(keyval,keycode,GDK_KEY_f,GDK_KEY_F) &&
     (state & (GDK_META_MASK|GDK_CONTROL_MASK))) {
    gtk_widget_grab_focus(search_entry);
    return TRUE;
  }
  if(keyval==GDK_KEY_Escape && search_entry!=NULL &&
     *gtk_editable_get_text(GTK_EDITABLE(search_entry))!='\0') {
    gtk_editable_set_text(GTK_EDITABLE(search_entry),"");
    return TRUE;
  }
  if(key_is_latin(keyval,keycode,GDK_KEY_w,GDK_KEY_W) &&
     (state & (GDK_META_MASK|GDK_ALT_MASK|GDK_CONTROL_MASK))) {
    gtk_window_close(GTK_WINDOW(data));
    return TRUE;
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
  else if(strncmp("Bands",text,5)==0) {
    // OC lives on the merged "Bands" tab now; refresh it on show as before.
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

  // Cmd-W / Ctrl-W closes the dialog on any keyboard layout (see key_is_w).
  GtkEventController *key_controller=gtk_event_controller_key_new();
  g_signal_connect(key_controller,"key-pressed",G_CALLBACK(configure_key_pressed),dialog);
  gtk_widget_add_controller(dialog,key_controller);

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

  // Page grouping is rebalanced so no tab is left near-empty and the crowded
  // Radio page is relieved. The Radio builder also constructs the audio-related
  // frames (Microphone / Audio / dBm Calibration) into a separate grid it
  // returns via create_radio_audio_dialog() — so the Radio add_page MUST run
  // before the Audio one. Sparse single-purpose pages are merged by theme:
  //   Audio  = radio-audio frames + Recording
  //   Bands  = OC + XVTR (both band-indexed hardware matrices)
  //   PA / Linearity = PA + EER + Pure Signal (TX amplifier/linearity)
  //   Network = DX Cluster + TCI
  add_page(create_radio_dialog(radio),"Radio");
  add_page(merge_pages(2,create_radio_audio_dialog(radio),
                         create_recording_dialog(radio)),"Audio");
  add_page(create_cw_dialog(radio),"CW");
  add_page(merge_pages(2,titled(create_xvtr_dialog(radio),"Transverters"),
                         titled(create_oc_dialog(radio),"Open Collector")),"Bands");
  // QO-100 gets its own tab rather than riding along with the transverters. It
  // does drive two of them, but it is a whole operating mode — converters,
  // transponder split, band-plan overlay, level reference and a beacon-tracking
  // loop — and burying that under "Bands" both hides it and overfills that page.
  // It is placed straight after Bands because the two converters are where its
  // setup begins.
  add_page(create_qo100_dialog(radio),"QO-100");

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
    add_page(merge_pages(3,create_pa_dialog(radio),
                           create_puresignal_dialog(radio->transmitter),
                           create_eer_dialog(radio)),
             "PA / Linearity");
  }

  if(radio->wideband) {
    add_page(create_wideband_dialog(radio->wideband),"Wideband");
  }

#ifdef MIDI
  add_page(create_midi_dialog(radio),"MIDI");
#endif

  // Beside MIDI because it is the same thing for the other input device: a
  // control surface the operator maps onto the radio's actions.
  add_page(create_keybind_dialog(radio),"Hotkeys");

  add_page(create_labels_dialog(radio),"Display");

#ifdef FT8
  add_page(create_ft8_dialog(radio),"FT8");
#endif

  add_page(merge_pages(2,create_cluster_dialog(radio),
                         create_tci_dialog(radio)),"Network");

  add_page(create_about_dialog(radio),"About");

  GtkWidget *nav=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  gtk_widget_set_name(nav,"config-nav");
  search_entry=gtk_search_entry_new();
  gtk_widget_set_name(search_entry,"config-search");
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry),"Search settings");
  gtk_widget_set_tooltip_text(search_entry,
      "Search setting names and show matching sections (Ctrl/Cmd+F)");
  gtk_box_append(GTK_BOX(nav),search_entry);
  search_empty=gtk_label_new("No settings found");
  gtk_widget_set_name(search_empty,"config-search-empty");
  gtk_widget_set_visible(search_empty,FALSE);
  gtk_box_append(GTK_BOX(nav),search_empty);
  gtk_widget_set_vexpand(sidebar,TRUE);
  gtk_box_append(GTK_BOX(nav),sidebar);
  g_signal_connect(search_entry,"search-changed",G_CALLBACK(search_changed),NULL);

  GtkWidget *hbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_name(hbox,"config-body");
  gtk_box_append(GTK_BOX(hbox),nav);
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

// Open the Configure dialog (creating it if needed) focused on a named page.
// Preferred over the integer-index create/set_tab pair: page merges/reordering
// no longer shift indices under callers, they just name the page they want.
void configure_dialog_open(RADIO *radio,const char *name) {
  if(radio->dialog==NULL) {
    radio->dialog=create_configure_dialog(radio,-1);
  }
  configure_dialog_set_page(name);
}
