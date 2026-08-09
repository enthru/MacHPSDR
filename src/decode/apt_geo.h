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
 * APT georeferencing — which point on the ground each pixel of a NOAA weather
 * satellite image came from.
 *
 * The decoder (apt_decoder.c) produces a raster in *scan* geometry: one row per
 * AVHRR scan, one column per sample across the scan.  That is not a map — the
 * satellite moves ~3 km per line and the scan sweeps ±55.37° from nadir, so a
 * pixel's ground position depends on where the satellite was when that line was
 * transmitted and which way that sample was looking.  Both are computable: the
 * first from the orbit (SGP4 over a TLE) plus the UTC of the line, the second
 * from fixed spacecraft geometry.
 *
 * What this buys is the difference between a pretty picture and a measurement:
 * coastlines and a graticule drawn over the image, and a lat/lon readout under
 * the pointer.
 *
 *   line  → UTC → satellite ECI position/velocity (SGP4)
 *   word  → scan angle → look vector in the spacecraft frame
 *   ray ∩ WGS84 ellipsoid → geodetic lat/lon
 *
 * Four things are worth knowing before touching this file:
 *
 *  - **A whole image row is one instant.**  The 909 video words of a line are
 *    transmitted over 0.25 s, but they are one AVHRR scan, read out of a buffer
 *    — the sweep itself takes ~50 ms and all of it belongs to one satellite
 *    position.  So the time depends on the row and NOT on the word; adding the
 *    word's transmission offset would smear each line by up to 3.7 km of
 *    along-track motion that never happened.
 *
 *  - **The row → time map comes from the decoder, not from arithmetic.**  Rows
 *    are only drawn while sync is locked, so a fade puts rows next to each other
 *    in the image that are seconds apart on the clock, and the image buffer
 *    scrolls once it is full.  `apt_geo_set_row_times()` therefore takes the
 *    decoder's own per-row stamps; `apt_geo_set_time_base()` is the fallback
 *    uniform assumption (row × 0.5 s) for a caller that has nothing better.
 *    Doppler is deliberately NOT corrected for: the decoder's clock servo has
 *    already taken the ±25 ppm time scaling out — that is what removes the
 *    slant — so correcting again would count it twice.
 *
 *  - **The time base is the weak link, not the orbit.**  SGP4 on a fresh TLE is
 *    good to ~1 km; one second of clock error is ~7 km along track.  Hence the
 *    operator time trim: it absorbs a stale TLE, a wrong capture clock, the
 *    audio-path latency between reception and the stamp, and the constant
 *    scan-to-transmission delay on board.  It is the control an operator
 *    actually uses to slide the coastline onto the coast.
 *
 *  - **Not thread-safe, and deliberately so.**  The propagator carries state in
 *    the sat_t and the module caches propagated frames on a fixed time grid;
 *    everything here is called from the GTK thread (overlay drawing, pointer
 *    readout).  The decoder itself never calls in — it only publishes the row
 *    stamps, which the caller hands over.
 */

#ifndef _APT_GEO_H
#define _APT_GEO_H

#include <glib.h>

// Geometry constants of the APT line, shared with the decoder's raster layout.
#define APT_LINE_SECONDS   0.5     // one image row (2 lines/s)
#define APT_SCAN_HALF_DEG  55.37   // AVHRR half scan angle from nadir

// ---- element set ----------------------------------------------------------

// Load two/three-line element sets from `path` (the usual multi-satellite TLE
// file).  Returns the number of sets read, 0 on failure; `err` (may be NULL)
// receives a short message the UI can show.
int apt_geo_load_tle(const char *path, char **err);

// Default TLE path: ~/.local/share/machpsdr/tle.txt (caller frees).
char *apt_geo_default_tle_path(void);

// Pick the satellite by NORAD catalogue number, or by APT downlink frequency
// (NOAA-15/18/19 are 137.620/137.9125/137.100 MHz; ±40 kHz is accepted so the
// dial does not have to be exact).  FALSE if the loaded file has no such set.
gboolean apt_geo_select_catnr(int catnr);
gboolean apt_geo_select_freq(long long hz);

// Name of the selected satellite, or NULL. Age = days between the TLE epoch and
// the current time base — a stale set is the usual reason an overlay is off.
// Only meaningful once apt_geo_ready(); 0 before that.
const char *apt_geo_satellite(void);
double      apt_geo_tle_age_days(void);

// ---- time base ------------------------------------------------------------

// Per-row UTC (unix seconds), `n` rows starting at image row 0 — the decoder's
// own stamps, copied.  This is the accurate map: it survives fades and the
// image buffer scrolling, neither of which a uniform row × 0.5 s does.  Rows
// beyond the table extrapolate at the nominal line period.  n < 2 drops back to
// the uniform map.
void apt_geo_set_row_times(const double *unix_utc, int n);

// UTC (unix seconds, fractional) at which image row 0 was transmitted, and an
// operator trim in seconds added to it.  Setting either invalidates the cache.
// The time base is the uniform fallback used when no row table has been set.
void apt_geo_set_time_base(double unix_utc_row0);
void apt_geo_set_time_offset(double seconds);
double apt_geo_get_time_offset(void);

// TRUE when a satellite is selected and a time base is set — i.e. when the
// projection can be computed at all.
gboolean apt_geo_ready(void);

// ---- projection -----------------------------------------------------------

// Ground point seen by image (row, word).  `word` is a column of the full
// 2080-word line, so it works for either channel and for the full-line view;
// both are fractional so the caller can work in sub-pixel coordinates.
// FALSE if that look direction misses the Earth (never in practice) or no time
// base is set.  lat/lon in degrees, lon in −180..180.
gboolean apt_geo_pixel_to_latlon(double row, double word, double *lat, double *lon);

// The inverse: where a ground point lands in the image.  FALSE when the point is
// outside the swath, below the horizon, or outside `row_lo..row_hi` (the rows the
// caller has, which bounds the search).  This is what draws a coastline: vectors
// are projected into image coordinates rather than the image being resampled.
gboolean apt_geo_latlon_to_pixel(double lat, double lon, double row_lo, double row_hi,
                                 double *row, double *word);

// Sub-satellite point and altitude for a row (the ground track).
gboolean apt_geo_subpoint(double row, double *lat, double *lon, double *alt_km);

// TRUE while the pass runs south → north (ascending).  This is the one thing the
// decoder cannot work out for itself and needs in order to hand out a north-up
// picture: the AVHRR scan crosses the ground the same way round whichever way
// the satellite is flying, so a northbound pass writes the image upside down and
// wants a 180° rotation (apt_decoder_set_flip).  FALSE when descending, and also
// when there is no orbit to ask — the caller must not rotate on a guess.
gboolean apt_geo_pass_ascending(void);

#endif
