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

#include "cw_panel.h"
#include "cw_decoder.h"
#include "log.h"

#define REFRESH_MS 200          // ~5 fps

typedef struct {
  GtkWidget     *status;      // "18 WPM · 600 Hz" readout
  GtkWidget     *view;        // scrolling decoded-text view
  GtkTextBuffer *buf;
  GtkTextMark   *end_mark;    // kept at the end so new text auto-scrolls into view
  guint          timer;
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

  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);

  return box;
}
