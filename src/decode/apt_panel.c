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

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"

#include "apt_panel.h"
#include "apt_decoder.h"
#include "gpu_image.h"
#include "log.h"

extern RADIO *radio;   // global application state (persisted APT settings)

#define REFRESH_MS 400          // ~2.5 fps (APT is 2 lines/s)

// Channel view: the whole 2080-word line, or one of the two 909-word images.
static const char *CHAN_LABELS[] = { "Both", "Channel A", "Channel B" };
#define N_CHAN 3

typedef struct {
  GtkWidget *area;          // image drawing area
  GtkWidget *status;        // status label
  GtkWidget *slant_lbl;
  GdkPixbuf *pb;            // latest decoded image (owned)
  guint      timer;
  char       last_status[64];
  int        last_line;
} AptPanel;

static GdkPixbuf *on_source(gpointer data) {
  AptPanel *p = data;
  return p->pb;
}

static gboolean tick(gpointer data) {
  AptPanel *p = data;
  apt_status_t st;
  apt_decoder_get_status(&st);

  if (strcmp(st.status, p->last_status) != 0 || st.lines != p->last_line) {
    char buf[160];
    g_snprintf(buf, sizeof(buf), "%s   %d lines", st.status, st.lines);
    gtk_label_set_text(GTK_LABEL(p->status), buf);
    g_strlcpy(p->last_status, st.status, sizeof(p->last_status));
    p->last_line = st.lines;
  }

  GdkPixbuf *np = apt_decoder_get_image();
  if (np != NULL) {
    if (p->pb != NULL) g_object_unref(p->pb);
    p->pb = np;
    gtk_widget_queue_draw(p->area);
  }
  return G_SOURCE_CONTINUE;
}

static void chan_changed(GtkDropDown *c, GParamSpec *ps, gpointer data) {
  int idx = (int)gtk_drop_down_get_selected(c);
  if (idx >= 0 && idx < N_CHAN) {
    apt_decoder_set_channel(idx);
    if (radio) radio->apt_channel = idx;
  }
}

static void update_slant(AptPanel *p) {
  char b[32];
  g_snprintf(b, sizeof(b), "slant %+.0f ppm", apt_decoder_get_slant());
  gtk_label_set_text(GTK_LABEL(p->slant_lbl), b);
}
static void slant_minus(GtkButton *b, gpointer data) {
  AptPanel *p = data; apt_decoder_adjust_slant(-20.0); update_slant(p);
}
static void slant_plus(GtkButton *b, gpointer data) {
  AptPanel *p = data; apt_decoder_adjust_slant(+20.0); update_slant(p);
}
static void clear_clicked(GtkButton *b, gpointer data) { apt_decoder_reset(); }

static void save_clicked(GtkButton *b, gpointer data) {
  AptPanel *p = data;
  if (p->pb == NULL) return;
  char dir[512], path[600];
  g_snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr/apt", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  time_t now = time(NULL);
  struct tm tmv; gmtime_r(&now, &tmv);
  char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  g_snprintf(path, sizeof(path), "%s/apt_%s.png", dir, ts);
  GError *err = NULL;
  if (gdk_pixbuf_save(p->pb, path, "png", &err, NULL)) {
    log_info("APT: saved %s\n", path);
  } else {
    log_error("APT: save failed: %s\n", err ? err->message : "?");
    if (err) g_error_free(err);
  }
}

static void on_destroy(GtkWidget *w, gpointer data) {
  AptPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  if (p->pb) g_object_unref(p->pb);
  g_free(p);
}

GtkWidget *apt_panel_create(void) {
  AptPanel *p = g_new0(AptPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  // Initialise from the persisted setting and push it into the decoder, so the
  // combo and the decode agree before the operator touches anything.
  int chan0 = radio ? radio->apt_channel : 0;
  if (chan0 < 0 || chan0 >= N_CHAN) chan0 = 0;
  apt_decoder_set_channel(chan0);

  gtk_box_append(GTK_BOX(bar), gtk_label_new("View:"));
  GtkStringList *ch_sl = gtk_string_list_new(NULL);
  for (int i = 0; i < N_CHAN; i++) gtk_string_list_append(ch_sl, CHAN_LABELS[i]);
  GtkWidget *ch = gtk_drop_down_new(G_LIST_MODEL(ch_sl), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(ch), chan0);
  g_signal_connect(ch, "notify::selected", G_CALLBACK(chan_changed), p);
  gtk_box_append(GTK_BOX(bar), ch);

  GtkWidget *sm = gtk_button_new_with_label("Slant −");
  GtkWidget *sp = gtk_button_new_with_label("Slant +");
  p->slant_lbl = gtk_label_new("slant +0 ppm");
  g_signal_connect(sm, "clicked", G_CALLBACK(slant_minus), p);
  g_signal_connect(sp, "clicked", G_CALLBACK(slant_plus), p);
  gtk_box_append(GTK_BOX(bar), sm);
  gtk_box_append(GTK_BOX(bar), p->slant_lbl);
  gtk_box_append(GTK_BOX(bar), sp);

  GtkWidget *clr = gtk_button_new_with_label("Clear");
  GtkWidget *save = gtk_button_new_with_label("Save");
  g_signal_connect(clr, "clicked", G_CALLBACK(clear_clicked), p);
  g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), p);
  gtk_box_append(GTK_BOX(bar), clr);
  gtk_box_append(GTK_BOX(bar), save);
  gtk_box_append(GTK_BOX(box), bar);

  // Image area.  Same treatment as WEFAX: a tall scrolling image fit to the
  // panel width and bottom-anchored (newest lines visible), mip-mapped down so
  // the 2080-px line keeps its detail instead of dropping every other column.
  p->area = gpu_image_new(on_source, p);
  gpu_image_set_fit(GPU_IMAGE(p->area), GPU_FIT_WIDTH_BOTTOM);
  gpu_image_set_filter(GPU_IMAGE(p->area), GSK_SCALING_FILTER_TRILINEAR);
  gtk_widget_set_size_request(p->area, 400, 80);
  gtk_widget_set_hexpand(p->area, TRUE);
  gtk_widget_set_vexpand(p->area, TRUE);
  gtk_box_append(GTK_BOX(box), p->area);

  p->status = gtk_label_new("Waiting for APT…");
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), p->status);

  p->last_status[0] = '\0';
  p->last_line = -1;
  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);
  return box;
}
