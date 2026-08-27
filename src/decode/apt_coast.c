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

// The world coastline for the APT overlay.  See apt_coast.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "resource_path.h"
#include "apt_coast.h"
#include "log.h"

#define COAST_SCALE 180.0f          // int16 units per degree (see the generator)

typedef struct { int first, n; float lon_min, lon_max, lat_min, lat_max; } POLY;

static APT_COAST_PT *pts = NULL;
static POLY         *polys = NULL;
static int           npolys = 0, npts = 0;
static gboolean      tried = FALSE;

static FILE *try_open(const char *p) {
  FILE *f = fopen(p, "rb");
  if (f) log_info("apt_coast: reading %s\n", p);
  return f;
}

// Same search order as cty.dat (ft8_dxcc.c) — the two files ship together and an
// operator who moved one has moved the other.
static FILE *open_coast(void) {
  const char *env = g_getenv("MACHPSDR_COASTLINE");
  if (env && env[0]) { FILE *f = try_open(env); if (f) return f; }

  char path[1024];
  if (machpsdr_resource_path("coastline.bin", path, sizeof(path))) {
    FILE *f = try_open(path); if (f) return f;
  }
  { FILE *f = try_open("assets/coastline.bin"); if (f) return f; }
  { FILE *f = try_open("coastline.bin"); if (f) return f; }
  snprintf(path, sizeof(path), "%s/.local/share/machpsdr/coastline.bin", g_get_home_dir());
  { FILE *f = try_open(path); if (f) return f; }
  { FILE *f = try_open("/usr/local/share/machpsdr/coastline.bin"); if (f) return f; }
  { FILE *f = try_open("/usr/share/machpsdr/coastline.bin"); if (f) return f; }
  return NULL;
}

static void discard(void) {
  g_free(pts);   pts = NULL;
  g_free(polys); polys = NULL;
  npts = npolys = 0;
}

gboolean apt_coast_load(const char *path) {
  if (npolys > 0) return TRUE;
  if (path == NULL && tried) return FALSE;      // do not re-search every redraw
  tried = TRUE;

  FILE *f = path ? try_open(path) : open_coast();
  if (!f) {
    log_info("apt_coast: coastline.bin not found — map overlay unavailable\n");
    return FALSE;
  }

  char magic[8];
  guint32 ver = 0, np = 0;
  if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "APTCOAST", 8) != 0 ||
      fread(&ver, 4, 1, f) != 1 || ver != 1 ||
      fread(&np, 4, 1, f) != 1 || np == 0 || np > 1000000) {
    log_error("apt_coast: not a coastline file (or version %u)\n", ver);
    fclose(f);
    return FALSE;
  }

  polys = g_new0(POLY, np);
  // Grown as the polylines are read; the count is not in the header because the
  // generator writes the file in one pass.
  int cap = 1 << 16;
  pts = g_new(APT_COAST_PT, cap);

  for (guint32 i = 0; i < np; i++) {
    guint32 n = 0;
    if (fread(&n, 4, 1, f) != 1 || n > 1000000) { discard(); fclose(f); return FALSE; }
    while (npts + (int)n > cap) { cap *= 2; pts = g_renew(APT_COAST_PT, pts, cap); }
    polys[npolys].first = npts;
    polys[npolys].n = (int)n;
    float lo0 = 181.0f, lo1 = -181.0f, la0 = 91.0f, la1 = -91.0f;
    for (guint32 k = 0; k < n; k++) {
      gint16 v[2];
      if (fread(v, 2, 2, f) != 2) { discard(); fclose(f); return FALSE; }
      pts[npts].lon = v[0] / COAST_SCALE;
      pts[npts].lat = v[1] / COAST_SCALE;
      if (pts[npts].lon < lo0) lo0 = pts[npts].lon;
      if (pts[npts].lon > lo1) lo1 = pts[npts].lon;
      if (pts[npts].lat < la0) la0 = pts[npts].lat;
      if (pts[npts].lat > la1) la1 = pts[npts].lat;
      npts++;
    }
    polys[npolys].lon_min = lo0; polys[npolys].lon_max = lo1;
    polys[npolys].lat_min = la0; polys[npolys].lat_max = la1;
    npolys++;
  }
  fclose(f);
  log_info("apt_coast: %d polylines, %d points\n", npolys, npts);
  return TRUE;
}

int apt_coast_count(void) { return npolys; }

void apt_coast_bbox(int i, float *lon_min, float *lon_max,
                    float *lat_min, float *lat_max) {
  if (i < 0 || i >= npolys) return;
  if (lon_min) *lon_min = polys[i].lon_min;
  if (lon_max) *lon_max = polys[i].lon_max;
  if (lat_min) *lat_min = polys[i].lat_min;
  if (lat_max) *lat_max = polys[i].lat_max;
}

const APT_COAST_PT *apt_coast_polyline(int i, int *n) {
  if (i < 0 || i >= npolys) { if (n) *n = 0; return NULL; }
  if (n) *n = polys[i].n;
  return &pts[polys[i].first];
}
