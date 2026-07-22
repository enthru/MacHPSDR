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
#include <string.h>
#include <gtk/gtk.h>

// Prerequisite types for radio.h (mirrors the include order used elsewhere).
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "mode.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"       // RADIO, set_mox()
#include "main.h"        // global RADIO *radio
#include "log.h"
#include "sstv_encoder.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR              48000.0    // mic / TX exchange rate (Hz)
#define F_BLACK         1500.0     // black / colour-0 level
#define F_WHITE         2300.0     // white / colour-255 level
#define F_SYNC          1200.0     // horizontal sync / VIS start-stop tone
#define F_PORCH         1500.0     // porch / separator level
#define TX_AMPL         0.9        // waveform peak (leaves TX headroom)
#define RAMP_MS         5.0        // start/end click-suppression ramp
#define ROBOT_CPORCH_MS 1.5        // Robot chroma porch (1900 Hz)
#define SSTV_TX_MARGIN_MS 3000     // MOX watchdog margin over the waveform length

#define MAX_W           640        // widest supported mode (PD120/180/240)

// ---- mode table (transmit spec; timings match sstv_decoder.c MODES[]) -------
// Families differ in line structure / colourspace, exactly as in the decoder.
typedef enum { FAM_MARTIN, FAM_SCOTTIE, FAM_ROBOT36, FAM_ROBOT72, FAM_PD } sstv_family_t;

typedef struct {
  int           vis;
  const char   *name;
  sstv_family_t family;
  int           width, height;
  double        sync_ms;    // horizontal sync pulse
  double        porch_ms;   // porch after sync
  double        scan_ms;    // luminance / colour scan duration per line
  double        c_ms;       // chroma scan duration (Robot only)
  double        sep_ms;     // separator length (GBR sep / Robot Y-chroma sep)
} sstv_mode_t;

static const sstv_mode_t TX_MODES[] = {
  { 44, "Martin M1",  FAM_MARTIN,  320, 256, 4.862, 0.572, 146.432,   0.0, 0.572 },
  { 40, "Martin M2",  FAM_MARTIN,  320, 256, 4.862, 0.572,  73.216,   0.0, 0.572 },
  { 60, "Scottie S1", FAM_SCOTTIE, 320, 256, 9.000, 1.500, 138.240,   0.0, 1.500 },
  { 56, "Scottie S2", FAM_SCOTTIE, 320, 256, 9.000, 1.500,  88.064,   0.0, 1.500 },
  { 76, "Scottie DX", FAM_SCOTTIE, 320, 256, 9.000, 1.500, 345.600,   0.0, 1.500 },
  {  8, "Robot 36",   FAM_ROBOT36, 320, 240, 9.000, 3.000,  88.000,  44.0, 4.500 },
  { 12, "Robot 72",   FAM_ROBOT72, 320, 240, 9.000, 3.000, 138.000,  69.0, 4.500 },
  { 93, "PD50",       FAM_PD,      320, 256, 20.00, 2.080,  91.520,   0.0, 0.0 },
  { 99, "PD90",       FAM_PD,      320, 256, 20.00, 2.080, 170.240,   0.0, 0.0 },
  { 95, "PD120",      FAM_PD,      640, 496, 20.00, 2.080, 121.7536,  0.0, 0.0 },
  { 98, "PD160",      FAM_PD,      512, 400, 20.00, 2.080, 195.584,   0.0, 0.0 },
  { 96, "PD180",      FAM_PD,      640, 496, 20.00, 2.080, 183.040,   0.0, 0.0 },
  { 97, "PD240",      FAM_PD,      640, 496, 20.00, 2.080, 244.480,   0.0, 0.0 },
};
#define N_TX_MODES (int)(sizeof(TX_MODES) / sizeof(TX_MODES[0]))

static const sstv_mode_t *mode_by_vis(int vis) {
  for (int i = 0; i < N_TX_MODES; i++)
    if (TX_MODES[i].vis == vis) return &TX_MODES[i];
  return NULL;
}

// ---- tone timeline (a list of constant-frequency segments) -----------------
// Built on the GTK thread in sstv_tx_prepare(); walked by the audio thread in
// sstv_tx_next_sample() with a phase accumulator.  Segment boundaries are decided
// from a cumulative double sample count (seg_end_smp), so the fractional per-pixel
// duration never accumulates integer-rounding drift (which would slant the image).
typedef struct { float freq; float dur_ms; } tone_seg_t;

static tone_seg_t *segs = NULL;
static int         n_segs = 0, cap_segs = 0;
static double      total_ms = 0.0;
static long        total_samples = 0;
static gboolean    have = FALSE;
static const sstv_mode_t *prepared = NULL;

// ---- transmit / scheduler state --------------------------------------------
static volatile gboolean tx_active = FALSE;   // clocking the waveform out now
static volatile gboolean finished = FALSE;    // audio thread hit the end
static volatile long     tx_pos = 0;          // next sample index (audio thread)
static int               play_seg = 0;        // current segment (audio thread)
static double            seg_end_smp = 0.0;   // sample at which play_seg ends
static double            play_phase = 0.0;    // phase accumulator (audio thread)
static gboolean          we_keyed = FALSE;    // did we raise MOX (so we drop it)
static guint             tick_id = 0;         // scheduler g_timeout id
static gint64            key_time_ms = 0;     // when WE raised MOX (watchdog, ms)

// ---- timeline builders -----------------------------------------------------
static void seg_reset(void) { n_segs = 0; total_ms = 0.0; }

static void seg_add(double freq, double dur_ms) {
  if (dur_ms <= 0.0) return;
  if (n_segs >= cap_segs) {
    cap_segs = cap_segs ? cap_segs * 2 : 8192;
    segs = g_realloc(segs, (gsize)cap_segs * sizeof(tone_seg_t));
  }
  segs[n_segs].freq = (float)freq;
  segs[n_segs].dur_ms = (float)dur_ms;
  n_segs++;
  total_ms += dur_ms;
}

// Map a 0..255 scan value to its FM tone (1500 Hz→0 .. 2300 Hz→255).
static inline double val2freq(double v) {
  if (v < 0.0) v = 0.0; if (v > 255.0) v = 255.0;
  return F_BLACK + v / 255.0 * (F_WHITE - F_BLACK);
}

// Emit one scan: `n` pixel segments spanning `scan_ms`, each the value's tone.
static void emit_scan(const double *vals, int n, double scan_ms) {
  double ppx = scan_ms / n;
  for (int x = 0; x < n; x++) seg_add(val2freq(vals[x]), ppx);
}

// Standard VIS header: 1900 Hz leader / 1200 Hz break / 1900 Hz leader / 1200 Hz
// start bit / 7 data bits LSB-first (1100 Hz = 1, 1300 Hz = 0) / even parity /
// 1200 Hz stop bit.  Matches decode_vis() in sstv_decoder.c.
static void emit_vis(int vis) {
  seg_add(1900.0, 300.0);
  seg_add(F_SYNC, 10.0);
  seg_add(1900.0, 300.0);
  seg_add(F_SYNC, 30.0);                       // start bit
  int parity = 0;
  for (int k = 0; k < 7; k++) {
    int b = (vis >> k) & 1;
    parity ^= b;
    seg_add(b ? 1100.0 : 1300.0, 30.0);
  }
  seg_add(parity ? 1100.0 : 1300.0, 30.0);     // even-parity bit
  seg_add(F_SYNC, 30.0);                        // stop bit
}

// --- pixel access -----------------------------------------------------------
static const guint8 *pix_base;
static int pix_stride, pix_nch, pix_w, pix_h;

static inline void px_rgb(int x, int y, double *R, double *G, double *B) {
  if (x < 0) x = 0; if (x >= pix_w) x = pix_w - 1;
  if (y < 0) y = 0; if (y >= pix_h) y = pix_h - 1;
  const guint8 *p = pix_base + (gsize)y * pix_stride + (gsize)x * pix_nch;
  *R = p[0]; *G = p[1]; *B = p[2];
}

// Full-range BT.601 RGB→YCbCr (the exact inverse of put_yuv_row() in the decoder).
static inline double rgb_y (double R, double G, double B) { return 0.299*R + 0.587*G + 0.114*B; }
static inline double rgb_cr(double R, double G, double B) { return 128.0 + 0.5*R - 0.418688*G - 0.081312*B; }
static inline double rgb_cb(double R, double G, double B) { return 128.0 - 0.168736*R - 0.331264*G + 0.5*B; }

// Fill a per-pixel value row from a single image row.  `sel`: 0=R 1=G 2=B, or
// 3=Y 4=Cr 5=Cb.
static void row_vals(int y, int sel, int w, double *out) {
  for (int x = 0; x < w; x++) {
    double R, G, B; px_rgb(x, y, &R, &G, &B);
    switch (sel) {
      case 0: out[x] = R; break;
      case 1: out[x] = G; break;
      case 2: out[x] = B; break;
      case 3: out[x] = rgb_y(R, G, B); break;
      case 4: out[x] = rgb_cr(R, G, B); break;
      default:out[x] = rgb_cb(R, G, B); break;
    }
  }
}

// Chroma from the vertical average of two rows (PD 4:2:0).  cr!=0 → Cr, else Cb.
static void row_chroma_avg(int y0, int y1, int cr, int w, double *out) {
  for (int x = 0; x < w; x++) {
    double R0,G0,B0,R1,G1,B1;
    px_rgb(x, y0, &R0,&G0,&B0); px_rgb(x, y1, &R1,&G1,&B1);
    double R=(R0+R1)*0.5, G=(G0+G1)*0.5, B=(B0+B1)*0.5;
    out[x] = cr ? rgb_cr(R,G,B) : rgb_cb(R,G,B);
  }
}

// --- per-family timelines ---------------------------------------------------
// GBR sequential: Martin sends sync,porch,G,sep,B,sep,R,sep; Scottie sends the
// sync between blue and red (sep,G,sep,B,sync,porch,R) with a leading start sync.
static void build_gbr(const sstv_mode_t *m) {
  int w = m->width, h = m->height;
  double v[MAX_W];
  for (int y = 0; y < h; y++) {
    if (m->family == FAM_MARTIN) {
      seg_add(F_SYNC, m->sync_ms);
      seg_add(F_PORCH, m->porch_ms);
      row_vals(y, 1, w, v); emit_scan(v, w, m->scan_ms);      // green
      seg_add(F_PORCH, m->sep_ms);
      row_vals(y, 2, w, v); emit_scan(v, w, m->scan_ms);      // blue
      seg_add(F_PORCH, m->sep_ms);
      row_vals(y, 0, w, v); emit_scan(v, w, m->scan_ms);      // red
      seg_add(F_PORCH, m->sep_ms);
    } else { // Scottie
      if (y == 0) seg_add(F_SYNC, m->sync_ms);                // starting sync
      seg_add(F_PORCH, m->sep_ms);
      row_vals(y, 1, w, v); emit_scan(v, w, m->scan_ms);      // green
      seg_add(F_PORCH, m->sep_ms);
      row_vals(y, 2, w, v); emit_scan(v, w, m->scan_ms);      // blue
      seg_add(F_SYNC, m->sync_ms);
      seg_add(F_PORCH, m->porch_ms);
      row_vals(y, 0, w, v); emit_scan(v, w, m->scan_ms);      // red
    }
  }
}

// Robot 36: Y every line; chroma alternates R-Y / B-Y (4:2:0), the Y/chroma
// separator tone encoding which (1500 Hz → R-Y, 2300 Hz → B-Y).
static void build_robot36(const sstv_mode_t *m) {
  int w = m->width, h = m->height;
  double v[MAX_W];
  for (int y = 0; y < h; y++) {
    int even = (y % 2) == 0;
    seg_add(F_SYNC, m->sync_ms);
    seg_add(F_PORCH, m->porch_ms);
    row_vals(y, 3, w, v); emit_scan(v, w, m->scan_ms);        // Y
    seg_add(even ? 1500.0 : 2300.0, m->sep_ms);              // chroma-select sep
    seg_add(1900.0, ROBOT_CPORCH_MS);
    row_vals(y, even ? 4 : 5, w, v); emit_scan(v, w, m->c_ms); // R-Y / B-Y
  }
}

// Robot 72: Y + R-Y + B-Y every line (4:2:2).
static void build_robot72(const sstv_mode_t *m) {
  int w = m->width, h = m->height;
  double v[MAX_W];
  for (int y = 0; y < h; y++) {
    seg_add(F_SYNC, m->sync_ms);
    seg_add(F_PORCH, m->porch_ms);
    row_vals(y, 3, w, v); emit_scan(v, w, m->scan_ms);        // Y
    seg_add(1500.0, m->sep_ms);
    seg_add(1900.0, ROBOT_CPORCH_MS);
    row_vals(y, 4, w, v); emit_scan(v, w, m->c_ms);           // R-Y
    seg_add(2300.0, m->sep_ms);
    seg_add(1900.0, ROBOT_CPORCH_MS);
    row_vals(y, 5, w, v); emit_scan(v, w, m->c_ms);           // B-Y
  }
}

// PD: two image rows per transmitted line — Y0, R-Y, B-Y, Y1 (4:2:0, chroma from
// the vertical average of the row pair).
static void build_pd(const sstv_mode_t *m) {
  int w = m->width, h = m->height;
  double y0[MAX_W], y1[MAX_W], cr[MAX_W], cb[MAX_W];
  for (int y = 0; y < h; y += 2) {
    int r0 = y, r1 = (y + 1 < h) ? y + 1 : y;
    row_vals(r0, 3, w, y0);
    row_vals(r1, 3, w, y1);
    row_chroma_avg(r0, r1, 1, w, cr);
    row_chroma_avg(r0, r1, 0, w, cb);
    seg_add(F_SYNC, m->sync_ms);
    seg_add(F_PORCH, m->porch_ms);
    emit_scan(y0, w, m->scan_ms);
    emit_scan(cr, w, m->scan_ms);
    emit_scan(cb, w, m->scan_ms);
    emit_scan(y1, w, m->scan_ms);
  }
}

// ===========================================================================
// Public API
// ===========================================================================
gboolean sstv_tx_prepare(int vis, GdkPixbuf *img) {
  if (tx_active) return FALSE;
  const sstv_mode_t *m = mode_by_vis(vis);
  if (m == NULL || img == NULL) return FALSE;

  GdkPixbuf *scaled = gdk_pixbuf_scale_simple(img, m->width, m->height, GDK_INTERP_BILINEAR);
  if (scaled == NULL) return FALSE;
  pix_base   = gdk_pixbuf_get_pixels(scaled);
  pix_stride = gdk_pixbuf_get_rowstride(scaled);
  pix_nch    = gdk_pixbuf_get_n_channels(scaled);
  pix_w      = m->width;
  pix_h      = m->height;

  seg_reset();
  emit_vis(m->vis);
  switch (m->family) {
    case FAM_MARTIN:
    case FAM_SCOTTIE: build_gbr(m);     break;
    case FAM_ROBOT36: build_robot36(m); break;
    case FAM_ROBOT72: build_robot72(m); break;
    case FAM_PD:      build_pd(m);      break;
  }
  g_object_unref(scaled);
  pix_base = NULL;

  total_samples = (long)(total_ms * SR / 1000.0);
  prepared = m;
  have = TRUE;
  log_info("SSTV TX: prepared %s (%d segs, %.1f s)\n", m->name, n_segs, total_ms / 1000.0);
  return TRUE;
}

double sstv_tx_duration(void) { return have ? total_ms / 1000.0 : 0.0; }

// ~100 ms scheduler tick (GTK thread): drops MOX when the waveform finishes and
// enforces the MOX safety watchdog.
static gboolean tx_tick(gpointer data) {
  if (!tx_active) { tick_id = 0; return G_SOURCE_REMOVE; }
  gint64 now_ms = g_get_real_time() / 1000;

  if (finished || tx_pos >= total_samples) {
    tx_active = FALSE;
    if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
    log_info("SSTV TX: complete\n");
    tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  // Safety watchdog: never let our keying hold MOX past the waveform length +
  // margin, even if the TX path stops pulling samples (tx_pos would stall).
  gint64 max_ms = (gint64)total_ms + SSTV_TX_MARGIN_MS;
  if (we_keyed && key_time_ms && (now_ms - key_time_ms) > max_ms) {
    log_error("SSTV TX: WATCHDOG — MOX held >%lldms (tx_pos=%ld/%ld), forcing key-down\n",
              (long long)max_ms, tx_pos, total_samples);
    tx_active = FALSE;
    we_keyed = FALSE;
    set_mox(radio, FALSE);
    tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void sstv_tx_start(void) {
  if (!have) { log_error("SSTV TX: nothing prepared\n"); return; }
  if (tx_active) return;
  if (radio == NULL || !radio->can_transmit) {
    log_error("SSTV TX: radio cannot transmit\n");
    return;
  }
  RECEIVER *rx = radio->active_receiver;
  int rxmode = (rx != NULL) ? rx->mode_a : -1;
  if (rxmode != DIGU && rxmode != DIGL && rxmode != FMN) {
    log_error("SSTV TX: active receiver must be in DIGU / DIGL / FMN\n");
    return;
  }

  // Initialise playback state before keying so the audio thread sees it ready.
  tx_pos = 0;
  play_seg = 0;
  play_phase = 0.0;
  seg_end_smp = (n_segs > 0) ? (double)segs[0].dur_ms * SR / 1000.0 : 0.0;
  finished = FALSE;
  tx_active = TRUE;

  key_time_ms = g_get_real_time() / 1000;
  we_keyed = FALSE;
  if (!radio->mox) { we_keyed = TRUE; set_mox(radio, TRUE); }
  if (tick_id == 0) tick_id = g_timeout_add(100, tx_tick, NULL);
  log_info("SSTV TX: start %s (%.1f s)\n", prepared ? prepared->name : "?", total_ms / 1000.0);
}

void sstv_tx_stop(void) {
  tx_active = FALSE;
  if (we_keyed) { we_keyed = FALSE; set_mox(radio, FALSE); }
  // tx_tick removes itself on its next fire now that tx_active is clear.
}

gboolean sstv_tx_active(void) { return tx_active; }

double sstv_tx_progress(void) {
  if (!tx_active || total_samples <= 0) return 0.0;
  double p = (double)tx_pos / (double)total_samples;
  return p > 1.0 ? 1.0 : p;
}

float sstv_tx_next_sample(void) {
  if (!tx_active) return 0.0f;
  long n = tx_pos;

  // Advance to the segment covering sample n (may skip sub-sample-length segs).
  while (play_seg < n_segs && (double)n >= seg_end_smp) {
    play_seg++;
    if (play_seg < n_segs)
      seg_end_smp += (double)segs[play_seg].dur_ms * SR / 1000.0;
  }
  if (play_seg >= n_segs) { finished = TRUE; return 0.0f; }

  double f = segs[play_seg].freq;
  play_phase += 2.0 * M_PI * f / SR;
  if (play_phase >= 2.0 * M_PI) play_phase -= 2.0 * M_PI;
  double s = sin(play_phase);

  // Raised-cosine click suppression on the very first / last RAMP_MS.
  long ramp = (long)(RAMP_MS * SR / 1000.0);
  double env = 1.0;
  if (n < ramp) {
    env = 0.5 * (1.0 - cos(M_PI * (double)n / (double)ramp));
  } else if (n > total_samples - ramp) {
    long k = total_samples - n;
    if (k < 0) k = 0;
    env = 0.5 * (1.0 - cos(M_PI * (double)k / (double)ramp));
  }

  tx_pos = n + 1;
  return (float)(TX_AMPL * s * env);
}
