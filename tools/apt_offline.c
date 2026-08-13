// Offline APT harness: feed a recording into the decoder and write the decoded
// image out as a PNG.  No GTK, no audio, no radio — this is how the APT decoder
// is verified, never by starting the app (which takes over the screen and
// rewrites the operator's saved settings on exit).
//
//   apt_offline <file.wav> [-o out.png]                  demodulated APT audio
//   apt_offline <file.wav> --iq <centre_hz> <cursor_hz> [-o out.png]   raw I/Q
//   apt_offline --selftest                               synthetic end-to-end
//
// The audio form is the widely-available recording format (mono 11025 Hz WAVs
// of a NOAA pass).  The I/Q form eats our own rec_*_iq.wav — the same 16-bit
// stereo file the I/Q Player replays — with the receiver centre and the tuned
// cursor given in absolute Hz.
//
// --selftest needs no recording at all: it synthesises an APT transmission from
// a known test pattern (line sync, 2400 Hz AM video), pushes it through both
// entry points — the audio path directly, and the I/Q path through a real FM
// modulation placed off-centre — and checks the decoded lines against the
// pattern it started from.

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "apt_decoder.h"
#include "apt_geo.h"
#include "apt_coast.h"
#include "apt_map.h"
#include <cairo.h>

#define WORDS      2080
#define WORD_RATE  4160.0
#define IMGA_OFF   86
#define IMG_LEN    909
#define IMGB_OFF   1126
#define CHAN_STEP  1040

// --- WAV reading -----------------------------------------------------------
static int rd32(FILE *f, unsigned *v) { unsigned char b[4]; if (fread(b,1,4,f)!=4) return 0;
  *v = b[0] | (b[1]<<8) | (b[2]<<16) | ((unsigned)b[3]<<24); return 1; }
static int rd16(FILE *f, unsigned *v) { unsigned char b[2]; if (fread(b,1,2,f)!=2) return 0;
  *v = b[0] | (b[1]<<8); return 1; }

// SDR recorders (SDRuno, SpectraVue and the rest of the SpectraVue-derived
// family) put the capture time and the centre frequency in an `auxi` chunk.
// Reading it is what lets a recording be georeferenced without the operator
// having to remember when it was made — and it is exact, where a memory is not.
// Layout: two Win32 SYSTEMTIMEs (8 × uint16: year, month, day-of-week, day,
// hour, minute, second, millisecond) then centre frequency and sample rate.
typedef struct { double utc; long long centre_hz; } WAV_AUXI;

static void auxi_parse(const unsigned char *b, unsigned len, WAV_AUXI *ax) {
  if (len < 36) return;
  unsigned short st[8];
  for (int i = 0; i < 8; i++) st[i] = (unsigned short)(b[i*2] | (b[i*2+1] << 8));
  if (st[0] < 1980 || st[0] > 2200 || st[1] < 1 || st[1] > 12) return;
  GDateTime *dt = g_date_time_new_utc(st[0], st[1], st[3], st[4], st[5], st[6]);
  if (dt) { ax->utc = (double)g_date_time_to_unix(dt) + st[7] / 1000.0; g_date_time_unref(dt); }
  ax->centre_hz = (long long)(b[32] | (b[33]<<8) | (b[34]<<16) | ((unsigned)b[35]<<24));
}

static short *wav_read(const char *path, unsigned *rate, unsigned *chans, long *nframes,
                       WAV_AUXI *ax) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return NULL; }
  unsigned v; char id[5] = {0};
  if (fread(id,1,4,f)!=4 || memcmp(id,"RIFF",4)) { fprintf(stderr,"not RIFF\n"); fclose(f); return NULL; }
  rd32(f,&v);
  if (fread(id,1,4,f)!=4) { fclose(f); return NULL; }
  unsigned bits = 0; long data_len = 0;
  *rate = 0; *chans = 0;
  if (ax) { ax->utc = 0.0; ax->centre_hz = 0; }
  for (;;) {
    if (fread(id,1,4,f)!=4) { fprintf(stderr,"no data chunk\n"); fclose(f); return NULL; }
    unsigned len; if (!rd32(f,&len)) { fclose(f); return NULL; }
    if (!memcmp(id,"fmt ",4)) {
      unsigned fmt, ch, sr, br, ba, bps;
      rd16(f,&fmt); rd16(f,&ch); rd32(f,&sr); rd32(f,&br); rd16(f,&ba); rd16(f,&bps);
      *chans = ch; *rate = sr; bits = bps;
      if (len > 16) fseek(f, len-16, SEEK_CUR);
    } else if (!memcmp(id,"auxi",4) && ax != NULL && len <= 4096) {
      unsigned char *b = g_malloc0(len);
      if (fread(b, 1, len, f) == len) auxi_parse(b, len, ax);
      g_free(b);
      if (len & 1) fseek(f, 1, SEEK_CUR);
    } else if (!memcmp(id,"data",4)) { data_len = len; break; }
    else fseek(f, len + (len&1), SEEK_CUR);
  }
  if (bits != 16) { fprintf(stderr,"need 16-bit PCM (got %u)\n", bits); fclose(f); return NULL; }
  long nf = data_len / (long)(2 * *chans);
  short *buf = g_new(short, (size_t)nf * *chans);
  if ((long)fread(buf, 2 * *chans, nf, f) != nf) fprintf(stderr,"short read\n");
  fclose(f);
  *nframes = nf;
  return buf;
}

// --- georeferencing report -------------------------------------------------
// The projection cannot be checked by round-tripping our own maths — that only
// proves it is self-consistent — so this prints the quantities that have known
// values outside this program: the swath width (NOAA publishes ~2900 km), the
// along-track step (0.5 s of a ~7.4 km/s orbit seen from the ground), the
// sub-satellite track, and whether the pass runs north-to-south.  A geometry
// error big enough to matter shows up in one of them.
static double gc_km(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0, D = M_PI / 180.0;
  double p1 = lat1*D, p2 = lat2*D, dp = (lat2-lat1)*D, dl = (lon2-lon1)*D;
  double a = sin(dp/2)*sin(dp/2) + cos(p1)*cos(p2)*sin(dl/2)*sin(dl/2);
  return 2.0 * R * atan2(sqrt(a), sqrt(1.0-a));
}

static void geo_report(int lines) {
  if (!apt_geo_ready()) { printf("geo: not ready (no TLE or no time base)\n"); return; }

  printf("geo: satellite %s, TLE age %+.2f days\n",
         apt_geo_satellite() ? apt_geo_satellite() : "?", apt_geo_tle_age_days());

  const double LEFT = IMGA_OFF, RIGHT = IMGA_OFF + IMG_LEN - 1;
  int rows[3] = { 0, lines / 2, lines - 1 };
  double mid_lat[3] = {0}, mid_lon[3] = {0};

  for (int i = 0; i < 3; i++) {
    double r = rows[i];
    double sl, so, alt, ll, lo, rl, ro;
    if (!apt_geo_subpoint(r, &sl, &so, &alt)) { printf("  row %4d: no frame\n", rows[i]); continue; }
    gboolean okl = apt_geo_pixel_to_latlon(r, LEFT,  &ll, &lo);
    gboolean okr = apt_geo_pixel_to_latlon(r, RIGHT, &rl, &ro);
    mid_lat[i] = sl; mid_lon[i] = so;
    printf("  row %4d  sub %7.3f %8.3f  alt %6.1f km", rows[i], sl, so, alt);
    if (okl && okr)
      printf("   left %7.3f %8.3f  right %7.3f %8.3f  swath %6.1f km",
             ll, lo, rl, ro, gc_km(ll, lo, rl, ro));
    printf("\n");
  }

  if (lines > 2) {
    double step = gc_km(mid_lat[0], mid_lon[0], mid_lat[2], mid_lon[2]) / (rows[2] - rows[0]);
    // Two independent answers to the same question — the subpoint latitudes at
    // the ends of the picture, and the velocity at the middle of it, which is
    // what the north-up rotation actually uses.  They must agree.
    gboolean asc_lat = mid_lat[2] > mid_lat[0];
    gboolean asc_vel = apt_geo_pass_ascending();
    printf("  along-track %.2f km/row (%.1f km/s ground speed), pass runs %s%s\n",
           step, step / APT_LINE_SECONDS,
           asc_lat ? "south to north (ascending — image is upside down)"
                   : "north to south (descending — north-up image)",
           asc_lat == asc_vel ? "" : "   *** direction test DISAGREES with the orbit ***");
  }

  // Self-consistency of the two directions.  Not proof of the geometry, but it
  // does catch a broken inverse, and a large error means the root-find is not
  // converging where it is being asked to.
  double worst = 0.0;
  for (int i = 0; i < 3; i++) {
    for (double w = LEFT; w <= RIGHT; w += (RIGHT - LEFT) / 8.0) {
      double la, lo, rr, ww;
      if (!apt_geo_pixel_to_latlon(rows[i], w, &la, &lo)) continue;
      if (!apt_geo_latlon_to_pixel(la, lo, 0, lines - 1, &rr, &ww)) { worst = 1e9; continue; }
      double e = fabs(rr - rows[i]) + fabs(ww - w);
      if (e > worst) worst = e;
    }
  }
  printf("  round-trip pixel -> lat/lon -> pixel: worst %.4g px\n", worst);
}

// --- time-base scan --------------------------------------------------------
// Which time base is right is the one thing about the georeferencing that
// cannot be settled from inside the geometry: every number in geo_report() is
// derived from the capture time, so all of them stay self-consistent however
// wrong that time is.  The only outside evidence is the picture itself — a
// coastline is a brightness edge, so the projection is right when the projected
// coastline lands on edges and wrong when it lands on featureless sea or
// unbroken cloud.
//
// Score = (mean gradient at the projected coast points) / (mean gradient at the
// same points displaced a long way).  The ratio, not the raw gradient: a Δ that
// happens to look at a cloudier part of the world would win on raw gradient
// alone, and the displaced control moves with it.
//
// It answers honestly when it cannot answer: over cloud-covered ocean there is
// no coastline signal to find and the ratio stays near 1 for every Δ.  That is a
// result — it means the recording cannot settle its own time base — and is not
// to be read as agreement with whichever Δ scored 1.02.
#define TS_VIDEO_A0  86
#define TS_VIDEO_A1  994
#define TS_VIDEO_B0  1126
#define TS_VIDEO_B1  2034
#define TS_CTRL      60      // control displacement, px (~240 km either way)

typedef struct { int w, h; float *g; } TS_GRAD;

// |∇I| of the smoothed picture, over the two video areas only (the sync bars and
// telemetry wedges are the strongest edges in the frame and none of them is a
// coast).
static void ts_gradient(GdkPixbuf *pb, TS_GRAD *out) {
  int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
  int stride = gdk_pixbuf_get_rowstride(pb), nch = gdk_pixbuf_get_n_channels(pb);
  const guchar *px = gdk_pixbuf_get_pixels(pb);

  float *sm = g_new0(float, (size_t)w * h);
  // 5×5 box blur, separable: APT noise is per-sample and a raw difference is
  // mostly noise.
  float *tmp = g_new0(float, (size_t)w * h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float s = 0; int n = 0;
      for (int d = -2; d <= 2; d++) {
        int xx = x + d; if (xx < 0 || xx >= w) continue;
        s += px[(size_t)y * stride + (size_t)xx * nch]; n++;
      }
      tmp[(size_t)y * w + x] = s / n;
    }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float s = 0; int n = 0;
      for (int d = -2; d <= 2; d++) {
        int yy = y + d; if (yy < 0 || yy >= h) continue;
        s += tmp[(size_t)yy * w + x]; n++;
      }
      sm[(size_t)y * w + x] = s / n;
    }
  g_free(tmp);

  float *g = g_new0(float, (size_t)w * h);
  for (int y = 3; y < h - 3; y++)
    for (int x = 3; x < w - 3; x++) {
      if (!((x >= TS_VIDEO_A0 && x < TS_VIDEO_A1) || (x >= TS_VIDEO_B0 && x < TS_VIDEO_B1)))
        continue;
      float gx = sm[(size_t)y * w + x + 3] - sm[(size_t)y * w + x - 3];
      float gy = sm[(size_t)(y + 3) * w + x] - sm[(size_t)(y - 3) * w + x];
      g[(size_t)y * w + x] = fabsf(gx) + fabsf(gy);
    }
  g_free(sm);
  out->w = w; out->h = h; out->g = g;
}

static gboolean ts_at(const TS_GRAD *G, double row, double word, double *v) {
  int x = (int)(word + 0.5), y = (int)(row + 0.5);
  if (x < 3 || x >= G->w - 3 || y < 3 || y >= G->h - 3) return FALSE;
  if (!((x >= TS_VIDEO_A0 && x < TS_VIDEO_A1) || (x >= TS_VIDEO_B0 && x < TS_VIDEO_B1)))
    return FALSE;
  *v = G->g[(size_t)y * G->w + x];
  return TRUE;
}

// Lat/lon bounds of the swath, so continents that cannot be in it are rejected
// before anything is projected.  Same sampling as apt_map.c's own (private)
// version — a whole-file copy would be worse than fifteen duplicated lines.
static void ts_bbox(int lines, double *la0, double *la1, double *lo0, double *lo1,
                    gboolean *wraps) {
  *la0 = 90.0; *la1 = -90.0; *lo0 = 180.0; *lo1 = -180.0;
  for (int i = 0; i <= 16; i++) {
    double row = (lines - 1.0) * i / 16.0;
    for (int j = 0; j <= 8; j++) {
      double la, lo;
      if (!apt_geo_pixel_to_latlon(row, 86.0 + 908.0 * j / 8.0, &la, &lo)) continue;
      if (la < *la0) *la0 = la;
      if (la > *la1) *la1 = la;
      if (lo < *lo0) *lo0 = lo;
      if (lo > *lo1) *lo1 = lo;
    }
  }
  *wraps = (*la1 > 78.0 || *la0 < -78.0 || (*lo1 - *lo0) > 180.0);
}

// One Δ: project the world coastline, then score it at every fine sub-shift.
//
// The sub-shift is what makes the search possible at all.  A wrong time base
// moves the picture along the track at 7 km/s, so the score has a peak only a
// few seconds wide — a scan that steps in minutes jumps straight over it and
// reports "no agreement anywhere", which is exactly what the first version of
// this did.  But a small change in Δ is, to a very good approximation, just a
// translation in rows (1 row = 0.5 s): the swath geometry barely rotates over a
// few tens of seconds.  So the expensive part — a root-find per vertex — is done
// once per coarse step, and ±TS_SHIFT rows of fine shifts are then free.
#define TS_SHIFT  64                 // ±32 s around each coarse step
#define TS_MAXPT  40000

static double ts_score(const TS_GRAD *G, int lines, double delta,
                       int *n_out, double *best_shift) {
  apt_geo_set_time_offset(delta);

  double la0, la1, lo0, lo1;
  gboolean wraps;
  ts_bbox(lines, &la0, &la1, &lo0, &lo1, &wraps);
  const double M = 2.0;
  la0 -= M; la1 += M; lo0 -= M; lo1 += M;

  static float pr[TS_MAXPT], pw[TS_MAXPT];
  int npt = 0;

  // The projection is searched over a widened row range, because a point that
  // falls off the picture at this Δ may well be on it after a sub-shift.
  for (int i = 0; i < apt_coast_count() && npt < TS_MAXPT; i++) {
    float plo0, plo1, pla0, pla1;
    apt_coast_bbox(i, &plo0, &plo1, &pla0, &pla1);
    if (pla1 < la0 || pla0 > la1) continue;
    if (!wraps && (plo1 < lo0 || plo0 > lo1)) continue;

    int np = 0;
    const APT_COAST_PT *p = apt_coast_polyline(i, &np);
    for (int k = 0; k < np && npt < TS_MAXPT; k += 4) {   // 620 m spacing -> 2.5 km
      double row, word;
      if (!apt_geo_latlon_to_pixel(p[k].lat, p[k].lon, -TS_SHIFT, lines - 1 + TS_SHIFT,
                                   &row, &word))
        continue;
      pr[npt] = (float)row; pw[npt] = (float)word; npt++;
    }
  }

  double best = 0.0; int best_s = 0, best_n = 0;
  for (int s = -TS_SHIFT; s <= TS_SHIFT; s++) {
    double sum = 0.0, ctrl = 0.0;
    int n = 0, nc = 0;
    for (int i = 0; i < npt; i++) {
      double row = pr[i] + s, word = pw[i], v;
      if (!ts_at(G, row, word, &v)) continue;
      sum += v; n++;
      // Control: the same point moved far enough that it is certainly not on
      // this piece of coast, in four directions so the comparison stays local.
      const int dr[4] = { TS_CTRL, -TS_CTRL, 0, 0 };
      const int dw[4] = { 0, 0, TS_CTRL, -TS_CTRL };
      for (int d = 0; d < 4; d++) {
        double cv;
        if (ts_at(G, row + dr[d], word + dw[d], &cv)) { ctrl += cv; nc++; }
      }
    }
    if (n < 200 || nc < 200) continue;           // too little coast to say anything
    double sc = (sum / n) / (ctrl / nc);
    if (sc > best) { best = sc; best_s = s; best_n = n; }
  }

  if (n_out) *n_out = best_n;
  if (best_shift) *best_shift = best_s * APT_LINE_SECONDS;
  return best;
}

static void time_scan(int lines, double lo, double hi, double step) {
  if (!apt_geo_ready()) { printf("timescan: no TLE or no time base\n"); return; }
  if (!apt_coast_load(NULL)) { printf("timescan: no coastline file\n"); return; }

  GdkPixbuf *pb = apt_decoder_get_full_image();
  if (!pb) { printf("timescan: no image\n"); return; }
  TS_GRAD G;
  ts_gradient(pb, &G);
  g_object_unref(pb);

  double keep = apt_geo_get_time_offset();
  printf("timescan: %+.0f .. %+.0f s step %.0f  (score = coast edge / control edge; "
         "1.00 = no agreement)\n", lo, hi, step);

  double best = 0.0, best_d = 0.0;
  for (double d = lo; d <= hi + 1e-6; d += step) {
    int n = 0; double sh = 0.0;
    double s = ts_score(&G, lines, d, &n, &sh);
    double sl = 0, so = 0, alt = 0;
    apt_geo_subpoint(lines / 2.0, &sl, &so, &alt);
    if (n > 0)
      printf("  %+8.0f s  coast pts %6d  best %+8.1f s  score %6.3f   mid-subpoint %7.2f %8.2f\n",
             d, n, d + sh, s, sl, so);
    if (s > best) { best = s; best_d = d + sh; }
  }
  printf("timescan: best %+.1f s (score %.3f)\n", best_d, best);

  apt_geo_set_time_offset(keep);
  g_free(G.g);
}

// --- telemetry wedges ------------------------------------------------------
// Each 909-word video area is followed by a 45-word telemetry column carrying a
// 16-wedge frame, repeated every 128 lines: wedges 1–8 are a calibration
// staircase, 9 is zero, 10–15 are temperatures, and **16 is the channel ID** —
// its level equals the staircase step whose number is the AVHRR channel in that
// video area.  It is worth reading because it is the only statement the
// transmission makes about ITSELF: channel 1 or 2 in area A means the scene was
// sunlit (they are the visible channels), channel 3 means it was not, and area B
// coming out as 4 is the check that the reading is right at all.
#define TEL_A0  995
#define TEL_B0  2035
#define TEL_W   45

static void tel_column(GdkPixbuf *pb, int x0, double *out, int h) {
  int stride = gdk_pixbuf_get_rowstride(pb), nch = gdk_pixbuf_get_n_channels(pb);
  const guchar *px = gdk_pixbuf_get_pixels(pb);
  for (int y = 0; y < h; y++) {
    double s = 0;
    for (int x = x0; x < x0 + TEL_W; x++) s += px[(size_t)y * stride + (size_t)x * nch];
    out[y] = s / TEL_W;
  }
}

static void telemetry_report(void) {
  GdkPixbuf *pb = apt_decoder_get_full_image();
  if (pb == NULL) return;
  int h = gdk_pixbuf_get_height(pb);
  if (gdk_pixbuf_get_width(pb) < 2080 || h < 140) { g_object_unref(pb); return; }

  double *ta = g_new0(double, h), *tb = g_new0(double, h);
  tel_column(pb, TEL_A0, ta, h);
  tel_column(pb, TEL_B0, tb, h);
  g_object_unref(pb);

  // A wedge is 8 lines; find the phase that makes the wedges most distinct.
  int phase = 0; double bestv = -1;
  for (int p = 0; p < 8; p++) {
    int n = (h - p) / 8;
    double m = 0, m2 = 0;
    for (int i = 0; i < n; i++) {
      double s = 0;
      for (int k = 0; k < 8; k++) s += ta[p + i * 8 + k];
      s /= 8; m += s; m2 += s * s;
    }
    m /= n; double var = m2 / n - m * m;
    if (var > bestv) { bestv = var; phase = p; }
  }
  int nw = (h - phase) / 8;
  double *wa = g_new0(double, nw), *wb = g_new0(double, nw);
  for (int i = 0; i < nw; i++) {
    double sa = 0, sb = 0;
    for (int k = 0; k < 8; k++) { sa += ta[phase + i * 8 + k]; sb += tb[phase + i * 8 + k]; }
    wa[i] = sa / 8; wb[i] = sb / 8;
  }
  g_free(ta); g_free(tb);

  // Wedge 8 is white and wedge 9 is zero, so that fall is the frame boundary and
  // the only alignment needed.
  int ca[6] = {0}, cb[6] = {0}, frames = 0;
  for (int i = 9; i + 7 < nw; i++) {
    if (!(wa[i-1] > 200.0 && wa[i] < 80.0)) continue;      // i = wedge 9
    int f = i - 8;                                        // f = wedge 1
    gboolean rising = TRUE;
    for (int k = 0; k < 7; k++) if (wa[f+k+1] <= wa[f+k]) rising = FALSE;
    if (!rising) continue;
    frames++;
    for (int c = 0; c < 2; c++) {
      const double *w = c ? wb : wa;
      double best = 1e30; int id = 0;
      for (int s = 0; s < 8; s++) {
        double d = fabs(w[f + s] - w[f + 15]);            // f+15 = wedge 16
        if (d < best) { best = d; id = s + 1; }
      }
      if (id >= 1 && id <= 5) { if (c) cb[id]++; else ca[id]++; }
    }
  }

  if (frames == 0) { printf("telemetry: no complete wedge frame found\n"); g_free(wa); g_free(wb); return; }
  int ida = 0, idb = 0;
  for (int i = 1; i <= 5; i++) { if (ca[i] > ca[ida]) ida = i; if (cb[i] > cb[idb]) idb = i; }
  static const char *chan[6] = { "?", "1 (visible 0.63 um)", "2 (near IR 0.86 um)",
                                 "3 (3.7 um / 1.6 um)", "4 (IR 10.8 um)", "5 (IR 12 um)" };
  printf("telemetry: %d wedge frames; channel A = AVHRR %s, channel B = AVHRR %s\n",
         frames, chan[ida], chan[idb]);
  if (ida == 1 || ida == 2)
    printf("  -> area A is a VISIBLE channel: the scene was sunlit.\n");
  else if (ida == 3)
    printf("  -> area A is 3.7/1.6 um: night (3.7) or day (1.6) — not decisive on its own.\n");
  g_free(wa); g_free(wb);
}

// --- solar geometry --------------------------------------------------------
// Sun elevation at a ground point, NOAA's low-precision algorithm (good to a
// hundredth of a degree, which is four orders of magnitude better than this is
// being asked to decide).  It exists here because illumination is the one thing
// the picture says about the capture time that survives total cloud cover: a
// visible-channel image cannot have been taken in the dark, and the way its
// brightness runs along the pass is a fingerprint of the sun's position.
static double sun_elevation(double unix_utc, double lat, double lon) {
  double jd = unix_utc / 86400.0 + 2440587.5;
  double n = jd - 2451545.0;
  double L = fmod(280.460 + 0.9856474 * n, 360.0);        // mean longitude
  double g = fmod(357.528 + 0.9856003 * n, 360.0) * M_PI / 180.0;
  double lam = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * M_PI / 180.0;
  double eps = (23.439 - 0.0000004 * n) * M_PI / 180.0;
  double dec = asin(sin(eps) * sin(lam));
  double ra = atan2(cos(eps) * sin(lam), cos(lam));       // radians
  double gmst = fmod(18.697374558 + 24.06570982441908 * n, 24.0);
  if (gmst < 0) gmst += 24.0;
  double ha = gmst * 15.0 * M_PI / 180.0 + lon * M_PI / 180.0 - ra;
  double la = lat * M_PI / 180.0;
  double s = sin(la) * sin(dec) + cos(la) * cos(dec) * cos(ha);
  if (s > 1) s = 1; if (s < -1) s = -1;
  return asin(s) * 180.0 / M_PI;
}

// Does the modelled illumination along the pass match the brightness the visible
// channel actually recorded?  The picture's own contrast servo flattens the
// absolute levels, so the observable is the ratio of the visible area to the
// infrared one — the IR channel does not care about the sun, so the ratio keeps
// the illumination trend while the servo cancels out of both.
#define SUN_ROWS 24
static void sun_scan(int lines, double lo, double hi, double step) {
  if (!apt_geo_ready()) { printf("sunscan: no TLE or no time base\n"); return; }
  GdkPixbuf *pb = apt_decoder_get_full_image();
  if (pb == NULL || lines < 100) { if (pb) g_object_unref(pb); return; }
  int h = gdk_pixbuf_get_height(pb);
  int stride = gdk_pixbuf_get_rowstride(pb), nch = gdk_pixbuf_get_n_channels(pb);
  const guchar *px = gdk_pixbuf_get_pixels(pb);

  double obs[SUN_ROWS], rows[SUN_ROWS];
  for (int i = 0; i < SUN_ROWS; i++) {
    int y0 = (int)((double)i * h / SUN_ROWS), y1 = (int)((double)(i + 1) * h / SUN_ROWS);
    double sa = 0, sb = 0;
    for (int y = y0; y < y1; y++)
      for (int x = 86; x < 995; x++) {
        sa += px[(size_t)y * stride + (size_t)x * nch];
        sb += px[(size_t)y * stride + (size_t)(x + 1040) * nch];
      }
    obs[i] = (sb > 0) ? sa / sb : 0.0;
    rows[i] = 0.5 * (y0 + y1);
  }
  g_object_unref(pb);

  double keep = apt_geo_get_time_offset();
  printf("sunscan: %+.0f .. %+.0f s step %.0f  (r = correlation of modelled "
         "illumination with the visible/IR brightness ratio)\n", lo, hi, step);
  double best_r = -2.0, best_d = 0.0;
  for (double d = lo; d <= hi + 1e-6; d += step) {
    apt_geo_set_time_offset(d);
    double mu[SUN_ROWS], elev[SUN_ROWS];
    double emin = 999, emax = -999;
    gboolean ok = TRUE;
    for (int i = 0; i < SUN_ROWS; i++) {
      double la, lo2, alt;
      if (!apt_geo_subpoint(rows[i], &la, &lo2, &alt)) { ok = FALSE; break; }
      double t = apt_geo_row_utc(rows[i]);           // includes the trim
      double e = sun_elevation(t, la, lo2);
      if (e < emin) emin = e;
      if (e > emax) emax = e;
      elev[i] = e;
      mu[i] = e > 0 ? sin(e * M_PI / 180.0) : 0.0;
    }
    if (!ok) continue;
    // Pearson r between model and observation.
    double mm = 0, mo = 0;
    for (int i = 0; i < SUN_ROWS; i++) { mm += mu[i]; mo += obs[i]; }
    mm /= SUN_ROWS; mo /= SUN_ROWS;
    double smm = 0, soo = 0, smo = 0;
    for (int i = 0; i < SUN_ROWS; i++) {
      smm += (mu[i]-mm)*(mu[i]-mm); soo += (obs[i]-mo)*(obs[i]-mo);
      smo += (mu[i]-mm)*(obs[i]-mo);
    }
    double r = (smm > 0 && soo > 0) ? smo / sqrt(smm * soo) : 0.0;
    printf("  %+7.0f s  sun %+6.1f -> %+6.1f deg (range %+.0f..%+.0f)   r %+.3f%s\n",
           d, elev[0], elev[SUN_ROWS-1], emin, emax, r,
           emax < 0 ? "   DARK — a visible-channel picture is impossible" : "");
    if (emax > 0 && r > best_r) { best_r = r; best_d = d; }
  }
  printf("sunscan: best %+.0f s (r %+.3f)\n", best_d, best_r);
  apt_geo_set_time_offset(keep);
}

// --- map overlay -----------------------------------------------------------
// The only honest test of the projection: put a map on the picture and look at
// whether the coast is where the coast is.  Every number in geo_report() can be
// right while the image sits 200 km from where it says it does, because the
// numbers are all derived from the same time base and the same TLE.
//
// This deliberately goes through apt_map.c — the same cache and the same drawing
// the panel uses, with the identity transform in place of the widget's scale and
// pan — so what is checked here is the code that runs in the app.
// The projection is cached in the picture's own (unrotated) coordinates, so a
// rotated picture puts the map a half turn away from it — the same correction
// the panel makes, and the reason it is worth making here: get it wrong in one
// place and the overlay silently disagrees with the image.
typedef struct { int w, h, flip; } XF;

static gboolean identity_xform(double ix, double iy, double *ox, double *oy, gpointer u) {
  const XF *t = u;
  // `w - x` and not `(w-1) - x`: continuous coordinates mirror about the edge of
  // the image, not about the centre of its last pixel (see apt_panel.c).
  if (t != NULL && t->flip) { ix = t->w - ix; iy = t->h - iy; }
  *ox = ix; *oy = iy;
  return TRUE;
}

static void map_overlay(GdkPixbuf *pb, int lines) {
  int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
  XF xf = { w, h, apt_decoder_get_flip() };
  apt_map_update(lines, 0, w);

  // gdk-pixbuf is RGB and cairo wants ARGB32, so draw the map on a transparent
  // surface and composite it back by hand rather than converting the picture.
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(surf);
  apt_map_draw(cr, w, h, identity_xform, &xf);
  cairo_destroy(cr);
  cairo_surface_flush(surf);

  const unsigned char *src = cairo_image_surface_get_data(surf);
  int sstride = cairo_image_surface_get_stride(surf);
  guchar *dst = gdk_pixbuf_get_pixels(pb);
  int dstride = gdk_pixbuf_get_rowstride(pb), nch = gdk_pixbuf_get_n_channels(pb);
  for (int y = 0; y < h; y++) {
    const guint32 *sp = (const guint32 *)(src + (size_t)y * sstride);
    guchar *dp = dst + (size_t)y * dstride;
    for (int x = 0; x < w; x++) {
      guint32 px = sp[x];
      unsigned a = px >> 24;
      if (a == 0) continue;
      // Cairo ARGB32 is premultiplied.
      unsigned r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
      guchar *o = dp + x * nch;
      o[0] = (guchar)(r + o[0] * (255 - a) / 255);
      o[1] = (guchar)(g + o[1] * (255 - a) / 255);
      o[2] = (guchar)(b + o[2] * (255 - a) / 255);
    }
  }
  cairo_surface_destroy(surf);
}

static void save_png(const char *out, int coast, int lines) {
  GdkPixbuf *pb = apt_decoder_get_image();
  if (!pb) { printf("no image decoded\n"); return; }
  if (coast && apt_geo_ready()) {
    map_overlay(pb, lines);
    printf("map: coastline, graticule and ground track drawn\n");
  }
  GError *err = NULL;
  if (gdk_pixbuf_save(pb, out, "png", &err, NULL))
    printf("wrote %s (%d x %d)\n", out, gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
  else {
    fprintf(stderr, "save failed: %s\n", err ? err->message : "?");
    if (err) g_error_free(err);
  }
  g_object_unref(pb);
}

static void report(void) {
  apt_status_t st;
  apt_decoder_get_status(&st);
  printf("status: %s\n", st.status);
  printf("locked: %s   lines: %d   sync corr: %.2f   clock %+.0f ppm\n",
         st.locked ? "yes" : "no", st.lines, st.quality, st.clock_ppm);
}

// --- self test -------------------------------------------------------------
// One line of the APT word pattern, values 0..1.  Sync A is 7 cycles of a
// 1040 Hz square wave (4 words/cycle), sync B 7 cycles of 832 Hz (5 words), and
// the two image areas carry a known test pattern: a ramp in A, vertical bars in B.
static void build_line(double *v, int line) {
  for (int w = 0; w < WORDS; w++) v[w] = 0.0;
  for (int w = 4; w < 32; w++)  v[w] = (((w - 4) / 2) & 1) ? 0.0 : 1.0;         // sync A
  for (int w = 1044; w < 1079; w++) {
    int k = (w - 1044) / 5;                                                     // sync B
    v[w] = (k & 1) ? 0.0 : 1.0;
  }
  for (int i = 0; i < IMG_LEN; i++) {
    v[IMGA_OFF + i] = 0.05 + 0.9 * (double)i / IMG_LEN;                         // ramp
    v[IMGB_OFF + i] = ((i / 45 + line / 8) & 1) ? 0.85 : 0.15;                  // bars
  }
  for (int i = 0; i < 45; i++) {                                                // telemetry
    v[995 + i]  = 0.5;
    v[2035 + i] = 0.5;
  }
}

// Pearson correlation of a decoded row against the pattern it should be.
static double row_corr(const guint8 *row, const double *v, int from, int len) {
  double ms = 0.0, mv = 0.0;
  for (int i = 0; i < len; i++) { ms += row[from + i]; mv += v[from + i]; }
  ms /= len; mv /= len;
  double num = 0.0, ds = 0.0, dv = 0.0;
  for (int i = 0; i < len; i++) {
    double a = row[from + i] - ms, b = v[from + i] - mv;
    num += a * b; ds += a * a; dv += b * b;
  }
  if (ds <= 0.0 || dv <= 0.0) return 0.0;
  return num / sqrt(ds * dv);
}

// Check the decoded image against the source pattern: every fully-decoded line
// after the lock-in must match the line it was built from.
static int check_image(int nlines, const char *what) {
  GdkPixbuf *pb = apt_decoder_get_image();
  if (!pb) { printf("  %s: FAIL (no image)\n", what); return 1; }
  int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
  int stride = gdk_pixbuf_get_rowstride(pb);
  const guint8 *pix = gdk_pixbuf_get_pixels(pb);
  int bad = 0;
  double worst = 1.0;
  if (w != WORDS) { printf("  %s: FAIL (width %d, expected %d)\n", what, w, WORDS); g_object_unref(pb); return 1; }
  // Skip the first two decoded rows: the lock-in line can start mid-pattern.
  for (int y = 2; y < h; y++) {
    guint8 row[WORDS];
    for (int x = 0; x < WORDS; x++) row[x] = pix[(size_t)y * stride + x * 3];
    // Which source line this is: the decoder drops the lines before lock, so
    // find the best match over the pattern's own vertical period (bars repeat
    // every 8 lines, so 16 covers it unambiguously).
    double best = -2.0;
    for (int l = 0; l < 16; l++) {
      double v[WORDS]; build_line(v, l);
      double c = 0.5 * (row_corr(row, v, IMGA_OFF, IMG_LEN) + row_corr(row, v, IMGB_OFF, IMG_LEN));
      if (c > best) best = c;
    }
    if (best < worst) worst = best;
    if (best < 0.90) bad++;
  }
  printf("  %s: %d lines, worst line correlation %.3f, %d bad\n", what, h, worst, bad);
  g_object_unref(pb);
  if (h < nlines - 4) { printf("  %s: FAIL (only %d of ~%d lines)\n", what, h, nlines); return 1; }
  return bad > 0;
}

// Deterministic noise source: a run that fails must fail the same way twice.
static unsigned rng_state = 12345;
static double urand(void) {
  rng_state = rng_state * 1103515245u + 12345u;
  return (double)((rng_state >> 8) & 0xFFFFFF) / (double)0x1000000 - 0.5;
}

// Synthesise the AM-on-2400-Hz video baseband for `nlines` lines at `rate`.
// `err_ppm` stretches the transmitter's clock against the receiver's, which is
// what makes a real picture slant — the decoder has to measure and cancel it.
static double *synth_baseband(int nlines, double rate, double err_ppm, long *n_out) {
  // The whole transmission is generated against `gen`, so seen through the
  // receiver's own `rate` every line is err_ppm too long.
  double gen = rate * (1.0 + err_ppm / 1e6);
  long n = (long)(nlines * (WORDS / WORD_RATE) * rate);
  double *x = g_new(double, n);
  double ph = 0.0, step = 2.0 * M_PI * 2400.0 / gen;
  double v[WORDS];
  int cur = -1;
  for (long i = 0; i < n; i++) {
    double t = (double)i / gen;
    long word = (long)(t * WORD_RATE);
    int line = (int)(word / WORDS);
    if (line != cur) { build_line(v, line); cur = line; }
    double amp = 0.1 + 0.9 * v[word % WORDS];
    x[i] = amp * cos(ph);
    ph += step;
    if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
  }
  *n_out = n;
  return x;
}

// Feed a synthetic transmission through the audio entry point.
static void run_audio(int nlines, double rate, double err_ppm) {
  long n; double *x = synth_baseband(nlines, rate, err_ppm, &n);
  apt_decoder_set_enabled(FALSE);
  apt_decoder_set_enabled(TRUE);
  apt_decoder_set_channel(0);
  for (long i = 0; i < n; i += 4096) {
    int blk = (int)MIN(4096L, n - i);
    apt_decoder_add_audio(x + i, blk, 1, rate);
  }
  g_free(x);
}

// Feed a synthetic transmission through the I/Q entry point: FM-modulated,
// parked `off` Hz from the receiver centre, with `noise` of additive noise.
// `reset` starts a fresh decoder session; pass FALSE to continue the previous
// one, which is how the retune-between-satellites case is exercised.
static void feed_iq(int nlines, double rate, double off, double err_ppm,
                    double noise, int reset) {
  const double DEV = 17000.0;
  long n; double *x = synth_baseband(nlines, rate, err_ppm, &n);
  double *iq = g_new(double, (size_t)n * 2);
  double ph = 0.0;
  for (long i = 0; i < n; i++) {
    ph += 2.0 * M_PI * (off + DEV * x[i]) / rate;
    if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
    // The receiver's buffer is (Q, I) per pair — write it the way WDSP reads it.
    iq[i * 2]     = sin(ph) + noise * urand();
    iq[i * 2 + 1] = cos(ph) + noise * urand();
  }
  if (reset) {
    apt_decoder_set_enabled(FALSE);
    apt_decoder_set_enabled(TRUE);
    apt_decoder_set_channel(0);
  }
  for (long i = 0; i < n; i += 4096) {
    int blk = (int)MIN(4096L, n - i);
    apt_decoder_add_iq(iq + i * 2, blk, rate, 137000000LL, 137000000LL + (long long)off);
  }
  g_free(iq); g_free(x);
}

static void run_iq(int nlines, double rate, double off, double err_ppm, double noise) {
  feed_iq(nlines, rate, off, err_ppm, noise, 1);
}

// The same transmission with I and Q swapped — a receiver wired the other way
// round, or "Swap I & Q" left in the wrong position.  It conjugates the samples,
// which mirrors the spectrum about the centre, so the signal has to be looked
// for on the other side; everything after that should be identical, because the
// video is the AMPLITUDE of the subcarrier and negating a real signal changes
// neither amplitudes nor frequencies.
static void run_iq_swapped(int nlines, double rate, double off, double err_ppm) {
  const double DEV = 17000.0;
  long n; double *x = synth_baseband(nlines, rate, err_ppm, &n);
  double *iq = g_new(double, (size_t)n * 2);
  double ph = 0.0;
  for (long i = 0; i < n; i++) {
    ph += 2.0 * M_PI * (off + DEV * x[i]) / rate;
    if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
    iq[i * 2]     = -sin(ph);        // Q negated == I and Q swapped
    iq[i * 2 + 1] = cos(ph);
  }
  apt_decoder_set_enabled(FALSE);
  apt_decoder_set_enabled(TRUE);
  apt_decoder_set_channel(0);
  for (long i = 0; i < n; i += 4096) {
    int blk = (int)MIN(4096L, n - i);
    // Point at where the signal now appears: the other side of the centre.
    apt_decoder_add_iq(iq + i * 2, blk, rate, 137000000LL, 137000000LL - (long long)off);
  }
  g_free(iq); g_free(x);
}

static int selftest(void) {
  int fails = 0;
  const int NL = 32;

  // --- audio path: the baseband fed straight in, as a demodulated recording.
  printf("APT selftest: audio path (48 kHz)\n");
  run_audio(NL, 48000.0, 0.0);
  report();
  fails += check_image(NL, "audio");

  // --- I/Q path: proves the NCO sign, the (Q, I) buffer order and the
  // decimating front-end, none of which the audio path touches.  Off-centre on
  // purpose: a mirrored frequency axis would look identical at the centre.
  printf("APT selftest: I/Q path (192 kHz, +30 kHz off centre)\n");
  run_iq(NL, 192000.0, 30000.0, 0.0, 0.0);
  report();
  fails += check_image(NL, "iq");

  // --- the other side of the centre, at a different receiver rate, with noise.
  printf("APT selftest: I/Q path (96 kHz, −20 kHz off centre, noisy)\n");
  run_iq(NL, 96000.0, -20000.0, 0.0, 0.35);
  report();
  fails += check_image(NL, "iq-noisy");

  // --- clock error: a transmitter clock 400 ppm off is what slants a real
  // picture.  The servo has to measure it and cancel it, not just re-align each
  // line — so the check is both the recovered ppm and the picture itself.
  printf("APT selftest: clock error (+400 ppm)\n");
  run_audio(NL, 48000.0, 400.0);
  report();
  fails += check_image(NL, "slant");
  {
    apt_status_t st; apt_decoder_get_status(&st);
    double err = fabs(st.clock_ppm - 400.0);
    printf("  slant: recovered %+.0f ppm (error %.0f)\n", st.clock_ppm, err);
    if (err > 100.0) { printf("  slant: FAIL (clock servo did not converge)\n"); fails++; }
  }

  // --- I and Q swapped: must decode exactly as well.  Verified against the real
  // recording too, where the conjugated decode came out pixel-identical.
  printf("APT selftest: I and Q swapped (mirrored spectrum)\n");
  run_iq_swapped(NL, 192000.0, 30000.0, 0.0);
  report();
  fails += check_image(NL, "iq-swapped");

  // --- pure noise: the decoder must stay silent.  A 39-point correlation has a
  // noise standard deviation around 1/sqrt(39) = 0.16, and the search takes the
  // MAX over hundreds of positions, so random noise reaches surprisingly high
  // scores — if the thresholds sit below that, the decoder locks onto nothing,
  // paints garbage and (worse) lets the noise drag the clock servo, destroying
  // the ppm it measured during the pass.  Same class of bug as the CW decoder
  // streaming garbage on band noise before it grew a squelch.
  printf("APT selftest: pure noise must not lock\n");
  {
    const double RATE = 62500.0;
    long n = (long)(RATE * 20);
    double *iq = g_new(double, (size_t)n * 2);
    for (long i = 0; i < n; i++) { iq[i*2] = urand(); iq[i*2+1] = urand(); }
    apt_decoder_set_enabled(FALSE);
    apt_decoder_set_enabled(TRUE);
    double worst_corr = -2.0;
    for (long i = 0; i < n; i += 4096) {
      int blk = (int)MIN(4096L, n - i);
      apt_decoder_add_iq(iq + i * 2, blk, RATE, 137000000LL, 137000000LL);
      apt_status_t st; apt_decoder_get_status(&st);
      if (st.quality > worst_corr) worst_corr = st.quality;
    }
    apt_status_t st; apt_decoder_get_status(&st);
    printf("  noise: best correlation reached %.2f, %d lines drawn, %s\n",
           worst_corr, st.lines, st.locked ? "LOCKED" : "not locked");
    if (st.locked || st.lines > 0) {
      printf("  noise: FAIL (locked onto noise)\n");
      fails++;
    }
    g_free(iq);
  }

  // --- retune to another satellite: the second pass must be a NEW picture, not
  // extra lines appended under the first.  Without that the two passes share one
  // strip and one exposure and both are ruined — the reason WEFAX calls
  // begin_page() when it spots a start tone.
  printf("APT selftest: retune to another satellite\n");
  feed_iq(NL, 192000.0, 30000.0, 0.0, 0.0, 1);      // first bird
  feed_iq(NL, 192000.0, -30000.0, 0.0, 0.0, 0);     // operator moves to the next
  report();
  {
    apt_status_t st; apt_decoder_get_status(&st);
    printf("  retune: %d lines after the move (one pass is ~%d)\n", st.lines, NL - 2);
    fails += check_image(NL, "retune");
    if (st.lines > NL) {
      printf("  retune: FAIL (the new pass was appended to the old picture)\n");
      fails++;
    }
  }

  printf(fails ? "APT selftest: FAILED\n" : "APT selftest: PASS\n");
  return fails ? 1 : 0;
}

// --- main ------------------------------------------------------------------
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage: %s <file.wav> [-o out.png]\n"
      "       %s <file.wav> --iq <centre_hz> <cursor_hz> [-o out.png]\n"
      "       %s <file.wav> ... --tle <file> [--utc <YYYY-MM-DDTHH:MM:SSZ>] [--trim <s>] [--coast]\n"
      "       %s <file.wav> ... [--rotate off|180|north] [--timescan <lo> <hi> <step>]\n"
      "       %s --selftest\n", argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }
  if (!strcmp(argv[1], "--selftest")) return selftest();

  const char *path = argv[1];
  const char *out = "apt.png";
  const char *autosave_dir = NULL;
  const char *tle_path = NULL;
  const char *utc_str = NULL;
  double geo_trim = 0.0;
  int sat_catnr = 0;
  int coast = 0;
  int rotate = 0;                  // 0 = as received, 1 = 180°, 2 = north-up
  int timescan = 0, sunscan = 0;
  double ts_lo = 0, ts_hi = 0, ts_step = 0;
  double ss_lo = 0, ss_hi = 0, ss_step = 0;
  const char *rowtimes_path = NULL;
  int iq_mode = 0, conj = 0;
  long long centre = 0, cursor = 0;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--tle") && i + 1 < argc) { tle_path = argv[++i]; continue; }
    if (!strcmp(argv[i], "--utc") && i + 1 < argc) { utc_str = argv[++i]; continue; }
    if (!strcmp(argv[i], "--trim") && i + 1 < argc) { geo_trim = atof(argv[++i]); continue; }
    if (!strcmp(argv[i], "--sat")  && i + 1 < argc) { sat_catnr = atoi(argv[++i]); continue; }
    if (!strcmp(argv[i], "--coast")) { coast = 1; continue; }
    // Sweep the time base and score the projected coastline against the
    // picture's own edges — the only test of the projection that does not come
    // from the projection itself.
    // The same sweep judged by illumination instead of coastlines: it is the
    // test that still works when the scene is solid cloud, because the sun does
    // not care what is under it.
    if (!strcmp(argv[i], "--sunscan") && i + 3 < argc) {
      sunscan = 1;
      ss_lo = atof(argv[i+1]); ss_hi = atof(argv[i+2]); ss_step = atof(argv[i+3]);
      i += 3;
      continue;
    }
    if (!strcmp(argv[i], "--timescan") && i + 3 < argc) {
      timescan = 1;
      ts_lo = atof(argv[i+1]); ts_hi = atof(argv[i+2]); ts_step = atof(argv[i+3]);
      i += 3;
      continue;
    }
    // Orientation, the same three settings the panel offers.  "north" needs an
    // orbit to ask, so it does nothing without --tle — which is the point: a
    // rotation on a guess is worse than none.
    if (!strcmp(argv[i], "--rotate") && i + 1 < argc) {
      const char *v = argv[++i];
      if (!strcmp(v, "off"))        rotate = 0;
      else if (!strcmp(v, "180"))   rotate = 1;
      else if (!strcmp(v, "north")) rotate = 2;
      else { fprintf(stderr, "--rotate wants off, 180 or north\n"); return 2; }
      continue;
    }
    // The row -> UTC map the decoder built, one stamp per line.  A projection
    // that is off is nearly always the time base rather than the geometry, and
    // this is the only way to look at it.
    if (!strcmp(argv[i], "--rowtimes") && i + 1 < argc) { rowtimes_path = argv[++i]; continue; }
    if (!strcmp(argv[i], "--autosave") && i + 1 < argc) {
      // Exercise the end-of-pass auto-save the panel turns on: the decoder
      // writes the picture itself when the pass ends, on its own worker thread.
      autosave_dir = argv[i+1]; i++;
    } else if (!strcmp(argv[i], "--iq") && i + 2 < argc) {
      iq_mode = 1; centre = atoll(argv[i+1]); cursor = atoll(argv[i+2]); i += 2;
    } else if (!strcmp(argv[i], "--conj")) {
      // Swap I and Q, i.e. conjugate the recording: what you get from a receiver
      // wired the other way round, or "Swap I & Q" left in the wrong position.
      // It mirrors the spectrum about the centre.
      conj = 1;
    } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
      out = argv[i+1]; i++;
    } else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }

  unsigned rate, chans; long nframes;
  WAV_AUXI ax;
  short *buf = wav_read(path, &rate, &chans, &nframes, &ax);
  if (!buf) return 1;
  printf("file: %u Hz, %u ch, %ld frames (%.1f s)\n", rate, chans, nframes, (double)nframes/rate);

  // Capture time: the command line wins over the recorder's own stamp, which
  // wins over nothing at all (in which case the decoder stamps from the wall
  // clock and the projection is meaningless — hence the warning).
  double utc0 = 0.0;
  if (utc_str) {
    GDateTime *dt = g_date_time_new_from_iso8601(utc_str, NULL);
    if (!dt) { fprintf(stderr, "cannot parse --utc %s\n", utc_str); return 2; }
    utc0 = (double)g_date_time_to_unix(dt);
    g_date_time_unref(dt);
  } else if (ax.utc > 0.0) {
    utc0 = ax.utc;
  }
  if (utc0 > 0.0) {
    GDateTime *dt = g_date_time_new_from_unix_utc((gint64)utc0);
    char *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
    printf("capture start: %s UTC%s%s\n", s, utc_str ? " (--utc)" : " (auxi chunk",
           utc_str ? "" : ")");
    g_free(s); g_date_time_unref(dt);
    if (!utc_str && ax.centre_hz) printf("auxi centre: %lld Hz\n", ax.centre_hz);
  }

  apt_decoder_set_enabled(TRUE);
  apt_decoder_set_channel(0);
  if (utc0 > 0.0) apt_decoder_set_stream_utc(utc0);
  if (autosave_dir) apt_decoder_set_autosave(TRUE, autosave_dir);

  if (tle_path) {
    char *err = NULL;
    if (!apt_geo_load_tle(tle_path, &err)) {
      fprintf(stderr, "TLE: %s\n", err ? err : "failed");
      g_free(err);
      return 1;
    }
    // Same rule as the app: the bird is chosen by where the decoder is
    // listening, not by a name typed twice.  A demodulated-audio file carries
    // no frequency, so there --sat is the only way to say which satellite it is.
    if (sat_catnr) {
      if (!apt_geo_select_catnr(sat_catnr))
        fprintf(stderr, "TLE: no element set for catalogue number %d\n", sat_catnr);
    } else if (iq_mode) {
      if (!apt_geo_select_freq(cursor))
        fprintf(stderr, "TLE: no APT satellite near %lld Hz — use --sat <catnr>\n", cursor);
    } else {
      fprintf(stderr, "TLE: an audio file says nothing about which satellite it is"
                      " — use --sat <catnr>\n");
    }
    apt_geo_set_time_offset(geo_trim);
  }

  // Progress trace: print the servo's state every 30 s of recording.  This is
  // how the clock/Doppler behaviour over a pass is read — the line clock arrives
  // scaled by the satellite's own v/c (about ±25 ppm, changing sign as it passes
  // overhead), so the recovered ppm should drift smoothly through the pass on
  // top of the receiver's fixed error.
  long trace_every = (long)(rate * 30);
  long next_trace = trace_every;

  const int BLK = 4096;
  double *d = g_new(double, (size_t)BLK * 2);
  if (iq_mode) {
    if (chans != 2) { fprintf(stderr, "--iq needs a stereo (I/Q) file\n"); return 1; }
    printf("centre %lld Hz, cursor %lld Hz (%+lld Hz)\n", centre, cursor, cursor - centre);
    for (long i = 0; i < nframes; i += BLK) {
      int blk = (int)MIN((long)BLK, nframes - i);
      for (int k = 0; k < blk * 2; k++) d[k] = buf[(i * 2) + k] / 32768.0;
      // The buffer is (Q, I); negating Q conjugates the complex sample.
      if (conj) for (int k = 0; k < blk; k++) d[k * 2] = -d[k * 2];
      apt_decoder_add_iq(d, blk, (double)rate, centre, cursor);
      if (i >= next_trace) {
        apt_status_t st; apt_decoder_get_status(&st);
        printf("  t=%4.0f s  lines %4d  sync %.2f  clock %+7.1f ppm  %s\n",
               (double)i / rate, st.lines, st.quality, st.clock_ppm,
               st.locked ? "lock" : "----");
        next_trace += trace_every;
      }
    }
  } else {
    for (long i = 0; i < nframes; i += BLK) {
      int blk = (int)MIN((long)BLK, nframes - i);
      for (int k = 0; k < blk; k++) d[k] = buf[(i + k) * chans] / 32768.0;
      apt_decoder_add_audio(d, blk, 1, (double)rate);
      if (i >= next_trace) {
        apt_status_t st; apt_decoder_get_status(&st);
        printf("  t=%4.0f s  lines %4d  sync %.2f  clock %+7.1f ppm  %s\n",
               (double)i / rate, st.lines, st.quality, st.clock_ppm,
               st.locked ? "lock" : "----");
        next_trace += trace_every;
      }
    }
  }
  g_free(d); g_free(buf);

  report();
  telemetry_report();

  int geo_lines = 0;
  if (tle_path) {
    // The decoder's own per-row stamps, not row × 0.5 s: this is the map that
    // survives the fades in the middle of a real pass.
    static double rt[4096];
    int n = apt_decoder_get_row_times(rt, (int)G_N_ELEMENTS(rt));
    if (rowtimes_path) {
      FILE *rf = fopen(rowtimes_path, "w");
      if (rf) { for (int i = 0; i < n; i++) fprintf(rf, "%d %.6f\n", i, rt[i]); fclose(rf);
                printf("wrote %s (%d rows)\n", rowtimes_path, n); }
    }
    if (n >= 2) apt_geo_set_row_times(rt, n);
    else if (utc0 > 0.0) apt_geo_set_time_base(utc0);
    geo_report(n);
    geo_lines = n;
    if (timescan) time_scan(n, ts_lo, ts_hi, ts_step);
    if (sunscan)  sun_scan(n, ss_lo, ss_hi, ss_step);
  }

  // Orientation, decided exactly as radio.c decides it: 180° on request, and on
  // an ascending pass when the operator asked for north-up and there is an orbit
  // to ask.  Set before anything is handed out, so the PNG, the overlay and the
  // auto-save all agree.
  if (rotate == 1 || (rotate == 2 && apt_geo_ready() && apt_geo_pass_ascending())) {
    apt_decoder_set_flip(TRUE);
    printf("rotate: picture turned 180%s\n",
           rotate == 2 ? "° (ascending pass — north up)" : "°");
  } else if (rotate == 2) {
    printf("rotate: north-up asked for, %s\n",
           apt_geo_ready() ? "pass is descending — left as received"
                           : "no orbit to ask — left as received");
  }

  save_png(out, coast, geo_lines);
  if (autosave_dir) {
    // Ending the decode ends the pass, which is one of the auto-save triggers.
    apt_decoder_set_enabled(FALSE);
    g_usleep(3 * G_USEC_PER_SEC);   // the PNG is encoded on a worker thread
    printf("auto-save: check %s\n", autosave_dir);
  }
  return 0;
}
