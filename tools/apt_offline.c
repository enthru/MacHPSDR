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
    printf("  along-track %.2f km/row (%.1f km/s ground speed), pass runs %s\n",
           step, step / APT_LINE_SECONDS,
           mid_lat[2] < mid_lat[0] ? "north to south (descending — north-up image)"
                                   : "south to north (ascending — image is upside down)");
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

// --- coastline overlay -----------------------------------------------------
// The only honest test of the projection: put a map on the picture and look at
// whether the coast is where the coast is.  Every number in geo_report() can be
// right while the image sits 200 km from where it says it does, because the
// numbers are all derived from the same time base and the same TLE.
//
// The map is the vendored assets/coastline.bin (apt_coast.c).  Long segments are
// subdivided, since a straight line in lat/lon is not a straight line in scan
// geometry.
static void draw_px(GdkPixbuf *pb, int x, int y, guchar r, guchar g, guchar b) {
  if (x < 0 || y < 0 || x >= gdk_pixbuf_get_width(pb) || y >= gdk_pixbuf_get_height(pb)) return;
  guchar *p = gdk_pixbuf_get_pixels(pb) + y * gdk_pixbuf_get_rowstride(pb)
              + x * gdk_pixbuf_get_n_channels(pb);
  p[0] = r; p[1] = g; p[2] = b;
}

static int coast_overlay(GdkPixbuf *pb, int lines) {
  if (!apt_coast_load(NULL)) return 0;
  int drawn = 0;

  for (int i = 0; i < apt_coast_count(); i++) {
    int n = 0;
    const APT_COAST_PT *p = apt_coast_polyline(i, &n);
    for (int k = 1; k < n; k++) {
      double plat = p[k-1].lat, plon = p[k-1].lon, lat = p[k].lat, lon = p[k].lon;
      // ~25 km steps: fine enough that a coast stays a line at 4 km/pixel.
      double d = hypot(lat - plat, (lon - plon) * cos(lat * M_PI / 180.0));
      int m = (int)(d * 111.0 / 25.0) + 1;
      if (m > 200) m = 200;
      for (int j = 0; j <= m; j++) {
        double la = plat + (lat - plat) * j / m, lo = plon + (lon - plon) * j / m;
        double r, w;
        if (!apt_geo_latlon_to_pixel(la, lo, 0, lines - 1, &r, &w)) continue;
        int x = (int)(w + 0.5), y = (int)(r + 0.5);
        draw_px(pb, x, y, 255, 60, 60);
        draw_px(pb, x + CHAN_STEP, y, 255, 60, 60);   // the same ground in channel B
        drawn++;
      }
    }
  }
  return drawn;
}

static void save_png(const char *out, int coast, int lines) {
  GdkPixbuf *pb = apt_decoder_get_image();
  if (!pb) { printf("no image decoded\n"); return; }
  if (coast && apt_geo_ready())
    printf("coastline: %d points drawn\n", coast_overlay(pb, lines));
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
      "       %s --selftest\n", argv[0], argv[0], argv[0], argv[0]);
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
  const char *rowtimes_path = NULL;
  int iq_mode = 0, conj = 0;
  long long centre = 0, cursor = 0;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--tle") && i + 1 < argc) { tle_path = argv[++i]; continue; }
    if (!strcmp(argv[i], "--utc") && i + 1 < argc) { utc_str = argv[++i]; continue; }
    if (!strcmp(argv[i], "--trim") && i + 1 < argc) { geo_trim = atof(argv[++i]); continue; }
    if (!strcmp(argv[i], "--sat")  && i + 1 < argc) { sat_catnr = atoi(argv[++i]); continue; }
    if (!strcmp(argv[i], "--coast")) { coast = 1; continue; }
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
