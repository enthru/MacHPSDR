// Offline WEFAX harness: synthesise an HF radiofax transmission, feed the tone
// stream straight into wefax_decoder.c, and score the decoded page against the
// picture that went in.  No GTK window, no audio device, no radio.
//
//   wefax_offline --selftest             every case, PASS/FAIL
//   wefax_offline --selftest -o /tmp/wf  write the TX/RX PNGs out as well
//   wefax_offline --slant 10000          one round trip with a deliberate
//                                        line-clock error, to see the score move
//
// WEFAX is the odd one out among this tree's image decoders: there is no
// encoder to pair it with (the app only receives fax), so unlike sstv_offline
// this harness carries its own modulator.  That is a weaker test in one
// specific way and a stronger one in another, and both are worth stating:
//
//   * WEAKER: a round trip against a modulator written from the same reading of
//     the format cannot catch a shared misunderstanding of the format.  What it
//     does catch is the decoder drifting away from the convention it was built
//     to -- which is the regression a refactor actually causes.
//   * STRONGER: the modulator is not the decoder's mirror image, so the tone
//     stream is not built from the decoder's own timing table.  It is generated
//     from the published numbers (1500 Hz black, 2300 Hz white, 120 lpm,
//     IOC 576, a 300 Hz start tone) and nothing else.
//
// Four things it has to get right, each of which is a separate failure:
//
//   * THE START-TONE DETECTOR IS TESTED WITH ITS NEGATIVE CONTROL.  A 300 Hz
//     black/white square starts an IOC576 page and a 675 Hz one starts IOC288;
//     what matters just as much is that a page of ordinary CHART CONTENT -- a
//     mostly-white image with sparse black lines, which is what a weather chart
//     is -- starts nothing.  That is the bimodality gate in the decoder, and
//     without a test for it the gate could be deleted and every self-test would
//     still pass while real reception restarted its page mid-chart.
//
//   * GEOMETRY IS SCORED WITH THE PHASING SERVO OFF.  Manual start plus
//     autophase off means the decoder's line boundaries are exactly where the
//     modulator put them, so the score measures only what it is supposed to:
//     the line period, the per-line resampling to IMG_W and the tone->grey map.
//
//   * THE PHASING SERVO IS SCORED SEPARATELY, by transmitting the SAME picture
//     deliberately mis-phased by a third of a line and requiring the servo to
//     recover it to the same score.  With the servo off that transmission is
//     torn in half down the middle, so the two cases together say the servo is
//     doing the work rather than the alignment being free.
//
//   * A NUMBER, NOT AN EYEBALL.  Every case is mean absolute error (0..255) and
//     PSNR against the source, and the thresholds come from the measured clean
//     figures.  To prove they discriminate, a flat grey page and a 1 % line-
//     clock error are scored too and both must FAIL.
//
// Registration: the decoded page is compared after a small search in x and y,
// and the winning offset is PRINTED.  A shift is all a search can absorb -- the
// discriminator's 51-tap group delay is a fixed sub-pixel-to-pixel translation
// of the whole page -- while a slant or a wrong line period is not a
// translation and still scores badly.  For the phasing case the x search is
// seeded by cross-correlating the column-mean profiles, because there the shift
// is a real (and correct) fraction of a line rather than group delay.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "wefax_decoder.h"

// ---- the format, from the published numbers --------------------------------
#define SR          48000.0
#define F_BLACK     1500.0
#define F_WHITE     2300.0
#define LPM         120           // lines per minute (weather-fax standard)
#define IOC576_TONE 300.0         // start-tone square-wave rate for IOC 576
#define IOC288_TONE 675.0         // ... and for IOC 288

// The decoder resamples every line to this width whatever the IOC, so the test
// picture is generated at it and no resampling is needed to compare.  Kept as
// its own name here rather than reaching into the decoder's IMG_W, so a change
// there shows up as a geometry failure instead of silently moving both sides.
#define W           1810
#define PIC_H       120           // 60 s of fax at 120 lpm

static double line_samples(void) { return SR * 60.0 / (double)LPM; }

// ---- modulator -------------------------------------------------------------
// Phase-continuous FM: the decoder measures the derivative of the phase, so a
// per-segment phase reset would put a spike at every pixel boundary.
static double mod_phase = 0.0;

#define BLK 1024
static double blk[BLK * 2];
static int    blkn = 0;

static void push(double s) {
  blk[blkn * 2] = s; blk[blkn * 2 + 1] = s;      // decoder reads samples[i*2]
  if (++blkn == BLK) { wefax_decoder_add_audio(blk, BLK); blkn = 0; }
}
static void push_flush(void) {
  if (blkn > 0) { wefax_decoder_add_audio(blk, blkn); blkn = 0; }
}

static void tone(double hz, long n) {
  for (long i = 0; i < n; i++) {
    mod_phase += 2.0 * M_PI * hz / SR;
    if (mod_phase > 2.0 * M_PI) mod_phase -= 2.0 * M_PI;
    push(sin(mod_phase));
  }
}

static double grey2freq(double v) {          // 0..255 -> 1500..2300 Hz
  if (v < 0.0) v = 0.0;
  if (v > 255.0) v = 255.0;
  return F_BLACK + v * (F_WHITE - F_BLACK) / 255.0;
}

// One scan line of `src` (row y), `len` samples long.  len carries the slant:
// a modulator clock error is the thing the decoder cannot absorb by shifting.
static void tx_line(const guint8 *src, int stride, int y, double len) {
  double spp = len / (double)W;
  long n = (long)(len + 0.5);
  for (long i = 0; i < n; i++) {
    int x = (int)((double)i / spp);
    if (x >= W) x = W - 1;
    tone(grey2freq(src[(gsize)y * stride + x * 3]), 1);
  }
}

// The start signal: a balanced black/white square wave at `hz`, for `secs`.
static void tx_start_tone(double hz, double secs) {
  long half = (long)(SR / hz / 2.0 + 0.5);
  long n = (long)(SR * secs);
  long done = 0;
  int white = 0;
  while (done < n) {
    long k = half; if (done + k > n) k = n - done;
    tone(white ? F_WHITE : F_BLACK, k);
    white = !white;
    done += k;
  }
}

// ---- test picture ----------------------------------------------------------
// Predominantly WHITE, like a real weather chart -- that is not decoration: the
// decoder's one-time AFC anchors on the dominant frequency over the first 24
// lines and calls it white, so a picture with a different balance would be
// testing a different code path from the one reception uses.
//
// Five bands, each aimed at a distinct way the decode can break:
//   left margin bar  -- the recurring vertical edge the phasing servo locks to
//   grey ramp        -- the 1500..2300 Hz level scale, end to end
//   grey steps       -- the same, quantised, where a gamma slip is obvious
//   vertical bars    -- horizontal phase; a line-clock error walks them over
//   horizontal bars  -- the line period itself; a wrong one smears them
#define MARGIN_BAR 15

static GdkPixbuf *make_test_image(void) {
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, W, PIC_H);
  if (pb == NULL) return NULL;
  guint8 *p = gdk_pixbuf_get_pixels(pb);
  int st = gdk_pixbuf_get_rowstride(pb);

  int b1 = PIC_H / 5, b2 = (2 * PIC_H) / 5, b3 = (3 * PIC_H) / 5, b4 = (4 * PIC_H) / 5;

  for (int y = 0; y < PIC_H; y++) {
    guint8 *row = p + (gsize)y * st;
    for (int x = 0; x < W; x++) {
      guint8 v;
      if (x < MARGIN_BAR) {
        v = 0;                                            // the fax margin
      } else if (y < b1) {
        v = 255;                                          // plain white header
      } else if (y < b2) {
        v = (guint8)(((x - MARGIN_BAR) * 255) / (W - 1 - MARGIN_BAR));
      } else if (y < b3) {
        int s = ((x - MARGIN_BAR) * 8) / (W - MARGIN_BAR); if (s > 7) s = 7;
        v = (guint8)(s * 255 / 7);
      } else if (y < b4) {
        v = (((x - MARGIN_BAR) / 40) & 1) ? 255 : 0;      // vertical bars
      } else {
        v = (((y - b4) / 3) & 1) ? 255 : 40;              // horizontal bars
      }
      row[x*3+0] = row[x*3+1] = row[x*3+2] = v;
    }
  }
  return pb;
}

// The negative control: a flat mid-grey page.  This is what a decode that has
// lost the signal altogether produces, and the thresholds have to reject it.
static GdkPixbuf *make_flat_image(void) {
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, W, PIC_H);
  if (pb != NULL) gdk_pixbuf_fill(pb, 0x808080ffu);
  return pb;
}

// ---- scoring ----------------------------------------------------------------
#define MARGIN_X 24     // columns dropped either side (line-edge discriminator smear)
#define MARGIN_Y 3      // rows dropped top and bottom (first/last line)
#define REG_X    4      // fine registration search, columns
#define REG_Y    2      // ... and rows

typedef struct { double mae, psnr; int dx, dy; } score_t;

static double score_at(const guint8 *a, int as, const guint8 *b, int bs,
                       int h, int dx, int dy, double *mse_out) {
  double sad = 0.0, sse = 0.0;
  long n = 0;
  for (int y = MARGIN_Y; y < h - MARGIN_Y; y++) {
    int yb = y + dy;
    if (yb < 0 || yb >= h) continue;
    const guint8 *pa = a + (gsize)y * as;
    const guint8 *pb = b + (gsize)yb * bs;
    for (int x = MARGIN_X; x < W - MARGIN_X; x++) {
      int xb = x + dx;
      if (xb < 0 || xb >= W) continue;
      double d = (double)pa[x*3] - (double)pb[xb*3];
      sad += fabs(d); sse += d * d; n++;
    }
  }
  if (n == 0) return 1e9;
  if (mse_out) *mse_out = sse / (double)n;
  return sad / (double)n;
}

// Coarse x offset from the column-mean profiles.  The fine +/-REG_X window is
// enough for group delay, but the phasing case is a real fraction of a line --
// hundreds of pixels -- so it has to be found rather than searched for.
static int coarse_dx(const guint8 *a, int as, const guint8 *b, int bs, int h) {
  static double pa[W], pb[W];
  for (int x = 0; x < W; x++) { pa[x] = 0.0; pb[x] = 0.0; }
  for (int y = MARGIN_Y; y < h - MARGIN_Y; y++)
    for (int x = 0; x < W; x++) {
      pa[x] += a[(gsize)y * as + x*3];
      pb[x] += b[(gsize)y * bs + x*3];
    }
  double ma = 0.0, mb = 0.0;
  for (int x = 0; x < W; x++) { ma += pa[x]; mb += pb[x]; }
  ma /= W; mb /= W;
  double best = -1e300; int bestk = 0;
  for (int k = -W/2; k < W/2; k++) {
    double s = 0.0;
    for (int x = 0; x < W; x++) {
      int xb = x + k;
      if (xb < 0 || xb >= W) continue;
      s += (pa[x] - ma) * (pb[xb] - mb);
    }
    if (s > best) { best = s; bestk = k; }
  }
  return bestk;
}

static gboolean score_images(GdkPixbuf *src, GdkPixbuf *dec, gboolean wide_x,
                             score_t *out) {
  if (src == NULL || dec == NULL) return FALSE;
  int h = gdk_pixbuf_get_height(src);
  if (gdk_pixbuf_get_width(src) != W || gdk_pixbuf_get_width(dec) != W) return FALSE;
  if (gdk_pixbuf_get_height(dec) < h) return FALSE;
  const guint8 *a = gdk_pixbuf_get_pixels(src); int as = gdk_pixbuf_get_rowstride(src);
  const guint8 *b = gdk_pixbuf_get_pixels(dec); int bs = gdk_pixbuf_get_rowstride(dec);

  int cx = wide_x ? coarse_dx(a, as, b, bs, h) : 0;
  out->mae = 1e9; out->psnr = 0.0; out->dx = 0; out->dy = 0;
  for (int dy = -REG_Y; dy <= REG_Y; dy++) {
    for (int dx = cx - REG_X; dx <= cx + REG_X; dx++) {
      double mse = 0.0;
      double mae = score_at(a, as, b, bs, h, dx, dy, &mse);
      if (mae < out->mae) {
        out->mae = mae; out->dx = dx; out->dy = dy;
        out->psnr = (mse > 0.0) ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
      }
    }
  }
  return TRUE;
}

// ---- round trip -------------------------------------------------------------
// Manual start rather than the start tone: begin_page() latches the line phase
// at whatever sample the third qualifying start-tone second happens to end on,
// which is a real property of reception but makes the geometry score depend on
// a rounding.  The start tone gets its own test above instead.
static GdkPixbuf *round_trip(GdkPixbuf *src, gboolean autophase,
                             double slant_ppm, double phase_error_frac) {
  wefax_decoder_set_enabled(TRUE);
  wefax_decoder_reset();
  wefax_decoder_set_lpm(LPM);
  wefax_decoder_set_ioc(576);
  wefax_decoder_set_autostart(FALSE);     // the tone test owns that path
  wefax_decoder_set_autophase(autophase);
  wefax_decoder_set_denoise(TRUE);
  wefax_decoder_set_invert(FALSE);
  wefax_decoder_set_levels(1.0, 0.0);
  wefax_decoder_adjust_slant(-wefax_decoder_get_slant());   // back to 0 ppm
  blkn = 0; mod_phase = 0.0;

  const guint8 *p = gdk_pixbuf_get_pixels(src);
  int st = gdk_pixbuf_get_rowstride(src);
  double len = line_samples() * (1.0 + slant_ppm / 1e6);

  // Lead-in: white, not silence.  Silence reads as 0 Hz, i.e. a black page, and
  // would both fill the Hilbert history with something the decoder never sees
  // on air and drag the white-anchored AFC.
  tone(F_WHITE, (long)(SR * 0.25));
  push_flush();

  // Latch the line phase here, then optionally slip the transmitter by a
  // fraction of a line so the phasing servo has something to correct.
  wefax_decoder_start();
  if (phase_error_frac != 0.0) tone(F_WHITE, (long)(len * phase_error_frac));

  for (int y = 0; y < PIC_H; y++) tx_line(p, st, y, len);
  // Tail: the last line is only decoded once the ring holds a little past its
  // end (the trailing pixel average).
  tone(F_WHITE, (long)(len * 2));
  push_flush();

  return wefax_decoder_get_image();
}

// ---- start-tone cases -------------------------------------------------------
// Returns the IOC the decoder settled on, or 0 if it never started a page.
static int try_start_tone(double hz, double secs) {
  wefax_decoder_set_enabled(TRUE);
  wefax_decoder_reset();
  wefax_decoder_set_lpm(LPM);
  wefax_decoder_set_autostart(TRUE);
  wefax_decoder_set_autophase(TRUE);
  blkn = 0; mod_phase = 0.0;
  tone(F_WHITE, (long)(SR * 0.25));
  tx_start_tone(hz, secs);
  push_flush();
  wefax_status_t st;
  wefax_decoder_get_status(&st);
  return st.receiving ? st.ioc : 0;
}

// The negative control for the detector: chart-like content, i.e. mostly white
// with sparse black lines, for long enough that a detector without the
// bimodality gate would have fired several times over.
static int try_chart_content(double secs) {
  wefax_decoder_set_enabled(TRUE);
  wefax_decoder_reset();
  wefax_decoder_set_lpm(LPM);
  wefax_decoder_set_autostart(TRUE);
  wefax_decoder_set_autophase(TRUE);
  blkn = 0; mod_phase = 0.0;
  tone(F_WHITE, (long)(SR * 0.25));
  // ~300 black ticks per second, but each only 4 % of its period: the crossing
  // RATE lands squarely in the IOC576 window, so only the balance test can
  // reject it.  That is the point -- it is the hardest thing to tell from a
  // start tone by rate alone.
  long period = (long)(SR / IOC576_TONE + 0.5);
  long n = (long)(SR * secs), done = 0;
  while (done < n) {
    long dark = period / 25; if (dark < 1) dark = 1;
    tone(F_BLACK, dark);
    tone(F_WHITE, period - dark);
    done += period;
  }
  push_flush();
  wefax_status_t st;
  wefax_decoder_get_status(&st);
  return st.receiving ? st.ioc : 0;
}

// ---- reporting --------------------------------------------------------------
static int failures = 0;
static const char *out_prefix = NULL;

static void report(const char *what, gboolean ok, const char *fmt, ...) {
  va_list ap;
  char detail[256];
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  printf("%-46s %-6s %s\n", what, ok ? "PASS" : "FAIL", detail);
  if (!ok) failures++;
}

static void save(GdkPixbuf *pb, const char *suffix) {
  if (out_prefix == NULL || pb == NULL) return;
  char path[512];
  snprintf(path, sizeof(path), "%s_%s.png", out_prefix, suffix);
  gdk_pixbuf_save(pb, path, "png", NULL, NULL);
  printf("   wrote %s\n", path);
}

// ---- pass thresholds --------------------------------------------------------
// Set from measurement, not from taste: ~3x the measured MAE and ~6 dB under
// the measured PSNR (a factor of four in MSE -- the same margin the other way
// round).  Measured on macOS/clang -O3 with this test picture:
//
//   clean round trip, servo off ................... MAE  2.40, PSNR 36.9 dB
//   mis-phased 1/3 line, servo ON ................. MAE  2.80, PSNR 27.6 dB
//   mis-phased 1/3 line, servo OFF ................ MAE 85.39   (the control)
//   flat grey page ................................ MAE 123.98, PSNR  4.4 dB
//   1 % line-clock error .......................... MAE 81.71,  PSNR  6.6 dB
//
// The clean MAE is not zero and should not be: the decoder's one-time AFC
// quantises the anchored white level to a 20 Hz bin, which is a systematic
// couple of grey levels across the whole page, and the 51-tap discriminator
// rounds every black/white transition in the bar patterns.
//
// The gap between the clean figure and the two controls is the whole point -- a
// line-clock error small enough to read as a slightly leaning chart already
// scores 34x the clean MAE.  Reproduce any of them with --slant <ppm>.
#define CLEAN_MAE_MAX   7.5
#define CLEAN_PSNR_MIN  31.0
#define PHASE_MAE_MAX   8.5
#define PHASE_PSNR_MIN  21.5

static void selftest(void) {
  GdkPixbuf *src = make_test_image();
  GdkPixbuf *flat = make_flat_image();
  score_t s;

  printf("\n-- start-tone detector --\n");
  int ioc = try_start_tone(IOC576_TONE, 6.0);
  report("300 Hz start tone starts an IOC 576 page", ioc == 576,
         "detected IOC %d", ioc);
  ioc = try_start_tone(IOC288_TONE, 6.0);
  report("675 Hz start tone starts an IOC 288 page", ioc == 288,
         "detected IOC %d", ioc);
  ioc = try_chart_content(12.0);
  report("chart content starts NOTHING (bimodality)", ioc == 0,
         "12 s of mostly-white content -> %s",
         ioc == 0 ? "no page started" : "a page started");

  printf("\n-- geometry (manual start, phasing servo off) --\n");
  GdkPixbuf *dec = round_trip(src, FALSE, 0.0, 0.0);
  gboolean ok = score_images(src, dec, FALSE, &s);
  report("clean round trip", ok && s.mae <= CLEAN_MAE_MAX && s.psnr >= CLEAN_PSNR_MIN,
         "MAE %.2f  PSNR %.1f dB  (dx %+d dy %+d)", s.mae, s.psnr, s.dx, s.dy);
  save(src, "tx"); save(dec, "rx");
  double clean_mae = s.mae;
  if (dec) g_object_unref(dec);

  printf("\n-- phasing servo --\n");
  // The same picture, transmitted a third of a line late.  With the servo off
  // this is torn down the middle; with it on the decoder has to put it back.
  dec = round_trip(src, FALSE, 0.0, 1.0 / 3.0);
  ok = score_images(src, dec, FALSE, &s);
  report("mis-phased by 1/3 line, servo OFF -> torn", ok && s.mae > 3.0 * clean_mae,
         "MAE %.2f (clean %.2f)", s.mae, clean_mae);
  save(dec, "misphased");
  if (dec) g_object_unref(dec);

  dec = round_trip(src, TRUE, 0.0, 1.0 / 3.0);
  ok = score_images(src, dec, TRUE, &s);
  report("mis-phased by 1/3 line, servo ON -> recovered",
         ok && s.mae <= PHASE_MAE_MAX && s.psnr >= PHASE_PSNR_MIN,
         "MAE %.2f  PSNR %.1f dB  (dx %+d dy %+d)", s.mae, s.psnr, s.dx, s.dy);
  save(dec, "rephased");
  if (dec) g_object_unref(dec);

  printf("\n-- negative controls (these must FAIL the thresholds) --\n");
  dec = round_trip(flat, FALSE, 0.0, 0.0);
  ok = score_images(src, dec, FALSE, &s);
  report("flat grey page scores WORSE than the limit",
         ok && (s.mae > CLEAN_MAE_MAX || s.psnr < CLEAN_PSNR_MIN),
         "MAE %.2f  PSNR %.1f dB", s.mae, s.psnr);
  if (dec) g_object_unref(dec);

  dec = round_trip(src, FALSE, 10000.0, 0.0);   // 1 % line-clock error
  ok = score_images(src, dec, FALSE, &s);
  report("1 % line-clock error scores WORSE than the limit",
         ok && (s.mae > CLEAN_MAE_MAX || s.psnr < CLEAN_PSNR_MIN),
         "MAE %.2f  PSNR %.1f dB", s.mae, s.psnr);
  save(dec, "slant");
  if (dec) g_object_unref(dec);

  g_object_unref(src);
  g_object_unref(flat);
}

int main(int argc, char **argv) {
  gboolean want_selftest = FALSE;
  double slant = 0.0;
  gboolean want_slant = FALSE;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--selftest")) want_selftest = TRUE;
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_prefix = argv[++i];
    else if (!strcmp(argv[i], "--slant") && i + 1 < argc) { slant = atof(argv[++i]); want_slant = TRUE; }
    else {
      fprintf(stderr,
        "usage: %s --selftest [-o prefix]\n"
        "       %s --slant <ppm> [-o prefix]\n", argv[0], argv[0]);
      return 2;
    }
  }

  if (want_slant) {
    GdkPixbuf *src = make_test_image();
    GdkPixbuf *dec = round_trip(src, FALSE, slant, 0.0);
    score_t s;
    if (score_images(src, dec, FALSE, &s))
      printf("slant %.0f ppm: MAE %.2f  PSNR %.1f dB  (dx %+d dy %+d)\n",
             slant, s.mae, s.psnr, s.dx, s.dy);
    else
      printf("slant %.0f ppm: nothing decoded\n", slant);
    save(src, "tx"); save(dec, "rx");
    if (dec) g_object_unref(dec);
    g_object_unref(src);
    return 0;
  }

  if (!want_selftest) {
    fprintf(stderr, "nothing to do; try --selftest\n");
    return 2;
  }

  selftest();
  printf("\n%s\n", failures == 0 ? "all cases passed" : "FAILURES ABOVE");
  return failures == 0 ? 0 : 1;
}
