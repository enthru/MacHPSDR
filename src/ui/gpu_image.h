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

/*
 * GpuImage — a small reusable GtkWidget that composites a GdkPixbuf as a
 * GdkTexture through the GTK4 render-node (GtkSnapshot) pipeline, i.e. on the
 * GPU, instead of the deprecated cairo gdk_cairo_set_source_pixbuf() blit.
 *
 * It follows the same "pull" model the old GtkDrawingArea draw callbacks used:
 * the widget asks a caller-supplied source function for the current pixbuf at
 * snapshot time, so the existing gtk_widget_queue_draw() call sites keep
 * driving repaints unchanged.  Optional vector overlays (grids, cursors,
 * markers) are drawn on top through gtk_snapshot_append_cairo() — the
 * non-deprecated cairo interop — so existing cairo overlay code is reused
 * verbatim.
 *
 * Backs the waterfalls (waterfall.c, wideband_waterfall.c, ft8_waterfall.c) and
 * the SSTV / WEFAX image panels.
 */

#ifndef GPU_IMAGE_H
#define GPU_IMAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GPU_TYPE_IMAGE (gpu_image_get_type())
G_DECLARE_FINAL_TYPE(GpuImage, gpu_image, GPU, IMAGE, GtkWidget)

// How the texture is fitted into the widget allocation.
typedef enum {
  GPU_FIT_STRETCH,       // fill the whole allocation, ignoring aspect (waterfalls)
  GPU_FIT_LETTERBOX,     // preserve aspect, centre, letterbox (SSTV)
  GPU_FIT_WIDTH_BOTTOM,  // scale to width, bottom-anchor a tall image (WEFAX)
} GpuFit;

// Return the pixbuf to display right now, or NULL for nothing.  The widget
// borrows the returned pixbuf (does not take a ref) and rebuilds a texture from
// it each snapshot, so an in-place-mutated pixbuf (the waterfalls) shows its
// latest contents.
typedef GdkPixbuf *(*GpuImageSource)(gpointer user_data);

// Vector overlay drawn on top of the texture, in widget coordinates.  May be
// NULL.  The cairo_t is clipped to the widget bounds and destroyed by the widget.
typedef void (*GpuImageOverlay)(cairo_t *cr, int width, int height, gpointer user_data);

GtkWidget *gpu_image_new(GpuImageSource source, gpointer user_data);
void gpu_image_set_fit(GpuImage *self, GpuFit fit);
void gpu_image_set_filter(GpuImage *self, GskScalingFilter filter);
void gpu_image_set_background(GpuImage *self, float r, float g, float b);
void gpu_image_set_overlay(GpuImage *self, GpuImageOverlay overlay);

// Interactive view for the image panels, where the picture is far bigger than
// the widget: an APT line is 2080 px wide and a WEFAX chart 1810, both squeezed
// into a few hundred, so fitting alone throws away most of what was decoded.
//
//   wheel          — scroll the image (shift: sideways)
//   Ctrl+wheel     — zoom about the pointer; zooming back out to the fit scale
//                    returns to plain fit mode, bottom-anchoring and all
//   drag           — pan, only when `drag_pan` (leave it FALSE where button 1
//                    already means something, as WEFAX's click-to-phase does)
//   double-click   — back to fit (also only with `drag_pan`, same reason)
//
// A live decoder keeps adding lines at the bottom.  While the view sits at the
// bottom it stays pinned there (new lines scroll into sight); once the operator
// scrolls up, the view holds its place instead of sliding under them.
void gpu_image_set_zoomable(GpuImage *self, gboolean on, gboolean drag_pan);

// Map widget coordinates to image (pixbuf) coordinates under the current view,
// for panels that act on a click position.  FALSE if nothing is displayed yet
// or the point lies outside the image.
gboolean gpu_image_widget_to_image(GpuImage *self, double wx, double wy,
                                   double *ix, double *iy);

G_END_DECLS

#endif
