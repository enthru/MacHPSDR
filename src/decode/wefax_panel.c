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

#include "wefax_panel.h"
#include "image_save.h"
#include "wefax_decoder.h"
#include "gpu_image.h"
#include "log.h"

extern RADIO *radio;   // global application state (persisted WEFAX settings)

#define REFRESH_MS 300          // ~3 fps (fax is slow)

static const int LPM_ENTRIES[] = { 60, 90, 120, 240 };
#define N_LPM (int)(sizeof(LPM_ENTRIES) / sizeof(LPM_ENTRIES[0]))
static const int IOC_ENTRIES[] = { 576, 288 };
#define N_IOC (int)(sizeof(IOC_ENTRIES) / sizeof(IOC_ENTRIES[0]))

typedef struct {
  GtkWidget *area;          // image drawing area
  GtkWidget *status;        // status label
  GtkWidget *slant_lbl;
  GtkWidget *contrast;      // manual exposure trim
  GtkWidget *bright;
  GdkPixbuf *pb;            // latest decoded image (owned)
  guint      timer;
  char       last_status[64];
  int        last_line;
} WefaxPanel;

// GPU path (GpuImage): the (tall, scrolling) image is fit to the widget width
// and bottom-anchored (newest lines visible) by GPU_FIT_WIDTH_BOTTOM.  The
// image is wide (~1810 px, native IOC576) and usually shrunk to fit the panel;
// TRILINEAR (mip-mapped) downscaling keeps the thin chart lines instead of
// dropping them as nearest-neighbour would.
static GdkPixbuf *on_source(gpointer data) {
  WefaxPanel *p = data;
  return p->pb;
}

// Click on the image → set that column as the left margin (manual phasing).
// GTK4: GtkGestureClick "pressed" (x,y in widget coords).
static void on_click(GtkGestureClick *gesture, int n_press, double px, double py, gpointer data) {
  WefaxPanel *p = data;
  int aw = gtk_widget_get_width(p->area);
  if (aw <= 0 || p->pb == NULL) return;
  // Ask the widget where that lands in the image: with the view zoomed or panned
  // the column under the pointer is no longer px/width, and phasing on the wrong
  // column is exactly the mistake the click is there to correct.
  double ix = 0.0;
  double frac = gpu_image_widget_to_image(GPU_IMAGE(p->area), px, py, &ix, NULL)
                  ? ix / (double)gdk_pixbuf_get_width(p->pb)
                  : px / (double)aw;
  if (frac < 0.0) frac = 0.0; if (frac > 1.0) frac = 1.0;
  wefax_decoder_nudge_phase(frac);
}

static gboolean tick(gpointer data) {
  WefaxPanel *p = data;
  wefax_status_t st;
  wefax_decoder_get_status(&st);

  if (strcmp(st.status, p->last_status) != 0 || st.line != p->last_line) {
    char buf[160];
    double afc = wefax_decoder_get_afc();
    if (st.receiving && fabs(afc) >= 30.0)
      g_snprintf(buf, sizeof(buf), "%s   %d lines   (AFC %+.0f Hz)", st.status, st.line, afc);
    else
      g_snprintf(buf, sizeof(buf), "%s   %d lines", st.status, st.line);
    gtk_label_set_text(GTK_LABEL(p->status), buf);
    g_strlcpy(p->last_status, st.status, sizeof(p->last_status));
    p->last_line = st.line;
  }

  GdkPixbuf *np = wefax_decoder_get_image();
  if (np != NULL) {
    if (p->pb != NULL) g_object_unref(p->pb);
    p->pb = np;
    gtk_widget_queue_draw(p->area);
  }
  return G_SOURCE_CONTINUE;
}

static void lpm_changed(GtkDropDown *c, GParamSpec *ps, gpointer data) {
  int idx = (int)gtk_drop_down_get_selected(c);
  if (idx >= 0 && idx < N_LPM) {
    wefax_decoder_set_lpm(LPM_ENTRIES[idx]);
    if (radio) radio->wefax_lpm = LPM_ENTRIES[idx];
  }
}
static void ioc_changed(GtkDropDown *c, GParamSpec *ps, gpointer data) {
  int idx = (int)gtk_drop_down_get_selected(c);
  if (idx >= 0 && idx < N_IOC) {
    wefax_decoder_set_ioc(IOC_ENTRIES[idx]);
    if (radio) radio->wefax_ioc = IOC_ENTRIES[idx];
  }
}
static void autostart_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  wefax_decoder_set_autostart(on);
  if (radio) radio->wefax_autostart = on;
}
static void autophase_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  wefax_decoder_set_autophase(on);
  if (radio) radio->wefax_autophase = on;
}
static void denoise_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  wefax_decoder_set_denoise(on);
  if (radio) radio->wefax_denoise = on;
}
static void invert_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  wefax_decoder_set_invert(on);
  if (radio) radio->wefax_invert = on;
}

static void update_slant(WefaxPanel *p) {
  char b[32];
  g_snprintf(b, sizeof(b), "slant %+.0f ppm", wefax_decoder_get_slant());
  gtk_label_set_text(GTK_LABEL(p->slant_lbl), b);
}
static void slant_minus(GtkButton *b, gpointer data) {
  WefaxPanel *p = data; wefax_decoder_adjust_slant(-50.0); update_slant(p);
}
static void slant_plus(GtkButton *b, gpointer data) {
  WefaxPanel *p = data; wefax_decoder_adjust_slant(+50.0); update_slant(p);
}
static void start_clicked(GtkButton *b, gpointer data) { wefax_decoder_start(); }
static void clear_clicked(GtkButton *b, gpointer data) { wefax_decoder_reset(); }

// GTK4: GtkFileDialog is async — the chosen path arrives here (mirrors SSTV/APT,
// which ask where to write rather than dropping the file somewhere by decree).
static void save_done(GObject *src, GAsyncResult *res, gpointer data) {
  GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (gf == NULL) return;                       // cancelled / error
  GdkPixbuf *pb = wefax_decoder_get_image();    // fresh, at full width
  char *path = g_file_get_path(gf);
  g_object_unref(gf);
  if (pb != NULL) {
    GError *err = NULL;
    if (gdk_pixbuf_save(pb, path, "png", &err, NULL)) {
      log_info("WEFAX: saved %s\n", path);
    } else {
      log_error("WEFAX: save failed: %s\n", err ? err->message : "?");
      if (err) g_error_free(err);
    }
    g_object_unref(pb);
  }
  g_free(path);
}

static void save_clicked(GtkButton *b, gpointer data) {
  WefaxPanel *p = data;
  if (p->pb == NULL) return;
  GtkWidget *top = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
  char dir[512];
  image_save_folder(dir, sizeof(dir), radio ? radio->wefax_save_dir : NULL, "wefax");
  g_mkdir_with_parents(dir, 0755);
  time_t now = time(NULL);
  struct tm tmv; gmtime_r(&now, &tmv);
  char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  char name[64]; g_snprintf(name, sizeof(name), "wefax_%s.png", ts);

  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Save WEFAX page");
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

// --- exposure + auto-save ---------------------------------------------------
static void levels_changed(GtkRange *r, gpointer data) {
  WefaxPanel *p = data;
  if (radio == NULL) return;
  radio->wefax_contrast = gtk_range_get_value(GTK_RANGE(p->contrast));
  radio->wefax_brightness = gtk_range_get_value(GTK_RANGE(p->bright));
  wefax_decoder_set_levels(radio->wefax_contrast, radio->wefax_brightness);
  gtk_widget_queue_draw(p->area);   // applied on output: the whole page re-maps
}

static void autosave_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  if (radio) radio->wefax_autosave = on;
  wefax_decoder_set_autosave(on, radio ? radio->wefax_save_dir : NULL);
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
      g_strlcpy(radio->wefax_save_dir, path, sizeof(radio->wefax_save_dir));
      wefax_decoder_set_autosave(radio->wefax_autosave, radio->wefax_save_dir);
      gtk_widget_set_tooltip_text(btn, radio->wefax_save_dir);
      log_info("WEFAX: save folder %s\n", radio->wefax_save_dir);
    }
    g_free(path);
  }
  g_object_unref(btn);
}

static void folder_clicked(GtkButton *b, gpointer data) {
  GtkWidget *top = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
  char dir[512];
  image_save_folder(dir, sizeof(dir), radio ? radio->wefax_save_dir : NULL, "wefax");
  g_mkdir_with_parents(dir, 0755);
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Folder for saved WEFAX pages");
  GFile *folder = g_file_new_for_path(dir);
  gtk_file_dialog_set_initial_folder(dlg, folder);
  g_object_unref(folder);
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(top), NULL, folder_done,
                                g_object_ref(b));
  g_object_unref(dlg);
}

static void on_destroy(GtkWidget *w, gpointer data) {
  WefaxPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  if (p->pb) g_object_unref(p->pb);
  g_free(p);
}

GtkWidget *wefax_panel_create(void) {
  WefaxPanel *p = g_new0(WefaxPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  // Toolbar row: LPM, IOC, auto-start, Start, Slant, Save, Clear.
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  // Initialise from the persisted settings, and push them into the decoder so it
  // matches the combos even before the operator touches anything.
  int lpm0 = radio ? radio->wefax_lpm : 120;
  int ioc0 = radio ? radio->wefax_ioc : 576;
  gboolean auto0 = radio ? radio->wefax_autostart : TRUE;
  gboolean phase0 = radio ? radio->wefax_autophase : TRUE;
  gboolean den0 = radio ? radio->wefax_denoise : TRUE;
  gboolean inv0 = radio ? radio->wefax_invert : FALSE;
  wefax_decoder_set_lpm(lpm0);
  wefax_decoder_set_ioc(ioc0);
  wefax_decoder_set_autostart(auto0);
  wefax_decoder_set_autophase(phase0);
  wefax_decoder_set_denoise(den0);
  wefax_decoder_set_invert(inv0);

  gtk_box_append(GTK_BOX(bar),gtk_label_new("LPM:"));
  GtkStringList *lpm_sl = gtk_string_list_new(NULL);
  int lpm_idx = 2;
  for (int i = 0; i < N_LPM; i++) {
    char t[8]; g_snprintf(t, sizeof(t), "%d", LPM_ENTRIES[i]);
    gtk_string_list_append(lpm_sl, t);
    if (LPM_ENTRIES[i] == lpm0) lpm_idx = i;
  }
  GtkWidget *lpm = gtk_drop_down_new(G_LIST_MODEL(lpm_sl), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(lpm), lpm_idx);
  g_signal_connect(lpm, "notify::selected", G_CALLBACK(lpm_changed), p);
  gtk_box_append(GTK_BOX(bar),lpm);

  gtk_box_append(GTK_BOX(bar),gtk_label_new("IOC:"));
  GtkStringList *ioc_sl = gtk_string_list_new(NULL);
  int ioc_idx = 0;
  for (int i = 0; i < N_IOC; i++) {
    char t[8]; g_snprintf(t, sizeof(t), "%d", IOC_ENTRIES[i]);
    gtk_string_list_append(ioc_sl, t);
    if (IOC_ENTRIES[i] == ioc0) ioc_idx = i;
  }
  GtkWidget *ioc = gtk_drop_down_new(G_LIST_MODEL(ioc_sl), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(ioc), ioc_idx);
  g_signal_connect(ioc, "notify::selected", G_CALLBACK(ioc_changed), p);
  gtk_box_append(GTK_BOX(bar),ioc);

  GtkWidget *autob = gtk_check_button_new_with_label("Auto-start");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(autob), auto0);
  g_signal_connect(autob, "toggled", G_CALLBACK(autostart_toggled), p);
  gtk_box_append(GTK_BOX(bar),autob);

  GtkWidget *phaseb = gtk_check_button_new_with_label("Auto-phase");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(phaseb), phase0);
  g_signal_connect(phaseb, "toggled", G_CALLBACK(autophase_toggled), p);
  gtk_box_append(GTK_BOX(bar),phaseb);

  GtkWidget *denb = gtk_check_button_new_with_label("Denoise");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(denb), den0);
  g_signal_connect(denb, "toggled", G_CALLBACK(denoise_toggled), p);
  gtk_box_append(GTK_BOX(bar),denb);

  GtkWidget *invb = gtk_check_button_new_with_label("Invert");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(invb), inv0);
  g_signal_connect(invb, "toggled", G_CALLBACK(invert_toggled), p);
  gtk_box_append(GTK_BOX(bar),invb);

  GtkWidget *startb = gtk_button_new_with_label("Start");
  g_signal_connect(startb, "clicked", G_CALLBACK(start_clicked), p);
  gtk_box_append(GTK_BOX(bar),startb);

  GtkWidget *clr = gtk_button_new_with_label("Clear");
  GtkWidget *save = gtk_button_new_with_label("Save");
  g_signal_connect(clr, "clicked", G_CALLBACK(clear_clicked), p);
  g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), p);
  gtk_box_append(GTK_BOX(bar),clr);
  gtk_box_append(GTK_BOX(bar),save);
  gtk_box_append(GTK_BOX(box),bar);

  // Exposure + auto-save row.  A weak or hazy chart comes out flat grey and the
  // decoder cannot fix that by itself — the tone→grey mapping is a convention,
  // not a measurement — so the operator gets the two knobs.
  GtkWidget *ebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  double c0 = (radio && radio->wefax_contrast > 0.0) ? radio->wefax_contrast : 1.0;
  double b0 = radio ? radio->wefax_brightness : 0.0;
  wefax_decoder_set_levels(c0, b0);

  gtk_box_append(GTK_BOX(ebar), gtk_label_new("Contrast:"));
  p->contrast = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.3, 3.0, 0.05);
  gtk_range_set_value(GTK_RANGE(p->contrast), c0);
  gtk_scale_set_draw_value(GTK_SCALE(p->contrast), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(p->contrast), GTK_POS_RIGHT);
  gtk_scale_set_digits(GTK_SCALE(p->contrast), 2);
  gtk_widget_set_size_request(p->contrast, 130, -1);
  g_signal_connect(p->contrast, "value-changed", G_CALLBACK(levels_changed), p);
  gtk_box_append(GTK_BOX(ebar), p->contrast);

  gtk_box_append(GTK_BOX(ebar), gtk_label_new("Brightness:"));
  p->bright = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -100.0, 100.0, 5.0);
  gtk_range_set_value(GTK_RANGE(p->bright), b0);
  gtk_scale_set_draw_value(GTK_SCALE(p->bright), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(p->bright), GTK_POS_RIGHT);
  gtk_scale_set_digits(GTK_SCALE(p->bright), 0);
  gtk_widget_set_size_request(p->bright, 130, -1);
  g_signal_connect(p->bright, "value-changed", G_CALLBACK(levels_changed), p);
  gtk_box_append(GTK_BOX(ebar), p->bright);

  gboolean as0 = radio ? radio->wefax_autosave : TRUE;
  GtkWidget *asb = gtk_check_button_new_with_label("Auto-save page");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(asb), as0);
  gtk_widget_set_tooltip_text(asb,
      "Write the page to disk when the next start tone wipes it (or the decoder "
      "is switched off). Clear does not save.");
  g_signal_connect(asb, "toggled", G_CALLBACK(autosave_toggled), p);
  gtk_box_append(GTK_BOX(ebar), asb);

  GtkWidget *fbtn = gtk_button_new_with_label("Folder…");
  char dir0[512];
  image_save_folder(dir0, sizeof(dir0), radio ? radio->wefax_save_dir : NULL, "wefax");
  gtk_widget_set_tooltip_text(fbtn, dir0);
  g_signal_connect(fbtn, "clicked", G_CALLBACK(folder_clicked), p);
  gtk_box_append(GTK_BOX(ebar), fbtn);
  wefax_decoder_set_autosave(as0, radio ? radio->wefax_save_dir : NULL);
  gtk_box_append(GTK_BOX(box), ebar);

  // Slant row.
  GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *sm = gtk_button_new_with_label("Slant −");
  GtkWidget *sp = gtk_button_new_with_label("Slant +");
  p->slant_lbl = gtk_label_new("slant +0 ppm");
  g_signal_connect(sm, "clicked", G_CALLBACK(slant_minus), p);
  g_signal_connect(sp, "clicked", G_CALLBACK(slant_plus), p);
  gtk_box_append(GTK_BOX(sbar),sm);
  gtk_box_append(GTK_BOX(sbar),p->slant_lbl);
  gtk_box_append(GTK_BOX(sbar),sp);
  GtkWidget *hint=gtk_label_new("(click: left margin · wheel: scroll · Ctrl+wheel: zoom)");
  gtk_widget_set_hexpand(hint,TRUE);
  gtk_widget_set_halign(hint,GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(sbar),hint);
  gtk_box_append(GTK_BOX(box),sbar);

  // Image area (click sets the phase / left margin).
  p->area = gpu_image_new(on_source, p);
  gpu_image_set_fit(GPU_IMAGE(p->area), GPU_FIT_WIDTH_BOTTOM);
  gpu_image_set_filter(GPU_IMAGE(p->area), GSK_SCALING_FILTER_TRILINEAR);
  // Scroll back through the chart and zoom in on it: fit-to-width squeezes an
  // 1810-px line into a few hundred, which is most of the detail gone.  No drag
  // panning here — button 1 already sets the left margin.
  gpu_image_set_zoomable(GPU_IMAGE(p->area), TRUE, FALSE);
  // Small min height: vexpand makes it fill the pane when there's room, but a
  // large min (was 300) forced the whole panel's minimum so high that the GTK4
  // paned could not shrink it, squeezing the RF spectrum above it to a sliver.
  gtk_widget_set_size_request(p->area, 400, 80);
  gtk_widget_set_hexpand(p->area, TRUE);
  gtk_widget_set_vexpand(p->area, TRUE);
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click, "pressed", G_CALLBACK(on_click), p);
  gtk_widget_add_controller(p->area, GTK_EVENT_CONTROLLER(click));
  gtk_box_append(GTK_BOX(box),p->area);

  // Status line.
  p->status = gtk_label_new("Waiting for WEFAX…");
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box),p->status);

  p->last_status[0] = '\0';
  p->last_line = -1;
  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);
  return box;
}
