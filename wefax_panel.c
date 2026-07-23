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

#include "wefax_panel.h"
#include "wefax_decoder.h"
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
  GdkPixbuf *pb;            // latest decoded image (owned)
  guint      timer;
  char       last_status[64];
  int        last_line;
  // geometry of the last-drawn image, for click→phase mapping
  int        draw_ox, draw_w;
} WefaxPanel;

// Draw the (tall, scrolling) image scaled to fit the drawing area width,
// bottom-anchored so the newest lines are always visible.
static void on_draw(GtkDrawingArea *da, cairo_t *cr, int aw, int ah, gpointer data) {
  WefaxPanel *p = data;
  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_paint(cr);
  p->draw_ox = 0; p->draw_w = aw;
  if (p->pb == NULL) return;
  int iw = gdk_pixbuf_get_width(p->pb);
  int ih = gdk_pixbuf_get_height(p->pb);
  double s = (double)aw / iw;            // fit to width
  double dh = ih * s;
  double oy = dh > ah ? (ah - dh) : 0.0; // bottom-anchor when taller than the view
  cairo_save(cr);
  cairo_translate(cr, 0, oy);
  cairo_scale(cr, s, s);
  gdk_cairo_set_source_pixbuf(cr, p->pb, 0, 0);
  // The image is wide (~1810 px, native IOC576) and usually shrunk to fit the
  // panel; smooth downscaling keeps the thin chart lines instead of dropping
  // them as nearest-neighbour would.
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_restore(cr);
}

// Click on the image → set that column as the left margin (manual phasing).
// GTK4: GtkGestureClick "pressed" (x,y in widget coords).
static void on_click(GtkGestureClick *gesture, int n_press, double px, double py, gpointer data) {
  WefaxPanel *p = data;
  if (p->draw_w <= 0) return;
  double frac = (px - p->draw_ox) / (double)p->draw_w;
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

static void lpm_changed(GtkComboBox *c, gpointer data) {
  int idx = gtk_combo_box_get_active(c);
  if (idx >= 0 && idx < N_LPM) {
    wefax_decoder_set_lpm(LPM_ENTRIES[idx]);
    if (radio) radio->wefax_lpm = LPM_ENTRIES[idx];
  }
}
static void ioc_changed(GtkComboBox *c, gpointer data) {
  int idx = gtk_combo_box_get_active(c);
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

static void save_clicked(GtkButton *b, gpointer data) {
  WefaxPanel *p = data;
  if (p->pb == NULL) return;
  char dir[512], path[600];
  g_snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr/wefax", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  time_t now = time(NULL);
  struct tm tmv; gmtime_r(&now, &tmv);
  char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  g_snprintf(path, sizeof(path), "%s/wefax_%s.png", dir, ts);
  GError *err = NULL;
  if (gdk_pixbuf_save(p->pb, path, "png", &err, NULL)) {
    log_info("WEFAX: saved %s\n", path);
  } else {
    log_error("WEFAX: save failed: %s\n", err ? err->message : "?");
    if (err) g_error_free(err);
  }
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
  GtkWidget *lpm = gtk_combo_box_text_new();
  int lpm_idx = 2;
  for (int i = 0; i < N_LPM; i++) {
    char t[8]; g_snprintf(t, sizeof(t), "%d", LPM_ENTRIES[i]);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(lpm), t);
    if (LPM_ENTRIES[i] == lpm0) lpm_idx = i;
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(lpm), lpm_idx);
  g_signal_connect(lpm, "changed", G_CALLBACK(lpm_changed), p);
  gtk_box_append(GTK_BOX(bar),lpm);

  gtk_box_append(GTK_BOX(bar),gtk_label_new("IOC:"));
  GtkWidget *ioc = gtk_combo_box_text_new();
  int ioc_idx = 0;
  for (int i = 0; i < N_IOC; i++) {
    char t[8]; g_snprintf(t, sizeof(t), "%d", IOC_ENTRIES[i]);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ioc), t);
    if (IOC_ENTRIES[i] == ioc0) ioc_idx = i;
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(ioc), ioc_idx);
  g_signal_connect(ioc, "changed", G_CALLBACK(ioc_changed), p);
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
  GtkWidget *hint=gtk_label_new("(click image to set left margin)");
  gtk_widget_set_hexpand(hint,TRUE);
  gtk_widget_set_halign(hint,GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(sbar),hint);
  gtk_box_append(GTK_BOX(box),sbar);

  // Image area (click sets the phase / left margin).
  p->area = gtk_drawing_area_new();
  gtk_widget_set_size_request(p->area, 400, 300);
  gtk_widget_set_hexpand(p->area, TRUE);
  gtk_widget_set_vexpand(p->area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->area), on_draw, p, NULL);
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
