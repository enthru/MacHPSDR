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

#ifndef SETTINGS_UI_H
#define SETTINGS_UI_H

#include <gtk/gtk.h>

/*
 * Shared spacing/alignment helpers for the configuration dialog.
 *
 * These do NOT create widgets or move anything around; they only apply one
 * consistent set of spacings and margins to grids that a dialog already built,
 * so every page and every group lines up the same way. `homogeneous` flags and
 * child positions are left untouched, so existing layouts keep working.
 */

/* Top-level page grid: outer margins + generous inter-group spacing. */
void sui_style_page(GtkWidget *grid);

/* A group's inner grid: tighter margins + row/column spacing for label/field rows. */
void sui_style_group(GtkWidget *grid);

/* Left-align a label (the common case for a "Name:" field caption). */
void sui_label_left(GtkWidget *label);

/*
 * Show the current numeric value beside a GtkScale slider. GtkScale draws its
 * value by default, but the config dialog's short (30 px) horizontal scales clip
 * the number when it is drawn on top of the trough, so it never shows. This puts
 * the readout on the trailing side (RIGHT for a horizontal scale, BOTTOM for a
 * vertical one) where there is room for it, with `digits` decimal places.
 */
void sui_scale_show_value(GtkWidget *scale, int digits);

#endif
