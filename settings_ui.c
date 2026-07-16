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

#include "settings_ui.h"

// One place to tune the whole configuration dialog's rhythm.
#define SUI_PAGE_MARGIN    10   // outer margin around a page
#define SUI_PAGE_ROW_SP     8   // vertical gap between stacked groups
#define SUI_PAGE_COL_SP     8   // horizontal gap between side-by-side groups

#define SUI_GROUP_MARGIN    6   // inner margin inside a group
#define SUI_GROUP_ROW_SP    4   // vertical gap between rows in a group
#define SUI_GROUP_COL_SP    9   // horizontal gap between label and field

void sui_style_page(GtkWidget *grid) {
  if(grid==NULL) return;
  gtk_grid_set_row_spacing(GTK_GRID(grid),SUI_PAGE_ROW_SP);
  gtk_grid_set_column_spacing(GTK_GRID(grid),SUI_PAGE_COL_SP);
  // Outer page margin is applied uniformly via CSS (#config-dialog notebook >
  // stack padding) so grid-pages and frame-pages get the same breathing room.
}

void sui_style_group(GtkWidget *grid) {
  if(grid==NULL) return;
  gtk_grid_set_row_spacing(GTK_GRID(grid),SUI_GROUP_ROW_SP);
  gtk_grid_set_column_spacing(GTK_GRID(grid),SUI_GROUP_COL_SP);
  gtk_widget_set_margin_top(grid,SUI_GROUP_MARGIN-2);
  gtk_widget_set_margin_bottom(grid,SUI_GROUP_MARGIN);
  gtk_widget_set_margin_start(grid,SUI_GROUP_MARGIN);
  gtk_widget_set_margin_end(grid,SUI_GROUP_MARGIN);
}

void sui_label_left(GtkWidget *label) {
  if(label==NULL) return;
  gtk_widget_set_halign(label,GTK_ALIGN_START);
}
