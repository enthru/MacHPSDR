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
// A fixed display width (each line is resampled to this regardless of IOC) and a
// scrolling buffer height: when full, the top third scrolls off so reception is
// continuous.
#define IMG_W           800
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
static int     st_prev_class = 0;          // previous block's IOC classification
static double  st_lo_sum = 0.0, st_hi_sum = 0.0;
static int     st_lo_n = 0, st_hi_n = 0;
static long    start_cooldown = 0;         // suppress re-triggering the start tone until here

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
  st_block_start = 0; st_state = 0; st_trans = 0; st_prev_class = 0;
  st_lo_sum = st_hi_sum = 0.0; st_lo_n = st_hi_n = 0; start_cooldown = 0;
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
}

// Decode one scan line starting at absolute sample L, spanning line_len samples,
// into the image; run the phasing servo from the strongest edge.
static void decode_line(long L, double line_len) {
  double psmp = line_len / (double)IMG_W;
  int navg = (int)(psmp * 0.6); if (navg < 1) navg = 1;
  static float row[IMG_W];
  double emax = 0.0; int epos = 0;
  for (int x = 0; x < IMG_W; x++) {
    double c = (double)L + (x + 0.5) * psmp;
    row[x] = (float)freq2val(fr_mean((long)(c) - navg / 2, navg));
    if (x > 0) {
      double d = fabs(row[x] - row[x - 1]);
      if (d > emax) { emax = d; epos = x; }
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
  if (ring_w >= phasing_until && ui.receiving)
    g_snprintf(ui.status, sizeof(ui.status), "Receiving  %d lpm / IOC %d", p_lpm, p_ioc);
  g_mutex_unlock(&lock);

  // Phasing servo: during the phasing window the transmission is a mostly-flat
  // line with one strong recurring edge (the phasing pulse).  Steer the line
  // start so that edge lands at the left margin.  After the window the phase
  // locks, so image content (coastlines, text) cannot drag the alignment.
  // Only a genuine phasing pulse (a near-full black<->white swing) steers the
  // phase; a gradual image edge (coastline, gradient) does not reach this, so
  // image content cannot drag the alignment even inside the phasing window.
  if (ring_w < phasing_until && emax > 200.0) {
    int e = epos; if (e > IMG_W / 2) e -= IMG_W;   // shortest way round
    line_start_d += 0.35 * (double)e * psmp;
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

    if (ring_w - st_block_start >= (long)SR) {
      int rate = st_trans;
      int cls = 0;
      if (rate >= 240 && rate <= 360) cls = 576;
      else if (rate >= 560 && rate <= 790) cls = 288;
      if (p_autostart && cls != 0 && cls == st_prev_class && ring_w >= start_cooldown) {
        // Seed the AFC from this block's bimodal black/white levels.
        if (st_lo_n > 0 && st_hi_n > 0) {
          double lo = st_lo_sum / st_lo_n, hi = st_hi_sum / st_hi_n;
          freq_offset = (lo + hi) / 2.0 - F_CENTER;
        }
        p_ioc = cls;
        log_info("WEFAX: start tone detected (IOC %d, AFC %+.0f Hz)\n", cls, freq_offset);
        begin_page("start tone");
        // Don't re-trigger for the rest of this start tone (it lasts several
        // seconds); the phasing signal that follows is not classified as a start
        // tone, so a genuinely new page's tone is still caught.
        start_cooldown = ring_w + (long)(SR * 10);
        st_prev_class = 0;
      } else {
        st_prev_class = cls;
      }
      st_block_start = ring_w; st_trans = 0;
      st_lo_sum = st_hi_sum = 0.0; st_lo_n = st_hi_n = 0;
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
  for (int y = 0; y < h; y++)
    memcpy(pix + y * stride, img + y * IMG_W * 3, IMG_W * 3);
  g_mutex_unlock(&lock);
  return pb;
}
