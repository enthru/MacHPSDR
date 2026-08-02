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

#include "pana_view.h"

struct _PanaView {
  GtkWidget      parent_instance;
  PanaViewBuild  build;
  gpointer       user_data;
  int            last_w, last_h;   // fire "resize" only on real changes
};

G_DEFINE_FINAL_TYPE(PanaView, pana_view, GTK_TYPE_WIDGET)

// "resize" (int width, int height): re-implements GtkDrawingArea's signal of the
// same name/signature so existing resize handlers attach unchanged.
enum { SIG_RESIZE, N_SIGNALS };
static guint signals[N_SIGNALS];

static void pana_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  PanaView *self = PANA_VIEW(widget);
  int w = gtk_widget_get_width(widget);
  int h = gtk_widget_get_height(widget);
  if (w <= 0 || h <= 0 || self->build == NULL) return;

  const graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, w, h);
  gtk_snapshot_push_clip(snapshot, &full);
  self->build(snapshot, w, h, self->user_data);
  gtk_snapshot_pop(snapshot);
}

static void pana_view_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  GTK_WIDGET_CLASS(pana_view_parent_class)->size_allocate(widget, width, height, baseline);
  PanaView *self = PANA_VIEW(widget);
  if (width != self->last_w || height != self->last_h) {
    self->last_w = width;
    self->last_h = height;
    g_signal_emit(self, signals[SIG_RESIZE], 0, width, height);
  }
}

static void pana_view_class_init(PanaViewClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  wc->snapshot = pana_view_snapshot;
  wc->size_allocate = pana_view_size_allocate;

  signals[SIG_RESIZE] = g_signal_new("resize",
      G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST, 0, NULL, NULL,
      NULL /* generic marshaller */, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

static void pana_view_init(PanaView *self) {
  self->last_w = -1;
  self->last_h = -1;
}

GtkWidget *pana_view_new(PanaViewBuild build, gpointer user_data) {
  PanaView *self = g_object_new(PANA_TYPE_VIEW, NULL);
  self->build = build;
  self->user_data = user_data;
  return GTK_WIDGET(self);
}
