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

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

#include "wefax_decoder.h"
#include "log.h"

#define SR              48000.0    // demod audio sample rate (Hz)
#define F_BLACK         1500.0     // black level
#define F_WHITE         2300.0     // white level
#define F_CENTER        1900.0     // black/white midpoint (start/phasing carrier)

// ---- Hilbert-transform FM discriminator (same design as the SSTV decoder) --
#define HIL             51
#define HIL_C           ((HIL - 1) / 2)

// ---- instantaneous-frequency ring ------------------------------------------
// Must hold several lines of the slowest rate (60 lpm = 1 s/line).  2^18 =
// 262144 samples ≈ 5.4 s (~10 lines at 120 lpm).  Power of two → modulo is a mask.
#define FR_BITS         18
#define FR_CAP          (1 << FR_BITS)
#define FR_MASK         (FR_CAP - 1)

// ---- image geometry --------------------------------------------------------
// A fixed image width (each line is resampled to this regardless of IOC) and a
// scrolling buffer height: when full, the top third scrolls off so reception is
// continuous.  IMG_W is ~the native IOC576 element count (576·π ≈ 1809) so the
// fine chart lines are resolved at ~1 element per pixel instead of being blurred
// to faint grey by undersampling; the panel scales the wide image to fit.
#define IMG_W           1810
#define BUF_H           1200
#define SCROLL          (BUF_H / 3)

// Phasing servo runs for this long after a page starts, then the phase locks so
// image content cannot drag the alignment.
#define PHASING_MS      25000.0

// ---- state (audio-thread owned unless noted) -------------------------------
static gboolean enabled = FALSE;

// live parameters (set from the GTK thread)
static volatile int      p_lpm = 120;
static volatile int      p_ioc = 576;
static volatile gboolean p_autostart = TRUE;
static volatile gboolean p_autophase = TRUE;  // continuous auto-phasing (self-align)
static volatile gboolean p_denoise   = TRUE;  // conditional-median despeckle
static volatile gboolean p_invert    = FALSE; // negative image (white<->black)
static volatile gboolean start_req = FALSE;   // manual Start button
static volatile gboolean reset_req = FALSE;
static volatile double   slant_ppm = 0.0;     // slant/clock trim (GTK adds)
static volatile double   pending_phase = 0.0; // manual phase nudge, samples (GTK adds)

// discriminator
static double  hcoef[HIL];
static gboolean hcoef_ready = FALSE;
static double  hbuf[HIL];
static int     hpos = 0;
static double  prevI = 0.0, prevQ = 0.0;

// inst-freq ring
static float   fr[FR_CAP];
static long    ring_w = 0;                 // total samples written (absolute)

// line tracking
static gboolean started = FALSE;
static double  line_start_d = 0.0;         // absolute sample of the current line start
static long    phasing_until = 0;          // phase servo active while ring_w < this
static double  freq_offset = 0.0;          // AFC (Hz)

// start-tone / AFC estimator (1-second blocks)
static long    st_block_start = 0;
static int     st_state = 0;               // hysteresis state for crossing counter
static int     st_trans = 0;               // low->high transitions in the block
static int     st_match_run = 0;           // consecutive qualifying start-tone blocks
static int     st_match_cls = 0;           // IOC of the current run
static double  st_lo_sum = 0.0, st_hi_sum = 0.0;
static int     st_lo_n = 0, st_hi_n = 0;
static int     st_blk_n = 0;               // samples in the current block
static int     st_black_n = 0, st_white_n = 0; // samples near black / near white
static long    start_cooldown = 0;         // suppress re-triggering the start tone until here

// continuous auto-phase: per-column EMA of the edge energy.  A vertical feature
// that recurs at the same column every line (the fax margin / border) builds a
// sharp peak; random image content spreads out and averages away.  The servo
// steers the line start so that peak sits at the left margin.
static float   col_energy[IMG_W];
static gboolean phase_locked = FALSE;      // the servo has found a stable reference
static int     acq_lines = 0;              // lines accumulated during acquisition

// One-time white-anchored AFC (see decode_line): a coarse frequency histogram
// over the first lines of a page; its dominant bin is the white background.
#define AFC_FMIN   1200.0
#define AFC_BINHZ  20.0
#define AFC_BINS   70                       // 1200..2600 Hz
static int     freq_hist[AFC_BINS];
static int     afc_lines = 0;
static gboolean afc_done = FALSE;

// image buffer (mutex protected)
static GMutex  lock;
static guint8  img[IMG_W * BUF_H * 3];
static int     cur_row = 0;                 // next row to write
static int     filled = 0;                  // rows currently valid (<= BUF_H)
static wefax_status_t ui;

// --------------------------------------------------------------------------
static void hcoef_init(void) {
  for (int k = 0; k < HIL; k++) {
    int m = k - HIL_C;
    double h = (m & 1) ? (2.0 / (M_PI * m)) : 0.0;            // ideal Hilbert
    double w = 0.54 - 0.46 * cos(2.0 * M_PI * k / (HIL - 1)); // Hamming window
    hcoef[k] = h * w;
  }
  hcoef_ready = TRUE;
}

static void set_status(const char *s) { g_strlcpy(ui.status, s, sizeof(ui.status)); }

static void do_reset(void) {
  memset(hbuf, 0, sizeof(hbuf));
  hpos = 0; prevI = prevQ = 0.0;
  ring_w = 0;
  started = FALSE; line_start_d = 0.0; phasing_until = 0;
  freq_offset = 0.0;
  st_block_start = 0; st_state = 0; st_trans = 0; st_match_run = 0; st_match_cls = 0;
  st_lo_sum = st_hi_sum = 0.0; st_lo_n = st_hi_n = 0;
  st_blk_n = st_black_n = st_white_n = 0; start_cooldown = 0;
  memset(col_energy, 0, sizeof(col_energy)); phase_locked = FALSE; acq_lines = 0;
  memset(freq_hist, 0, sizeof(freq_hist)); afc_lines = 0; afc_done = FALSE;
  g_mutex_lock(&lock);
  memset(img, 0, sizeof(img));
  cur_row = 0; filled = 0;
  ui.receiving = FALSE; ui.lpm = p_lpm; ui.ioc = p_ioc;
  ui.width = IMG_W; ui.height = BUF_H; ui.line = 0;
  set_status("Waiting for WEFAX…");
  g_mutex_unlock(&lock);
}

void wefax_decoder_set_enabled(gboolean on) {
  if (on == enabled) return;
  enabled = on;
  if (on && !hcoef_ready) hcoef_init();
  if (on) reset_req = TRUE;
}

void wefax_decoder_set_lpm(int lpm)  { if (lpm > 0) p_lpm = lpm; }
void wefax_decoder_set_ioc(int ioc)  { if (ioc > 0) p_ioc = ioc; }
void wefax_decoder_set_autostart(gboolean on) { p_autostart = on; }
void wefax_decoder_set_autophase(gboolean on) { p_autophase = on; }
void wefax_decoder_set_denoise(gboolean on) { p_denoise = on; }
void wefax_decoder_set_invert(gboolean on) { p_invert = on; }

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
void wefax_decoder_start(void) { start_req = TRUE; }
void wefax_decoder_reset(void) { reset_req = TRUE; }
void   wefax_decoder_adjust_slant(double dppm) { slant_ppm += dppm; }
double wefax_decoder_get_slant(void) { return slant_ppm; }
double wefax_decoder_get_afc(void) { return freq_offset; }
void wefax_decoder_nudge_phase(double frac) {
  // Convert the fraction of a line to samples at the current LPM.
  double line_len = SR * 60.0 / (double)p_lpm;
  pending_phase += frac * line_len;
}

// --- ring access -----------------------------------------------------------
static inline float fr_at(long abs) {
  if (abs < 0) abs = 0;
  if (abs >= ring_w) abs = ring_w - 1;
  if (abs < 0) abs = 0;
  return fr[abs & FR_MASK];
}
static double fr_mean(long a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; i++) s += fr_at(a + i);
  return s / (n > 0 ? n : 1);
}

// Map a scan frequency to 0..255 (1500 Hz→0 .. 2300 Hz→255), AFC-corrected.
static inline double freq2val(double f) {
  double v = (f - freq_offset - F_BLACK) / (F_WHITE - F_BLACK) * 255.0;
  if (v < 0.0)   v = 0.0;
  if (v > 255.0) v = 255.0;
  return v;
}
static inline guint8 clamp8(double v) {
  if (v < 0.0)   v = 0.0;
  if (v > 255.0) v = 255.0;
  return (guint8)(v + 0.5);
}

// Begin a fresh page: clear the image, arm the phasing servo, publish status.
static void begin_page(const char *why) {
  g_mutex_lock(&lock);
  memset(img, 0, sizeof(img));
  cur_row = 0; filled = 0;
  ui.receiving = TRUE; ui.lpm = p_lpm; ui.ioc = p_ioc; ui.line = 0;
  g_snprintf(ui.status, sizeof(ui.status), "Phasing… (%s)", why);
  g_mutex_unlock(&lock);
  started = TRUE;
  line_start_d = (double)ring_w;
  phasing_until = ring_w + (long)(PHASING_MS * SR / 1000.0);
  memset(col_energy, 0, sizeof(col_energy)); phase_locked = FALSE; acq_lines = 0;
  memset(freq_hist, 0, sizeof(freq_hist)); afc_lines = 0; afc_done = FALSE;
}

// Decode one scan line starting at absolute sample L, spanning line_len samples,
// into the image; run the phasing servo from the strongest edge.
static void decode_line(long L, double line_len) {
  double psmp = line_len / (double)IMG_W;
  int navg = (int)(psmp * 0.6); if (navg < 1) navg = 1;
  static float row[IMG_W];
  static double rawf[IMG_W];
  for (int x = 0; x < IMG_W; x++) {
    double c = (double)L + (x + 0.5) * psmp;
    double fm = fr_mean((long)(c) - navg / 2, navg);
    rawf[x] = fm;
    row[x] = (float)freq2val(fm);
  }

  // One-time white-anchored AFC.  A weather chart is predominantly white
  // background, so the dominant frequency over the first ~24 lines is the white
  // level; anchor it to 2300 Hz once, then freeze.  This corrects mistuning /
  // drift (which would otherwise wash the picture lighter or darker) without a
  // continuous loop that would chase noise during fades.  Skipped when the AFC
  // was already seeded by a start tone (afc_done set there).
  if (!afc_done) {
    for (int x = 0; x < IMG_W; x++) {
      int b = (int)((rawf[x] - AFC_FMIN) / AFC_BINHZ);
      if (b >= 0 && b < AFC_BINS) freq_hist[b]++;
    }
    if (++afc_lines >= 24) {
      int pk = 0; for (int b = 1; b < AFC_BINS; b++) if (freq_hist[b] > freq_hist[pk]) pk = b;
      double white = AFC_FMIN + (pk + 0.5) * AFC_BINHZ;
      if (white > 1900.0 && white < 2700.0) {           // a plausible white level
        double off = white - F_WHITE;
        if (off >  400.0) off =  400.0;
        if (off < -400.0) off = -400.0;
        freq_offset = off;
      }
      afc_done = TRUE;
    }
  }

  // Conditional-median despeckle: replace only pixels that are an isolated spike
  // vs BOTH neighbours (FM click / impulse noise) — real edges and thin lines
  // (which agree with one neighbour) are left untouched.
  if (p_denoise) {
    static float sp[IMG_W];
    memcpy(sp, row, sizeof(float) * IMG_W);
    for (int x = 1; x < IMG_W - 1; x++) {
      float a = sp[x - 1], b = sp[x], d = sp[x + 1];
      if (fabsf(b - a) > 50.0f && fabsf(b - d) > 50.0f && ((b > a) == (b > d))) {
        float mn = a < d ? a : d, mx = a < d ? d : a;      // median of {a,b,d}
        row[x] = b < mn ? mn : (b > mx ? mx : b);
      }
    }
  }

  // Write the greyscale row.
  g_mutex_lock(&lock);
  if (cur_row >= BUF_H) {                    // scroll the top third off
    memmove(img, img + SCROLL * IMG_W * 3, (BUF_H - SCROLL) * IMG_W * 3);
    memset(img + (BUF_H - SCROLL) * IMG_W * 3, 0, SCROLL * IMG_W * 3);
    cur_row = BUF_H - SCROLL;
  }
  guint8 *dst = &img[cur_row * IMG_W * 3];
  for (int x = 0; x < IMG_W; x++) {
    guint8 v = clamp8(row[x]);
    dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = v;
  }
  cur_row++;
  if (filled < BUF_H) filled++;
  ui.line = filled;
  if (p_autophase) {
    // Auto-phase does the alignment; phase_locked (from the previous line) says
    // whether it has settled on a recurring reference yet.
    if (phase_locked)
      g_snprintf(ui.status, sizeof(ui.status), "Receiving  %d lpm / IOC %d  (auto-phased)", p_lpm, p_ioc);
    else
      g_snprintf(ui.status, sizeof(ui.status), "Auto-phasing…  %d lpm / IOC %d", p_lpm, p_ioc);
  } else if (ui.receiving && ring_w >= phasing_until) {
    g_snprintf(ui.status, sizeof(ui.status), "Receiving  %d lpm / IOC %d", p_lpm, p_ioc);
  } else if (!ui.receiving) {
    // Free-running with auto-phase off: the operator sets the left margin by
    // clicking the image.
    g_snprintf(ui.status, sizeof(ui.status),
               "Free-run  %d lpm  (click image to set left margin)", p_lpm);
  }
  g_mutex_unlock(&lock);

  // Auto-phase: acquire the left margin ONCE, then freeze.  A per-line phase
  // servo would itself be a slant (a row-dependent horizontal shift), so instead
  // we accumulate a per-column EMA of the edge energy over the first ~24 lines —
  // a vertical feature that recurs at the same column every line (the fax
  // margin / border, or a chart grid line) builds a sharp peak, while random
  // picture content (coastlines, text) spreads across all columns and averages
  // to a flat floor.  Once a clear peak has formed we apply a single shift to
  // bring it to the left edge and lock; the picture then stays put (no injected
  // slant), exactly like a good free-run but with the seam removed automatically.
  // A start tone (begin_page) or Clear resets this so a new page re-acquires.
  if (p_autophase && !phase_locked) {
    const double A = 1.0 / 12.0;
    double sum = 0.0, peakv = 0.0; int peak = 0;
    for (int x = 1; x < IMG_W; x++) {
      double e = fabs(row[x] - row[x - 1]);
      col_energy[x] = (float)(col_energy[x] * (1.0 - A) + e * A);
      sum += col_energy[x];
      if (col_energy[x] > peakv) { peakv = col_energy[x]; peak = x; }
    }
    double mean = sum / (IMG_W - 1);
    acq_lines++;
    // Wait for the EMA to build (~24 lines) and require a clear recurring edge.
    if (acq_lines >= 24 && peakv > 40.0 && peakv > 3.0 * mean) {
      int P = peak; if (P > IMG_W / 2) P -= IMG_W;   // shortest way to column 0
      line_start_d += (double)P * psmp;              // one-time alignment
      phase_locked = TRUE;
    }
  }
}

// --- main audio entry ------------------------------------------------------
void wefax_decoder_add_audio(const gdouble *samples, int nframes) {
  if (!enabled) return;
  if (reset_req) { reset_req = FALSE; do_reset(); }

  for (int i = 0; i < nframes; i++) {
    // 1) discriminator -> instantaneous frequency.
    double x = samples[i * 2];
    hbuf[hpos] = x;
    int newest = hpos;
    hpos = (hpos + 1) % HIL;
    double I = hbuf[(newest - HIL_C + HIL) % HIL];
    double Q = 0.0;
    for (int j = 0; j < HIL; j++)
      Q += hcoef[j] * hbuf[(newest - j + HIL) % HIL];
    double num = Q * prevI - I * prevQ;
    double den = I * prevI + Q * prevQ;
    double f = atan2(num, den) * SR / (2.0 * M_PI);
    prevI = I; prevQ = Q;
    if (f < 0.0) f = 0.0;
    fr[ring_w & FR_MASK] = (float)f;
    ring_w++;

    // 2) start-tone + AFC estimator (per 1-second block).  The start signal is a
    // black/white square wave: 300 Hz for IOC576, 675 Hz for IOC288 — counted as
    // low->high transitions per second.  The same block yields the black/white
    // level means for the AFC seed.
    double val = freq2val(f);
    if (val > 170.0 && st_state == 0) { st_state = 1; st_trans++; }
    else if (val < 86.0 && st_state == 1) { st_state = 0; }
    if (f < F_CENTER) { st_lo_sum += f; st_lo_n++; } else { st_hi_sum += f; st_hi_n++; }
    st_blk_n++;
    if (val < 64.0)  st_black_n++;
    else if (val > 191.0) st_white_n++;

    if (ring_w - st_block_start >= (long)SR) {
      // Classify the block by its low->high crossing rate (= the start-tone
      // frequency): 300 Hz for IOC576, 675 Hz for IOC288.
      int rate = st_trans;
      int cls = 0;
      if (rate >= 260 && rate <= 340) cls = 576;
      else if (rate >= 600 && rate <= 750) cls = 288;
      // Bimodality gate: a real start tone is a balanced black<->white square
      // wave, so both levels are present in roughly equal measure and there is
      // almost nothing in between.  Image content (a weather chart is mostly
      // white with sparse black lines) is heavily unbalanced and fails this — the
      // key discriminator against false starts on picture content.
      int blk = st_blk_n > 0 ? st_blk_n : 1;
      gboolean bimodal = st_black_n > blk / 4 && st_white_n > blk / 4 &&
                         (st_black_n + st_white_n) > (blk * 3) / 4;
      if (cls != 0 && bimodal && cls == st_match_cls) st_match_run++;
      else if (cls != 0 && bimodal)                  { st_match_cls = cls; st_match_run = 1; }
      else if (st_match_run > 0)                        st_match_run--;   // decay, don't hard-reset
      else                                             st_match_cls = 0;  // (tolerates a noisy second mid-tone)

      // Trigger only after 3 consecutive clean start-tone seconds — a real start
      // signal lasts ~5 s, so this is easily met while transient look-alikes are
      // rejected.
      if (p_autostart && st_match_run >= 3 && ring_w >= start_cooldown) {
        if (st_lo_n > 0 && st_hi_n > 0) {
          double lo = st_lo_sum / st_lo_n, hi = st_hi_sum / st_hi_n;
          freq_offset = (lo + hi) / 2.0 - F_CENTER;
        }
        p_ioc = st_match_cls;
        log_info("WEFAX: start tone detected (IOC %d, AFC %+.0f Hz)\n", p_ioc, freq_offset);
        begin_page("start tone");
        afc_done = TRUE;   // trust the start-tone AFC seed; skip the histogram estimate
        // A full page is minutes long, so suppress any re-trigger for a while —
        // this covers the rest of this start tone and its phasing, while still
        // catching the next chart's start tone.
        start_cooldown = ring_w + (long)(SR * 60);
        st_match_run = 0; st_match_cls = 0;
      }
      st_block_start = ring_w; st_trans = 0;
      st_lo_sum = st_hi_sum = 0.0; st_lo_n = st_hi_n = 0;
      st_blk_n = st_black_n = st_white_n = 0;
    }

    // 3) manual Start.
    if (start_req) { start_req = FALSE; begin_page("manual"); }

    // 4) free-run: once anything has started (auto or manual, or the very first
    // audio) decode whole lines as they complete and scroll.
    if (!started) { started = TRUE; line_start_d = (double)ring_w; phasing_until = 0; }

    if (pending_phase != 0.0) { line_start_d += pending_phase; pending_phase = 0.0; }

    double line_len = SR * 60.0 / (double)p_lpm * (1.0 + slant_ppm / 1e6);
    // Decode any lines fully present in the ring (guard: a little past line end
    // for the trailing pixel average).
    int guard = 8;
    while ((double)ring_w >= line_start_d + line_len + guard) {
      long L = (long)(line_start_d + 0.5);
      decode_line(L, line_len);
      line_start_d += line_len;
      line_len = SR * 60.0 / (double)p_lpm * (1.0 + slant_ppm / 1e6);
    }
  }
}

// --- UI accessors ----------------------------------------------------------
void wefax_decoder_get_status(wefax_status_t *st) {
  g_mutex_lock(&lock);
  *st = ui;
  g_mutex_unlock(&lock);
}

GdkPixbuf *wefax_decoder_get_image(void) {
  g_mutex_lock(&lock);
  int h = filled;
  if (h <= 0) { g_mutex_unlock(&lock); return NULL; }
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, IMG_W, h);
  guint8 *pix = gdk_pixbuf_get_pixels(pb);
  int stride = gdk_pixbuf_get_rowstride(pb);
  for (int y = 0; y < h; y++) {
    if (p_invert)                                // negative image: white<->black
      for (int b = 0; b < IMG_W * 3; b++) pix[y * stride + b] = 255 - img[y * IMG_W * 3 + b];
    else
      memcpy(pix + y * stride, img + y * IMG_W * 3, IMG_W * 3);
  }
  g_mutex_unlock(&lock);
  return pb;
}
