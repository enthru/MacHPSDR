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

/*
 * HFDL RX front-end DSP (phase 2a). See hfdl_demod.h for the design and the
 * dumphfdl (GPL-3.0) provenance.
 */

#include <glib.h>
#include <math.h>
#include <complex.h>
#include <stdint.h>
#include <liquid/liquid.h>

#include "hfdl_demod.h"
#include "log.h"

// Root-raised-cosine matched filter, verbatim from dumphfdl src/hfdl.c
// (HFDL_MF_TAPS_CNT = SPS*3 symbols*2 + 1 = 19 taps, symmetric).
#define HFDL_MF_TAPS_CNT 19
static const float hfdl_matched_filter[HFDL_MF_TAPS_CNT] = {
  -0.0170974647427123f,  0.01148231492068473f,  0.03138375667422348f,  0.009454398851680437f,
  -0.04161644170893816f, -0.06451564801420356f, -0.005495792933327306f, 0.1316404671361545f,
   0.2759693160697777f,   0.3375901874933208f,   0.2759693160697777f,    0.1316404671361545f,
  -0.005495792933327306f,-0.06451564801420356f, -0.04161644170893816f,  0.009454398851680437f,
   0.03138375667422348f,  0.01148231492068473f, -0.0170974647427123f
};

// Costas carrier-recovery loop — ported verbatim from dumphfdl src/hfdl.c (its
// own small 2nd-order loop, not liquid's costas). alpha/beta are the loop gains.
typedef struct {
  float phi;    // instantaneous phase
  float dphi;   // phase increment (frequency estimate)
  float alpha;  // proportional gain
  float beta;   // integral gain
  float err;    // last phase error
} hfdl_costas;

static void costas_init(hfdl_costas *c) {
  c->phi = c->dphi = c->err = 0.f;
  c->alpha = 0.1f;
  c->beta = 0.047f * c->alpha * c->alpha;
}
static inline float branchless_limit(float x, float limit) {
  float x1 = fabsf(x + limit);
  float x2 = fabsf(x - limit);
  x1 -= x2;
  return 0.5f * x1;
}
static inline void costas_execute(hfdl_costas *c, float complex in, float complex *out) {
  *out = in * cexpf(-I * c->phi);
}
static inline void costas_adjust(hfdl_costas *c, float err) {
  c->err = branchless_limit(err, 1.0f);
  c->phi += c->alpha * c->err;
  c->dphi += c->beta * c->err;
}
static inline void costas_step(hfdl_costas *c) {
  c->phi += c->dphi;
  if (c->phi > (float)M_PI)       c->phi -= 2.f * (float)M_PI;
  else if (c->phi < -(float)M_PI) c->phi += 2.f * (float)M_PI;
}

struct hfdl_demod {
  double        input_rate;    // off-air complex sample rate (Hz)
  float         resamp_rate;   // HFDL_BASEBAND_RATE / input_rate
  nco_crcf      osc;           // carrier downmix (offset -> DC)
  msresamp_crcf resampler;     // input_rate -> HFDL_BASEBAND_RATE
  agc_crcf      agc;           // level normalisation
  firfilt_crcf  mf;            // matched filter
  float complex *mix;          // scratch: downmixed input block
  int            mix_cap;      // capacity of mix (complex samples)
  float complex *rs;           // scratch: resampled block
  int            rs_cap;       // capacity of rs (complex samples)
  // symbol-recovery stage (phase 2b)
  symsync_crcf  ss;            // symbol-timing recovery (SPS in, 2/symbol out)
  hfdl_costas   loop;          // carrier recovery
  modem         m[4];          // slicers by arity: [1]=BPSK [2]=PSK4 [3]=PSK8
  int           arity;         // modulation the carrier loop currently slices against
  unsigned long ss_idx;        // symsync output index (odd = decision instant)
};

hfdl_demod *hfdl_demod_create(double input_rate) {
  return hfdl_demod_create_at(input_rate, 0.0);
}

hfdl_demod *hfdl_demod_create_at(double input_rate, double channel_offset_hz) {
  if (input_rate < HFDL_BASEBAND_RATE) {
    log_error("hfdl_demod: input_rate %.0f below baseband rate %d\n",
              input_rate, HFDL_BASEBAND_RATE);
    return NULL;
  }
  hfdl_demod *d = g_new0(hfdl_demod, 1);
  d->input_rate = input_rate;
  d->resamp_rate = (float)((double)HFDL_BASEBAND_RATE / input_rate);

  // Downmix the SSB carrier (at +HFDL_CARRIER_OFFSET_HZ in our USB baseband) to
  // DC. mix_block_down multiplies by e^{-j*w*n}, so set w = +offset. (Sign
  // follows the HPSDR non-inverted-I/Q convention; flip if a real signal comes
  // out mirror-imaged, as ppm_cal notes for its own carrier maths.)
  // channel_offset_hz shifts the whole channel: a channel sitting Δ Hz from the
  // receiver centre has its carrier at Δ + HFDL_CARRIER_OFFSET_HZ, which is what
  // lets several channels inside one receiver passband each get their own
  // front-end. Δ=0 is the dial itself — the single-channel case.
  d->osc = nco_crcf_create(LIQUID_NCO);
  nco_crcf_set_frequency(d->osc,
    (float)(2.0 * M_PI * (HFDL_CARRIER_OFFSET_HZ + channel_offset_hz) / input_rate));

  // Multi-stage resampler to the symbol domain, 60 dB stopband (dumphfdl).
  d->resampler = msresamp_crcf_create(d->resamp_rate, 60.0f);

  // AGC, slow bandwidth (dumphfdl uses 0.01).
  d->agc = agc_crcf_create();
  agc_crcf_set_bandwidth(d->agc, 0.01f);

  d->mf = firfilt_crcf_create((float *)hfdl_matched_filter, HFDL_MF_TAPS_CNT);

  // Symbol recovery (phase 2b): symsync (Kaiser, SPS in, 2/symbol out) + Costas
  // + BPSK slicer — all params from dumphfdl hfdl_channel_create().
  d->ss = symsync_crcf_create_kaiser(HFDL_SPS, 3, 0.9f, 16);
  symsync_crcf_set_lf_bw(d->ss, 0.001f);
  // One output per symbol. (dumphfdl runs symsync at 2/symbol and gates on the
  // odd index — but that 2× rate exists only to feed its LMS equalizer's
  // fractional processing; with no equalizer in this phase, 1/symbol is the
  // correct, unambiguous decision instant. Revisit when the equalizer lands.)
  symsync_crcf_set_output_rate(d->ss, 1);
  costas_init(&d->loop);
  // One slicer per modulation. The carrier loop's decision-directed phase error
  // must come from the modulation actually being received, so the framer switches
  // this to the data modulation for the data segments (hfdl_demod_set_slicer).
  d->m[1] = modem_create(LIQUID_MODEM_BPSK);
  d->m[2] = modem_create(LIQUID_MODEM_PSK4);
  d->m[3] = modem_create(LIQUID_MODEM_PSK8);
  d->arity = 1;                 // preamble/mode/training are always BPSK
  d->ss_idx = 0;
  return d;
}

void hfdl_demod_destroy(hfdl_demod *d) {
  if (d == NULL) return;
  if (d->osc)       nco_crcf_destroy(d->osc);
  if (d->resampler) msresamp_crcf_destroy(d->resampler);
  if (d->agc)       agc_crcf_destroy(d->agc);
  if (d->mf)        firfilt_crcf_destroy(d->mf);
  if (d->ss)        symsync_crcf_destroy(d->ss);
  for (int i = 1; i <= 3; i++) if (d->m[i]) modem_destroy(d->m[i]);
  g_free(d->mix);
  g_free(d->rs);
  g_free(d);
}

// Grow a scratch buffer to at least `need` complex samples.
static void ensure_cap(float complex **buf, int *cap, int need) {
  if (*cap >= need) return;
  g_free(*buf);
  *buf = g_new(float complex, need);
  *cap = need;
}

int hfdl_demod_process(hfdl_demod *d, const double *iq, int nframes,
                       float *out, int max_out) {
  if (d == NULL || iq == NULL || nframes <= 0 || out == NULL || max_out <= 0)
    return 0;

  ensure_cap(&d->mix, &d->mix_cap, nframes);
  // resampled count ≤ ceil(nframes*resamp_rate); generous margin for msresamp's
  // internal state so the execute() below can never overrun d->rs.
  int rs_max = (int)(nframes * d->resamp_rate) + 64;
  ensure_cap(&d->rs, &d->rs_cap, rs_max);

  // doubles -> float complex, then downmix carrier -> DC in one block.
  for (int i = 0; i < nframes; i++)
    d->mix[i] = (float)iq[2 * i] + (float)iq[2 * i + 1] * I;
  nco_crcf_mix_block_down(d->osc, d->mix, d->mix, (unsigned int)nframes);

  unsigned int nrs = 0;
  msresamp_crcf_execute(d->resampler, d->mix, (unsigned int)nframes, d->rs, &nrs);

  // AGC + matched filter each resampled sample; emit up to max_out. The AGC/MF
  // still run on every sample so filter state stays continuous even if the
  // caller's buffer fills (can't happen at real off-air rates — the symbol
  // domain is 5400 S/s, orders of magnitude below max_out per block).
  int nout = 0;
  static gboolean warned_trunc = FALSE;
  for (unsigned int k = 0; k < nrs; k++) {
    float complex g, s;
    agc_crcf_execute(d->agc, d->rs[k], &g);
    firfilt_crcf_push(d->mf, g);
    firfilt_crcf_execute(d->mf, &s);
    if (nout < max_out) {
      out[2 * nout]     = crealf(s);
      out[2 * nout + 1] = cimagf(s);
      nout++;
    } else if (!warned_trunc) {
      warned_trunc = TRUE;
      log_error("hfdl_demod: output truncated at %d (nrs=%u) — dropping symbols\n",
                max_out, nrs);
    }
  }
  return nout;
}

void hfdl_demod_set_slicer(hfdl_demod *d, int arity) {
  if (d == NULL || arity < 1 || arity > 3) return;
  d->arity = arity;
}

int hfdl_demod_symbols_cb(hfdl_demod *d, const float *baseband, int nbb,
                          hfdl_symbol_cb cb, void *ctx) {
  if (d == NULL || baseband == NULL || nbb <= 0) return 0;
  int nout = 0;
  for (int j = 0; j < nbb; j++) {
    float complex in = baseband[2 * j] + baseband[2 * j + 1] * I;
    float complex sy[8];
    unsigned int np = 0;
    symsync_crcf_execute(d->ss, &in, 1, sy, &np);
    for (unsigned int i = 0; i < np; i++, d->ss_idx++) {
      // Carrier recovery: advance the loop, de-rotate this symsync output (one
      // per symbol — the decision instant).
      costas_step(&d->loop);
      float complex r;
      costas_execute(&d->loop, sy[i], &r);
      // Decision-directed: the phase error comes from the modulation currently
      // being carried (d->arity), which the framer keeps up to date. Slicing a
      // QPSK/8-PSK symbol as BPSK would feed the loop a bogus error.
      unsigned int bits = 0;
      modem_demodulate(d->m[d->arity], r, &bits);
      costas_adjust(&d->loop, modem_get_demodulator_phase_error(d->m[d->arity]));
      nout++;
      // The callback may change the slicer for the next symbol (framer state).
      if (cb) cb(ctx, crealf(r), cimagf(r));
    }
  }
  return nout;
}

// Buffer-filling wrapper over the per-symbol path (one implementation).
typedef struct { float *out; int max, n; } sym_sink;
static void sym_sink_cb(void *ctx, float re, float im) {
  sym_sink *s = ctx;
  if (s->n < s->max) { s->out[2 * s->n] = re; s->out[2 * s->n + 1] = im; s->n++; }
}

int hfdl_demod_symbols(hfdl_demod *d, const float *baseband, int nbb,
                       float *out_syms, int max_out) {
  if (out_syms == NULL || max_out <= 0) return 0;
  sym_sink s = { out_syms, max_out, 0 };
  hfdl_demod_symbols_cb(d, baseband, nbb, sym_sink_cb, &s);
  return s.n;
}

double hfdl_demod_level_db(hfdl_demod *d) {
  if (d == NULL) return -160.0;
  return (double)agc_crcf_get_rssi(d->agc);
}

gboolean hfdl_demod_selftest(void) {
  const double fs = 48000.0;
  hfdl_demod *d = hfdl_demod_create(fs);
  if (d == NULL) return FALSE;

  // ~0.5 s of a complex tone exactly at the HFDL carrier offset. After the
  // downmix it must sit at DC, so the conditioned output is (near-)coherent:
  // |mean(out)| / mean(|out|) ≈ 1. An off-DC residual would average toward 0.
  const int N = 24000;
  double *iq = g_new(double, 2 * N);
  for (int i = 0; i < N; i++) {
    double ph = 2.0 * M_PI * HFDL_CARRIER_OFFSET_HZ * i / fs;
    iq[2 * i]     = cos(ph);
    iq[2 * i + 1] = sin(ph);
  }
  const int OUT = HFDL_BASEBAND_RATE;   // 1 s worth of headroom (>expected)
  float *out = g_new(float, 2 * OUT);
  int n = hfdl_demod_process(d, iq, N, out, OUT);

  gboolean ok = TRUE;
  // (a) output rate ≈ N * baseband/input (within 2%).
  double expect = N * (double)HFDL_BASEBAND_RATE / fs;
  if (fabs(n - expect) > 0.02 * expect + 4) {
    log_error("hfdl_demod selftest: rate %d, expected ~%.0f\n", n, expect);
    ok = FALSE;
  }
  // (b) coherence at DC — skip the resampler/AGC/MF settling transient.
  int skip = n / 4;
  double sr = 0, si = 0, mag = 0; int cnt = 0;
  for (int i = skip; i < n; i++) {
    double re = out[2 * i], im = out[2 * i + 1];
    sr += re; si += im; mag += hypot(re, im); cnt++;
  }
  if (cnt > 0) {
    double coh = hypot(sr, si) / (mag > 0 ? mag : 1.0);   // ∈[0,1], ≈1 at DC
    if (coh < 0.9) {
      log_error("hfdl_demod selftest: DC coherence %.3f (< 0.9) — downmix off\n", coh);
      ok = FALSE;
    } else {
      log_info("hfdl_demod selftest: PASS (rate %d/%.0f, DC coherence %.3f)\n", n, expect, coh);
    }
  } else {
    ok = FALSE;
  }

  g_free(iq); g_free(out);
  hfdl_demod_destroy(d);

  // (c) End-to-end BPSK recovery through the WHOLE chain. Synthesize RRC-shaped
  // BPSK at 1800 baud directly in the 5400-S/s domain (SPS=3), upconvert onto
  // the +1440 Hz carrier (plus a small offset to exercise Costas), then run it
  // through hfdl_demod_process() (downmix + unity resample + matched filter) →
  // hfdl_demod_symbols() (symsync + Costas + BPSK). TX pulse = the same RRC
  // matched filter, so TX⊛RX = raised cosine → ISI-free at symbol instants.
  // Assert a low BER after the loops settle, resolving the pipeline delay and
  // BPSK's inherent 180° phase ambiguity.
  const double fs2 = HFDL_BASEBAND_RATE;   // 5400 = symbol rate × integer SPS
  hfdl_demod *d2 = hfdl_demod_create(fs2);
  if (d2 == NULL) return FALSE;

  const int NS = 4000;                      // symbols
  int *txbits = g_new(int, NS);
  firinterp_crcf fi = firinterp_crcf_create(HFDL_SPS, (float *)hfdl_matched_filter, HFDL_MF_TAPS_CNT);
  const int NB = NS * HFDL_SPS;             // baseband samples
  double *tx = g_new(double, 2 * NB);
  uint32_t lcg = 0x1234567u;                // deterministic PRBS (reproducible)
  const double coff = 3.0, cph = 0.7;       // carrier offset (Hz) + initial phase
  int w = 0;
  for (int s = 0; s < NS; s++) {
    lcg = lcg * 1103515245u + 12345u;
    int bit = (int)((lcg >> 16) & 1u);
    txbits[s] = bit;
    float complex y[HFDL_SPS];
    firinterp_crcf_execute(fi, bit ? 1.0f : -1.0f, y);   // BPSK ±1
    for (int k = 0; k < HFDL_SPS; k++, w++) {
      double ph = 2.0 * M_PI * (HFDL_CARRIER_OFFSET_HZ + coff) * w / fs2 + cph;
      tx[2 * w]     = creal(y[k] * (cos(ph) + I * sin(ph)));
      tx[2 * w + 1] = cimag(y[k] * (cos(ph) + I * sin(ph)));
    }
  }
  firinterp_crcf_destroy(fi);

  float *bb = g_new(float, 2 * (NB + 64));
  int nbb = hfdl_demod_process(d2, tx, NB, bb, NB + 64);
  float *rs = g_new(float, 2 * (NS + 64));
  int nsy = hfdl_demod_symbols(d2, bb, nbb, rs, NS + 64);

  // Score with DIFFERENTIAL BER: d[i] = bit[i] XOR bit[i-1]. BPSK carries an
  // inherent 180° phase ambiguity (and the Costas loop may slip a half-turn),
  // which the frame preamble resolves later — but the *transitions* are
  // invariant to it, so a low differential BER proves symbol/carrier recovery is
  // working. Sweep the pipeline delay for the best alignment.
  gboolean sym_ok = FALSE;
  if (nsy > 800) {
    const int settle = 400;                 // skip loop settling
    int best_err = INT32_MAX, best_tot = 0;
    for (int delay = 0; delay <= 30; delay++) {
      int err = 0, tot = 0;
      for (int i = settle + 1; i < nsy; i++) {
        int ti = i - delay;
        if (ti < 1 || ti >= NS) continue;
        int drx = ((rs[2 * i] > 0.f) ? 1 : 0) ^ ((rs[2 * (i - 1)] > 0.f) ? 1 : 0);
        int dtx = txbits[ti] ^ txbits[ti - 1];
        if (drx != dtx) err++;
        tot++;
      }
      if (tot > 500 && err < best_err) { best_err = err; best_tot = tot; }
    }
    double ber = best_tot > 0 ? (double)best_err / best_tot : 1.0;
    if (ber < 0.02) {
      sym_ok = TRUE;
      log_info("hfdl_demod selftest: BPSK recovery PASS (%d symbols, diff-BER %.4f)\n", nsy, ber);
    } else {
      log_error("hfdl_demod selftest: BPSK recovery FAIL (diff-BER %.4f over %d symbols)\n", ber, best_tot);
    }
  } else {
    log_error("hfdl_demod selftest: BPSK recovery — too few symbols (%d)\n", nsy);
  }

  g_free(txbits); g_free(tx); g_free(bb); g_free(rs);
  hfdl_demod_destroy(d2);

  // (d) PSK4 carrier recovery — the 1200/1800 bps configs carry their data as
  // QPSK/8-PSK, so the decision-directed loop must slice against that modulation
  // (dumphfdl switches its `current_mod_arity` on entering the data section; we
  // do the same via hfdl_demod_set_slicer, driven by the framer).
  //
  // What this asserts: with the slicer set correctly, QPSK is recovered and the
  // constellation STAYS PUT (scored absolutely — the data decoder slices
  // absolutely, with only a 180° preamble-derived flip). What it deliberately
  // does NOT assert: that a BPSK slicer fails here. Measured on this clean
  // synthetic signal it is only slightly worse (steady-state |R| 0.97 vs 0.99),
  // so an "it must fail" assertion would be false. The switch is kept because it
  // is what the reference implementation does and is measurably no worse — not
  // because this test proves it rescues anything.
  //
  // Also measured while writing this: NEITHER slicer acquires a 100 Hz residual
  // carrier offset — that is the Costas loop's bandwidth, not the slicer. Real
  // signals arrive after the NCO has removed the nominal 1440 Hz, so the residual
  // is small, but this is the acquisition limit if a badly mistuned dial fails.
  gboolean psk4_ok = FALSE;
  {
    const int NS4 = 4000;
    float complex *txc = g_new(float complex, NS4);   // transmitted constellation
    double *tx4 = g_new(double, 2 * NS4 * HFDL_SPS);
    modem mq = modem_create(LIQUID_MODEM_PSK4);
    firinterp_crcf fi4 = firinterp_crcf_create(HFDL_SPS, (float *)hfdl_matched_filter,
                                               HFDL_MF_TAPS_CNT);
    uint32_t lcg4 = 0x0badf00du;
    const double coff4 = 10.0;    // residual offset the loop must acquire and hold
    int w4 = 0;
    for (int s = 0; s < NS4; s++) {
      lcg4 = lcg4 * 1103515245u + 12345u;
      float complex c;
      modem_modulate(mq, (lcg4 >> 16) & 3u, &c);
      txc[s] = c;
      float complex y[HFDL_SPS];
      firinterp_crcf_execute(fi4, c, y);
      for (int k = 0; k < HFDL_SPS; k++, w4++) {
        double ph = 2.0 * M_PI * (HFDL_CARRIER_OFFSET_HZ + coff4) * w4 / fs2 + cph;
        tx4[2 * w4]     = creal(y[k] * (cos(ph) + I * sin(ph)));
        tx4[2 * w4 + 1] = cimag(y[k] * (cos(ph) + I * sin(ph)));
      }
    }
    firinterp_crcf_destroy(fi4);
    modem_destroy(mq);

    const int NB4 = NS4 * HFDL_SPS;
    float *bb4 = g_new(float, 2 * (NB4 + 64));
    float *rs4 = g_new(float, 2 * (NS4 + 64));
    hfdl_demod *d4 = hfdl_demod_create(fs2);
    double ser = 1.0;
    if (d4 != NULL) {
      hfdl_demod_set_slicer(d4, 2);                    // PSK4 — what the data is
      int nbb4 = hfdl_demod_process(d4, tx4, NB4, bb4, NB4 + 64);
      int nsy4 = hfdl_demod_symbols(d4, bb4, nbb4, rs4, NS4 + 64);
      if (nsy4 > 1200) {
        // Score ROTATION-AGNOSTICALLY: r[i]*conj(tx[i-delay]) is a constant if the
        // loop holds the constellation still, whatever constant rotation it settled
        // on. The circular mean length |R| of that product is 1 for a rock-steady
        // constellation and 0 for one that wanders. (Scoring by "which quadrant"
        // does NOT work here — liquid's PSK4 points sit on the axes, so a quadrant
        // decision is a fixed 45° off and reports ~50% errors on a perfect signal.
        // That false failure is what this comment exists to prevent repeating.)
        const int settle = 1000;                       // loop acquisition
        for (int delay = 0; delay <= 30; delay++) {
          float complex acc = 0.f;
          int n = 0;
          for (int i = settle; i < nsy4; i++) {
            int ti = i - delay;
            if (ti < 0 || ti >= NS4) continue;
            float complex v = (rs4[2 * i] + rs4[2 * i + 1] * I) * conjf(txc[ti]);
            float m = cabsf(v);
            if (m > 0.f) { acc += v / m; n++; }
          }
          if (n > 500) { double R = cabsf(acc) / n; if (1.0 - R < ser) ser = 1.0 - R; }
        }
      }
      hfdl_demod_destroy(d4);
    }
    psk4_ok = (ser < 0.10);          // constellation steadiness |R| >= 0.90
    if (psk4_ok)
      log_info("hfdl_demod selftest: PSK4 recovery PASS (constellation steadiness %.3f)\n",
               1.0 - ser);
    else
      log_error("hfdl_demod selftest: PSK4 recovery FAIL (constellation steadiness %.3f, want >=0.90)\n",
                1.0 - ser);
    g_free(txc); g_free(tx4); g_free(bb4); g_free(rs4);
  }

  return ok && sym_ok && psk4_ok;
}
