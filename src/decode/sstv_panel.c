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
#include "time_compat.h"   // <time.h> + gmtime_r() on Windows
#include <math.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"

#include "sstv_panel.h"
#include "sstv_decoder.h"
#include "sstv_encoder.h"
#include "image_save.h"
#include "gpu_image.h"
#include "log.h"

extern RADIO *radio;   // global application state (persisted SSTV settings)

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
  // --- transmit ---
  GtkWidget *tx_mode;       // TX mode combo (concrete modes, no Auto)
  GtkWidget *tx_send;       // Send / Stop toggle button
  GtkWidget *tx_progress;   // TX progress bar
  GtkWidget *tx_file_lbl;   // loaded-file name
  GdkPixbuf *tx_img;        // image to transmit (owned), NULL until loaded
  gboolean   was_txing;     // to detect the TX→idle edge in tick()
} SstvPanel;

// GPU path (GpuImage, letterboxed nearest-neighbour): pull the pixbuf to show.
// Show the decoded image normally, but preview the loaded TX image while
// transmitting (or when nothing has been received yet).
static GdkPixbuf *on_source(gpointer data) {
  SstvPanel *p = data;
  GdkPixbuf *src = p->pb;
  if ((sstv_tx_active() || src == NULL) && p->tx_img != NULL) src = p->tx_img;
  return src;
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

  // Transmit progress + Send/Stop button state.
  gboolean txing = sstv_tx_active();
  if (txing) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->tx_progress), sstv_tx_progress());
    gtk_widget_queue_draw(p->area);   // keep the TX preview live
  }
  if (txing != p->was_txing) {        // TX just started or finished
    gtk_button_set_label(GTK_BUTTON(p->tx_send), txing ? "Stop" : "Send");
    gtk_widget_set_sensitive(p->tx_mode, !txing);
    if (!txing) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->tx_progress), 0.0);
    p->was_txing = txing;
  }
  return G_SOURCE_CONTINUE;
}

static void mode_changed(GtkDropDown *combo, GParamSpec *ps, gpointer data) {
  int idx = (int)gtk_drop_down_get_selected(combo);
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

// GTK4: GtkFileDialog is async — the chosen path arrives in this finish
// callback (mirrors the Load flow).
static void save_done(GObject *src, GAsyncResult *res, gpointer data) {
  SstvPanel *p = data;
  GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (gf == NULL) return;   // cancelled / error
  if (p->pb == NULL) { g_object_unref(gf); return; }
  char *path = g_file_get_path(gf);
  g_object_unref(gf);
  GError *err = NULL;
  if (gdk_pixbuf_save(p->pb, path, "png", &err, NULL)) {
    log_info("SSTV: saved %s\n", path);
  } else {
    log_error("SSTV: save failed: %s\n", err ? err->message : "?");
    if (err) g_error_free(err);
  }
  g_free(path);
}

static void save_clicked(GtkButton *b, gpointer data) {
  SstvPanel *p = data;
  if (p->pb == NULL) return;
  GtkWidget *top = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
  // Suggested filename + default folder (created if missing).
  char dir[512];
  g_snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr/sstv", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  time_t now = time(NULL);
  struct tm tmv; gmtime_r(&now, &tmv);
  char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  char name[64];
  g_snprintf(name, sizeof(name), "sstv_%s.png", ts);

  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Save SSTV image");
  gtk_file_dialog_set_initial_name(dlg, name);
  GFile *folder = g_file_new_for_path(dir);
  gtk_file_dialog_set_initial_folder(dlg, folder);
  g_object_unref(folder);
  GtkFileFilter *filt = gtk_file_filter_new();
  gtk_file_filter_set_name(filt, "PNG image");
  gtk_file_filter_add_mime_type(filt, "image/png");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filt);
  g_object_unref(filt);
  gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
  g_object_unref(filters);
  gtk_file_dialog_save(dlg, GTK_WINDOW(top), NULL, save_done, p);
  g_object_unref(dlg);
}

// --- auto-save --------------------------------------------------------------
// On a busy SSTV frequency the next picture wipes the last one the moment its
// VIS header arrives, so the decoder writes the outgoing one out itself.
static void autosave_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  if (radio) radio->sstv_autosave = on;
  sstv_decoder_set_autosave(on, radio ? radio->sstv_save_dir : NULL);
}

// The dialog outlives the click and the panel can be closed while it is open,
// so this holds a ref on the button rather than the panel struct.
static void folder_done(GObject *src, GAsyncResult *res, gpointer data) {
  GtkWidget *btn = data;
  GFile *gf = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (gf != NULL) {
    char *path = g_file_get_path(gf);
    g_object_unref(gf);
    if (path != NULL && radio != NULL) {
      g_strlcpy(radio->sstv_save_dir, path, sizeof(radio->sstv_save_dir));
      sstv_decoder_set_autosave(radio->sstv_autosave, radio->sstv_save_dir);
      gtk_widget_set_tooltip_text(btn, radio->sstv_save_dir);
      log_info("SSTV: save folder %s\n", radio->sstv_save_dir);
    }
    g_free(path);
  }
  g_object_unref(btn);
}

static void folder_clicked(GtkButton *b, gpointer data) {
  GtkWidget *top = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
  char dir[512];
  image_save_folder(dir, sizeof(dir), radio ? radio->sstv_save_dir : NULL, "sstv");
  g_mkdir_with_parents(dir, 0755);
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Folder for saved SSTV pictures");
  GFile *folder = g_file_new_for_path(dir);
  gtk_file_dialog_set_initial_folder(dlg, folder);
  g_object_unref(folder);
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(top), NULL, folder_done,
                                g_object_ref(b));
  g_object_unref(dlg);
}

// --- transmit ---------------------------------------------------------------
// Load an image to transmit (any format GdkPixbuf reads; scaled to the mode at
// encode time).
// GTK4: GtkFileChooserDialog is deprecated; GtkFileDialog is async — the
// accept path runs in this finish callback.
static void load_done(GObject *src, GAsyncResult *res, gpointer data) {
  SstvPanel *p = data;
  GFile *gf = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (gf == NULL) return;   // cancelled / error
  char *path = g_file_get_path(gf);
  g_object_unref(gf);
  GError *err = NULL;
  GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
  if (pb != NULL) {
    if (p->tx_img != NULL) g_object_unref(p->tx_img);
    p->tx_img = pb;
    char *base = g_path_get_basename(path);
    gtk_label_set_text(GTK_LABEL(p->tx_file_lbl), base);
    g_free(base);
    gtk_widget_set_sensitive(p->tx_send, TRUE);
    gtk_widget_queue_draw(p->area);
    log_info("SSTV TX: loaded %s (%dx%d)\n", path,
             gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
  } else {
    log_error("SSTV TX: load failed: %s\n", err ? err->message : "?");
    if (err) g_error_free(err);
  }
  g_free(path);
}

static void load_clicked(GtkButton *b, gpointer data) {
  SstvPanel *p = data;
  GtkWidget *top = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Load image to transmit");
  GtkFileFilter *filt = gtk_file_filter_new();
  gtk_file_filter_set_name(filt, "Images");
  gtk_file_filter_add_mime_type(filt, "image/*");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filt);
  g_object_unref(filt);
  gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
  g_object_unref(filters);
  gtk_file_dialog_open(dlg, GTK_WINDOW(top), NULL, load_done, p);
  g_object_unref(dlg);
}

static void send_clicked(GtkButton *b, gpointer data) {
  SstvPanel *p = data;
  if (sstv_tx_active()) { sstv_tx_stop(); return; }
  if (p->tx_img == NULL) return;
  int idx = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(p->tx_mode));
  // The TX combo lists the concrete modes only (MODE_ENTRIES[1..]).
  if (idx < 0) idx = 0;
  int vis = MODE_ENTRIES[idx + 1].vis;
  if (!sstv_tx_prepare(vis, p->tx_img)) {
    log_error("SSTV TX: prepare failed\n");
    return;
  }
  sstv_tx_start();
}

static void on_destroy(GtkWidget *w, gpointer data) {
  SstvPanel *p = data;
  if (sstv_tx_active()) sstv_tx_stop();
  if (p->timer) g_source_remove(p->timer);
  if (p->pb) g_object_unref(p->pb);
  if (p->tx_img) g_object_unref(p->tx_img);
  g_free(p);
}

GtkWidget *sstv_panel_create(void) {
  SstvPanel *p = g_new0(SstvPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  // Breathing room so the rows don't sit flush against the panel edges (as in
  // the FT8 panel).
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  // Toolbar row.
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkStringList *mode_sl = gtk_string_list_new(NULL);
  for (int i = 0; i < N_MODE_ENTRIES; i++)
    gtk_string_list_append(mode_sl, MODE_ENTRIES[i].name);
  GtkWidget *mode = gtk_drop_down_new(G_LIST_MODEL(mode_sl), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(mode), 0);
  g_signal_connect(mode, "notify::selected", G_CALLBACK(mode_changed), p);
  gtk_box_append(GTK_BOX(bar),gtk_label_new("Mode:"));
  gtk_box_append(GTK_BOX(bar),mode);

  GtkWidget *sm = gtk_button_new_with_label("Slant −");
  GtkWidget *sp = gtk_button_new_with_label("Slant +");
  p->slant_lbl = gtk_label_new("slant +0 ppm");
  g_signal_connect(sm, "clicked", G_CALLBACK(slant_minus), p);
  g_signal_connect(sp, "clicked", G_CALLBACK(slant_plus), p);
  gtk_box_append(GTK_BOX(bar),sm);
  gtk_box_append(GTK_BOX(bar),p->slant_lbl);
  gtk_box_append(GTK_BOX(bar),sp);

  GtkWidget *save = gtk_button_new_with_label("Save");
  GtkWidget *clr  = gtk_button_new_with_label("Clear");
  g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), p);
  g_signal_connect(clr,  "clicked", G_CALLBACK(clear_clicked), p);
  gtk_box_append(GTK_BOX(bar),clr);
  gtk_box_append(GTK_BOX(bar),save);

  gboolean as0 = radio ? radio->sstv_autosave : TRUE;
  GtkWidget *asb = gtk_check_button_new_with_label("Auto-save");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(asb), as0);
  gtk_widget_set_tooltip_text(asb,
      "Write each picture to disk before the next transmission overwrites it. "
      "Clear does not save.");
  g_signal_connect(asb, "toggled", G_CALLBACK(autosave_toggled), p);
  gtk_box_append(GTK_BOX(bar), asb);

  GtkWidget *fbtn = gtk_button_new_with_label("Folder…");
  char dir0[512];
  image_save_folder(dir0, sizeof(dir0), radio ? radio->sstv_save_dir : NULL, "sstv");
  gtk_widget_set_tooltip_text(fbtn, dir0);
  g_signal_connect(fbtn, "clicked", G_CALLBACK(folder_clicked), p);
  gtk_box_append(GTK_BOX(bar), fbtn);
  sstv_decoder_set_autosave(as0, radio ? radio->sstv_save_dir : NULL);

  gtk_box_append(GTK_BOX(box),bar);

  // Transmit row: mode picker (concrete modes only), image loader, Send/Stop,
  // and a progress bar.
  GtkWidget *txbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append(GTK_BOX(txbar),gtk_label_new("Tx:"));
  GtkStringList *txmode_sl = gtk_string_list_new(NULL);
  for (int i = 1; i < N_MODE_ENTRIES; i++)   // skip "Auto" — TX needs a real mode
    gtk_string_list_append(txmode_sl, MODE_ENTRIES[i].name);
  p->tx_mode = gtk_drop_down_new(G_LIST_MODEL(txmode_sl), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(p->tx_mode), 0);
  gtk_box_append(GTK_BOX(txbar),p->tx_mode);

  GtkWidget *load = gtk_button_new_with_label("Load…");
  g_signal_connect(load, "clicked", G_CALLBACK(load_clicked), p);
  gtk_box_append(GTK_BOX(txbar),load);

  p->tx_file_lbl = gtk_label_new("(no image)");
  gtk_label_set_ellipsize(GTK_LABEL(p->tx_file_lbl), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(txbar),p->tx_file_lbl);

  p->tx_send = gtk_button_new_with_label("Send");
  gtk_widget_set_sensitive(p->tx_send, FALSE);   // enabled once an image loads
  g_signal_connect(p->tx_send, "clicked", G_CALLBACK(send_clicked), p);
  gtk_box_append(GTK_BOX(txbar),p->tx_send);

  p->tx_progress = gtk_progress_bar_new();
  gtk_widget_set_valign(p->tx_progress, GTK_ALIGN_CENTER);
  // hexpand only: a vexpand here made the whole Tx row grow vertically and
  // stretched every button in the row to that height (GTK4 default valign=FILL).
  gtk_box_append(GTK_BOX(txbar),p->tx_progress); gtk_widget_set_hexpand(p->tx_progress,TRUE);
  gtk_box_append(GTK_BOX(box),txbar);

  // Image area. Small min height (was 256) so the GTK4 paned can shrink the
  // panel and leave the RF spectrum a usable height; vexpand still fills it.
  p->area = gpu_image_new(on_source, p);
  gpu_image_set_fit(GPU_IMAGE(p->area), GPU_FIT_LETTERBOX);
  gpu_image_set_filter(GPU_IMAGE(p->area), GSK_SCALING_FILTER_NEAREST);
  // Zoom in on a corner of the picture (Ctrl+wheel), pan with wheel or drag,
  // double-click back to the whole frame.
  gpu_image_set_zoomable(GPU_IMAGE(p->area), TRUE, TRUE);
  gtk_widget_set_size_request(p->area, 320, 80);
  gtk_widget_set_hexpand(p->area, TRUE);
  gtk_widget_set_vexpand(p->area, TRUE);
  gtk_box_append(GTK_BOX(box),p->area); gtk_widget_set_hexpand(p->area,TRUE); gtk_widget_set_vexpand(p->area,TRUE);

  // Status line.
  p->status = gtk_label_new("Waiting for SSTV…");
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box),p->status);

  p->last_status[0] = '\0';
  p->last_line = -1;
  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);

  return box;
}
