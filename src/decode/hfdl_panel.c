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

#include "hfdl_panel.h"
#include "hfdl_decoder.h"
#include "log.h"

#define REFRESH_MS 300          // ~3 fps (HFDL is a slow data link)

typedef struct {
  GtkWidget     *status;      // "listening · 192 kHz · 1234 blocks" readout
  GtkWidget     *view;        // scrolling decoded-message view
  GtkTextBuffer *buf;
  GtkTextMark   *end_mark;    // kept at the end so new text auto-scrolls into view
  guint          timer;
} HfdlPanel;

static gboolean tick(gpointer data) {
  HfdlPanel *p = data;
  char chunk[512];
  int n = hfdl_decoder_get_messages(chunk, sizeof(chunk));
  if (n > 0) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(p->buf, &end);
    gtk_text_buffer_insert(p->buf, &end, chunk, n);
    gtk_text_buffer_get_end_iter(p->buf, &end);
    gtk_text_buffer_move_mark(p->buf, p->end_mark, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(p->view), p->end_mark, 0.0, FALSE, 0.0, 0.0);
  }

  gboolean listening; int rate; glong syms;
  hfdl_decoder_get_status(&listening, &rate, &syms);
  double level = hfdl_decoder_get_level_db();
  char buf[128];
  if (listening)
    g_snprintf(buf, sizeof(buf), "signal %.0f dB \xC2\xB7 %d kHz \xC2\xB7 %ld ksym  (front-end only \xE2\x80\x94 no framing yet)",
               level, rate / 1000, syms / 1000);
  else
    g_snprintf(buf, sizeof(buf), "idle");
  gtk_label_set_text(GTK_LABEL(p->status), buf);
  return G_SOURCE_CONTINUE;
}

static void clear_clicked(GtkButton *b, gpointer data) {
  HfdlPanel *p = data;
  gtk_text_buffer_set_text(p->buf, "", 0);
  hfdl_decoder_reset();
}

static void on_destroy(GtkWidget *w, gpointer data) {
  HfdlPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  g_free(p);
}

GtkWidget *hfdl_panel_create(void) {
  HfdlPanel *p = g_new0(HfdlPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  // Toolbar row: Clear + status readout.
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append(GTK_BOX(bar), gtk_label_new("HFDL"));
  GtkWidget *clr = gtk_button_new_with_label("Clear");
  g_signal_connect(clr, "clicked", G_CALLBACK(clear_clicked), p);
  gtk_box_append(GTK_BOX(bar), clr);

  p->status = gtk_label_new("idle");
  gtk_widget_set_hexpand(p->status, TRUE);
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(bar), p->status);
  gtk_box_append(GTK_BOX(box), bar);

  // Decoded-message view: non-editable, monospace.
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
  p->end_mark = gtk_text_buffer_create_mark(p->buf, "hfdl-end", &end, FALSE);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->view);
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  // Small min height (like the CW/SSTV panels) so the GTK4 paned can still shrink
  // the panel and leave the RF spectrum above it a usable height.
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);
  gtk_box_append(GTK_BOX(box), scroll);

  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);

  return box;
}
