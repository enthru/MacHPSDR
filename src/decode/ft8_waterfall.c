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
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

#include "discovered.h"
#include "css.h"
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
#include "gpu_image.h"

#define WF_MAX_HZ    5000.0   // hard cap on the displayed audio span
#define WF_ROWS      160      // spectrogram history depth (pixbuf rows)
#define WF_MAXBINS   1800     // >= 5000 / (12000/4096) ≈ 1706
#define WF_HEIGHT    150      // widget height request (px)
#define WF_INTERVAL  70       // refresh period (ms) ≈ 14 fps

// Single live instance.
static GtkWidget *area   = NULL;
static GdkPixbuf *pb     = NULL;   // WF_ROWS rows × nbins cols, RGB
static int        nbins  = 0;
static guint      timer  = 0;
static float      sm_low = -20.0f; // smoothed colour-map floor / ceiling (dB)
static float      sm_high = 20.0f;
static double     cur_span = 3000.0;   // audio span currently displayed (Hz)

static int wf_theme(void) {
  if (radio != NULL && radio->active_receiver != NULL)
    return radio->active_receiver->waterfall_color_theme;
  return 0;
}

// Audio span to show: as wide as the DIGU receive filter (its upper edge),
// clamped to a sane [500 Hz .. 5 kHz] range.
static double wf_span_hz(void) {
  double hi = 3000.0;
  if (radio != NULL && radio->active_receiver != NULL) {
    int fl = radio->active_receiver->filter_low_a;
    int fh = radio->active_receiver->filter_high_a;
    hi = (double)(abs(fl) > abs(fh) ? abs(fl) : abs(fh));
  }
  if (hi > WF_MAX_HZ) hi = WF_MAX_HZ;
  if (hi < 500.0)     hi = 500.0;
  return hi;
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
  float hpb = 0.0f;
  int nb = ft8_decoder_get_spectrum(spec, WF_MAXBINS, &hpb);
  if (nb > 0 && hpb > 0.0f) {
    // Display only the sub-span the DIGU filter covers (<= 5 kHz).
    int span_bins = (int)(wf_span_hz() / hpb);
    if (span_bins > nb) span_bins = nb;
    if (span_bins < 1)  span_bins = 1;
    cur_span = span_bins * hpb;              // exact span the pixbuf represents
    push_row(spec, span_bins);
    if (area != NULL) gtk_widget_queue_draw(area);
  }
  return G_SOURCE_CONTINUE;
}

// GPU path: GpuImage composites this spectrogram pixbuf (stretched to the
// widget) as a texture; the grid / TX marker are drawn on top by the overlay.
static GdkPixbuf *on_source(gpointer data) {
  return (pb != NULL && nbins > 0) ? pb : NULL;
}

static void on_overlay(cairo_t *cr, int W, int H, gpointer data) {
  // Frequency grid + labels every 500 Hz across the displayed span.
  cairo_select_font_face(cr, css_ui_font(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 10);
  for (int f = 0; f <= (int)cur_span; f += 500) {
    double x = (double)f / cur_span * W;
    cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, H);
    cairo_stroke(cr);
    if (f > 0 && f < (int)cur_span) {
      char lbl[8];
      snprintf(lbl, sizeof(lbl), "%d", f);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
      cairo_move_to(cr, x + 2, H - 3);
      cairo_show_text(cr, lbl);
    }
  }

  // TX offset marker (green): a line at the base tone plus a translucent band
  // spanning the signal's occupied bandwidth, which is protocol-dependent — FT8 =
  // 8 tones × 6.25 Hz ≈ 50 Hz, FT4 = 4 tones × 20.83 Hz ≈ 83 Hz.  The tones climb
  // from the base offset, so the band runs [offset, offset+bw]; it makes picking a
  // clear TX slot honest for FT4's wider footprint.
  if (radio != NULL) {
    gboolean ft4 = radio->ft8_proto;
    // Occupied bandwidth = tones × spacing; spacing = 1/symbol_period
    // (FT8 6.25 Hz over 8 tones = 50 Hz; FT4 20.833 Hz over 4 tones ≈ 83 Hz).
    double bw = ft4 ? (4.0 * 20.8333) : (8.0 * 6.25);
    double x  = (double)radio->ft8_tx_offset / cur_span * W;
    double xb = (double)(radio->ft8_tx_offset + bw) / cur_span * W;
    cairo_set_source_rgba(cr, 0.2, 0.9, 0.2, 0.20);         // occupied-band fill
    cairo_rectangle(cr, x, 0, xb - x, H);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.9, 0.2);                // base-tone line
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, H);
    cairo_stroke(cr);
    char lbl[24];
    snprintf(lbl, sizeof(lbl), "%s %d", ft4 ? "FT4" : "TX", radio->ft8_tx_offset);
    cairo_move_to(cr, x + 3, 12);
    cairo_show_text(cr, lbl);
  }
}

// Left-click sets the FT8 TX offset to the clicked audio frequency (clamped to
// the usable passband), so the operator can drop TX on a clear spot.
// GTK4: GtkGestureClick "pressed" handler (x,y in widget coords).
static void on_click(GtkGestureClick *gesture, int n_press, double px, double py, gpointer data) {
  if (radio == NULL || area == NULL) return;
  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != 1) return;
  int W = gtk_widget_get_width(area);
  if (W <= 0) return;
  int off = (int)(px / (double)W * cur_span);
  if (off < 200)  off = 200;
  if (off > 2800) off = 2800;
  radio->ft8_tx_offset = off;
  gtk_widget_queue_draw(area);
}

static void on_destroy(GtkWidget *w, gpointer data) {
  if (timer != 0) { g_source_remove(timer); timer = 0; }
  if (pb != NULL) { g_object_unref(pb); pb = NULL; }
  area = NULL;
  nbins = 0;
}

GtkWidget *ft8_waterfall_create(void) {
  area = gpu_image_new(on_source, NULL);
  gpu_image_set_overlay(GPU_IMAGE(area), on_overlay);
  gtk_widget_set_name(area, "ft8-waterfall");
  // A non-zero minimum width so the pane is visible immediately, before the
  // 2/3 : 1/3 split position is applied (see receiver_ft8_waterfall_sync).
  gtk_widget_set_size_request(area, 140, WF_HEIGHT);

  // GTK4: left-click via a gesture controller (button masks are gone).
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
  g_signal_connect(click, "pressed", G_CALLBACK(on_click), NULL);
  gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));

  g_signal_connect(area, "destroy", G_CALLBACK(on_destroy), NULL);
  timer = g_timeout_add(WF_INTERVAL, tick, NULL);
  return area;
}
