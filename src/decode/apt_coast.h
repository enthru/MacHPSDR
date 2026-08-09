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
 * The world coastline, for drawing over a georeferenced APT image.
 *
 * Natural Earth 1:50m, public domain, converted to a flat binary by
 * tools/make_coastline.py (which documents the format and where to get the
 * source data).  Coordinates are int16 hundred-and-eightieths of a degree —
 * 620 m, well inside one 4 km APT pixel — so the whole world is 247 kB and is
 * read with one fread rather than a JSON parser.
 *
 * The file is found the same way cty.dat is: $MACHPSDR_COASTLINE, then the .app
 * Resources, then assets/, then the cwd, then the data dirs.  A missing file is
 * not an error anywhere — the overlay simply does not draw.
 *
 * Loaded once and kept; every caller is on the GTK thread.
 */

#ifndef _APT_COAST_H
#define _APT_COAST_H

#include <glib.h>

typedef struct { float lon, lat; } APT_COAST_PT;

// Load (idempotent — later calls are a no-op once something is loaded).  TRUE if
// a coastline is available.  `path` may be NULL for the search described above.
gboolean apt_coast_load(const char *path);

// Number of polylines, and one of them: `n` receives its point count.
int apt_coast_count(void);
const APT_COAST_PT *apt_coast_polyline(int i, int *n);

// Bounding box of polyline `i`, computed at load.  The projection of a single
// vertex costs a root-find down the image rows, so rejecting whole continents
// that cannot be in the swath is what makes drawing the map affordable.
void apt_coast_bbox(int i, float *lon_min, float *lon_max,
                    float *lat_min, float *lat_max);

#endif
