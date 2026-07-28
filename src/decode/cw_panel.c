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
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"

#include "cw_panel.h"
#include "cw_decoder.h"
#include "cw_encoder.h"
#include "log.h"

#define REFRESH_MS 200          // ~5 fps

typedef struct {
  GtkWidget     *status;      // "18 WPM · 600 Hz" readout
  GtkWidget     *view;        // scrolling decoded-text view
  GtkTextBuffer *buf;
  GtkTextMark   *end_mark;    // kept at the end so new text auto-scrolls into view
  guint          timer;
  GtkWidget     *tx_entry;    // free-text TX entry
} CwPanel;

static gboolean tick(gpointer data) {
  CwPanel *p = data;
  char chunk[256];
  int n = cw_decoder_get_text(chunk, sizeof(chunk));
  if (n > 0) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(p->buf, &end);
    gtk_text_buffer_insert(p->buf, &end, chunk, n);
    gtk_text_buffer_get_end_iter(p->buf, &end);
    gtk_text_buffer_move_mark(p->buf, p->end_mark, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(p->view), p->end_mark, 0.0, FALSE, 0.0, 0.0);
  }

  int wpm; double tone_hz; gboolean locked;
  cw_decoder_get_status(&wpm, &tone_hz, &locked);
  char buf[64];
  g_snprintf(buf, sizeof(buf), "%d WPM \xC2\xB7 %.0f Hz%s",
             wpm, tone_hz, locked ? "" : "  (searching…)");
  gtk_label_set_text(GTK_LABEL(p->status), buf);
  return G_SOURCE_CONTINUE;
}

static void clear_clicked(GtkButton *b, gpointer data) {
  CwPanel *p = data;
  gtk_text_buffer_set_text(p->buf, "", 0);
  cw_decoder_reset();
}

// TX operating row -----------------------------------------------------------

static void mem_clicked(GtkButton *b, gpointer data) {
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "cw-mem-index"));
  if (radio == NULL || idx < 0 || idx >= CW_N_MEMORIES) return;
  cw_tx_send_text(radio->cw_memory[idx]);
}

static void tx_send_entry(CwPanel *p) {
  const char *text = gtk_editable_get_text(GTK_EDITABLE(p->tx_entry));
  if (text == NULL || text[0] == '\0') return;
  if (cw_tx_send_text(text)) gtk_editable_set_text(GTK_EDITABLE(p->tx_entry), "");
}

static void send_clicked(GtkButton *b, gpointer data) {
  tx_send_entry((CwPanel *)data);
}

static void tx_entry_activate(GtkEntry *entry, gpointer data) {
  tx_send_entry((CwPanel *)data);
}

static void stop_clicked(GtkButton *b, gpointer data) {
  cw_tx_abort();
}

static void wpm_changed(GtkSpinButton *sb, gpointer data) {
  if (radio != NULL) radio->cw_keyer_speed = gtk_spin_button_get_value_as_int(sb);
}

static void on_destroy(GtkWidget *w, gpointer data) {
  CwPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  g_free(p);
}

GtkWidget *cw_panel_create(void) {
  CwPanel *p = g_new0(CwPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  // Toolbar row: Clear + status readout.
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *clr = gtk_button_new_with_label("Clear");
  g_signal_connect(clr, "clicked", G_CALLBACK(clear_clicked), p);
  gtk_box_append(GTK_BOX(bar), clr);

  p->status = gtk_label_new("0 WPM");
  gtk_widget_set_hexpand(p->status, TRUE);
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(bar), p->status);
  gtk_box_append(GTK_BOX(box), bar);

  // Decoded-text view: non-editable, monospace, word-wrapping.
  p->view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(p->view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(p->view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(p->view), TRUE);
  gtk_widget_set_margin_start(p->view, 4);
  gtk_widget_set_margin_end(p->view, 4);
  p->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->view));

  GtkTextIter end;
  gtk_text_buffer_get_end_iter(p->buf, &end);
  p->end_mark = gtk_text_buffer_create_mark(p->buf, "cw-end", &end, FALSE);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->view);
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  // Small min height (like the SSTV image area) so the GTK4 paned can still
  // shrink the panel and leave the RF spectrum above it a usable height.
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);
  gtk_box_append(GTK_BOX(box), scroll);

  // TX operating row: message-memory buttons (non-empty only)...
  GtkWidget *mem_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  if (radio != NULL) {
    for (int i = 0; i < CW_N_MEMORIES; i++) {
      if (radio->cw_memory[i][0] == '\0') continue;
      char label[8];
      g_snprintf(label, sizeof(label), "M%d", i + 1);
      GtkWidget *mb = gtk_button_new_with_label(label);
      gtk_widget_set_tooltip_text(mb, radio->cw_memory[i]);
      g_object_set_data(G_OBJECT(mb), "cw-mem-index", GINT_TO_POINTER(i));
      g_signal_connect(mb, "clicked", G_CALLBACK(mem_clicked), NULL);
      gtk_box_append(GTK_BOX(mem_bar), mb);
    }
  }
  gtk_box_append(GTK_BOX(box), mem_bar);

  // ...plus a free-text sender, Stop, and a live WPM control.
  GtkWidget *tx_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  p->tx_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(p->tx_entry), "Free text\xE2\x80\xA6");
  gtk_widget_set_hexpand(p->tx_entry, TRUE);
  g_signal_connect(p->tx_entry, "activate", G_CALLBACK(tx_entry_activate), p);
  gtk_box_append(GTK_BOX(tx_bar), p->tx_entry);

  GtkWidget *send_b = gtk_button_new_with_label("Send");
  g_signal_connect(send_b, "clicked", G_CALLBACK(send_clicked), p);
  gtk_box_append(GTK_BOX(tx_bar), send_b);

  GtkWidget *stop_b = gtk_button_new_with_label("Stop");
  g_signal_connect(stop_b, "clicked", G_CALLBACK(stop_clicked), NULL);
  gtk_box_append(GTK_BOX(tx_bar), stop_b);

  gtk_box_append(GTK_BOX(tx_bar), gtk_label_new("WPM:"));
  GtkWidget *wpm_spin = gtk_spin_button_new_with_range(5.0, 60.0, 1.0);
  if (radio != NULL) gtk_spin_button_set_value(GTK_SPIN_BUTTON(wpm_spin), (double)radio->cw_keyer_speed);
  g_signal_connect(wpm_spin, "value-changed", G_CALLBACK(wpm_changed), NULL);
  gtk_box_append(GTK_BOX(tx_bar), wpm_spin);

  gtk_box_append(GTK_BOX(box), tx_bar);

  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);

  return box;
}
