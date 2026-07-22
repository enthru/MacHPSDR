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
#define F_BLACK         1500.0     // black / colour-0 level
#define F_WHITE         2300.0     // white / colour-255 level
#define F_SYNC          1200.0     // sync / VIS start-stop tone

// ---- Hilbert-transform FM discriminator -----------------------------------
// A 51-tap windowed Hilbert FIR turns the real audio into an analytic signal;
// the instantaneous frequency is the phase step between consecutive samples.
#define HIL             51
#define HIL_C           ((HIL - 1) / 2)

// ---- instantaneous-frequency ring ------------------------------------------
// Holds a little over one line of the slowest mode (Scottie DX ~1050 ms, PD240
// ~1000 ms).  Power of two so the modulo is a mask.
#define FR_BITS         17
#define FR_CAP          (1 << FR_BITS)     // 131072 samples ≈ 2.7 s
#define FR_MASK         (FR_CAP - 1)

// ---- image geometry --------------------------------------------------------
#define MAX_W           640        // widest supported mode (PD120/180/240)
#define MAX_H           496        // tallest supported mode (PD120/180/240)

// ---- mode table ------------------------------------------------------------
// Families differ in line structure / colourspace:
//   MARTIN/SCOTTIE : GBR sequential (three direct R/G/B scans per line)
//   ROBOT36        : Y each line + alternating R-Y / B-Y (4:2:0, chroma shared)
//   ROBOT72        : Y + R-Y + B-Y each line (4:2:2)
//   PD             : two image rows per line — Y0, R-Y, B-Y, Y1 (4:2:0)
typedef enum { FAM_MARTIN, FAM_SCOTTIE, FAM_ROBOT36, FAM_ROBOT72, FAM_PD } sstv_family_t;

typedef struct {
  int           vis;
  const char   *name;
  sstv_family_t family;
  int           width, height;
  double        sync_ms;    // horizontal sync pulse
  double        porch_ms;   // porch after sync
  double        y_ms;       // luminance / colour scan duration per line
  double        c_ms;       // chroma scan duration (0 for GBR — uses y_ms)
  double        sep_ms;     // separator length (GBR sep / Robot Y-chroma sep)
} sstv_mode_t;

static const sstv_mode_t MODES[] = {
  // GBR sequential (N7CXI timings)
  { 44, "Martin M1",  FAM_MARTIN,  320, 256, 4.862, 0.572, 146.432,   0.0, 0.572 },
  { 40, "Martin M2",  FAM_MARTIN,  320, 256, 4.862, 0.572,  73.216,   0.0, 0.572 },
  { 60, "Scottie S1", FAM_SCOTTIE, 320, 256, 9.000, 1.500, 138.240,   0.0, 1.500 },
  { 56, "Scottie S2", FAM_SCOTTIE, 320, 256, 9.000, 1.500,  88.064,   0.0, 1.500 },
  { 76, "Scottie DX", FAM_SCOTTIE, 320, 256, 9.000, 1.500, 345.600,   0.0, 1.500 },
  // Robot colour (YUV)
  {  8, "Robot 36",   FAM_ROBOT36, 320, 240, 9.000, 3.000,  88.000,  44.0, 4.500 },
  { 12, "Robot 72",   FAM_ROBOT72, 320, 240, 9.000, 3.000, 138.000,  69.0, 4.500 },
  // PD (YUV, two rows per line)
  { 93, "PD50",       FAM_PD,      320, 256, 20.00, 2.080,  91.520,   0.0, 0.0 },
  { 99, "PD90",       FAM_PD,      320, 256, 20.00, 2.080, 170.240,   0.0, 0.0 },
  { 95, "PD120",      FAM_PD,      640, 496, 20.00, 2.080, 121.7536,  0.0, 0.0 },
  { 98, "PD160",      FAM_PD,      512, 400, 20.00, 2.080, 195.584,   0.0, 0.0 },
  { 96, "PD180",      FAM_PD,      640, 496, 20.00, 2.080, 183.040,   0.0, 0.0 },
  { 97, "PD240",      FAM_PD,      640, 496, 20.00, 2.080, 244.480,   0.0, 0.0 },
};
#define N_MODES (int)(sizeof(MODES) / sizeof(MODES[0]))

// PD chroma porch (1900 Hz) sits between Robot's Y separator and the chroma; the
// Robot chroma porch is a fixed 1.5 ms.
#define ROBOT_CPORCH_MS 1.5

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
static long    leader_deadline = 0;        // latch timeout for leader_ok (abs)
static long    cand_start = 0;              // candidate start-bit start (abs)
static gboolean vis_pending = FALSE;
static long    vis_bit_ready = 0;           // decode VIS once ring_w passes this
static int     sync_run = 0;                // consecutive ~1200 Hz samples (forced-mode sync hunt)

// decode state
typedef enum { ST_HUNT, ST_DECODE } sstv_state_t;
static sstv_state_t state = ST_HUNT;
static const sstv_mode_t *mode = NULL;
static double  slant_ppm = 0.0;             // clock/slant trim (GTK thread sets)
static long    sync0_abs = 0;               // located sync of line 0
static int     cur_line = 0;                // next image ROW to write
static long    tx_line = 0;                 // transmitted-line index (PD: 2 rows each)
static long    line_samples = 0;            // samples per transmitted line (auto-slant adapts)
static long    nominal_line = 0;            // the un-adapted line period (auto-slant reference)
static double  clock_scale = 1.0;           // measured pixel-clock correction (auto-slant)
static int     img_w = 320, img_h = 256;    // current mode geometry

// Robot 36 chroma pairing (a chroma line is shared by two image rows).
static float   r36_cr[MAX_W];
static float   r36_y_prev[MAX_W];
static int     r36_row_prev = -1;
static gboolean r36_have_cr = FALSE;

// shared image + status (mutex protected)
static GMutex  lock;
static guint8  img[MAX_W * MAX_H * 3];
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
  r36_have_cr = FALSE; r36_row_prev = -1;
  img_w = 320; img_h = 256;
  g_mutex_lock(&lock);
  memset(img, 0, sizeof(img));
  ui.receiving = FALSE; ui.vis = 0; ui.mode_name[0] = '\0';
  ui.width = img_w; ui.height = img_h; ui.line = 0; ui.progress = 0;
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

// Map a scan frequency to a 0..255 value (1500 Hz→0 .. 2300 Hz→255).
static inline double freq2val(double f) {
  double v = (f - F_BLACK) / (F_WHITE - F_BLACK) * 255.0;
  if (v < 0.0)   v = 0.0;
  if (v > 255.0) v = 255.0;
  return v;
}
static inline guint8 clamp8(double v) {
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
// Find the sync pulse near predicted position `pred` (abs), snapped within ±win;
// if no clear sync is found returns `pred` (free-run).
static long locate_sync(long pred, int win) {
  int syncn = (int)ms2smp(mode->sync_ms);
  double best = 1e9; long bestpos = pred; gboolean found = FALSE;
  int step = 4;
  for (int d = -win; d <= win; d += step) {
    long p = pred + d;
    if (p < 0 || p + syncn >= ring_w) continue;
    double m = 0.0;
    for (int i = 0; i < syncn; i += 2) m += fabs(fr_at(p + i) - F_SYNC);
    m /= (syncn / 2);
    if (m < best) { best = m; bestpos = p; found = TRUE; }
  }
  if (found && best < 120.0) return bestpos;
  return pred;
}

// Pixel clock for this mode (with the slant trim applied).
static double pixel_smp(double scan_ms, int n) {
  return ms2smp(scan_ms) * clock_scale * (1.0 + slant_ppm / 1e6) / n;
}

// Sample one scan (from `sync` + off_ms, `n` pixels of `psmp` samples each) into
// a 0..255 float row.
static void scan_row(long sync, double off_ms, double psmp, int n, float *out) {
  double start = sync + ms2smp(off_ms);
  int navg = (int)(psmp * 0.5); if (navg < 1) navg = 1;
  for (int x = 0; x < n; x++) {
    long c = (long)(start + (x + 0.5) * psmp);
    out[x] = (float)freq2val(fr_mean(c - navg / 2, navg));
  }
}

// Write an RGB row directly (caller holds the lock).
static void put_rgb_row(int row, const float *R, const float *G, const float *B) {
  if (row < 0 || row >= img_h) return;
  guint8 *p = &img[row * img_w * 3];
  for (int x = 0; x < img_w; x++) {
    p[x*3+0] = clamp8(R[x]); p[x*3+1] = clamp8(G[x]); p[x*3+2] = clamp8(B[x]);
  }
}

// Write a YUV row (Y, R-Y=Cr, B-Y=Cb, full-range BT.601).  Caller holds lock.
static void put_yuv_row(int row, const float *Y, const float *Cr, const float *Cb) {
  if (row < 0 || row >= img_h) return;
  guint8 *p = &img[row * img_w * 3];
  for (int x = 0; x < img_w; x++) {
    double y = Y[x], v = Cr[x] - 128.0, u = Cb[x] - 128.0;
    p[x*3+0] = clamp8(y + 1.402 * v);
    p[x*3+1] = clamp8(y - 0.344 * u - 0.714 * v);
    p[x*3+2] = clamp8(y + 1.772 * u);
  }
}

// --- per-family line decoders ----------------------------------------------
static void decode_gbr(long sync) {
  int w = img_w;
  double ch = mode->y_ms, sep = mode->sep_ms, porch = mode->porch_ms;
  double psmp = pixel_smp(ch, w);
  double g_off, b_off, r_off;
  if (mode->family == FAM_MARTIN) {
    g_off = mode->sync_ms + porch;
    b_off = g_off + ch + sep;
    r_off = b_off + ch + sep;
  } else { // Scottie: green/blue precede the sync, red follows it
    g_off = -(2.0 * ch + sep);
    b_off = -ch;
    r_off = mode->sync_ms + porch;
  }
  float R[MAX_W], G[MAX_W], B[MAX_W];
  scan_row(sync, g_off, psmp, w, G);
  scan_row(sync, b_off, psmp, w, B);
  scan_row(sync, r_off, psmp, w, R);
  g_mutex_lock(&lock);
  put_rgb_row(cur_line, R, G, B);
  ui.line = cur_line; ui.progress = (cur_line + 1) * 100 / img_h;
  g_mutex_unlock(&lock);
  cur_line++;
}

static void decode_robot72(long sync) {
  int w = img_w;
  double base = mode->sync_ms + mode->porch_ms;
  double yp = pixel_smp(mode->y_ms, w), cp = pixel_smp(mode->c_ms, w);
  double ry_off = base + mode->y_ms + mode->sep_ms + ROBOT_CPORCH_MS;
  double by_off = ry_off + mode->c_ms + mode->sep_ms + ROBOT_CPORCH_MS;
  float Y[MAX_W], Cr[MAX_W], Cb[MAX_W];
  scan_row(sync, base,   yp, w, Y);
  scan_row(sync, ry_off, cp, w, Cr);
  scan_row(sync, by_off, cp, w, Cb);
  g_mutex_lock(&lock);
  put_yuv_row(cur_line, Y, Cr, Cb);
  ui.line = cur_line; ui.progress = (cur_line + 1) * 100 / img_h;
  g_mutex_unlock(&lock);
  cur_line++;
}

static void decode_robot36(long sync) {
  int w = img_w;
  double base = mode->sync_ms + mode->porch_ms;
  double yp = pixel_smp(mode->y_ms, w), cp = pixel_smp(mode->c_ms, w);
  double c_off = base + mode->y_ms + mode->sep_ms + ROBOT_CPORCH_MS;
  // The Y/chroma separator frequency encodes which chroma this line carries:
  // ~1500 Hz → R-Y (Cr), ~2300 Hz → B-Y (Cb).
  double sepf = fr_mean(sync + (long)ms2smp(base + mode->y_ms + mode->sep_ms * 0.5),
                        (int)ms2smp(mode->sep_ms * 0.5));
  float Y[MAX_W], C[MAX_W];
  scan_row(sync, base,  yp, w, Y);
  scan_row(sync, c_off, cp, w, C);
  gboolean is_cr = (sepf < 1900.0);
  g_mutex_lock(&lock);
  if (is_cr) {
    memcpy(r36_cr, C, sizeof(float) * w);
    memcpy(r36_y_prev, Y, sizeof(float) * w);
    r36_row_prev = cur_line;
    r36_have_cr = TRUE;
  } else {
    const float *cr = r36_have_cr ? r36_cr : C;   // fallback: reuse this chroma
    if (r36_have_cr) put_yuv_row(r36_row_prev, r36_y_prev, cr, C);
    put_yuv_row(cur_line, Y, cr, C);
    r36_have_cr = FALSE;
  }
  ui.line = cur_line; ui.progress = (cur_line + 1) * 100 / img_h;
  g_mutex_unlock(&lock);
  cur_line++;
}

static void decode_pd(long sync) {
  int w = img_w;
  double base = mode->sync_ms + mode->porch_ms;
  double sc = mode->y_ms;
  double psmp = pixel_smp(sc, w);
  double y0_off = base;
  double ry_off = base + sc;
  double by_off = base + 2.0 * sc;
  double y1_off = base + 3.0 * sc;
  float Y0[MAX_W], Y1[MAX_W], Cr[MAX_W], Cb[MAX_W];
  scan_row(sync, y0_off, psmp, w, Y0);
  scan_row(sync, ry_off, psmp, w, Cr);
  scan_row(sync, by_off, psmp, w, Cb);
  scan_row(sync, y1_off, psmp, w, Y1);
  g_mutex_lock(&lock);
  put_yuv_row(cur_line,     Y0, Cr, Cb);
  put_yuv_row(cur_line + 1, Y1, Cr, Cb);
  ui.line = cur_line + 1; ui.progress = (cur_line + 2) * 100 / img_h;
  g_mutex_unlock(&lock);
  cur_line += 2;
}

// Samples from the located sync to the end of the last scan of the line.
static double line_r_end_ms(void) {
  switch (mode->family) {
    case FAM_MARTIN:  return mode->sync_ms + mode->porch_ms + 3.0 * mode->y_ms + 3.0 * mode->sep_ms;
    case FAM_SCOTTIE: return mode->sync_ms + mode->porch_ms + mode->y_ms;  // red is last, after sync
    case FAM_ROBOT36: return mode->sync_ms + mode->porch_ms + mode->y_ms + mode->sep_ms + ROBOT_CPORCH_MS + mode->c_ms;
    case FAM_ROBOT72: return mode->sync_ms + mode->porch_ms + mode->y_ms + 2.0 * (mode->sep_ms + ROBOT_CPORCH_MS + mode->c_ms);
    case FAM_PD:      return mode->sync_ms + mode->porch_ms + 4.0 * mode->y_ms;
  }
  return 0.0;
}

static double line_period_ms(void) {
  switch (mode->family) {
    case FAM_MARTIN:  return mode->sync_ms + mode->porch_ms + 3.0 * mode->y_ms + 3.0 * mode->sep_ms;
    case FAM_SCOTTIE: return mode->sync_ms + 3.0 * mode->y_ms + 3.0 * mode->sep_ms;
    case FAM_ROBOT36: return mode->sync_ms + mode->porch_ms + mode->y_ms + mode->sep_ms + ROBOT_CPORCH_MS + mode->c_ms;
    case FAM_ROBOT72: return mode->sync_ms + mode->porch_ms + mode->y_ms + 2.0 * (mode->sep_ms + ROBOT_CPORCH_MS + mode->c_ms);
    case FAM_PD:      return mode->sync_ms + mode->porch_ms + 4.0 * mode->y_ms;
  }
  return 0.0;
}

// Begin decoding mode `m` with line 0's reference sync at absolute sample
// `sync0` (the sync between blue and red for Scottie; the line-start sync for
// every other family).  Shared by the VIS path and the forced-mode sync anchor.
static void start_decode(const sstv_mode_t *m, long sync0) {
  mode = m;
  img_w = m->width; img_h = m->height;
  line_samples = (long)ms2smp(line_period_ms());
  nominal_line = line_samples; clock_scale = 1.0;   // reset auto-slant servo
  sync0_abs = sync0;
  cur_line = 0; tx_line = 0;
  r36_have_cr = FALSE; r36_row_prev = -1;
  state = ST_DECODE;
  g_mutex_lock(&lock);
  memset(img, 0, sizeof(img));
  ui.receiving = TRUE; ui.vis = m->vis;
  g_strlcpy(ui.mode_name, m->name, sizeof(ui.mode_name));
  ui.width = img_w; ui.height = img_h; ui.line = 0; ui.progress = 0;
  g_snprintf(ui.status, sizeof(ui.status), "Receiving %s", m->name);
  g_mutex_unlock(&lock);
  log_info("SSTV: start %s (VIS %d, %dx%d)\n", m->name, m->vis, img_w, img_h);
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

    // 2) VIS leader detector.  The header is 1900 Hz leader → 1200 Hz start bit →
    // 8×30 ms data bits.  Real transmissions (and the discriminator's finite
    // response) glide through the intermediate frequencies between the 1900 Hz
    // leader and the 1200 Hz start bit, which would reset a raw consecutive-run
    // counter before f reaches 1200.  So once ≥180 ms of ~1900 Hz has been seen
    // we LATCH the leader for a short window (leader_deadline), and the start bit
    // is detected against that latch — not the instantaneous run counter.
    if (fabs(f - 1900.0) < 150.0) {
      run1900++; run1200 = 0;
      if (run1900 >= (int)ms2smp(180.0)) {
        leader_ok = TRUE;
        leader_deadline = n + (long)ms2smp(600.0);   // refreshed by each 1900 segment
      }
    } else if (fabs(f - F_SYNC) < 150.0) {
      run1200++;
      // start-bit leading edge — but do not disturb a VIS already buffering its
      // bits (image content re-latches the leader and would move cand_start).
      if (leader_ok && run1200 == 1 && !vis_pending) cand_start = n;
      // Don't overwrite a VIS that is already buffering its bits: the real
      // header comes first, and image content can otherwise re-trigger the
      // (deliberately permissive) leader latch during the ~320 ms bit window and
      // clobber the correct cand_start before it is decoded.
      if (leader_ok && run1200 == (int)ms2smp(20.0) && !vis_pending) {
        vis_pending = TRUE;
        vis_bit_ready = cand_start + (long)ms2smp(30.0 * 10 + 20.0);
      }
      run1900 = 0;
    } else { run1900 = 0; run1200 = 0; }   // dead-zone glide keeps the latch
    if (leader_ok && n > leader_deadline) leader_ok = FALSE;

    // 3) VIS decode once the whole header has been buffered (auto mode only —
    // a forced mode anchors on the sync pulse below instead, which is far more
    // robust to a noisy / Doppler-shifted header).
    if (vis_pending && ring_w >= vis_bit_ready) {
      vis_pending = FALSE; leader_ok = FALSE;
      const sstv_mode_t *m = forced_vis ? NULL : decode_vis();
      if (m != NULL && state == ST_HUNT) {
        mode = m;   // needed by line_*_ms() used inside start_decode
        long img_start = cand_start + (long)ms2smp(30.0 * 10);  // after stop bit
        long s0 = (m->family == FAM_SCOTTIE)
                    ? img_start + (long)ms2smp(m->sync_ms + 2.0*m->sep_ms + 2.0*m->y_ms)
                    : img_start;
        start_decode(m, s0);
      }
    }

    // 3b) Forced-mode sync anchor: skip the VIS entirely and start on the first
    // clean sync pulse (a run of ~sync_ms at ~1200 Hz).  Works even when the VIS
    // header is unreadable; the per-line sync lock keeps it aligned thereafter.
    if (forced_vis && state == ST_HUNT) {
      const sstv_mode_t *m = mode_by_vis(forced_vis);
      if (m != NULL) {
        if (fabs(f - F_SYNC) < 120.0) sync_run++; else sync_run = 0;
        if (sync_run >= (int)ms2smp(m->sync_ms * 0.7)) {
          long s0 = n - sync_run + 1;   // start of this sync pulse = line-0 ref
          sync_run = 0;
          start_decode(m, s0);
        }
      }
    }

    // 4) line decoding: decode the current line once its last scan is buffered.
    if (state == ST_DECODE) {
      long pred = sync0_abs + (long)((double)tx_line * line_samples);
      long need = pred + (long)ms2smp(line_r_end_ms()) + (long)ms2smp(20.0);
      if (ring_w >= need) {
        // Line 0 is anchored on the VIS-derived timing, NOT searched: the first
        // sync abuts the 1200 Hz VIS stop bit (Martin/Robot/PD) so a search would
        // lock onto the stop bit.  From line 1 on every sync is cleanly flanked
        // by a 1500 Hz porch/separator, so searching cancels clock drift/slant.
        long sync = (tx_line == 0) ? pred : locate_sync(pred, (int)ms2smp(12.0));
        // Auto-slant: the residual (sync − pred) is the per-line clock error
        // (true line period − assumed).  Servo the assumed line period toward the
        // truth and scale the pixel clock by the same factor (clock_scale), so a
        // sound-card / sample-rate offset that would slant the picture is removed
        // automatically.  Skip the first few lines (forced-mode anchor settling)
        // and clamp to ±5 % to stay stable.
        if (tx_line > 2) {
          line_samples += (long)llround(0.25 * (double)(sync - pred));
          if (line_samples < (long)(nominal_line * 0.95)) line_samples = (long)(nominal_line * 0.95);
          if (line_samples > (long)(nominal_line * 1.05)) line_samples = (long)(nominal_line * 1.05);
          clock_scale = (double)line_samples / (double)nominal_line;
        }
        sync0_abs = sync - (long)((double)tx_line * line_samples);  // re-lock
        tx_line++;

        switch (mode->family) {
          case FAM_MARTIN:
          case FAM_SCOTTIE: decode_gbr(sync);     break;
          case FAM_ROBOT36: decode_robot36(sync); break;
          case FAM_ROBOT72: decode_robot72(sync); break;
          case FAM_PD:      decode_pd(sync);      break;
        }

        if (cur_line >= img_h) {
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
  g_mutex_lock(&lock);
  int w = img_w, h = img_h;
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
  if (pb != NULL) {
    guint8 *dst = gdk_pixbuf_get_pixels(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    for (int y = 0; y < h; y++)
      memcpy(dst + y * stride, &img[y * w * 3], w * 3);
  }
  g_mutex_unlock(&lock);
  return pb;
}
