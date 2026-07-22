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
#include <time.h>
#include <math.h>

#include "sstv_panel.h"
#include "sstv_decoder.h"
#include "log.h"

#define REFRESH_MS 200          // ~5 fps

// Mode combo entries → VIS codes (index-aligned with the combo appends below).
static const struct { const char *name; int vis; } MODE_ENTRIES[] = {
  { "Auto",       0 },
  { "Martin M1",  44 }, { "Martin M2",  40 },
  { "Scottie S1", 60 }, { "Scottie S2", 56 }, { "Scottie DX", 76 },
  { "Robot 36",    8 }, { "Robot 72",   12 },
  { "PD50",       93 }, { "PD90",       99 }, { "PD120",      95 },
  { "PD160",      98 }, { "PD180",      96 }, { "PD240",      97 },
};
#define N_MODE_ENTRIES (int)(sizeof(MODE_ENTRIES) / sizeof(MODE_ENTRIES[0]))

typedef struct {
  GtkWidget *area;          // image drawing area
  GtkWidget *status;        // status label
  GtkWidget *slant_lbl;     // slant readout
  GdkPixbuf *pb;            // latest decoded image (owned)
  guint      timer;
  char       last_status[64];
  int        last_line;
} SstvPanel;

// Draw the image scaled to fit the drawing area, preserving 4:3, letterboxed.
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
  SstvPanel *p = data;
  int aw = gtk_widget_get_allocated_width(w);
  int ah = gtk_widget_get_allocated_height(w);
  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_paint(cr);
  if (p->pb == NULL) return FALSE;
  int iw = gdk_pixbuf_get_width(p->pb);
  int ih = gdk_pixbuf_get_height(p->pb);
  double sx = (double)aw / iw, sy = (double)ah / ih;
  double s = sx < sy ? sx : sy;
  double dw = iw * s, dh = ih * s;
  double ox = (aw - dw) / 2.0, oy = (ah - dh) / 2.0;
  cairo_translate(cr, ox, oy);
  cairo_scale(cr, s, s);
  gdk_cairo_set_source_pixbuf(cr, p->pb, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  return FALSE;
}

static gboolean tick(gpointer data) {
  SstvPanel *p = data;
  sstv_status_t st;
  sstv_decoder_get_status(&st);

  if (strcmp(st.status, p->last_status) != 0 || st.line != p->last_line) {
    char buf[160];
    double afc = sstv_decoder_get_afc();
    if (st.receiving && fabs(afc) >= 30.0)   // hint the operator to nudge the dial
      g_snprintf(buf, sizeof(buf), "%s   %d%%   (AFC %+.0f Hz)", st.status, st.progress, afc);
    else
      g_snprintf(buf, sizeof(buf), "%s   %d%%", st.status, st.progress);
    gtk_label_set_text(GTK_LABEL(p->status), buf);
    g_strlcpy(p->last_status, st.status, sizeof(p->last_status));
    p->last_line = st.line;
  }

  // Refresh the image (cheap: a 320×256 copy).
  GdkPixbuf *np = sstv_decoder_get_image();
  if (np != NULL) {
    if (p->pb != NULL) g_object_unref(p->pb);
    p->pb = np;
    gtk_widget_queue_draw(p->area);
  }
  return G_SOURCE_CONTINUE;
}

static void mode_changed(GtkComboBox *combo, gpointer data) {
  int idx = gtk_combo_box_get_active(combo);
  if (idx < 0 || idx >= N_MODE_ENTRIES) idx = 0;
  sstv_decoder_set_mode(MODE_ENTRIES[idx].vis);
}

static void update_slant(SstvPanel *p) {
  char b[32];
  g_snprintf(b, sizeof(b), "slant %+.0f ppm", sstv_decoder_get_slant());
  gtk_label_set_text(GTK_LABEL(p->slant_lbl), b);
}

// Fine manual trim on top of the decoder's automatic slant correction.
static void slant_minus(GtkButton *b, gpointer data) {
  SstvPanel *p = data; sstv_decoder_adjust_slant(-20.0); update_slant(p);
}
static void slant_plus(GtkButton *b, gpointer data) {
  SstvPanel *p = data; sstv_decoder_adjust_slant(+20.0); update_slant(p);
}

static void clear_clicked(GtkButton *b, gpointer data) {
  sstv_decoder_reset();
}

static void save_clicked(GtkButton *b, gpointer data) {
  SstvPanel *p = data;
  if (p->pb == NULL) return;
  char dir[512], path[600];
  g_snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr/sstv", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  time_t now = time(NULL);
  struct tm tmv; gmtime_r(&now, &tmv);
  char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  g_snprintf(path, sizeof(path), "%s/sstv_%s.png", dir, ts);
  GError *err = NULL;
  if (gdk_pixbuf_save(p->pb, path, "png", &err, NULL)) {
    log_info("SSTV: saved %s\n", path);
  } else {
    log_error("SSTV: save failed: %s\n", err ? err->message : "?");
    if (err) g_error_free(err);
  }
}

static void on_destroy(GtkWidget *w, gpointer data) {
  SstvPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  if (p->pb) g_object_unref(p->pb);
  g_free(p);
}

GtkWidget *sstv_panel_create(void) {
  SstvPanel *p = g_new0(SstvPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  // Toolbar row.
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *mode = gtk_combo_box_text_new();
  for (int i = 0; i < N_MODE_ENTRIES; i++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), MODE_ENTRIES[i].name);
  gtk_combo_box_set_active(GTK_COMBO_BOX(mode), 0);
  g_signal_connect(mode, "changed", G_CALLBACK(mode_changed), p);
  gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Mode:"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), mode, FALSE, FALSE, 0);

  GtkWidget *sm = gtk_button_new_with_label("Slant −");
  GtkWidget *sp = gtk_button_new_with_label("Slant +");
  p->slant_lbl = gtk_label_new("slant +0 ppm");
  g_signal_connect(sm, "clicked", G_CALLBACK(slant_minus), p);
  g_signal_connect(sp, "clicked", G_CALLBACK(slant_plus), p);
  gtk_box_pack_start(GTK_BOX(bar), sm, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), p->slant_lbl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), sp, FALSE, FALSE, 0);

  GtkWidget *save = gtk_button_new_with_label("Save");
  GtkWidget *clr  = gtk_button_new_with_label("Clear");
  g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), p);
  g_signal_connect(clr,  "clicked", G_CALLBACK(clear_clicked), p);
  gtk_box_pack_end(GTK_BOX(bar), clr,  FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(bar), save, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

  // Image area.
  p->area = gtk_drawing_area_new();
  gtk_widget_set_size_request(p->area, 320, 256);
  gtk_widget_set_hexpand(p->area, TRUE);
  gtk_widget_set_vexpand(p->area, TRUE);
  g_signal_connect(p->area, "draw", G_CALLBACK(on_draw), p);
  gtk_box_pack_start(GTK_BOX(box), p->area, TRUE, TRUE, 0);

  // Status line.
  p->status = gtk_label_new("Waiting for SSTV…");
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), p->status, FALSE, FALSE, 0);

  p->last_status[0] = '\0';
  p->last_line = -1;
  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);

  return box;
}
