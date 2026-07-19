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

#include <math.h>
#include <string.h>
#include <gtk/gtk.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"
#include "waterfall_theme.h"

#include "ft8_decoder.h"
#include "ft8_waterfall.h"

#define WF_BAND_HZ   3000.0   // audio span shown (0..3000 Hz FT8 passband)
#define WF_ROWS      160      // spectrogram history depth (pixbuf rows)
#define WF_MAXBINS   1200     // >= 3000 / (12000/4096) ≈ 1024
#define WF_HEIGHT    150      // widget height request (px)
#define WF_INTERVAL  70       // refresh period (ms) ≈ 14 fps

// Single live instance.
static GtkWidget *area   = NULL;
static GdkPixbuf *pb     = NULL;   // WF_ROWS rows × nbins cols, RGB
static int        nbins  = 0;
static guint      timer  = 0;
static float      sm_low = -20.0f; // smoothed colour-map floor / ceiling (dB)
static float      sm_high = 20.0f;

static int wf_theme(void) {
  if (radio != NULL && radio->active_receiver != NULL)
    return radio->active_receiver->waterfall_color_theme;
  return 0;
}

// Push one new spectrum row into the top of the pixbuf, scrolling the rest down.
static void push_row(const float *spec, int nb) {
  if (pb == NULL || nbins != nb) {
    if (pb != NULL) g_object_unref(pb);
    pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, nb, WF_ROWS);
    gdk_pixbuf_fill(pb, 0x000000ff);
    nbins = nb;
  }
  int stride = gdk_pixbuf_get_rowstride(pb);
  guchar *px = gdk_pixbuf_get_pixels(pb);
  memmove(px + stride, px, (size_t)stride * (WF_ROWS - 1));  // scroll down 1 row

  // Auto-scale the colour map: track the row's mean as the noise floor and give
  // it a fixed dynamic range, smoothed across frames so it doesn't flicker.
  double sum = 0.0;
  for (int i = 0; i < nb; i++) sum += spec[i];
  float mean = (float)(sum / nb);
  float tgt_low  = mean - 3.0f;
  float tgt_high = mean + 40.0f;
  sm_low  = sm_low  * 0.9f + tgt_low  * 0.1f;
  sm_high = sm_high * 0.9f + tgt_high * 0.1f;
  float span = sm_high - sm_low;
  if (span < 1.0f) span = 1.0f;

  int theme = wf_theme();
  for (int i = 0; i < nb; i++) {
    int level = (int)((spec[i] - sm_low) / span * 255.0f);
    if (level < 0)   level = 0;
    if (level > 255) level = 255;
    unsigned char r, g, b;
    get_waterfall_color(theme, level, &r, &g, &b);
    guchar *p = px + i * 3;
    p[0] = r; p[1] = g; p[2] = b;
  }
}

static gboolean tick(gpointer data) {
  float spec[WF_MAXBINS];
  int nb = ft8_decoder_get_spectrum(spec, WF_MAXBINS, NULL);
  if (nb > 0) {
    push_row(spec, nb);
    if (area != NULL) gtk_widget_queue_draw(area);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
  int W = gtk_widget_get_allocated_width(w);
  int H = gtk_widget_get_allocated_height(w);

  // Background.
  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_rectangle(cr, 0, 0, W, H);
  cairo_fill(cr);

  // Spectrogram, stretched to the widget.
  if (pb != NULL && nbins > 0) {
    cairo_save(cr);
    cairo_scale(cr, (double)W / nbins, (double)H / WF_ROWS);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
  }

  // Frequency grid + labels every 500 Hz.
  cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 10);
  for (int f = 0; f <= (int)WF_BAND_HZ; f += 500) {
    double x = (double)f / WF_BAND_HZ * W;
    cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, H);
    cairo_stroke(cr);
    if (f > 0 && f < (int)WF_BAND_HZ) {
      char lbl[8];
      snprintf(lbl, sizeof(lbl), "%d", f);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
      cairo_move_to(cr, x + 2, H - 3);
      cairo_show_text(cr, lbl);
    }
  }

  // FT8 TX offset marker (green), and its label.
  if (radio != NULL) {
    double x = (double)radio->ft8_tx_offset / WF_BAND_HZ * W;
    cairo_set_source_rgb(cr, 0.2, 0.9, 0.2);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, H);
    cairo_stroke(cr);
    char lbl[16];
    snprintf(lbl, sizeof(lbl), "TX %d", radio->ft8_tx_offset);
    cairo_move_to(cr, x + 3, 12);
    cairo_show_text(cr, lbl);
  }
  return FALSE;
}

// Left-click sets the FT8 TX offset to the clicked audio frequency (clamped to
// the usable passband), so the operator can drop TX on a clear spot.
static gboolean on_click(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  if (ev->button != 1 || radio == NULL) return FALSE;
  int W = gtk_widget_get_allocated_width(w);
  int off = (int)(ev->x / (double)W * WF_BAND_HZ);
  if (off < 200)  off = 200;
  if (off > 2800) off = 2800;
  radio->ft8_tx_offset = off;
  gtk_widget_queue_draw(w);
  return TRUE;
}

static void on_destroy(GtkWidget *w, gpointer data) {
  if (timer != 0) { g_source_remove(timer); timer = 0; }
  if (pb != NULL) { g_object_unref(pb); pb = NULL; }
  area = NULL;
  nbins = 0;
}

GtkWidget *ft8_waterfall_create(void) {
  area = gtk_drawing_area_new();
  gtk_widget_set_name(area, "ft8-waterfall");
  gtk_widget_set_size_request(area, -1, WF_HEIGHT);
  gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
  g_signal_connect(area, "button-press-event", G_CALLBACK(on_click), NULL);
  g_signal_connect(area, "destroy", G_CALLBACK(on_destroy), NULL);
  timer = g_timeout_add(WF_INTERVAL, tick, NULL);
  return area;
}
