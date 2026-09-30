/* Copyright (C)
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
*/

#include <stdio.h>
#include "settings_ui.h"

// One place to tune the whole configuration dialog's rhythm.
#define SUI_PAGE_MARGIN    10   // outer margin around a page
#define SUI_PAGE_ROW_SP     8   // vertical gap between stacked groups
#define SUI_PAGE_COL_SP     8   // horizontal gap between side-by-side groups

#define SUI_GROUP_MARGIN    6   // inner margin inside a group
#define SUI_GROUP_ROW_SP    4   // vertical gap between rows in a group
#define SUI_GROUP_COL_SP    9   // horizontal gap between label and field

// Apply row/column spacing to whatever container this is.  Not every page root
// is a GtkGrid — the TX page is three GtkBoxes side by side, so that a short
// frame is not stretched to match a tall one sharing a grid row — and calling
// gtk_grid_set_*_spacing() on a box is two GTK-CRITICALs and no spacing.  A box
// has one spacing, along its own orientation, so it takes the row or column
// number depending on which way it runs.
static void sui_spacing(GtkWidget *w,int row_sp,int col_sp) {
  if(GTK_IS_GRID(w)) {
    gtk_grid_set_row_spacing(GTK_GRID(w),row_sp);
    gtk_grid_set_column_spacing(GTK_GRID(w),col_sp);
  } else if(GTK_IS_BOX(w)) {
    gtk_box_set_spacing(GTK_BOX(w),
      gtk_orientable_get_orientation(GTK_ORIENTABLE(w))==GTK_ORIENTATION_HORIZONTAL
        ? col_sp : row_sp);
  }
}

void sui_style_page(GtkWidget *grid) {
  if(grid==NULL) return;
  sui_spacing(grid,SUI_PAGE_ROW_SP,SUI_PAGE_COL_SP);
  // Outer page margin is applied uniformly via CSS (#config-dialog notebook >
  // stack padding) so grid-pages and frame-pages get the same breathing room.
}

void sui_style_group(GtkWidget *grid) {
  if(grid==NULL) return;
  sui_spacing(grid,SUI_GROUP_ROW_SP,SUI_GROUP_COL_SP);
  gtk_widget_set_margin_top(grid,SUI_GROUP_MARGIN-2);
  gtk_widget_set_margin_bottom(grid,SUI_GROUP_MARGIN);
  gtk_widget_set_margin_start(grid,SUI_GROUP_MARGIN);
  gtk_widget_set_margin_end(grid,SUI_GROUP_MARGIN);
}

void sui_label_left(GtkWidget *label) {
  if(label==NULL) return;
  gtk_widget_set_halign(label,GTK_ALIGN_START);
}

void sui_label_desc(GtkWidget *label) {
  if(label==NULL || !GTK_IS_LABEL(label)) return;
  // A settings-page description is left-aligned and MUST wrap: the English text
  // is hand-broken with '\n' at ~70 chars, but translations are longer single
  // lines (the RU/UK/BE PPM blurb is one ~1800 px line) and without wrapping they
  // drive a horizontal scrollbar across the whole page. Capping the natural width
  // at SUI_DESC_WRAP_CHARS makes the label wrap to the page instead; the hard
  // '\n's in the source still break where the author intended.
  gtk_widget_set_halign(label,GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(label),0.0f);
  gtk_label_set_wrap(GTK_LABEL(label),TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label),PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(label),SUI_DESC_WRAP_CHARS);
}

void sui_scale_show_value(GtkWidget *scale, int digits) {
  if(scale==NULL || !GTK_IS_SCALE(scale)) return;
  gtk_scale_set_draw_value(GTK_SCALE(scale),TRUE);
  gtk_scale_set_digits(GTK_SCALE(scale),digits);
  // Put the number on the trailing edge, where a short slider has room for it.
  if(gtk_orientable_get_orientation(GTK_ORIENTABLE(scale))==GTK_ORIENTATION_VERTICAL)
    gtk_scale_set_value_pos(GTK_SCALE(scale),GTK_POS_BOTTOM);
  else
    gtk_scale_set_value_pos(GTK_SCALE(scale),GTK_POS_RIGHT);
}

void sui_rate_label(char *buf, size_t n, int hz) {
  if(hz>=1000 && (hz%1000)==0) snprintf(buf,n,"%dk",hz/1000);
  else                         snprintf(buf,n,"%d",hz);
}
