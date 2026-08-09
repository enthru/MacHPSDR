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
 * The map drawn over a georeferenced APT image: coastline, graticule and ground
 * track.
 *
 * Split from apt_panel.c because of one number.  Putting a coastline on the
 * picture means running apt_geo_latlon_to_pixel — a root-find down the rows —
 * for every vertex of the world, and there are 60 000 of them; doing that inside
 * the draw handler would spend tens of milliseconds per frame redrawing a map
 * that has not moved.  So the projection is computed into IMAGE coordinates and
 * cached, and the draw handler only maps image to widget, which is an affine
 * transform the widget already knows.  The cache is rebuilt when the picture
 * grows, when the operator moves the time trim, or when the satellite changes —
 * never per frame.
 *
 * Everything here runs on the GTK thread, like apt_geo.
 */

#ifndef _APT_MAP_H
#define _APT_MAP_H

#include <glib.h>
#include <cairo.h>

// Rebuild the cached projection if anything it depends on has changed.  `lines`
// is the number of rows in the displayed picture, `x0`/`width` the full-line
// word range it shows (the View crop).  Cheap when nothing changed.
void apt_map_update(int lines, int x0, int width);

// Throw the cache away (the picture was cleared, or a new pass started).
void apt_map_invalidate(void);

// Image (pixbuf) coordinates to whatever the caller is drawing in.  Passed in
// rather than assumed, so this module needs no widget: in the app it is the
// GpuImage's current scale and pan, and in the offline tool it is the identity —
// which is what lets the overlay be checked without starting the app.
typedef gboolean (*AptMapXform)(double ix, double iy, double *ox, double *oy, gpointer user);

// Draw the cached projection.  Does nothing when there is none.
void apt_map_draw(cairo_t *cr, int width, int height, AptMapXform xf, gpointer user);

#endif
