// Offline SSTV harness: encode a synthetic test image with sstv_encoder.c, feed
// the resulting 48 kHz tone stream straight into sstv_decoder.c, and score the
// decoded picture against the one that went in.  No GTK window, no audio device,
// no radio — the standing rule is that nothing is verified by starting the app.
//
//   sstv_offline --selftest              every family, PASS/FAIL per mode
//   sstv_offline --selftest -o /tmp/sstv write the TX/RX PNGs out as well
//   sstv_offline --mode 44 [-o pfx]      one mode, by VIS code (see --list)
//   sstv_offline --mode 44 --slant 10000 the same, with a deliberate pixel-clock
//                                        error, to see what the score does
//   sstv_offline --mode 44 --forced      decode with the mode forced instead of
//                                        read from the VIS — reproduces the
//                                        documented gotcha, and fails
//   sstv_offline --list                  the modes this harness covers
//
// The encoder and the decoder are a matched pair — one writes the tone timeline
// the other measures — so a round trip is the only test that exercises both
// halves of the timing table at once, and it is exactly the pairing a refactor
// can break silently: a wrong porch length in one file alone still produces a
// picture, just a smeared or shifted one.
//
// Three things this harness has to respect:
//
//  * DECODE IN AUTO (VIS).  The decoder's forced-mode anchor starts on the first
//    plausible sync pulse, and in a same-app round trip that is the VIS break —
//    a 10 ms 1200 Hz pulse that sits inside every mode's sync-length gate.
//    Measured (--mode 44 --forced): the picture anchors ~300 ms early, completes
//    against nothing, and the re-anchor on a later sync then wipes it — MAE
//    123.78, i.e. worse than the flat-green control.  Auto mode reads the
//    transmitted VIS, which is what this pairing is designed around (documented
//    in CLAUDE.md).  sstv_decoder_set_mode(0) is therefore set explicitly, not
//    left to the decoder's default.
//
//  * ONE MODE PER FAMILY AT LEAST.  The per-line decode is family-dispatched
//    (Martin / Scottie / Robot36 / Robot72 / PD each have their own line
//    layout), so a break in one family is completely invisible in another.
//
//  * A NUMBER, NOT AN EYEBALL.  Each mode is scored as mean absolute error per
//    channel (0..255) and PSNR against the source image, and the thresholds are
//    set from the measured clean-round-trip figures with a wide margin.  To
//    prove the threshold means something, every mode is also scored against a
//    flat green picture (the exact failure a squelched SSTV decode produces —
//    see the squelch-bypass note in CLAUDE.md) and that has to FAIL it.
//
// Geometry note: the encoder scales the source to the mode's native size, so
// the test image is generated at native size and no resampling is needed to
// compare.  Two real effects are allowed for, and only these two:
//   - a border crop (MARGIN_X columns, MARGIN_Y rows) because the first and last
//     pixel of every scan sits against the porch/separator tone and the
//     discriminator's 51-tap response smears across that boundary;
//   - a small integer registration search (±REG_X, ±REG_Y) whose result is
//     PRINTED, because the discriminator group delay is a fixed sub-pixel-to-
//     pixel shift of the whole picture.  A shift is all it can absorb: a slant
//     or a wrong line period is not a translation and still scores badly.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

// Prerequisite types for radio.h — the same include order sstv_encoder.c uses.
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "mode.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"

#include "sstv_encoder.h"
#include "sstv_decoder.h"

// ---- the application, reduced to what sstv_encoder.c actually reaches for ---
// The encoder keys the transmitter and reads the global radio; the decoder has
// no RADIO coupling at all.  That is the whole application surface here.
RADIO *radio;
static int mox_transitions;
void set_mox(RADIO *r, gboolean state) { (void)r; (void)state; mox_transitions++; }

// ---- mode table (geometry only; the timings live in the two modules) --------
typedef struct { int vis; const char *name; int w, h; const char *family; } hmode_t;

static const hmode_t HMODES[] = {
  { 44, "Martin M1",  320, 256, "Martin"  },
  { 40, "Martin M2",  320, 256, "Martin"  },
  { 60, "Scottie S1", 320, 256, "Scottie" },
  { 56, "Scottie S2", 320, 256, "Scottie" },
  { 76, "Scottie DX", 320, 256, "Scottie" },
  {  8, "Robot 36",   320, 240, "Robot36" },
  { 12, "Robot 72",   320, 240, "Robot72" },
  { 93, "PD50",       320, 256, "PD"      },
  { 99, "PD90",       320, 256, "PD"      },
  { 95, "PD120",      640, 496, "PD"      },
  { 98, "PD160",      512, 400, "PD"      },
  { 96, "PD180",      640, 496, "PD"      },
  { 97, "PD240",      640, 496, "PD"      },
};
#define N_HMODES (int)(sizeof(HMODES) / sizeof(HMODES[0]))

static const hmode_t *hmode_by_vis(int vis) {
  for (int i = 0; i < N_HMODES; i++) if (HMODES[i].vis == vis) return &HMODES[i];
  return NULL;
}

// ---- test image -------------------------------------------------------------
// Four bands, each aimed at a different way the pair can break:
//   1. colour bars           — colourspace and the GBR / YUV per-family ordering
//   2. horizontal grey ramp  — the 1500..2300 Hz level scale end to end
//   3. vertical stripes      — horizontal phase: a slant walks them line by line
//   4. horizontal stripes    — vertical alignment; the right half alternates
//                              red/green so a chroma error shows up too
// Deliberately full-amplitude: a 100 % bar's Cr/Cb just touches the 0..255 rail,
// which is a real transmission's behaviour and worth carrying in the score.
static GdkPixbuf *make_test_image(int w, int h) {
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
  if (pb == NULL) return NULL;
  guint8 *p = gdk_pixbuf_get_pixels(pb);
  int st = gdk_pixbuf_get_rowstride(pb);

  static const guint8 bars[8][3] = {
    {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
    {255,0,255},   {255,0,0},   {0,0,255},   {0,0,0},
  };
  int b1 = h / 4, b2 = h / 2, b3 = (3 * h) / 4;
  int vper = w / 16;  if (vper < 4) vper = 4;     // vertical stripe half-period
  int hper = h / 32;  if (hper < 4) hper = 4;     // horizontal stripe half-period

  for (int y = 0; y < h; y++) {
    guint8 *row = p + (gsize)y * st;
    for (int x = 0; x < w; x++) {
      guint8 R, G, B;
      if (y < b1) {
        int bi = (x * 8) / w; if (bi > 7) bi = 7;
        R = bars[bi][0]; G = bars[bi][1]; B = bars[bi][2];
      } else if (y < b2) {
        guint8 v = (guint8)((x * 255) / (w - 1));
        R = G = B = v;
      } else if (y < b3) {
        guint8 v = ((x / vper) & 1) ? 255 : 0;
        R = G = B = v;
      } else {
        guint8 v = (((y - b3) / hper) & 1) ? 255 : 0;
        if (x >= w / 2) { R = v; G = (guint8)(255 - v); B = 64; }
        else            { R = G = B = v; }
      }
      row[x*3+0] = R; row[x*3+1] = G; row[x*3+2] = B;
    }
  }
  return pb;
}

// A flat "squelch green" picture — RGB(0,135,0) is what (Y,Cr,Cb)=(0,0,0) maps
// to in the decoder's full-range BT.601 conversion, i.e. the exact thing a
// broken decode produces.  Used as the negative control for the thresholds.
static GdkPixbuf *make_green_image(int w, int h) {
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
  if (pb != NULL) gdk_pixbuf_fill(pb, 0x008700ffu);
  return pb;
}

// ---- scoring ----------------------------------------------------------------
#define MARGIN_X 8      // columns dropped either side (scan/porch boundary)
#define MARGIN_Y 4      // rows dropped top and bottom (line-0 anchor, last line)
#define REG_X    3      // registration search, columns
#define REG_Y    2      // registration search, rows

typedef struct { double mae, psnr; int dx, dy; } score_t;

static double score_at(const guint8 *a, int as, const guint8 *b, int bs,
                       int w, int h, int dx, int dy, double *mse_out) {
  double sad = 0.0, sse = 0.0;
  long n = 0;
  for (int y = MARGIN_Y; y < h - MARGIN_Y; y++) {
    const guint8 *pa = a + (gsize)y * as;
    const guint8 *pb = b + (gsize)(y + dy) * bs;
    for (int x = MARGIN_X; x < w - MARGIN_X; x++) {
      for (int c = 0; c < 3; c++) {
        double d = (double)pa[x*3+c] - (double)pb[(x+dx)*3+c];
        sad += fabs(d); sse += d * d; n++;
      }
    }
  }
  if (n == 0) return 1e9;
  if (mse_out) *mse_out = sse / (double)n;
  return sad / (double)n;
}

// Best score over the small registration window; the winning offset is reported
// so a shift that is NOT sub-pixel group delay is visible in the output.
static gboolean score_images(GdkPixbuf *src, GdkPixbuf *dec, score_t *out) {
  if (src == NULL || dec == NULL) return FALSE;
  int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
  if (gdk_pixbuf_get_width(dec) != w || gdk_pixbuf_get_height(dec) != h) return FALSE;
  if (w <= 2 * (MARGIN_X + REG_X) || h <= 2 * (MARGIN_Y + REG_Y)) return FALSE;
  const guint8 *a = gdk_pixbuf_get_pixels(src); int as = gdk_pixbuf_get_rowstride(src);
  const guint8 *b = gdk_pixbuf_get_pixels(dec); int bs = gdk_pixbuf_get_rowstride(dec);

  out->mae = 1e9; out->psnr = 0.0; out->dx = 0; out->dy = 0;
  for (int dy = -REG_Y; dy <= REG_Y; dy++) {
    for (int dx = -REG_X; dx <= REG_X; dx++) {
      double mse = 0.0;
      double mae = score_at(a, as, b, bs, w, h, dx, dy, &mse);
      if (mae < out->mae) {
        out->mae = mae; out->dx = dx; out->dy = dy;
        out->psnr = (mse > 0.0) ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
      }
    }
  }
  return TRUE;
}

// ---- audio plumbing ---------------------------------------------------------
#define BLK 1024
static double blk[BLK * 2];
static int    blkn = 0;

static void push(double s) {
  blk[blkn * 2] = s; blk[blkn * 2 + 1] = s;      // decoder reads samples[i*2]
  if (++blkn == BLK) { sstv_decoder_add_audio(blk, BLK); blkn = 0; }
}
static void push_flush(void) {
  if (blkn > 0) { sstv_decoder_add_audio(blk, blkn); blkn = 0; }
}
static void push_silence(double ms) {
  long n = (long)(ms * 48.0);
  for (long i = 0; i < n; i++) push(0.0);
}

// Encode `src` as mode `vis`, run the tone stream through the decoder, and hand
// back the decoded picture (caller unrefs).  Clean tones only: this test is
// about whether the two timing tables agree, and a noise floor would put a
// calibration argument in front of that question rather than behind it.
// --forced sets the decoder's forced mode instead, which is NOT how this pairing
// is meant to be driven — it exists so the documented gotcha can be reproduced
// (and measured) rather than only asserted.  See the header comment.
static gboolean opt_forced = FALSE;

static GdkPixbuf *round_trip(int vis, GdkPixbuf *src, double slant_ppm, double *secs) {
  sstv_decoder_set_enabled(TRUE);
  sstv_decoder_set_mode(opt_forced ? vis : 0);   // AUTO (VIS) — header comment
  sstv_decoder_reset();
  sstv_decoder_adjust_slant(slant_ppm - sstv_decoder_get_slant());
  blkn = 0;

  if (!sstv_tx_prepare(vis, src)) { fprintf(stderr, "sstv_tx_prepare(%d) failed\n", vis); return NULL; }
  double dur = sstv_tx_duration();
  if (secs) *secs = dur;
  sstv_tx_start();
  if (!sstv_tx_active()) { fprintf(stderr, "sstv_tx_start did not key\n"); return NULL; }

  push_silence(50.0);                     // lead-in: fills the Hilbert history
  long n = (long)(dur * 48000.0) + 2;
  for (long i = 0; i < n; i++) {
    push(sstv_tx_next_sample());
  }
  sstv_tx_stop();
  // Tail: the decoder will not emit a line until ~20 ms past the end of its last
  // scan, so the final line needs silence after the waveform to come out.
  push_silence(1000.0);
  push_flush();

  return sstv_decoder_get_image();
}

// ---- per-mode pass thresholds ----------------------------------------------
// Set from measurement, not from taste.  The clean round trip scores (this
// harness, this test image, macOS/clang -O3) are in the trailing comment of each
// row; the MAE limit is ~3x that and the PSNR floor ~6 dB under it (a factor of
// four in MSE — the same margin expressed the other way).  Wide enough that
// ordinary retuning of the DSP will not trip them, and nowhere near wide enough
// to pass a broken decode:
//
//   flat "squelch green" picture .................. MAE ~118   (checked below)
//   0.5 % pixel-clock error (Martin M1, 1.6 px
//     stretch across the line) .................... MAE  2.29, PSNR 24.1 dB
//   1.0 % pixel-clock error (3.2 px stretch) ...... MAE  3.96, PSNR 19.9 dB
//   5.0 % pixel-clock error ....................... MAE 24.58, PSNR 10.6 dB
//
// against Martin M1's clean 0.97 / 44.0 dB — i.e. a clock error small enough to
// look like a slightly leaning picture already fails, which is the sensitivity
// this test exists for.  (Reproduce any of them with --mode 44 --slant <ppm>.)
typedef struct { int vis; double mae_max; double psnr_min; } thresh_t;

static const thresh_t THRESH[] = {
  { 44,  3.0, 38.0 },   // Martin M1   clean 0.97 / 44.02
  { 40,  6.0, 27.0 },   // Martin M2   clean 1.90 / 33.33  (half M1's pixel time)
  { 60,  3.0, 34.0 },   // Scottie S1  clean 0.99 / 40.54
  { 56,  5.0, 26.0 },   // Scottie S2  clean 1.68 / 32.61
  { 76,  2.0, 44.0 },   // Scottie DX  clean 0.60 / 50.21
  {  8, 12.0, 17.0 },   // Robot 36    clean 4.13 / 22.84  (4:2:0, chroma shared
                        //              between row pairs — the loosest by far,
                        //              and legitimately so)
  { 12,  6.0, 30.0 },   // Robot 72    clean 1.89 / 36.43
  { 93,  6.0, 26.0 },   // PD50        clean 2.02 / 32.54
  { 99,  2.5, 39.0 },   // PD90        clean 0.70 / 45.11
  { 95,  9.0, 20.0 },   // PD120       clean 3.10 / 26.48
  { 98,  4.0, 30.0 },   // PD160       clean 1.15 / 36.83
  { 96,  7.5, 21.0 },   // PD180       clean 2.50 / 27.53
  { 97,  6.0, 21.0 },   // PD240       clean 1.87 / 27.76
};

static const thresh_t *thresh_by_vis(int vis) {
  for (int i = 0; i < (int)(sizeof(THRESH)/sizeof(THRESH[0])); i++)
    if (THRESH[i].vis == vis) return &THRESH[i];
  return NULL;
}

// ---- one mode ---------------------------------------------------------------
static int run_mode(int vis, const char *png_prefix, gboolean verbose, double slant_ppm) {
  const hmode_t *m = hmode_by_vis(vis);
  const thresh_t *t = thresh_by_vis(vis);
  if (m == NULL || t == NULL) { printf("unknown VIS %d\n", vis); return 1; }

  GdkPixbuf *src = make_test_image(m->w, m->h);
  double secs = 0.0;
  GdkPixbuf *dec = round_trip(vis, src, slant_ppm, &secs);

  int fails = 0;
  sstv_status_t st;
  sstv_decoder_get_status(&st);

  if (dec == NULL) {
    printf("FAIL  %-11s (%-7s) no image decoded\n", m->name, m->family);
    g_object_unref(src);
    return 1;
  }

  // The VIS header must have been read: this is the auto-detect path, and a
  // decode that landed on the right geometry by accident is not a pass.
  if (st.vis != vis) {
    printf("FAIL  %-11s (%-7s) auto-detect chose VIS %d (%s), not %d\n",
           m->name, m->family, st.vis, st.mode_name, vis);
    fails++;
  }

  score_t sc;
  if (!score_images(src, dec, &sc)) {
    printf("FAIL  %-11s (%-7s) geometry %dx%d, expected %dx%d\n",
           m->name, m->family,
           gdk_pixbuf_get_width(dec), gdk_pixbuf_get_height(dec), m->w, m->h);
    g_object_unref(src); g_object_unref(dec);
    return 1;
  }

  // Negative control: the same comparison against a flat green picture must be
  // nowhere near the threshold, or the threshold is decoration.
  GdkPixbuf *green = make_green_image(m->w, m->h);
  score_t gsc; score_images(src, green, &gsc);
  g_object_unref(green);

  gboolean ok = sc.mae <= t->mae_max && sc.psnr >= t->psnr_min &&
                gsc.mae > t->mae_max * 2.0;
  if (!ok) fails++;

  printf("%s  %-11s (%-7s) MAE %6.2f (<=%5.1f)  PSNR %5.2f dB (>=%4.1f)  "
         "offset %+d,%+d  green-control MAE %6.2f  [%d lines, %.1f s audio]\n",
         ok ? "PASS" : "FAIL", m->name, m->family,
         sc.mae, t->mae_max, sc.psnr, t->psnr_min, sc.dx, sc.dy, gsc.mae,
         st.line + 1, secs);

  if (verbose)
    printf("      status: \"%s\" progress %d%%  slant %.1f ppm  afc %.1f Hz\n",
           st.status, st.progress, sstv_decoder_get_slant(), sstv_decoder_get_afc());

  if (png_prefix != NULL) {
    char path[512];
    g_snprintf(path, sizeof(path), "%s_%d_tx.png", png_prefix, vis);
    gdk_pixbuf_save(src, path, "png", NULL, NULL);
    g_snprintf(path, sizeof(path), "%s_%d_rx.png", png_prefix, vis);
    gdk_pixbuf_save(dec, path, "png", NULL, NULL);
  }

  g_object_unref(src);
  g_object_unref(dec);
  return fails;
}

// ---- geometry negative control ---------------------------------------------
// The flat-green control proves the threshold rejects a picture with no content.
// This one proves it rejects a picture with the RIGHT content in the WRONG place
// — the failure a broken timing table actually produces.  A deliberate 1 %
// pixel-clock trim stretches every scan by 3.2 px across a 320 px line while the
// per-line sync lock keeps the lines themselves aligned, so it is a pure
// geometry error and cannot be absorbed by the registration search.
#define CONTROL_SLANT_PPM 10000.0

static int geometry_control(void) {
  const int vis = 44;                       // Martin M1
  const hmode_t *m = hmode_by_vis(vis);
  const thresh_t *t = thresh_by_vis(vis);
  GdkPixbuf *src = make_test_image(m->w, m->h);
  GdkPixbuf *dec = round_trip(vis, src, CONTROL_SLANT_PPM, NULL);
  score_t sc = { 1e9, 0.0, 0, 0 };
  gboolean scored = dec != NULL && score_images(src, dec, &sc);
  // "Rejected" means it must miss the threshold, not that it must be unreadable.
  gboolean rejected = !scored || sc.mae > t->mae_max || sc.psnr < t->psnr_min;
  printf("%s  %-11s (control) 1%% clock error scores MAE %6.2f / PSNR %5.2f dB "
         "-> rejected by the %s threshold\n",
         rejected ? "PASS" : "FAIL", m->name, sc.mae, sc.psnr,
         rejected ? "mode's" : "(NOT REJECTED)");
  if (src) g_object_unref(src);
  if (dec) g_object_unref(dec);
  return rejected ? 0 : 1;
}

// ---- selftest ---------------------------------------------------------------
// One mode per family plus a second Martin/Scottie/PD, so a family-dispatch
// break cannot hide and the two different pixel clocks inside a family are both
// exercised.  Every mode in the table can still be run by hand with --mode.
static const int SELFTEST_VIS[] = { 44, 40, 60, 56, 8, 12, 99, 95 };

static int selftest(const char *png_prefix) {
  int fails = 0;
  printf("SSTV round trip: sstv_encoder.c -> 48 kHz tones -> sstv_decoder.c, "
         "decoded in Auto (VIS)\n");
  printf("  compared over the picture less a %d px / %d row border, best of a "
         "+-%d,+-%d px registration\n\n", MARGIN_X, MARGIN_Y, REG_X, REG_Y);

  int n_modes = (int)(sizeof(SELFTEST_VIS)/sizeof(SELFTEST_VIS[0]));
  for (int i = 0; i < n_modes; i++)
    fails += run_mode(SELFTEST_VIS[i], png_prefix, FALSE, 0.0);

  printf("\n");
  fails += geometry_control();

  // The encoder must have keyed and unkeyed the transmitter for every pass — a
  // waveform that plays without MOX is a silent transmission, and the drop is
  // the half that matters (a TX left keyed is the cardinal sin).
  int expect_mox = 2 * (n_modes + 1);
  printf("%s  MOX transitions: %d (expected %d, one key + one drop per pass)\n",
         mox_transitions == expect_mox ? "PASS" : "FAIL", mox_transitions, expect_mox);
  if (mox_transitions != expect_mox) fails++;

  printf("\n%s\n", fails == 0 ? "all cases passed" : "FAILURES ABOVE");
  return fails == 0 ? 0 : 1;
}

// ---- main -------------------------------------------------------------------
int main(int argc, char **argv) {
  const char *png_prefix = NULL;
  int mode_vis = 0;
  gboolean want_selftest = FALSE, want_list = FALSE;
  double slant = 0.0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--selftest")) want_selftest = TRUE;
    else if (!strcmp(argv[i], "--list")) want_list = TRUE;
    else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode_vis = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--slant") && i + 1 < argc) slant = atof(argv[++i]);
    else if (!strcmp(argv[i], "--forced")) opt_forced = TRUE;
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) png_prefix = argv[++i];
    else {
      fprintf(stderr,
        "usage: %s --selftest [-o <png-prefix>]\n"
        "       %s --mode <vis> [--slant <ppm>] [--forced] [-o <png-prefix>]\n"
        "       %s --list\n", argv[0], argv[0], argv[0]);
      return 2;
    }
  }

  if (want_list) {
    printf("%-4s %-11s %-8s %s\n", "VIS", "mode", "family", "size");
    for (int i = 0; i < N_HMODES; i++)
      printf("%-4d %-11s %-8s %dx%d\n",
             HMODES[i].vis, HMODES[i].name, HMODES[i].family, HMODES[i].w, HMODES[i].h);
    return 0;
  }

  // The encoder refuses to key unless the radio can transmit and the active
  // receiver is in a mode that carries SSTV.  This is the whole RADIO surface it
  // touches, so a couple of zeroed structs stand in for the application.
  RECEIVER *rx = g_new0(RECEIVER, 1);
  rx->mode_a = DIGU;
  radio = g_new0(RADIO, 1);
  radio->active_receiver = rx;
  radio->can_transmit = TRUE;
  radio->mox = FALSE;

  int rc;
  if (mode_vis != 0)      rc = run_mode(mode_vis, png_prefix, TRUE, slant) == 0 ? 0 : 1;
  else if (want_selftest) rc = selftest(png_prefix);
  else {
    fprintf(stderr, "usage: %s --selftest [-o <png-prefix>]\n", argv[0]);
    rc = 2;
  }

  g_free(radio); g_free(rx); radio = NULL;
  return rc;
}
