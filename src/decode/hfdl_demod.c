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
};

hfdl_demod *hfdl_demod_create(double input_rate) {
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
  d->osc = nco_crcf_create(LIQUID_NCO);
  nco_crcf_set_frequency(d->osc, (float)(2.0 * M_PI * HFDL_CARRIER_OFFSET_HZ / input_rate));

  // Multi-stage resampler to the symbol domain, 60 dB stopband (dumphfdl).
  d->resampler = msresamp_crcf_create(d->resamp_rate, 60.0f);

  // AGC, slow bandwidth (dumphfdl uses 0.01).
  d->agc = agc_crcf_create();
  agc_crcf_set_bandwidth(d->agc, 0.01f);

  d->mf = firfilt_crcf_create((float *)hfdl_matched_filter, HFDL_MF_TAPS_CNT);
  return d;
}

void hfdl_demod_destroy(hfdl_demod *d) {
  if (d == NULL) return;
  if (d->osc)       nco_crcf_destroy(d->osc);
  if (d->resampler) msresamp_crcf_destroy(d->resampler);
  if (d->agc)       agc_crcf_destroy(d->agc);
  if (d->mf)        firfilt_crcf_destroy(d->mf);
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
  return ok;
}
