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

// Map overlay for a georeferenced APT image.  See apt_map.h for why the
// projection is cached rather than computed in the draw handler.

#include <math.h>
#include <string.h>

#include "apt_map.h"
#include "apt_geo.h"
#include "apt_coast.h"
#include "log.h"

#define WORDS       2080
#define CHAN_STEP   1040
#define SEG_KM      25.0     // subdivision of a coastline segment
#define GRAT_STEP   10       // graticule spacing, degrees

// A projected polyline in image coordinates, as a flat run of x,y pairs.  A
// break in the projection (the vector leaving the swath) ends the run and starts
// a new one, so a stroke never jumps across the picture.
typedef struct { int first, n; } RUN;

static GArray *xy   = NULL;      // float pairs
static GArray *runs = NULL;      // RUN
static int     coast_runs = 0;   // the first `coast_runs` are coastline...
static int     grat_runs = 0;    // ...then this many graticule, then the track

// What the cache was built for.
static int    c_lines = -1, c_x0 = -1, c_width = -1;
static double c_trim = 1e30;
static const char *c_sat = NULL;

static void begin(void) {
  if (!xy)   xy   = g_array_new(FALSE, FALSE, sizeof(float));
  if (!runs) runs = g_array_new(FALSE, FALSE, sizeof(RUN));
  g_array_set_size(xy, 0);
  g_array_set_size(runs, 0);
  coast_runs = grat_runs = 0;
}

static int cur_first = 0, cur_n = 0;

static void run_flush(void) {
  if (cur_n >= 2) {
    RUN r = { cur_first, cur_n };
    g_array_append_val(runs, r);
  }
  cur_first = (int)xy->len / 2;
  cur_n = 0;
}

static void run_point(double x, double y) {
  float v[2] = { (float)x, (float)y };
  g_array_append_vals(xy, v, 2);
  cur_n++;
}

// Project one ground point into the displayed picture.  `x0`/`width` are the
// View crop; on the whole-line view the same ground appears twice (once per
// channel) and `second` asks for the channel-B copy.
static gboolean project(double lat, double lon, int lines, int x0, int width,
                        gboolean second, double *x, double *y) {
  double row, word;
  if (!apt_geo_latlon_to_pixel(lat, lon, 0, lines - 1, &row, &word)) return FALSE;
  if (second) word += CHAN_STEP;
  double px = word - x0;
  if (px < 0.0 || px >= width) return FALSE;
  *x = px; *y = row;
  return TRUE;
}

static void add_vector(double lat0, double lon0, double lat1, double lon1,
                       int lines, int x0, int width, gboolean second,
                       gboolean *pen_down) {
  // Subdivide: a straight line in lat/lon is not straight in scan geometry.
  double d = hypot(lat1 - lat0, (lon1 - lon0) * cos(lat1 * M_PI / 180.0)) * 111.0;
  int n = (int)(d / SEG_KM) + 1;
  if (n > 64) n = 64;
  for (int i = 1; i <= n; i++) {
    double la = lat0 + (lat1 - lat0) * i / n;
    double lo = lon0 + (lon1 - lon0) * i / n;
    double x, y;
    if (project(la, lo, lines, x0, width, second, &x, &y)) {
      if (!*pen_down) { run_flush(); *pen_down = TRUE; }
      run_point(x, y);
    } else if (*pen_down) {
      run_flush();
      *pen_down = FALSE;
    }
  }
}

// The lat/lon window the picture covers, from a coarse sample of its corners and
// edges.  Rejecting a polyline against this is what makes drawing the world
// affordable: each surviving vertex costs a root-find down the rows.
static void image_bbox(int lines, double *la0, double *la1, double *lo0, double *lo1,
                       gboolean *lon_wraps) {
  *la0 = 90.0; *la1 = -90.0; *lo0 = 180.0; *lo1 = -180.0;
  for (int i = 0; i <= 16; i++) {
    double row = (lines - 1.0) * i / 16.0;
    for (int j = 0; j <= 8; j++) {
      double word = 86.0 + 908.0 * j / 8.0;
      double la, lo;
      if (!apt_geo_pixel_to_latlon(row, word, &la, &lo)) continue;
      if (la < *la0) *la0 = la;
      if (la > *la1) *la1 = la;
      if (lo < *lo0) *lo0 = lo;
      if (lo > *lo1) *lo1 = lo;
    }
  }
  // Near a pole the swath spans every meridian and the min/max in longitude
  // means nothing; also catch a pass straddling the date line the same way.
  *lon_wraps = (*la1 > 78.0 || *la0 < -78.0 || (*lo1 - *lo0) > 180.0);
}

void apt_map_invalidate(void) {
  c_lines = -1; c_x0 = -1; c_width = -1; c_trim = 1e30; c_sat = NULL;
  if (runs) g_array_set_size(runs, 0);
  if (xy)   g_array_set_size(xy, 0);
  coast_runs = grat_runs = 0;
}

void apt_map_update(int lines, int x0, int width) {
  if (!apt_geo_ready() || lines < 2 || width < 2) { apt_map_invalidate(); return; }

  // Rebuild only when something it depends on moved.  The row count changes
  // twice a second while a pass runs, so it is quantised: a rebuild every 32
  // rows (16 s) keeps the map within a few pixels of the newest lines, and the
  // rest of the picture does not move at all.
  const char *sat = apt_geo_satellite();
  double trim = apt_geo_get_time_offset();
  int q = lines - (lines % 32);
  if (q == c_lines && x0 == c_x0 && width == c_width && trim == c_trim && sat == c_sat)
    return;
  c_lines = q; c_x0 = x0; c_width = width; c_trim = trim; c_sat = sat;

  gint64 t0 = g_get_monotonic_time();
  begin();
  cur_first = 0; cur_n = 0;

  double la0, la1, lo0, lo1;
  gboolean wraps;
  image_bbox(lines, &la0, &la1, &lo0, &lo1, &wraps);
  const double MARGIN = 2.0;
  la0 -= MARGIN; la1 += MARGIN; lo0 -= MARGIN; lo1 += MARGIN;

  gboolean both = (x0 == 0 && width >= WORDS);   // whole-line view: draw both channels

  // ---- coastline ----------------------------------------------------------
  if (apt_coast_load(NULL)) {
    for (int i = 0; i < apt_coast_count(); i++) {
      float plo0, plo1, pla0, pla1;
      apt_coast_bbox(i, &plo0, &plo1, &pla0, &pla1);
      if (pla1 < la0 || pla0 > la1) continue;
      if (!wraps && (plo1 < lo0 || plo0 > lo1)) continue;

      int n = 0;
      const APT_COAST_PT *p = apt_coast_polyline(i, &n);
      for (int pass = 0; pass < (both ? 2 : 1); pass++) {
        gboolean pen = FALSE;
        run_flush();
        for (int k = 1; k < n; k++)
          add_vector(p[k-1].lat, p[k-1].lon, p[k].lat, p[k].lon,
                     lines, x0, width, pass == 1, &pen);
        run_flush();
      }
    }
  }
  coast_runs = (int)runs->len;

  // ---- graticule ----------------------------------------------------------
  for (int lat = -80; lat <= 80; lat += GRAT_STEP) {
    if (lat < la0 - GRAT_STEP || lat > la1 + GRAT_STEP) continue;
    for (int pass = 0; pass < (both ? 2 : 1); pass++) {
      gboolean pen = FALSE;
      run_flush();
      for (int lon = -180; lon < 180; lon += 2)
        add_vector(lat, lon, lat, lon + 2, lines, x0, width, pass == 1, &pen);
      run_flush();
    }
  }
  for (int lon = -180; lon < 180; lon += GRAT_STEP) {
    for (int pass = 0; pass < (both ? 2 : 1); pass++) {
      gboolean pen = FALSE;
      run_flush();
      for (int lat = -88; lat < 88; lat += 2)
        add_vector(lat, lon, lat + 2, lon, lines, x0, width, pass == 1, &pen);
      run_flush();
    }
  }
  grat_runs = (int)runs->len - coast_runs;

  // ---- ground track -------------------------------------------------------
  // The sub-satellite point is by definition the centre of the swath, so this is
  // a straight line down the picture; it is drawn anyway because it is the one
  // feature whose position is not in doubt, and a map that has slipped shows it
  // by NOT sitting on the middle of the scan.
  for (int pass = 0; pass < (both ? 2 : 1); pass++) {
    gboolean pen = FALSE;
    run_flush();
    for (int r = 0; r < lines; r += 8) {
      double la, lo;
      if (!apt_geo_subpoint(r, &la, &lo, NULL)) continue;
      double x, y;
      if (project(la, lo, lines, x0, width, pass == 1, &x, &y)) {
        if (!pen) { run_flush(); pen = TRUE; }
        run_point(x, y);
      } else if (pen) { run_flush(); pen = FALSE; }
    }
    run_flush();
  }

  log_debug("apt_map: %d runs, %d points, %.1f ms\n", runs->len, xy->len / 2,
            (g_get_monotonic_time() - t0) / 1000.0);
}

static void stroke_runs(cairo_t *cr, AptMapXform xf, gpointer user, int from, int to,
                        int width, int height) {
  for (int i = from; i < to; i++) {
    RUN r = g_array_index(runs, RUN, i);
    const float *v = &g_array_index(xy, float, r.first * 2);
    gboolean started = FALSE;
    for (int k = 0; k < r.n; k++) {
      double wx, wy;
      if (!xf(v[k*2], v[k*2+1], &wx, &wy, user)) return;
      // Clip generously rather than exactly: cairo handles the rest, and a
      // segment with one end off-screen still has to be drawn.
      if (wx < -4000 || wx > width + 4000 || wy < -4000 || wy > height + 4000) {
        started = FALSE;
        continue;
      }
      if (!started) { cairo_move_to(cr, wx, wy); started = TRUE; }
      else            cairo_line_to(cr, wx, wy);
    }
  }
  cairo_stroke(cr);
}

void apt_map_draw(cairo_t *cr, int width, int height, AptMapXform xf, gpointer user) {
  if (!runs || runs->len == 0 || xf == NULL) return;

  cairo_set_line_width(cr, 1.0);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  // Graticule first and dimmest — it is a reference grid, not information.
  cairo_set_source_rgba(cr, 0.4, 0.8, 1.0, 0.35);
  stroke_runs(cr, xf, user, coast_runs, coast_runs + grat_runs, width, height);

  // Ground track.
  cairo_set_source_rgba(cr, 1.0, 0.9, 0.2, 0.5);
  stroke_runs(cr, xf, user, coast_runs + grat_runs, (int)runs->len, width, height);

  // Coastline last, brightest: it is what the operator checks the fit against.
  cairo_set_line_width(cr, 1.4);
  cairo_set_source_rgba(cr, 1.0, 0.25, 0.25, 0.9);
  stroke_runs(cr, xf, user, 0, coast_runs, width, height);
}
