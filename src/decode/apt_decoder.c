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
#include <time.h>

#include "apt_decoder.h"
#include "log.h"

// ---- APT line geometry (words; 4160 words/s, 2 lines/s) --------------------
#define WORDS       2080                 // one full line
#define WORD_RATE   4160.0
#define LINE_S      (WORDS / WORD_RATE)  // 0.5 s
#define SUBCARRIER  2400.0               // AM video subcarrier (Hz)
#define VIDEO_BW    2100.0               // video low-pass after the subcarrier mix

#define SYNCA_LEN   39                   // 7 cycles of 1040 Hz square wave
#define IMGA_OFF    86                   // sync A 39 + space A 47
#define IMG_LEN     909
#define IMGB_OFF    1126                 // + telemetry A 45 + sync B 39 + space B 47

// ---- image buffer ---------------------------------------------------------
// A full pass is ~15 minutes ≈ 1800 lines; the buffer scrolls a third off when
// it fills, exactly like WEFAX, so reception is continuous.  Stored as 8-bit
// greyscale (expanded to RGB only when the UI asks for a pixbuf) — at 2080 px
// wide an RGB buffer would be three times this for no gain.
#define BUF_H       1600
#define SCROLL      (BUF_H / 3)

// ---- envelope ring --------------------------------------------------------
// Holds the detected video envelope at the working rate.  The line search needs
// two lines plus margin; 2^18 samples is 5.4 s (≈11 lines) at 48 kHz.
#define FR_BITS     18
#define FR_CAP      (1 << FR_BITS)
#define FR_MASK     (FR_CAP - 1)

// ---- front-end (I/Q path) -------------------------------------------------
// Decimating low-pass ahead of the discriminator.  The target working rate is
// just above the ~44 kHz the FM signal occupies (Carson: 2·(17 + 4.8) kHz), so
// the filter keeps the whole signal and nothing else.
#define FE_TAPS     63
#define FE_TARGET   44000.0
#define FE_CUTOFF   22000.0

// ---- video low-pass (both paths) ------------------------------------------
#define VF_TAPS     33

// Sync thresholds on the normalised (Pearson) sync-A correlation.
//
// A raw score threshold on its own is NOT enough, and getting that wrong is the
// classic failure of this kind of decoder (the CW decoder streamed garbage on
// band noise until it grew a squelch).  The correlation runs over 39 words, so
// on noise it has a standard deviation of about 1/sqrt(39) = 0.16 — but the
// search takes the MAX over hundreds of trial positions, which pushes the noise
// score far higher: MEASURED at 0.62 on pure noise (see the selftest).  A
// decoder that locks there paints a picture out of nothing and, worse, lets the
// noise drag the clock servo, destroying the ppm it measured during the pass.
//
// So acquisition also demands the thing noise cannot fake: a sync that comes
// back at the SAME place line after line.  Noise maxima jump around the line at
// random, so requiring LOCK_RUN consecutive lines to agree on the position makes
// a false lock vanishingly unlikely — and lets the score threshold stay low
// enough for a weak pass to still get in.
#define LOCK_CORR   0.65                 // per-line score needed to be a candidate
#define LOCK_RUN    3                    // consecutive lines that must agree
#define LOCK_TOL    0.005                // position agreement, as a fraction of a line
// Once locked, only a sync THIS good is trusted to move the line phase and drive
// the clock servo; anything weaker is treated as a fade and simply free-runs on
// the geometry already measured.  Losing the sync for longer than MISS_MAX drops
// the lock, and then nothing is drawn until it comes back.
#define TRIM_CORR   0.75                 // above the 0.69 noise max, below a real 0.9+
#define MISS_MAX    10                   // lines of fade ridden out (5 s)
#define CLOCK_GAIN  0.15                 // integral gain of the line-period servo
// Ceiling on what ONE line may do to the clock estimate.  A real sync moves by a
// sample or two per line (100 ppm is 3 samples), so a large correction is never
// the clock — it is a bad correlation, and without this ceiling a single weak
// line late in a pass can undo a measurement that hundreds of good lines built.
// Twenty ppm per line still converges from cold in a couple of dozen lines.
#define CLOCK_STEP_MAX 20.0

// What counts as "this is a different transmission, start a new picture".
// Mirrors wefax_decoder.c, where the start-tone detector calls begin_page():
// appending a second pass under the first gives one ruined strip on a shared
// exposure, not two pictures.  APT has no start tone, so the two triggers are:
//   - the operator retuning the cursor onto a different signal;
//   - the sync being gone for long enough that this cannot be the same pass.
// Both thresholds are deliberately generous, because the cost of triggering
// wrongly is destroying a pass that cannot be repeated.  RETUNE_HZ must clear
// the APT signal's OWN width: it is ±17 kHz of FM, so an operator aiming at the
// hump on the panadapter can easily click 15 kHz off centre and still mean the
// same satellite — while the real channels (137.100 / 137.620 / 137.9125) are
// 500 kHz apart, so 50 kHz separates the two cases with room on both sides.
// NEWPASS_LINES likewise: a polarisation null on a low pass can mute the signal
// for several seconds and must NOT wipe the picture, while two passes are
// ~100 minutes apart, so anything from ~10 s to minutes is correct.
#define RETUNE_HZ      50000.0
#define NEWPASS_LINES  60                // 30 s at 2 lines/s

// ---- state (audio-thread owned unless noted) ------------------------------
static gboolean enabled = FALSE;

// live parameters (written from the GTK thread)
static volatile gboolean reset_req = FALSE;
static volatile int      p_channel = 0;      // 0 = whole line, 1 = A, 2 = B
static volatile double   slant_ppm = 0.0;
static volatile double   p_contrast = 1.0;   // manual exposure trim, applied on output
static volatile double   p_bright = 0.0;
static volatile gboolean p_autosave = FALSE;
static char              save_dir[512];      // guarded by lock_img
// A pass is 15 minutes; anything shorter than half a minute of picture is a
// fragment of a fly-by or a false start, and auto-saving those would bury the
// real passes in junk.
#define AUTOSAVE_MIN_LINES 60
static void autosave_locked(const char *why);   // defined below, needs the image state

// front-end
static double  fe_h[FE_TAPS];
static double  fe_dI[FE_TAPS], fe_dQ[FE_TAPS];
static int     fe_pos = 0, fe_cnt = 0, fe_dec = 1;
static double  fe_rate = 0.0, fe_off = 0.0;  // rate/offset the front-end is built for
static double  fe_bw = 0.0;                  // actual half-bandwidth accepted (Hz)
// Downmix oscillator as a rotating phasor rather than cos/sin per sample: a
// SoapySDR front-end can hand us 2.4 MS/s, where two trig calls per sample is
// most of a core.  Renormalised periodically so rounding cannot let it drift
// off the unit circle.
static double  osc_r = 1.0, osc_i = 0.0;     // current phasor
static double  osc_dr = 1.0, osc_di = 0.0;   // per-sample rotation
static int     osc_n = 0;
static double  prevI = 0.0, prevQ = 0.0;     // discriminator history

// video detector (shared by both entry points)
static double  work_rate = 0.0;              // sample rate of the envelope ring
static double  vf_h[VF_TAPS];
static double  vf_dI[VF_TAPS], vf_dQ[VF_TAPS];
static int     vf_pos = 0;
static double  sc_r = 1.0, sc_i = 0.0;       // 2400 Hz subcarrier phasor
static double  sc_dr = 1.0, sc_di = 0.0;
static int     sc_n = 0;

// envelope ring
static float   fr[FR_CAP];
static long    ring_w = 0;                   // total samples written (absolute)

// line tracking
static double  line_start = 0.0;             // absolute sample of the next line start
static double  clock_trim = 0.0;             // ppm, automatic slant servo
static gboolean locked = FALSE;
static int     miss = 0;
static int     unlocked_lines = 0;           // consecutive lines with no sync
static double  cand_off = 0.0;               // candidate sync position within the line
static int     cand_run = 0;                 // consecutive lines agreeing on it
static double  last_corr = 0.0;
// Where the front-end is actually listening, in absolute Hz.  Published for the
// readout: a decoder pointed somewhere other than the operator believes looks
// exactly like a dead band, which is the lesson HFDL paid for.  Written on the
// audio thread, read on the GTK thread — a scalar, benign.
static long long tuned_hz = 0;

// sync-A correlation template (mean-removed, unit-ish scale)
static float   tpl[SYNCA_LEN];
static double  tpl_norm = 0.0;
static gboolean tpl_ready = FALSE;

// contrast tracking (EMA of the per-line 2nd/98th percentile of the image area)
static double  black_lvl = 0.0, white_lvl = 0.0;
static gboolean levels_seeded = FALSE;

// image buffer (mutex protected)
static GMutex  lock_img;
static guint8  img[WORDS * BUF_H];
static int     cur_row = 0;                  // next row to write
static int     filled = 0;                   // rows currently valid (<= BUF_H)
static apt_status_t ui;

// ---------------------------------------------------------------------------
// Windowed-sinc low-pass.  fc is normalised (cycles/sample, i.e. Hz / rate).
static void fir_lowpass(double *h, int n, double fc) {
  int c = (n - 1) / 2;
  double sum = 0.0;
  for (int i = 0; i < n; i++) {
    int m = i - c;
    double s = (m == 0) ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
    double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (n - 1));   // Hamming
    h[i] = s * w;
    sum += h[i];
  }
  if (sum != 0.0) for (int i = 0; i < n; i++) h[i] /= sum;
}

static void tpl_init(void) {
  // Sync A: 4 words of black, 7 cycles of a 1040 Hz square wave (4160/1040 = 4
  // words per cycle, so 2 white + 2 black × 7 = 28 words), 7 words of black.
  double sum = 0.0;
  for (int w = 0; w < SYNCA_LEN; w++) {
    double v = 0.0;
    if (w >= 4 && w < 32) v = (((w - 4) / 2) & 1) ? 0.0 : 1.0;
    tpl[w] = (float)v;
    sum += v;
  }
  double mean = sum / SYNCA_LEN;
  double n2 = 0.0;
  for (int w = 0; w < SYNCA_LEN; w++) { tpl[w] -= (float)mean; n2 += tpl[w] * tpl[w]; }
  tpl_norm = sqrt(n2);
  tpl_ready = TRUE;
}

static void set_status(const char *s) { g_strlcpy(ui.status, s, sizeof(ui.status)); }

static void do_reset(void) {
  memset(fe_dI, 0, sizeof(fe_dI)); memset(fe_dQ, 0, sizeof(fe_dQ));
  fe_pos = 0; fe_cnt = 0;
  osc_r = 1.0; osc_i = 0.0; osc_n = 0; prevI = prevQ = 0.0;
  memset(vf_dI, 0, sizeof(vf_dI)); memset(vf_dQ, 0, sizeof(vf_dQ));
  vf_pos = 0; sc_r = 1.0; sc_i = 0.0; sc_n = 0;
  ring_w = 0;
  line_start = 0.0; clock_trim = 0.0; locked = FALSE; miss = 0; last_corr = 0.0;
  unlocked_lines = 0; cand_off = 0.0; cand_run = 0;
  black_lvl = white_lvl = 0.0; levels_seeded = FALSE;
  g_mutex_lock(&lock_img);
  memset(img, 0, sizeof(img));
  cur_row = 0; filled = 0;
  ui.locked = FALSE; ui.lines = 0; ui.quality = 0.0; ui.clock_ppm = 0.0;
  set_status("Waiting for APT…");
  g_mutex_unlock(&lock_img);
}

void apt_decoder_set_enabled(gboolean on) {
  if (on == enabled) return;
  enabled = on;
  if (on && !tpl_ready) tpl_init();
  if (on) reset_req = TRUE;
  else {
    // Switching the decoder off (or leaving FMN / the APT selection) ends the
    // pass just as surely as a retune does.
    g_mutex_lock(&lock_img);
    autosave_locked("decoder off");
    g_mutex_unlock(&lock_img);
  }
}

void apt_decoder_reset(void) { reset_req = TRUE; }
void apt_decoder_set_channel(int ch) { if (ch >= 0 && ch <= 2) p_channel = ch; }

void apt_decoder_set_levels(double contrast, double brightness) {
  if (contrast < 0.1) contrast = 0.1;
  if (contrast > 5.0) contrast = 5.0;
  if (brightness < -128.0) brightness = -128.0;
  if (brightness > 128.0) brightness = 128.0;
  p_contrast = contrast;
  p_bright = brightness;
}

void apt_decoder_set_autosave(gboolean on, const char *dir) {
  g_mutex_lock(&lock_img);
  p_autosave = on;
  if (dir != NULL && dir[0] != '\0') g_strlcpy(save_dir, dir, sizeof(save_dir));
  else save_dir[0] = '\0';
  g_mutex_unlock(&lock_img);
}

// Manual exposure trim, as a lookup table over the automatically-levelled grey.
static void build_lut(guint8 lut[256]) {
  double c = p_contrast, b = p_bright;
  for (int i = 0; i < 256; i++) {
    double v = (i - 128.0) * c + 128.0 + b;
    if (v < 0.0) v = 0.0;
    if (v > 255.0) v = 255.0;
    lut[i] = (guint8)(v + 0.5);
  }
}

// --- auto-save --------------------------------------------------------------
// The decode runs on the audio thread and a pass is several megabytes of PNG,
// so the picture is copied out under the image lock and written by a throwaway
// worker: neither the audio thread nor the UI waits on the encoder.
typedef struct {
  int     w, h;
  guint8 *pix;          // w*h greyscale, LUT already applied
  char    path[640];
} apt_save_job;

static gpointer save_worker(gpointer data) {
  apt_save_job *j = data;
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, j->w, j->h);
  if (pb != NULL) {
    guint8 *dst = gdk_pixbuf_get_pixels(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    for (int y = 0; y < j->h; y++) {
      const guint8 *src = j->pix + (size_t)y * j->w;
      guint8 *row = dst + (size_t)y * stride;
      for (int x = 0; x < j->w; x++)
        row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = src[x];
    }
    GError *err = NULL;
    if (gdk_pixbuf_save(pb, j->path, "png", &err, NULL))
      log_info("APT: auto-saved %s (%d lines)\n", j->path, j->h);
    else {
      log_error("APT: auto-save failed: %s\n", err ? err->message : "?");
      if (err) g_error_free(err);
    }
    g_object_unref(pb);
  }
  g_free(j->pix);
  g_free(j);
  return NULL;
}

// Hand the finished pass to a writer thread.  MUST be called with lock_img held
// (it reads the image buffer and save_dir), and never for an explicit Clear.
static void autosave_locked(const char *why) {
  if (!p_autosave || filled < AUTOSAVE_MIN_LINES) return;

  char dir[640];
  if (save_dir[0] != '\0') g_strlcpy(dir, save_dir, sizeof(dir));
  else g_snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr/apt", g_get_home_dir());
  if (g_mkdir_with_parents(dir, 0755) != 0) {
    log_error("APT: auto-save cannot create %s\n", dir);
    return;
  }

  apt_save_job *j = g_new0(apt_save_job, 1);
  j->w = WORDS;
  j->h = filled;
  j->pix = g_malloc((size_t)j->w * j->h);
  guint8 lut[256];
  build_lut(lut);
  for (size_t i = 0; i < (size_t)j->w * j->h; i++) j->pix[i] = lut[img[i]];

  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  g_snprintf(j->path, sizeof(j->path), "%s/apt_%s.png", dir, ts);

  log_info("APT: saving pass (%s)\n", why);
  GThread *t = g_thread_new("apt-save", save_worker, j);
  if (t != NULL) g_thread_unref(t);
}
void   apt_decoder_adjust_slant(double dppm) { slant_ppm += dppm; }
double apt_decoder_get_slant(void) { return slant_ppm; }
double apt_decoder_get_bandwidth(void) { return fe_bw; }

// Start a fresh picture: the decoder has decided it is looking at a different
// transmission (see RETUNE_HZ / NEWPASS_LINES).  Line tracking is left to the
// caller, which knows whether it already has a sync position to keep.
//
// `clock_trim` is deliberately NOT reset: it measures OUR receiver's sample
// clock, which is the same for the next pass as it was for the last, so keeping
// it means the next picture comes out square from its first line instead of
// spending 20 lines re-converging on a number we already knew.
static void clear_image(const char *why) {
  g_mutex_lock(&lock_img);
  autosave_locked(why);            // the pass about to be wiped cannot be repeated
  memset(img, 0, sizeof(img));
  cur_row = 0; filled = 0;
  ui.lines = 0; ui.locked = FALSE; ui.quality = 0.0;
  g_snprintf(ui.status, sizeof(ui.status), "New pass (%s) — searching for sync…", why);
  g_mutex_unlock(&lock_img);
  levels_seeded = FALSE;
  log_info("APT: new pass (%s)\n", why);
}

// --- ring access -----------------------------------------------------------
static inline float fr_at(long abs) {
  if (abs < 0) abs = 0;
  if (abs >= ring_w) abs = ring_w - 1;
  if (abs < 0) return 0.0f;
  if (ring_w - abs >= FR_CAP) abs = ring_w - FR_CAP + 1;   // fell out of the ring
  return fr[abs & FR_MASK];
}
static double fr_mean(double centre, int n) {
  long a = (long)(centre - n / 2.0 + 0.5);
  double s = 0.0;
  for (int i = 0; i < n; i++) s += fr_at(a + i);
  return s / (n > 0 ? n : 1);
}

// --- video detector: 2400 Hz quadrature AM demodulator ---------------------
// Mixing the discriminator output down by the subcarrier and taking the
// magnitude of the low-passed result recovers the video amplitude, and drops
// both the DC term (any residual carrier offset / Doppler) and everything
// outside the 2.1 kHz video band.
static void video_sample(double x) {
  double nr = sc_r * sc_dr - sc_i * sc_di;
  double ni = sc_r * sc_di + sc_i * sc_dr;
  sc_r = nr; sc_i = ni;
  if (++sc_n >= 1024) {
    double m = 1.0 / hypot(sc_r, sc_i);
    sc_r *= m; sc_i *= m; sc_n = 0;
  }

  vf_dI[vf_pos] = x * sc_r;
  vf_dQ[vf_pos] = x * sc_i;
  int p = vf_pos;
  vf_pos = (vf_pos + 1) % VF_TAPS;

  double oi = 0.0, oq = 0.0;
  for (int k = 0; k < VF_TAPS; k++) {
    int idx = p - k; if (idx < 0) idx += VF_TAPS;
    oi += vf_h[k] * vf_dI[idx];
    oq += vf_h[k] * vf_dQ[idx];
  }
  fr[ring_w & FR_MASK] = (float)(2.0 * hypot(oi, oq));   // ×2: the mixer halves it
  ring_w++;
}

// Configure the detector for a (new) working rate.
static void video_configure(double rate) {
  if (rate == work_rate) return;
  work_rate = rate;
  double w = -2.0 * M_PI * SUBCARRIER / rate;
  sc_dr = cos(w); sc_di = sin(w);
  sc_r = 1.0; sc_i = 0.0; sc_n = 0;
  fir_lowpass(vf_h, VF_TAPS, VIDEO_BW / rate);
  memset(vf_dI, 0, sizeof(vf_dI)); memset(vf_dQ, 0, sizeof(vf_dQ));
  vf_pos = 0;
  log_info("APT: video detector at %.0f Hz\n", rate);
}

// --- sync correlation ------------------------------------------------------
// Normalised (Pearson) correlation of the envelope against the sync-A template
// at candidate line start `pos`.  Rejecting on a normalised score means the lock
// threshold is a signal-shape test, not a level test, so it holds through fades.
static double sync_corr(double pos, double spw) {
  double e[SYNCA_LEN];
  double sum = 0.0;
  int navg = (int)(spw * 0.7); if (navg < 1) navg = 1;
  for (int w = 0; w < SYNCA_LEN; w++) {
    e[w] = fr_mean(pos + (w + 0.5) * spw, navg);
    sum += e[w];
  }
  double mean = sum / SYNCA_LEN;
  double num = 0.0, den = 0.0;
  for (int w = 0; w < SYNCA_LEN; w++) {
    double d = e[w] - mean;
    num += d * tpl[w];
    den += d * d;
  }
  if (den <= 0.0 || tpl_norm <= 0.0) return 0.0;
  return num / (sqrt(den) * tpl_norm);
}

static int cmp_float(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// Decode one line starting at absolute sample `L` spanning `line_len` samples.
static void decode_line(double L, double line_len) {
  double spw = line_len / (double)WORDS;
  int navg = (int)(spw * 0.8); if (navg < 1) navg = 1;
  static float row[WORDS];
  for (int x = 0; x < WORDS; x++)
    row[x] = (float)fr_mean(L + (x + 0.5) * spw, navg);

  // Contrast: the 2nd/98th percentile of the two image areas, smoothed across
  // lines.  Proper APT calibration reads the telemetry wedges; this is the
  // honest approximation — it tracks the picture rather than claiming a
  // radiometric level it has not measured.
  static float scratch[2 * IMG_LEN];
  memcpy(scratch, row + IMGA_OFF, IMG_LEN * sizeof(float));
  memcpy(scratch + IMG_LEN, row + IMGB_OFF, IMG_LEN * sizeof(float));
  qsort(scratch, 2 * IMG_LEN, sizeof(float), cmp_float);
  double lo = scratch[(int)(2 * IMG_LEN * 0.02)];
  double hi = scratch[(int)(2 * IMG_LEN * 0.98)];
  if (hi <= lo) hi = lo + 1e-9;
  if (!levels_seeded) { black_lvl = lo; white_lvl = hi; levels_seeded = TRUE; }
  else {
    const double A = 0.08;
    black_lvl += (lo - black_lvl) * A;
    white_lvl += (hi - white_lvl) * A;
  }
  double span = white_lvl - black_lvl;
  if (span < 1e-9) span = 1e-9;

  g_mutex_lock(&lock_img);
  if (cur_row >= BUF_H) {                       // scroll the top third off
    memmove(img, img + SCROLL * WORDS, (size_t)(BUF_H - SCROLL) * WORDS);
    memset(img + (size_t)(BUF_H - SCROLL) * WORDS, 0, (size_t)SCROLL * WORDS);
    cur_row = BUF_H - SCROLL;
  }
  guint8 *dst = &img[(size_t)cur_row * WORDS];
  for (int x = 0; x < WORDS; x++) {
    double v = (row[x] - black_lvl) / span * 255.0;
    if (v < 0.0) v = 0.0;
    if (v > 255.0) v = 255.0;
    dst[x] = (guint8)(v + 0.5);
  }
  cur_row++;
  if (filled < BUF_H) filled++;
  ui.lines = filled;
  ui.locked = locked;
  ui.quality = last_corr;
  ui.clock_ppm = clock_trim;
  g_snprintf(ui.status, sizeof(ui.status), "Receiving   sync %.2f   %+.0f ppm",
             last_corr, clock_trim);
  g_mutex_unlock(&lock_img);
}

// Length of one line in samples at the current clock trim.  The trim is clamped
// where it is set, but the operator's manual slant is unbounded and this value
// divides and drives the drain loop — a zero or negative line length would spin
// `run_lines` forever on the audio thread, so it is pinned to a sane range here,
// at the single point every caller goes through.
static double current_line_len(void) {
  if (work_rate <= 0.0) return 0.0;
  double f = 1.0 + (clock_trim + slant_ppm) / 1e6;
  if (f < 0.5) f = 0.5;
  if (f > 2.0) f = 2.0;
  return work_rate * LINE_S * f;
}

static double search_sync(double from, double to, double step, double spw, double *bestpos) {
  double best = -2.0;
  for (double p = from; p <= to; p += step) {
    double c = sync_corr(p, spw);
    if (c > best) { best = c; *bestpos = p; }
  }
  return best;
}

// Find the sync, update the lock/clock servo, and draw the line.
static void process_line(void) {
  double line_len = current_line_len();
  double spw = line_len / (double)WORDS;

  // Search for sync A: a narrow window around the predicted start once locked,
  // the whole line while hunting.  The hunt is coarse-then-fine — a per-sample
  // sweep of a whole line is ~24 000 correlations twice a second, which is real
  // work to do on the audio thread for no extra accuracy.
  double best, bestpos = line_start;
  if (locked) {
    double w = line_len * 0.006;
    best = search_sync(line_start - w, line_start + w, 1.0, spw, &bestpos);
  } else {
    double coarse = spw / 3.0; if (coarse < 1.0) coarse = 1.0;
    best = search_sync(line_start, line_start + line_len, coarse, spw, &bestpos);
    double c0 = bestpos;
    best = search_sync(c0 - spw, c0 + spw, 1.0, spw, &bestpos);
  }
  last_corr = best;

  if (!locked) {
    // Candidate tracking: does this line's best position agree with the last
    // one's?  A real sync sits at the same offset every line (drifting by only
    // a sample or so from the clock error); noise picks a fresh place each time.
    double off_in_line = bestpos - line_start;
    if (best >= LOCK_CORR) {
      if (cand_run > 0 && fabs(off_in_line - cand_off) < line_len * LOCK_TOL) cand_run++;
      else cand_run = 1;
      cand_off = off_in_line;
    } else {
      cand_run = 0;
    }

    if (cand_run >= LOCK_RUN) {
      // Sync back after a long silence is a new pass, not a continuation — wipe
      // before the first line lands, but keep the position we just found.
      if (unlocked_lines > NEWPASS_LINES) clear_image("signal returned");
      locked = TRUE; miss = 0; unlocked_lines = 0; cand_run = 0;
      line_start = bestpos;
      log_info("APT: sync locked (corr %.2f)\n", best);
    } else {
      unlocked_lines++;
      // Nothing to draw — a picture built from unsynchronised noise is worse
      // than an empty panel.  Step on by one line and keep hunting.
      line_start += line_len;
      g_mutex_lock(&lock_img);
      ui.locked = FALSE; ui.quality = best;
      g_snprintf(ui.status, sizeof(ui.status), "Searching for sync…  (best %.2f)", best);
      g_mutex_unlock(&lock_img);
      return;
    }
  } else {
    if (best >= TRIM_CORR) {
      // Proportional phase correction plus an integral term on the line period:
      // the phase error accumulating line after line IS the sample-clock error,
      // and feeding it back as ppm is what keeps the picture from slanting
      // instead of just shifting it back every line.  With the phase corrected
      // in full each line the residual decays as (1−K) per line, so K sets the
      // convergence time (0.15 ≈ 20 lines / 10 s); the sync position is good to
      // about a sample in 24 000, so the ppm jitter K feeds back is negligible.
      double err = bestpos - line_start;
      line_start = bestpos;
      double step = 1e6 * err / line_len * CLOCK_GAIN;
      if (step >  CLOCK_STEP_MAX) step =  CLOCK_STEP_MAX;
      if (step < -CLOCK_STEP_MAX) step = -CLOCK_STEP_MAX;
      clock_trim += step;
      if (clock_trim >  5000.0) clock_trim =  5000.0;
      if (clock_trim < -5000.0) clock_trim = -5000.0;
      miss = 0;
    } else if (++miss > MISS_MAX) {
      locked = FALSE; cand_run = 0;
      log_info("APT: sync lost\n");
    }
    // Between the two: a fade.  The line keeps its predicted position and the
    // clock trim is left exactly where the good syncs put it — letting a weak
    // correlation move either would throw away the measurement that a whole
    // pass of strong syncs paid for.
  }

  decode_line(line_start, line_len);
  line_start += line_len;
}

// Drain whole lines out of the ring.  A line needs its own length plus the
// search window ahead of it, hence the two-line requirement.
static void run_lines(void) {
  for (;;) {
    double line_len = current_line_len();
    if (line_len <= 0.0) break;                       // detector not configured yet
    if ((double)ring_w < line_start + 2.0 * line_len + 8.0) break;
    process_line();
  }
}

// --- entry point: already-demodulated APT audio ---------------------------
void apt_decoder_add_audio(const gdouble *samples, int nframes, int stride, double rate) {
  if (!enabled || rate <= 0.0) return;
  if (!tpl_ready) tpl_init();
  if (reset_req) { reset_req = FALSE; do_reset(); }
  video_configure(rate);
  for (int i = 0; i < nframes; i++) video_sample(samples[(size_t)i * stride]);
  run_lines();
}

// --- entry point: raw off-air I/Q -----------------------------------------
// Rebuild the front-end for a new sample rate or a new tuned offset.  The
// decimation lands the working rate just above the FM signal's own bandwidth.
static void fe_configure(double rate, double off) {
  int dec = (int)(rate / FE_TARGET);
  if (dec < 1) dec = 1;
  double wr = rate / (double)dec;
  double fc = FE_CUTOFF;
  if (fc > wr * 0.45) fc = wr * 0.45;
  fir_lowpass(fe_h, FE_TAPS, fc / rate);
  fe_bw = fc;
  fe_dec = dec;
  fe_rate = rate; fe_off = off;
  memset(fe_dI, 0, sizeof(fe_dI)); memset(fe_dQ, 0, sizeof(fe_dQ));
  fe_pos = 0; fe_cnt = 0; prevI = prevQ = 0.0;
  double w = -2.0 * M_PI * off / rate;
  osc_dr = cos(w); osc_di = sin(w);
  osc_r = 1.0; osc_i = 0.0; osc_n = 0;
  video_configure(wr);
  log_info("APT: front-end %.0f Hz /%d -> %.0f Hz, offset %+.0f Hz\n", rate, dec, wr, off);
}

void apt_decoder_add_iq(const gdouble *iq, int nframes, double rate,
                        long long centre_hz, long long cursor_hz) {
  if (!enabled || rate <= 0.0) return;
  if (!tpl_ready) tpl_init();
  if (reset_req) { reset_req = FALSE; do_reset(); }

  double off = (double)(cursor_hz - centre_hz);
  tuned_hz = cursor_hz;
  // Retune in place for a small move (the operator nudging the dial); rebuild
  // only when the rate changes or the cursor jumps somewhere else entirely.
  if (rate != fe_rate || fabs(off - fe_off) > 1.0) {
    // A move bigger than the front-end's own passband means the operator has
    // gone to a different signal — another satellite — so the picture that was
    // building belongs to the previous one and must not be continued into.
    gboolean elsewhere = (fe_rate != 0.0) && fabs(off - fe_off) > RETUNE_HZ;
    if (rate != fe_rate) fe_configure(rate, off);
    else {
      // Retune in place, keeping the phasor where it is so there is no phase
      // discontinuity (and no click) as the operator moves the cursor.
      fe_off = off;
      double w = -2.0 * M_PI * off / rate;
      osc_dr = cos(w); osc_di = sin(w);
    }
    if (elsewhere) {
      clear_image("retuned");
      locked = FALSE; miss = 0; unlocked_lines = 0;
      line_start = (double)ring_w;         // hunt from now, not from stale data
    }
  }

  for (int i = 0; i < nframes; i++) {
    // The receiver's buffer is (Q, I) per pair — the order WDSP reads it in.
    // Building the complex sample the other way round conjugates it, which
    // mirrors the frequency axis against the panadapter: a satellite at +30 kHz
    // would be hunted at −30 kHz.
    double I = iq[i * 2 + 1];
    double Q = iq[i * 2];

    double nr = osc_r * osc_dr - osc_i * osc_di;
    double ni = osc_r * osc_di + osc_i * osc_dr;
    osc_r = nr; osc_i = ni;
    if (++osc_n >= 1024) {
      double m = 1.0 / hypot(osc_r, osc_i);
      osc_r *= m; osc_i *= m; osc_n = 0;
    }

    fe_dI[fe_pos] = I * osc_r - Q * osc_i;
    fe_dQ[fe_pos] = I * osc_i + Q * osc_r;
    int p = fe_pos;
    fe_pos = (fe_pos + 1) % FE_TAPS;

    if (++fe_cnt < fe_dec) continue;
    fe_cnt = 0;

    double oi = 0.0, oq = 0.0;
    for (int k = 0; k < FE_TAPS; k++) {
      int idx = p - k; if (idx < 0) idx += FE_TAPS;
      oi += fe_h[k] * fe_dI[idx];
      oq += fe_h[k] * fe_dQ[idx];
    }

    // FM discriminator: the phase step between consecutive samples.
    double num = oq * prevI - oi * prevQ;
    double den = oi * prevI + oq * prevQ;
    prevI = oi; prevQ = oq;
    double f = atan2(num, den) * work_rate / (2.0 * M_PI);   // Hz
    video_sample(f);
  }
  run_lines();
}

// --- UI accessors ----------------------------------------------------------
void apt_decoder_get_status(apt_status_t *st) {
  g_mutex_lock(&lock_img);
  *st = ui;
  g_mutex_unlock(&lock_img);
  st->tuned_hz = tuned_hz;   // audio-thread scalar; benign cross-thread read
}

// Shared by both accessors: copy out `w` columns from `x0`, through the manual
// exposure LUT.
static GdkPixbuf *build_image(int x0, int w) {
  guint8 lut[256];
  build_lut(lut);

  g_mutex_lock(&lock_img);
  int h = filled;
  if (h <= 0) { g_mutex_unlock(&lock_img); return NULL; }
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
  guint8 *pix = gdk_pixbuf_get_pixels(pb);
  int stride = gdk_pixbuf_get_rowstride(pb);
  for (int y = 0; y < h; y++) {
    const guint8 *src = &img[(size_t)y * WORDS + x0];
    guint8 *dst = pix + (size_t)y * stride;
    for (int x = 0; x < w; x++) {
      guint8 v = lut[src[x]];
      dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = v;
    }
  }
  g_mutex_unlock(&lock_img);
  return pb;
}

GdkPixbuf *apt_decoder_get_image(void) {
  int ch = p_channel;
  int x0 = 0, w = WORDS;
  if (ch == 1) { x0 = IMGA_OFF; w = IMG_LEN; }
  else if (ch == 2) { x0 = IMGB_OFF; w = IMG_LEN; }
  return build_image(x0, w);
}

GdkPixbuf *apt_decoder_get_full_image(void) {
  return build_image(0, WORDS);
}
