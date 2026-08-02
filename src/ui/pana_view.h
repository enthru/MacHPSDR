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
 * PanaView — a small reusable GtkWidget that renders through the GTK4
 * render-node (GtkSnapshot) pipeline, i.e. on the GPU, instead of rasterising a
 * whole scene into a CPU cairo image surface every frame and blitting it (which
 * the panadapters used to do — cost proportional to the window area, hence the
 * fullscreen lag).
 *
 * It is the vector-drawing sibling of GpuImage (which composites a raster
 * pixbuf): instead of a source pixbuf, the caller supplies a "build" callback
 * that emits GSK render nodes (append_color / append_stroke / append_fill /
 * append_linear_gradient / append_layout / append_texture) for the current
 * frame.  GTK calls it from the widget's snapshot() vfunc, so the existing
 * gtk_widget_queue_draw() call sites keep driving repaints unchanged.
 *
 * A "resize" signal with the same name/signature as GtkDrawingArea's is
 * re-emitted on size changes, so existing *_resize_cb handlers attach with no
 * edits.  Input controllers (click/motion/scroll) are added by the caller
 * exactly as on a GtkDrawingArea.
 */

#ifndef PANA_VIEW_H
#define PANA_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PANA_TYPE_VIEW (pana_view_get_type())
G_DECLARE_FINAL_TYPE(PanaView, pana_view, PANA, VIEW, GtkWidget)

// Emit the current frame's GSK render nodes into snapshot, for a widget sized
// w x h (device-independent pixels).  Called on the GTK main thread from
// snapshot(); must not block or touch other threads' state without a lock.  The
// snapshot is already clipped to the widget bounds.
typedef void (*PanaViewBuild)(GtkSnapshot *snapshot, int w, int h, gpointer user_data);

GtkWidget *pana_view_new(PanaViewBuild build, gpointer user_data);

G_END_DECLS

#endif
