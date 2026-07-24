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

#include "gpu_image.h"

struct _GpuImage {
  GtkWidget        parent_instance;
  GpuImageSource   source;
  GpuImageOverlay  overlay;
  gpointer         user_data;
  GpuFit           fit;
  GskScalingFilter filter;
  GdkRGBA          bg;
  int              last_w, last_h;   // to fire "resize" only on real changes
};

G_DEFINE_FINAL_TYPE(GpuImage, gpu_image, GTK_TYPE_WIDGET)

// "resize" (int width, int height): re-implements GtkDrawingArea's signal of the
// same name/signature so waterfall.c / wideband_waterfall.c keep re-allocating
// their pixbuf on size changes with no callback edits.
enum { SIG_RESIZE, N_SIGNALS };
static guint signals[N_SIGNALS];

// Wrap a GdkPixbuf's pixels in a GdkTexture without a copy where possible.
// gdk_texture_new_for_pixbuf() is itself deprecated (GTK 4.20), so build a
// GdkMemoryTexture directly from the pixel bytes — the current, non-deprecated
// path.  RGB pixbufs (the waterfalls) map to R8G8B8, RGBA to R8G8B8A8.
static GdkTexture *texture_from_pixbuf(GdkPixbuf *pb) {
  int w = gdk_pixbuf_get_width(pb);
  int h = gdk_pixbuf_get_height(pb);
  gsize stride = (gsize)gdk_pixbuf_get_rowstride(pb);
  gboolean alpha = gdk_pixbuf_get_has_alpha(pb);
  // Copy the pixels (not gdk_pixbuf_read_pixel_bytes, which shares the buffer):
  // GdkMemoryTexture requires its backing memory to stay unchanged for the
  // texture's lifetime, but the waterfall pixbufs are mutated in place every
  // frame.  A copy gives the texture an immutable snapshot — the same semantics
  // the (now deprecated) gdk_texture_new_for_pixbuf() provided.
  GBytes *bytes = g_bytes_new(gdk_pixbuf_get_pixels(pb), stride * (gsize)h);
  GdkTexture *tex = gdk_memory_texture_new(w, h,
      alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes, stride);
  g_bytes_unref(bytes);
  return tex;
}

static void gpu_image_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  GpuImage *self = GPU_IMAGE(widget);
  int w = gtk_widget_get_width(widget);
  int h = gtk_widget_get_height(widget);
  if (w <= 0 || h <= 0) return;

  const graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, w, h);

  // Background fill (the area a letterboxed / bottom-anchored image leaves bare).
  gtk_snapshot_append_color(snapshot, &self->bg, &full);

  GdkPixbuf *pb = self->source ? self->source(self->user_data) : NULL;
  if (pb != NULL) {
    GdkTexture *tex = texture_from_pixbuf(pb);
    int iw = gdk_texture_get_width(tex);
    int ih = gdk_texture_get_height(tex);
    graphene_rect_t dst;
    switch (self->fit) {
      case GPU_FIT_LETTERBOX: {
        double sx = (double)w / iw, sy = (double)h / ih;
        double s = sx < sy ? sx : sy;
        double dw = iw * s, dh = ih * s;
        graphene_rect_init(&dst, (float)((w - dw) / 2.0), (float)((h - dh) / 2.0),
                           (float)dw, (float)dh);
        break;
      }
      case GPU_FIT_WIDTH_BOTTOM: {
        double s = (double)w / iw;
        double dh = ih * s;
        double oy = dh > h ? (h - dh) : 0.0;   // bottom-anchor when taller than view
        graphene_rect_init(&dst, 0, (float)oy, (float)w, (float)dh);
        break;
      }
      case GPU_FIT_STRETCH:
      default:
        graphene_rect_init(&dst, 0, 0, (float)w, (float)h);
        break;
    }
    // Clip to the widget so a tall bottom-anchored image doesn't overdraw siblings.
    gtk_snapshot_push_clip(snapshot, &full);
    gtk_snapshot_append_scaled_texture(snapshot, tex, self->filter, &dst);
    gtk_snapshot_pop(snapshot);
    g_object_unref(tex);
  }

  if (self->overlay != NULL) {
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &full);
    self->overlay(cr, w, h, self->user_data);
    cairo_destroy(cr);
  }
}

static void gpu_image_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  GTK_WIDGET_CLASS(gpu_image_parent_class)->size_allocate(widget, width, height, baseline);
  GpuImage *self = GPU_IMAGE(widget);
  if (width != self->last_w || height != self->last_h) {
    self->last_w = width;
    self->last_h = height;
    g_signal_emit(self, signals[SIG_RESIZE], 0, width, height);
  }
}

static void gpu_image_class_init(GpuImageClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  wc->snapshot = gpu_image_snapshot;
  wc->size_allocate = gpu_image_size_allocate;

  signals[SIG_RESIZE] = g_signal_new("resize",
      G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL,
      NULL /* generic marshaller */, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

static void gpu_image_init(GpuImage *self) {
  self->fit = GPU_FIT_STRETCH;
  self->filter = GSK_SCALING_FILTER_LINEAR;
  self->bg = (GdkRGBA){0.0f, 0.0f, 0.0f, 1.0f};
  self->last_w = -1;
  self->last_h = -1;
}

GtkWidget *gpu_image_new(GpuImageSource source, gpointer user_data) {
  GpuImage *self = g_object_new(GPU_TYPE_IMAGE, NULL);
  self->source = source;
  self->user_data = user_data;
  return GTK_WIDGET(self);
}

void gpu_image_set_fit(GpuImage *self, GpuFit fit) {
  self->fit = fit;
}

void gpu_image_set_filter(GpuImage *self, GskScalingFilter filter) {
  self->filter = filter;
}

void gpu_image_set_background(GpuImage *self, float r, float g, float b) {
  self->bg = (GdkRGBA){r, g, b, 1.0f};
}

void gpu_image_set_overlay(GpuImage *self, GpuImageOverlay overlay) {
  self->overlay = overlay;
}
