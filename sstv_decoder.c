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

#include "sstv_decoder.h"
#include "log.h"

#define SR              48000.0    // demod audio sample rate (Hz)
#define F_BLACK         1500.0     // black level
#define F_WHITE         2300.0     // white level
#define F_SYNC          1200.0     // sync / VIS start-stop tone

// ---- Hilbert-transform FM discriminator -----------------------------------
// A 51-tap windowed Hilbert FIR turns the real audio into an analytic signal;
// the instantaneous frequency is the phase step between consecutive samples.
#define HIL             51
#define HIL_C           ((HIL - 1) / 2)

// ---- instantaneous-frequency ring ------------------------------------------
// Holds a little over one line of Scottie DX (~1050 ms ≈ 50k samples); sized as
// a power of two so the modulo is a mask.  All decoding indexes absolute sample
// positions into this ring.
#define FR_BITS         17
#define FR_CAP          (1 << FR_BITS)     // 131072 samples ≈ 2.7 s
#define FR_MASK         (FR_CAP - 1)

// ---- mode table ------------------------------------------------------------
typedef enum { FAM_MARTIN, FAM_SCOTTIE } sstv_family_t;

typedef struct {
  int           vis;
  const char   *name;
  sstv_family_t family;
  double        pixel_ms;   // per-pixel scan time
  double        sync_ms;    // horizontal sync pulse length
  double        sep_ms;     // separator / porch length
  int           width, height;
} sstv_mode_t;

// GBR-sequential modes.  pixel/sync/sep times are the standard N7CXI values.
static const sstv_mode_t MODES[] = {
  { 44, "Martin M1",  FAM_MARTIN,  0.457600, 4.862, 0.572, 320, 256 },
  { 40, "Martin M2",  FAM_MARTIN,  0.228800, 4.862, 0.572, 320, 256 },
  { 60, "Scottie S1", FAM_SCOTTIE, 0.432000, 9.000, 1.500, 320, 256 },
  { 56, "Scottie S2", FAM_SCOTTIE, 0.275200, 9.000, 1.500, 320, 256 },
  { 76, "Scottie DX", FAM_SCOTTIE, 1.080000, 9.000, 1.500, 320, 256 },
};
#define N_MODES (int)(sizeof(MODES) / sizeof(MODES[0]))

#define IMG_W 320
#define IMG_H 256

// ---- state (audio-thread owned, unless noted) ------------------------------
static gboolean enabled = FALSE;
static volatile int forced_vis = 0;        // 0 = auto (set from GTK thread)
static volatile gboolean reset_req = FALSE;

// discriminator
static double  hcoef[HIL];
static gboolean hcoef_ready = FALSE;
static double  hbuf[HIL];
static int     hpos = 0;
static double  prevI = 0.0, prevQ = 0.0;

// inst-freq ring
static float   fr[FR_CAP];
static long    ring_w = 0;                  // total samples written (absolute)

// VIS leader detector
static int     run1900 = 0, run1200 = 0;
static gboolean leader_ok = FALSE;
static long    cand_start = 0;              // candidate start-bit start (abs)
static gboolean vis_pending = FALSE;
static long    vis_bit_ready = 0;           // decode VIS once ring_w passes this

// decode state
typedef enum { ST_HUNT, ST_DECODE } sstv_state_t;
static sstv_state_t state = ST_HUNT;
static const sstv_mode_t *mode = NULL;
static double  slant_ppm = 0.0;             // clock/slant trim (GTK thread sets)
static long    sync0_abs = 0;               // located sync of line 0
static long    prev_sync_abs = 0;           // last located sync (for snapping)
static int     cur_line = 0;
static long    line_samples = 0;            // nominal samples per line period

// shared image + status (mutex protected)
static GMutex  lock;
static guint8  img[IMG_W * IMG_H * 3];
static sstv_status_t ui;                    // published status

// --------------------------------------------------------------------------

static void hcoef_init(void) {
  for (int k = 0; k < HIL; k++) {
    int m = k - HIL_C;
    double h = (m & 1) ? (2.0 / (M_PI * m)) : 0.0;     // ideal Hilbert
    double w = 0.54 - 0.46 * cos(2.0 * M_PI * k / (HIL - 1));  // Hamming
    hcoef[k] = h * w;
  }
  hcoef_ready = TRUE;
}

static void set_status(const char *s) {
  g_strlcpy(ui.status, s, sizeof(ui.status));
}

// Reset all decoder state (called under lock from the audio thread).
static void do_reset(void) {
  memset(hbuf, 0, sizeof(hbuf));
  hpos = 0; prevI = prevQ = 0.0;
  ring_w = 0;
  run1900 = run1200 = 0; leader_ok = FALSE; vis_pending = FALSE;
  state = ST_HUNT; mode = NULL; cur_line = 0;
  g_mutex_lock(&lock);
  memset(img, 0, sizeof(img));
  ui.receiving = FALSE; ui.vis = 0; ui.mode_name[0] = '\0';
  ui.width = IMG_W; ui.height = IMG_H; ui.line = 0; ui.progress = 0;
  set_status("Waiting for SSTV…");
  g_mutex_unlock(&lock);
}

void sstv_decoder_set_enabled(gboolean on) {
  if (on == enabled) return;
  enabled = on;
  if (on && !hcoef_ready) hcoef_init();
  if (on) reset_req = TRUE;   // fresh start each time the decoder is selected
}

void sstv_decoder_set_mode(int vis) { forced_vis = vis; }

void sstv_decoder_reset(void) { reset_req = TRUE; }

void   sstv_decoder_adjust_slant(double dppm) { slant_ppm += dppm; }
double sstv_decoder_get_slant(void) { return slant_ppm; }

// --- ring access -----------------------------------------------------------
static inline float fr_at(long abs) {
  if (abs < 0) abs = 0;
  if (abs >= ring_w) abs = ring_w - 1;
  return fr[abs & FR_MASK];
}

// Mean frequency over [a, a+n) absolute samples.
static double fr_mean(long a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; i++) s += fr_at(a + i);
  return s / (n > 0 ? n : 1);
}

static inline double ms2smp(double ms) { return ms * SR / 1000.0; }

// Map a scan frequency to an 8-bit intensity (1500 Hz→0 .. 2300 Hz→255).
static inline guint8 freq2lum(double f) {
  double v = (f - F_BLACK) / (F_WHITE - F_BLACK) * 255.0;
  if (v < 0.0)   v = 0.0;
  if (v > 255.0) v = 255.0;
  return (guint8)(v + 0.5);
}

// --- VIS handling ----------------------------------------------------------
static const sstv_mode_t *mode_by_vis(int vis) {
  for (int i = 0; i < N_MODES; i++)
    if (MODES[i].vis == vis) return &MODES[i];
  return NULL;
}

// Decode the 8 VIS bits sampled from cand_start, returning the mode (or NULL).
static const sstv_mode_t *decode_vis(void) {
  double bit = ms2smp(30.0);
  int val = 0, parity = 0;
  for (int k = 0; k < 7; k++) {
    long c = cand_start + (long)(bit * (k + 1) + bit * 0.5);   // bit-k centre
    int b = (fr_mean(c - (int)(bit * 0.3), (int)(bit * 0.6)) < F_SYNC) ? 1 : 0;
    val |= b << k;
    parity ^= b;
  }
  long pc = cand_start + (long)(bit * 8 + bit * 0.5);
  int pbit = (fr_mean(pc - (int)(bit * 0.3), (int)(bit * 0.6)) < F_SYNC) ? 1 : 0;
  if (pbit != parity) return NULL;                 // even-parity check failed
  return mode_by_vis(val);
}

// --- per-line sync locating ------------------------------------------------
// Find the sync pulse near predicted position `pred` (abs).  Returns the abs
// sample of the sync leading edge, snapped within ±win; if no clear sync is
// found returns `pred` (free-run).
static long locate_sync(long pred, int win) {
  int syncn = (int)ms2smp(mode->sync_ms);
  double best = 1e9; long bestpos = pred; gboolean found = FALSE;
  int step = 4;                                    // coarse search step
  for (int d = -win; d <= win; d += step) {
    long p = pred + d;
    if (p < 0 || p + syncn >= ring_w) continue;
    double m = 0.0;
    for (int i = 0; i < syncn; i += 2) m += fabs(fr_at(p + i) - F_SYNC);
    m /= (syncn / 2);
    if (m < best) { best = m; bestpos = p; found = TRUE; }
  }
  // Accept only a genuinely sync-like match; otherwise trust the prediction.
  if (found && best < 120.0) return bestpos;
  return pred;
}

// Sample one color channel of the current line into a row buffer.
static void scan_channel(long sync, double off_ms, double pixel_smp,
                         guint8 *dst, int stride) {
  double start = sync + ms2smp(off_ms);
  int navg = (int)(pixel_smp * 0.5);
  if (navg < 1) navg = 1;
  for (int x = 0; x < IMG_W; x++) {
    long c = (long)(start + (x + 0.5) * pixel_smp);
    double f = fr_mean(c - navg / 2, navg);
    dst[x * stride] = freq2lum(f);
  }
}

// Decode line `cur_line` given its located sync, write it into img.
static void decode_line(long sync) {
  double pixel_smp = ms2smp(mode->pixel_ms) * (1.0 + slant_ppm / 1e6);
  double channel_ms = mode->pixel_ms * IMG_W;
  double g_off, b_off, r_off;
  if (mode->family == FAM_MARTIN) {
    g_off = mode->sync_ms + mode->sep_ms;
    b_off = g_off + channel_ms + mode->sep_ms;
    r_off = b_off + channel_ms + mode->sep_ms;
  } else { // Scottie: green/blue precede the sync, red follows it
    g_off = -(2.0 * channel_ms + mode->sep_ms);
    b_off = -channel_ms;
    r_off = mode->sync_ms + mode->sep_ms;
  }
  guint8 *row = &img[cur_line * IMG_W * 3];
  g_mutex_lock(&lock);
  scan_channel(sync, g_off, pixel_smp, row + 1, 3);  // G
  scan_channel(sync, b_off, pixel_smp, row + 2, 3);  // B
  scan_channel(sync, r_off, pixel_smp, row + 0, 3);  // R
  ui.line = cur_line;
  ui.progress = (cur_line + 1) * 100 / IMG_H;
  g_mutex_unlock(&lock);
}

// --- main audio entry ------------------------------------------------------
void sstv_decoder_add_audio(const gdouble *samples, int nframes) {
  if (!enabled) return;
  if (reset_req) { reset_req = FALSE; do_reset(); }

  for (int i = 0; i < nframes; i++) {
    // 1) discriminator: push sample, form analytic (I,Q), inst frequency.
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
    if (f < 0.0) f = 0.0;                 // real audio: fold to positive

    fr[ring_w & FR_MASK] = (float)f;
    long n = ring_w;
    ring_w++;

    // 2) VIS leader detector (always running, so a new transmission restarts).
    if (fabs(f - 1900.0) < 130.0) { run1900++; run1200 = 0; }
    else if (fabs(f - F_SYNC) < 130.0) {
      run1200++;
      if (run1900 >= (int)ms2smp(200.0) && run1200 == 1) {
        cand_start = n; leader_ok = TRUE;          // leading edge of start bit
      }
      if (leader_ok && run1200 == (int)ms2smp(20.0)) {
        vis_pending = TRUE;
        vis_bit_ready = cand_start + (long)ms2smp(30.0 * 10 + 20.0);
      }
      run1900 = 0;
    } else { run1900 = 0; run1200 = 0; }

    // 3) VIS decode once the whole header has been buffered.
    if (vis_pending && ring_w >= vis_bit_ready) {
      vis_pending = FALSE; leader_ok = FALSE;
      const sstv_mode_t *m = forced_vis ? mode_by_vis(forced_vis) : decode_vis();
      if (m == NULL && forced_vis == 0) {
        // parity/unknown: retry hunting
      } else {
        if (m == NULL) m = mode_by_vis(forced_vis);
        mode = m;
        line_samples = (long)ms2smp(mode->sync_ms + 3.0 * (mode->pixel_ms * IMG_W) +
                                    (mode->family == FAM_MARTIN ? 4.0 : 3.0) * mode->sep_ms);
        // Image starts after the 30 ms stop bit that follows the 8 data bits.
        long img_start = cand_start + (long)ms2smp(30.0 * 10);
        double ch_ms = mode->pixel_ms * IMG_W;
        // Position of line 0's reference sync.  Martin's sync is at the line
        // start (≈ img_start); Scottie's reference is the sync between blue and
        // red, so line 0's sits after the starting sync + green + blue.  Both are
        // refined per line by locate_sync().
        if (mode->family == FAM_MARTIN)
          sync0_abs = img_start;
        else
          sync0_abs = img_start + (long)ms2smp(mode->sync_ms + 2.0 * mode->sep_ms +
                                               2.0 * ch_ms);
        prev_sync_abs = 0;
        cur_line = 0;
        state = ST_DECODE;
        g_mutex_lock(&lock);
        memset(img, 0, sizeof(img));
        ui.receiving = TRUE; ui.vis = mode->vis;
        g_strlcpy(ui.mode_name, mode->name, sizeof(ui.mode_name));
        ui.line = 0; ui.progress = 0;
        g_snprintf(ui.status, sizeof(ui.status), "Receiving %s", mode->name);
        g_mutex_unlock(&lock);
        log_info("SSTV: start %s (VIS %d)\n", mode->name, mode->vis);
      }
    }

    // 4) line decoding: decode line `cur_line` once its red channel is buffered.
    if (state == ST_DECODE) {
      double channel_ms = mode->pixel_ms * IMG_W;
      double r_end_ms = (mode->family == FAM_MARTIN)
                          ? (mode->sync_ms + 3.0 * mode->sep_ms + 3.0 * channel_ms)
                          : (mode->sync_ms + mode->sep_ms + channel_ms);
      long pred = sync0_abs + (long)((double)cur_line * line_samples);
      long need = pred + (long)ms2smp(r_end_ms) + (long)ms2smp(20.0);
      if (ring_w >= need) {
        // Line 0 is anchored purely on the timing derived from the VIS (cand_start),
        // NOT by searching: Martin's first sync pulse abuts the VIS stop bit (both
        // 1200 Hz), so a sync search would lock onto the stop bit.  From line 1 on,
        // every sync is cleanly flanked by a 1500 Hz porch/separator, so searching
        // is safe and cancels clock drift / slant.
        long sync = (cur_line == 0) ? pred : locate_sync(pred, (int)ms2smp(12.0));
        // Re-lock the timeline to the detected sync to cancel drift/slant.
        sync0_abs = sync - (long)((double)cur_line * line_samples);
        decode_line(sync);
        prev_sync_abs = sync;
        cur_line++;
        if (cur_line >= IMG_H) {
          state = ST_HUNT;
          g_mutex_lock(&lock);
          ui.receiving = FALSE; ui.progress = 100;
          g_snprintf(ui.status, sizeof(ui.status), "%s complete", mode->name);
          g_mutex_unlock(&lock);
          log_info("SSTV: %s image complete\n", mode->name);
        }
      }
    }
  }
}

// --- UI accessors (GTK thread) ---------------------------------------------
void sstv_decoder_get_status(sstv_status_t *st) {
  g_mutex_lock(&lock);
  *st = ui;
  g_mutex_unlock(&lock);
}

GdkPixbuf *sstv_decoder_get_image(void) {
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, IMG_W, IMG_H);
  if (pb == NULL) return NULL;
  guint8 *dst = gdk_pixbuf_get_pixels(pb);
  int stride = gdk_pixbuf_get_rowstride(pb);
  g_mutex_lock(&lock);
  for (int y = 0; y < IMG_H; y++)
    memcpy(dst + y * stride, &img[y * IMG_W * 3], IMG_W * 3);
  g_mutex_unlock(&lock);
  return pb;
}
