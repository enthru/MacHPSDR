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

// APT georeferencing.  See apt_geo.h for what this is and what it is not.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "apt_geo.h"
#include "log.h"

#include "sgp4sdp4.h"

// ---- raster layout (must match apt_decoder.c) -----------------------------
#define WORDS      2080
#define IMGA_OFF   86      // first video word of channel A
#define IMG_LEN    909     // video words per channel
#define CHAN_STEP  1040    // channel B video starts this many words after A

// ---- Earth ---------------------------------------------------------------
// WGS84.  The propagator's own geoid is WGS72, which differs by ~1 m in the
// flattening — far below the ~4 km ground sample of this image mode.
#define EARTH_A    6378.137
#define EARTH_F    (1.0 / 298.257223563)

// ---- element sets --------------------------------------------------------
#define MAX_SATS   64

static sat_t   sats[MAX_SATS];
static int     nsats = 0;
static int     sel = -1;              // index into sats[], −1 = none

static double  base_utc = 0.0;        // UTC of image row 0 (unix seconds)
static gboolean base_set = FALSE;
static double  time_offset = 0.0;     // operator trim, seconds

// A satellite's own transmit clock: row → seconds.  Deliberately the nominal
// line period — the decoder's sync servo has already taken the Doppler time
// scaling out (see apt_geo.h).
#define ROW_SECONDS APT_LINE_SECONDS

// The decoder's per-row stamps, when it has given us any.  Sized to the
// decoder's image buffer with room to spare; a longer table is truncated rather
// than refused (the rows that are there still project correctly).
#define MAX_ROWS   2048
static double  row_utc[MAX_ROWS];
static int     row_utc_n = 0;

// ---- propagated-frame cache ----------------------------------------------
// A coastline is thousands of vertices and each one is a root-find over the
// rows, so the propagator would otherwise be called tens of thousands of times
// per redraw.  Frames are therefore computed on a fixed 4 s grid and linearly
// interpolated: over 4 s the orbit's chord departs from its arc by ~4 m, which
// is four decimal places below the ~4 km ground sample.  The grid is in TIME,
// not rows, because the row → time map is not uniform (see apt_geo.h).
#define CACHE_DT   4.0                // seconds between propagated frames
#define CACHE_N    1024               // 68 min — longer than any pass
typedef struct { double jd; vector_t pos; double down[3], fwd[3], side[3]; } FRAME;
static FRAME    cache[CACHE_N];
static gboolean cache_ok[CACHE_N];
static double   cache_t0 = 0.0;       // unix seconds of slot 0

// The grid starts a few minutes before row 0 so that a negative operator trim,
// or a ground point searched for above the first row, still lands on it.
static void cache_invalidate(void) {
  memset(cache_ok, 0, sizeof cache_ok);
  cache_t0 = base_utc - 64.0 * CACHE_DT;
}

// FRAME: where the satellite is at one instant, and the three unit vectors the
// look direction is built from — jd (UTC Julian date), pos (ECI km), down
// (nadir), fwd (along-track, horizontal), side (cross-track, in the direction of
// increasing word).  Declared with the cache above.

static void v_sub(const double *a, const double *b, double *o) {
  o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static double v_dot(const double *a, const double *b) {
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static void v_cross(const double *a, const double *b, double *o) {
  o[0] = a[1]*b[2] - a[2]*b[1];
  o[1] = a[2]*b[0] - a[0]*b[2];
  o[2] = a[0]*b[1] - a[1]*b[0];
}
static void v_norm(double *v) {
  double m = sqrt(v_dot(v, v));
  if (m > 0.0) { v[0] /= m; v[1] /= m; v[2] /= m; }
}

// Row → UTC.  The decoder's table when there is one, interpolated between rows
// and extrapolated at the nominal period past either end (a caller searching for
// a ground point legitimately probes outside the rows it has); otherwise the
// uniform fallback.
static double row_to_unix(double row) {
  double t;
  if (row_utc_n >= 2) {
    if (row <= 0.0)                 t = row_utc[0] + row * ROW_SECONDS;
    else if (row >= row_utc_n - 1)  t = row_utc[row_utc_n - 1]
                                        + (row - (row_utc_n - 1)) * ROW_SECONDS;
    else {
      int i = (int)row;
      double f = row - i;
      t = row_utc[i] + (row_utc[i + 1] - row_utc[i]) * f;
    }
  } else {
    t = base_utc + row * ROW_SECONDS;
  }
  return t + time_offset;
}
static double unix_to_jd(double t) {
  return t / 86400.0 + 2440587.5;
}

// ---------------------------------------------------------------------------
// Propagate to UTC `t` and build the spacecraft frame.
//
// The frame is the whole of the pointing model: the instrument looks straight
// down and sweeps across the flight direction, so `down` and `side` span the
// scan plane.  `side` is chosen so that it points the way the image columns
// run, which for a southbound (descending) pass makes the picture come out
// north-up/west-left — the orientation a NOAA APT image is conventionally seen
// in, and the reason a northbound pass looks upside down.
static gboolean frame_propagate(double t, FRAME *f) {
  if (sel < 0) return FALSE;
  sat_t *s = &sats[sel];

  f->jd = unix_to_jd(t);
  double tsince = (f->jd - s->jul_epoch) * xmnpda;

  if (s->flags & DEEP_SPACE_EPHEM_FLAG) SDP4(s, tsince);
  else                                  SGP4(s, tsince);
  Convert_Sat_State(&s->pos, &s->vel);
  f->pos = s->pos;

  double r[3] = { s->pos.x, s->pos.y, s->pos.z };
  double v[3] = { s->vel.x, s->vel.y, s->vel.z };
  if (v_dot(r, r) <= 0.0) return FALSE;

  double up[3] = { r[0], r[1], r[2] };
  v_norm(up);
  f->down[0] = -up[0]; f->down[1] = -up[1]; f->down[2] = -up[2];

  // Along-track, with the radial part removed (the orbit is not exactly
  // circular, so the velocity is not exactly horizontal).
  double vr = v_dot(v, up);
  f->fwd[0] = v[0] - vr * up[0];
  f->fwd[1] = v[1] - vr * up[1];
  f->fwd[2] = v[2] - vr * up[2];
  v_norm(f->fwd);

  v_cross(f->fwd, f->down, f->side);
  v_norm(f->side);
  return TRUE;
}

// Frame at UTC `t`, off the 4 s grid (see the cache note above).  A time outside
// the grid — which only a wildly wrong operator trim produces — is propagated
// directly rather than refused.
static gboolean frame_time(double t, FRAME *f) {
  if (sel < 0) return FALSE;

  double x = (t - cache_t0) / CACHE_DT;
  int i = (int)floor(x);
  if (i < 0 || i + 1 >= CACHE_N) return frame_propagate(t, f);

  for (int k = i; k <= i + 1; k++) {
    if (!cache_ok[k]) {
      if (!frame_propagate(cache_t0 + k * CACHE_DT, &cache[k])) return FALSE;
      cache_ok[k] = TRUE;
    }
  }

  const FRAME *a = &cache[i], *b = &cache[i + 1];
  double u = x - i;
  f->jd = a->jd + (b->jd - a->jd) * u;
  f->pos.x = a->pos.x + (b->pos.x - a->pos.x) * u;
  f->pos.y = a->pos.y + (b->pos.y - a->pos.y) * u;
  f->pos.z = a->pos.z + (b->pos.z - a->pos.z) * u;
  f->pos.w = 0.0;
  for (int k = 0; k < 3; k++) {
    f->down[k] = a->down[k] + (b->down[k] - a->down[k]) * u;
    f->fwd[k]  = a->fwd[k]  + (b->fwd[k]  - a->fwd[k])  * u;
    f->side[k] = a->side[k] + (b->side[k] - a->side[k]) * u;
  }
  v_norm(f->down); v_norm(f->fwd); v_norm(f->side);
  return TRUE;
}

static gboolean frame_at(double row, FRAME *f) {
  if (sel < 0 || !base_set) return FALSE;
  return frame_time(row_to_unix(row), f);
}

// Scan angle (radians, signed, + towards increasing word) of an image column.
// Words outside the video area extrapolate, so an overlay drawn over the whole
// 2080-word line stays continuous instead of stopping at the video edges.
static double word_to_angle(double word) {
  double base = (word < CHAN_STEP) ? IMGA_OFF : (IMGA_OFF + CHAN_STEP);
  double frac = (word - base) / (double)(IMG_LEN - 1);
  return (frac - 0.5) * 2.0 * APT_SCAN_HALF_DEG * M_PI / 180.0;
}

// The inverse, for channel A (channel B is the same ground, CHAN_STEP later).
static double angle_to_word(double ang) {
  double frac = ang / (2.0 * APT_SCAN_HALF_DEG * M_PI / 180.0) + 0.5;
  return IMGA_OFF + frac * (double)(IMG_LEN - 1);
}

// Where a ray from `org` along unit `dir` first meets the WGS84 ellipsoid.
static gboolean ray_earth(const double *org, const double *dir, double *hit) {
  const double a2 = EARTH_A * EARTH_A;
  const double b  = EARTH_A * (1.0 - EARTH_F);
  const double b2 = b * b;

  double A = (dir[0]*dir[0] + dir[1]*dir[1]) / a2 + dir[2]*dir[2] / b2;
  double B = 2.0 * ((org[0]*dir[0] + org[1]*dir[1]) / a2 + org[2]*dir[2] / b2);
  double C = (org[0]*org[0] + org[1]*org[1]) / a2 + org[2]*org[2] / b2 - 1.0;

  double disc = B*B - 4.0*A*C;
  if (disc < 0.0 || A == 0.0) return FALSE;      // looks past the limb
  double k = (-B - sqrt(disc)) / (2.0 * A);      // near intersection
  if (k <= 0.0) return FALSE;

  hit[0] = org[0] + k*dir[0];
  hit[1] = org[1] + k*dir[1];
  hit[2] = org[2] + k*dir[2];
  return TRUE;
}

static void eci_to_geodetic(double jd, const double *p, double *lat, double *lon) {
  vector_t v = { p[0], p[1], p[2], 0.0 };
  geodetic_t g;
  Calculate_LatLonAlt(jd, &v, &g);
  *lat = Degrees(g.lat);
  *lon = Degrees(g.lon);
  while (*lon > 180.0)  *lon -= 360.0;
  while (*lon < -180.0) *lon += 360.0;
}

static void geodetic_to_eci(double jd, double lat, double lon, double alt_km, double *p) {
  geodetic_t g;
  g.lat = Radians(lat);
  g.lon = Radians(lon);
  g.alt = alt_km;
  g.theta = 0.0;
  vector_t pos, vel;
  Calculate_User_PosVel(jd, &g, &pos, &vel);
  p[0] = pos.x; p[1] = pos.y; p[2] = pos.z;
}

// ---- element sets ---------------------------------------------------------

char *apt_geo_default_tle_path(void) {
  return g_build_filename(g_get_user_data_dir(), "machpsdr", "tle.txt", NULL);
}

int apt_geo_load_tle(const char *path, char **err) {
  if (err) *err = NULL;
  if (path == NULL || path[0] == '\0') { if (err) *err = g_strdup("no TLE file set"); return 0; }

  FILE *fp = fopen(path, "r");
  if (fp == NULL) {
    if (err) *err = g_strdup_printf("cannot open %s", path);
    return 0;
  }

  // Parse into a temporary table and only commit if something came of it: a
  // mistyped path or a file of the wrong kind must not cost the operator the
  // element set that was working, mid-pass.
  sat_t *tmp = g_new0(sat_t, MAX_SATS);
  int    ntmp = 0;

  char line[3][80];
  char buf[160];
  int have = 0;                          // lines held in `line`
  int bad = 0;
  int over = 0;                          // sets past MAX_SATS

  while (fgets(buf, sizeof buf, fp) != NULL) {
    // Strip the newline but keep the column layout — the TLE parser reads by
    // fixed offsets, so leading whitespace must not be touched.
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    if (n == 0) continue;

    if (buf[0] == '1' && buf[1] == ' ') {
      if (have == 0) { line[0][0] = '\0'; have = 1; }   // bare 2-line set
      g_strlcpy(line[1], buf, sizeof line[1]);
      have = 2;
      continue;
    }
    if (buf[0] == '2' && buf[1] == ' ' && have == 2) {
      g_strlcpy(line[2], buf, sizeof line[2]);
      if (ntmp < MAX_SATS) {
        sat_t *s = &tmp[ntmp];
        memset(s, 0, sizeof *s);
        if (Get_Next_Tle_Set(line, &s->tle) == 1) {
          // select_ephemeris() converts the element set to the units the
          // propagator wants, in place — exactly once per set.
          select_ephemeris(s);
          s->jul_epoch = Julian_Date_of_Epoch(s->tle.epoch);
          ntmp++;
        } else {
          bad++;
        }
      } else {
        over++;
      }
      have = 0;
      continue;
    }
    // Anything else is a name line for the set that follows.
    g_strlcpy(line[0], buf, sizeof line[0]);
    have = 1;
  }
  fclose(fp);

  if (ntmp == 0) {
    if (err) *err = g_strdup_printf("no valid element sets in %s%s", path,
                                    bad ? " (checksum failed)" : "");
    g_free(tmp);
    return 0;
  }

  int old_sel_catnr = (sel >= 0) ? sats[sel].tle.catnr : 0;
  memcpy(sats, tmp, (size_t)ntmp * sizeof(sat_t));
  g_free(tmp);
  nsats = ntmp;
  sel = -1;
  cache_invalidate();
  if (old_sel_catnr) apt_geo_select_catnr(old_sel_catnr);

  log_info("apt_geo: %d element set(s) from %s%s", nsats, path,
           bad ? " (some rejected)" : "");
  if (over) log_info("apt_geo: %d further set(s) ignored (limit %d)", over, MAX_SATS);
  return nsats;
}

gboolean apt_geo_select_catnr(int catnr) {
  for (int i = 0; i < nsats; i++) {
    if (sats[i].tle.catnr == catnr) {
      if (sel != i) { sel = i; cache_invalidate(); }
      return TRUE;
    }
  }
  return FALSE;
}

gboolean apt_geo_select_freq(long long hz) {
  // The APT downlinks.  Matching on frequency rather than name is what lets the
  // operator just tune the bird: the decoder already publishes where it is
  // listening, and the name in a TLE file is not something to depend on.
  static const struct { long long hz; int catnr; } apt_sats[] = {
    { 137620000LL, 25338 },   // NOAA 15
    { 137912500LL, 28654 },   // NOAA 18
    { 137100000LL, 33591 },   // NOAA 19
  };
  for (unsigned i = 0; i < G_N_ELEMENTS(apt_sats); i++) {
    if (llabs(hz - apt_sats[i].hz) <= 40000LL)
      return apt_geo_select_catnr(apt_sats[i].catnr);
  }
  return FALSE;
}

const char *apt_geo_satellite(void) {
  return (sel >= 0) ? sats[sel].tle.sat_name : NULL;
}

double apt_geo_tle_age_days(void) {
  if (sel < 0 || !base_set) return 0.0;
  return unix_to_jd(base_utc + time_offset) - sats[sel].jul_epoch;
}

// ---- time base ------------------------------------------------------------

void apt_geo_set_time_base(double unix_utc_row0) {
  base_utc = unix_utc_row0;
  base_set = (unix_utc_row0 > 0.0);
  cache_invalidate();
}

void apt_geo_set_row_times(const double *unix_utc, int n) {
  if (unix_utc == NULL || n < 2) { row_utc_n = 0; return; }
  if (n > MAX_ROWS) n = MAX_ROWS;
  memcpy(row_utc, unix_utc, (size_t)n * sizeof(double));
  row_utc_n = n;
  base_utc = row_utc[0];
  base_set = (base_utc > 0.0);
  cache_invalidate();
}

// The trim shifts every row in time, so the cached frames — which are keyed by
// the *trimmed* time — all move with it.
void apt_geo_set_time_offset(double seconds) { time_offset = seconds; }
double apt_geo_get_time_offset(void) { return time_offset; }

gboolean apt_geo_ready(void) { return sel >= 0 && base_set; }

// ---- projection -----------------------------------------------------------

gboolean apt_geo_pixel_to_latlon(double row, double word, double *lat, double *lon) {
  FRAME f;
  if (!frame_at(row, &f)) return FALSE;

  double ang = word_to_angle(word);
  double c = cos(ang), s = sin(ang);
  double dir[3] = {
    c * f.down[0] + s * f.side[0],
    c * f.down[1] + s * f.side[1],
    c * f.down[2] + s * f.side[2]
  };
  double org[3] = { f.pos.x, f.pos.y, f.pos.z };
  double hit[3];
  if (!ray_earth(org, dir, hit)) return FALSE;

  eci_to_geodetic(f.jd, hit, lat, lon);
  return TRUE;
}

gboolean apt_geo_subpoint(double row, double *lat, double *lon, double *alt_km) {
  FRAME f;
  if (!frame_at(row, &f)) return FALSE;
  double p[3] = { f.pos.x, f.pos.y, f.pos.z };
  vector_t v = { p[0], p[1], p[2], 0.0 };
  geodetic_t g;
  Calculate_LatLonAlt(f.jd, &v, &g);
  if (lat) *lat = Degrees(g.lat);
  if (lon) {
    *lon = Degrees(g.lon);
    while (*lon > 180.0)  *lon -= 360.0;
    while (*lon < -180.0) *lon += 360.0;
  }
  if (alt_km) *alt_km = g.alt;
  return TRUE;
}

// Signed along-track offset of a ground point at a given row: positive while the
// point is still ahead of the scan plane, negative once it is behind.  Crossing
// zero is the row that imaged it, and the crossing is single because the
// satellite runs 7.4 km/s against the ground point's 0.46 km/s.
static gboolean along_track(double row, double lat, double lon, double *out, FRAME *fout) {
  FRAME f;
  if (!frame_at(row, &f)) return FALSE;
  double tgt[3];
  geodetic_to_eci(f.jd, lat, lon, 0.0, tgt);
  double d[3], sp[3] = { f.pos.x, f.pos.y, f.pos.z };
  v_sub(tgt, sp, d);
  *out = v_dot(d, f.fwd);       // > 0 while the point is still ahead
  if (fout) *fout = f;
  return TRUE;
}

gboolean apt_geo_latlon_to_pixel(double lat, double lon, double row_lo, double row_hi,
                                 double *row, double *word) {
  if (sel < 0 || !base_set || row_hi <= row_lo) return FALSE;

  // The offset falls monotonically with the row (the satellite runs 7.4 km/s
  // against the ground point's 0.46 km/s), so the point is inside the rows we
  // have exactly when it is still ahead at the first and already behind at the
  // last.  The bracket is opened half a row at each end because a point taken
  // FROM the first or last row lands exactly on the boundary, where rounding
  // decides the answer — and that is every pixel of the top and bottom rows of
  // the image.  A result may therefore come back half a row outside the range;
  // the caller is drawing into a raster and clips it anyway.
  double lo = row_lo - 0.5, hi = row_hi + 0.5, flo, fhi;
  if (!along_track(lo, lat, lon, &flo, NULL)) return FALSE;
  if (!along_track(hi, lat, lon, &fhi, NULL)) return FALSE;
  if (flo < 0.0 || fhi > 0.0) return FALSE;

  for (int i = 0; i < 40 && (hi - lo) > 1e-3; i++) {
    double mid = 0.5 * (lo + hi), fm;
    if (!along_track(mid, lat, lon, &fm, NULL)) return FALSE;
    if (fm > 0.0) { lo = mid; flo = fm; }
    else          { hi = mid; fhi = fm; }
  }

  double r = 0.5 * (lo + hi);
  FRAME f;
  double dummy;
  if (!along_track(r, lat, lon, &dummy, &f)) return FALSE;

  double tgt[3];
  geodetic_to_eci(f.jd, lat, lon, 0.0, tgt);
  double sp[3] = { f.pos.x, f.pos.y, f.pos.z }, d[3];
  v_sub(tgt, sp, d);

  // Below the horizon: the point is on the far side of the Earth.  (The ellipsoid
  // ray test cannot catch this — it is a genuine intersection, just not a
  // visible one.)
  double up[3] = { tgt[0], tgt[1], tgt[2] };
  v_norm(up);
  double to_sat[3] = { -d[0], -d[1], -d[2] };
  v_norm(to_sat);
  if (v_dot(up, to_sat) <= 0.0) return FALSE;

  double dn = v_dot(d, f.down), sd = v_dot(d, f.side);
  double ang = atan2(sd, dn);
  if (fabs(ang) > APT_SCAN_HALF_DEG * M_PI / 180.0) return FALSE;   // outside the swath

  if (row)  *row = r;
  if (word) *word = angle_to_word(ang);
  return TRUE;
}
