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

  // Interactive view (see gpu_image_set_zoomable).  `zoom` is widget pixels per
  // image pixel, or 0 for "whatever the fit mode says"; `off_*` is the image
  // coordinate sitting at the widget's top-left corner.  Both are kept in step
  // with the fit mode even while zoom is 0, so a zoom starts from what is on
  // screen rather than jumping.
  gboolean         zoomable;
  gboolean         drag_pan;
  double           zoom;
  double           off_x, off_y;
  gboolean         follow_end;       // keep the bottom edge pinned as lines arrive
  int              img_w, img_h;     // last displayed image size (for the handlers)
  double           ptr_x, ptr_y;     // last pointer position, widget coords
  double           drag_x, drag_y;   // last drag offset, for the delta
};

#define GPU_ZOOM_MAX      32.0
#define GPU_WHEEL_PIXELS  60.0   // one wheel detent, in widget pixels

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

// Scale the fit mode alone would pick for this image in this allocation.
static double fit_scale(GpuImage *self, int iw, int ih, int w, int h) {
  if (iw <= 0 || ih <= 0) return 1.0;
  switch (self->fit) {
    case GPU_FIT_LETTERBOX: {
      double sx = (double)w / iw, sy = (double)h / ih;
      return sx < sy ? sx : sy;
    }
    case GPU_FIT_WIDTH_BOTTOM:
      return (double)w / iw;
    case GPU_FIT_STRETCH:
    default:
      return 1.0;                       // stretch has no single scale; never zoomed
  }
}

// Where the image lands in the widget, honouring the fit mode and — when the
// operator has zoomed — the pan offsets.  Also writes the offsets back while in
// fit mode, so `off_*` always describes what is on screen and a zoom can start
// from there.
static void view_geometry(GpuImage *self, int iw, int ih, int w, int h,
                          graphene_rect_t *dst) {
  double base = fit_scale(self, iw, ih, w, h);

  if (!self->zoomable || self->zoom <= 0.0 || self->fit == GPU_FIT_STRETCH) {
    switch (self->fit) {
      case GPU_FIT_LETTERBOX: {
        double dw = iw * base, dh = ih * base;
        graphene_rect_init(dst, (float)((w - dw) / 2.0), (float)((h - dh) / 2.0),
                           (float)dw, (float)dh);
        break;
      }
      case GPU_FIT_WIDTH_BOTTOM: {
        double dh = ih * base;
        double oy = dh > h ? (h - dh) : 0.0;   // bottom-anchor when taller than view
        graphene_rect_init(dst, 0, (float)oy, (float)w, (float)dh);
        break;
      }
      case GPU_FIT_STRETCH:
      default:
        graphene_rect_init(dst, 0, 0, (float)w, (float)h);
        break;
    }
    if (base > 0.0) {
      self->off_x = -dst->origin.x / base;
      self->off_y = -dst->origin.y / base;
    }
    return;
  }

  double s = self->zoom;
  double vw = w / s, vh = h / s;             // visible slice, in image pixels
  if (iw <= vw) self->off_x = (iw - vw) / 2.0;         // fits: centre it
  else if (self->off_x < 0.0) self->off_x = 0.0;
  else if (self->off_x > iw - vw) self->off_x = iw - vw;

  if (ih <= vh) self->off_y = (ih - vh) / 2.0;
  else if (self->follow_end) self->off_y = ih - vh;    // pinned to the newest lines
  else if (self->off_y < 0.0) self->off_y = 0.0;
  else if (self->off_y > ih - vh) self->off_y = ih - vh;

  graphene_rect_init(dst, (float)(-self->off_x * s), (float)(-self->off_y * s),
                     (float)(iw * s), (float)(ih * s));
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
    self->img_w = iw;
    self->img_h = ih;
    graphene_rect_t dst;
    view_geometry(self, iw, ih, w, h, &dst);
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
  self->zoom = 0.0;                  // fit mode until the operator zooms
  self->follow_end = TRUE;
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

// --- interactive view -------------------------------------------------------

// Current scale, whether it comes from the fit mode or from a zoom.
static double current_scale(GpuImage *self) {
  int w = gtk_widget_get_width(GTK_WIDGET(self));
  int h = gtk_widget_get_height(GTK_WIDGET(self));
  double base = fit_scale(self, self->img_w, self->img_h, w, h);
  return self->zoom > 0.0 ? self->zoom : base;
}

// Re-pin to the bottom once a pan has been clamped back against it, so scrolling
// down to the newest line resumes following the decode instead of freezing one
// line short of it.
static void recheck_follow(GpuImage *self, double s) {
  int h = gtk_widget_get_height(GTK_WIDGET(self));
  double vh = h / s;
  if (self->img_h > vh) self->follow_end = (self->off_y >= self->img_h - vh - 0.5);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud) {
  GpuImage *self = ud;
  self->ptr_x = x;
  self->ptr_y = y;
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud) {
  GpuImage *self = ud;
  if (!self->zoomable || self->img_w <= 0 || self->img_h <= 0) return GDK_EVENT_PROPAGATE;
  int w = gtk_widget_get_width(GTK_WIDGET(self));
  int h = gtk_widget_get_height(GTK_WIDGET(self));
  if (w <= 0 || h <= 0) return GDK_EVENT_PROPAGATE;

  double base = fit_scale(self, self->img_w, self->img_h, w, h);
  double s = self->zoom > 0.0 ? self->zoom : base;
  GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));
  // A trackpad delivers a stream of small precise deltas already in pixels; a
  // wheel delivers one ±1 per detent (see scroll_notches() in receiver.c for the
  // same distinction on the panadapter).
  gboolean precise = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_SURFACE;

  if (mods & GDK_CONTROL_MASK) {
    double ns = s * (precise ? exp(-dy * 0.01) : pow(1.25, -dy));
    if (ns > GPU_ZOOM_MAX) ns = GPU_ZOOM_MAX;
    if (ns <= base * 1.001) {                 // all the way out = plain fit again
      self->zoom = 0.0;
      self->follow_end = TRUE;
    } else {
      // Hold the image point under the pointer still while the scale changes.
      double ix = self->off_x + self->ptr_x / s;
      double iy = self->off_y + self->ptr_y / s;
      self->off_x = ix - self->ptr_x / ns;
      self->off_y = iy - self->ptr_y / ns;
      self->zoom = ns;
      self->follow_end = FALSE;
      recheck_follow(self, ns);
    }
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return GDK_EVENT_STOP;
  }

  // Panning.  With nothing hidden there is nothing to scroll — let the event
  // through to whatever is above us rather than swallowing it.
  double vh = h / s, vw = w / s;
  if (self->zoom <= 0.0 && self->img_h <= vh && self->img_w <= vw) return GDK_EVENT_PROPAGATE;
  if (self->zoom <= 0.0) self->zoom = base;   // fit mode, but taller than the view

  double step_y = precise ? dy : dy * GPU_WHEEL_PIXELS;
  double step_x = precise ? dx : dx * GPU_WHEEL_PIXELS;
  if (mods & GDK_SHIFT_MASK) {                // shift+wheel = sideways
    self->off_x += (step_y != 0.0 ? step_y : step_x) / s;
  } else {
    self->off_x += step_x / s;
    self->off_y += step_y / s;
  }
  self->follow_end = FALSE;
  recheck_follow(self, s);
  gtk_widget_queue_draw(GTK_WIDGET(self));
  return GDK_EVENT_STOP;
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer ud) {
  GpuImage *self = ud;
  self->drag_x = 0.0;
  self->drag_y = 0.0;
}

static void on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer ud) {
  GpuImage *self = ud;
  if (!self->zoomable || self->img_w <= 0) return;
  double s = current_scale(self);
  if (s <= 0.0) return;
  if (self->zoom <= 0.0) self->zoom = s;
  self->off_x -= (ox - self->drag_x) / s;
  self->off_y -= (oy - self->drag_y) / s;
  self->drag_x = ox;
  self->drag_y = oy;
  self->follow_end = FALSE;
  recheck_follow(self, s);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

// Double-click: back to fit, following the newest lines again.
static void on_click(GtkGestureClick *g, int n_press, double x, double y, gpointer ud) {
  GpuImage *self = ud;
  if (n_press < 2) return;
  self->zoom = 0.0;
  self->follow_end = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void gpu_image_set_zoomable(GpuImage *self, gboolean on, gboolean drag_pan) {
  if (self->zoomable == on && self->drag_pan == drag_pan) return;
  self->zoomable = on;
  self->drag_pan = drag_pan;
  if (!on) {
    self->zoom = 0.0;
    self->follow_end = TRUE;
    return;
  }
  GtkEventController *sc = gtk_event_controller_scroll_new(
      GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), self);
  gtk_widget_add_controller(GTK_WIDGET(self), sc);

  GtkEventController *mc = gtk_event_controller_motion_new();
  g_signal_connect(mc, "motion", G_CALLBACK(on_motion), self);
  gtk_widget_add_controller(GTK_WIDGET(self), mc);

  if (drag_pan) {
    GtkGesture *dg = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dg), 1);
    g_signal_connect(dg, "drag-begin", G_CALLBACK(on_drag_begin), self);
    g_signal_connect(dg, "drag-update", G_CALLBACK(on_drag_update), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(dg));

    GtkGesture *cg = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cg), 1);
    g_signal_connect(cg, "pressed", G_CALLBACK(on_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(cg));
  }
}

gboolean gpu_image_widget_to_image(GpuImage *self, double wx, double wy,
                                   double *ix, double *iy) {
  if (self->img_w <= 0 || self->img_h <= 0) return FALSE;
  double s = current_scale(self);
  if (s <= 0.0) return FALSE;
  double x = self->off_x + wx / s;
  double y = self->off_y + wy / s;
  if (ix) *ix = x;
  if (iy) *iy = y;
  return x >= 0.0 && y >= 0.0 && x < self->img_w && y < self->img_h;
}
