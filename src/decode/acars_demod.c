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
 * VHF ACARS physical layer — see acars_demod.h for the format and provenance.
 *
 * Chain: NCO downmix -> decimating windowed-sinc low-pass -> AM envelope ->
 * MSK demod (1800 Hz VCO, bit clock derived from its phase, matched filter) ->
 * bit/byte framer -> parity + CRC -> one ACARS block.
 */

#include <glib.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "acars_demod.h"
#include "hfdl_crc.h"      // vendored crc16_ccitt — the same table acarsdec uses

// The MSK demod is rate-parameterised, but 12500 is acarsdec's INTRATE and the
// rate its (and our) real-signal verification was done at, so the front-end
// aims just above it.
#define WORK_TARGET   12500.0
// Half-bandwidth kept out of the channel.  An ACARS channel is 25 kHz wide and
// the modulation reaches 2400 Hz, so ±4.5 kHz takes the signal with room for
// mistuning while still rejecting the neighbours.
#define CHAN_CUTOFF    4500.0

#define MFLTOVER  12          // matched-filter oversampling (acarsdec)
#define PLLG      38e-4
#define PLLC      0.52

// Framing bytes as they appear ON THE WIRE, i.e. with the odd-parity bit
// already in place (ETX = 0x03|0x80, ETB = 0x17|0x80).
#define SYN 0x16
#define SOH 0x01
#define STX 0x02
#define ETX 0x83
#define ETB 0x97
#define DEL 0x7f

#define MAX_TXT 250

enum { ST_WSYN = 0, ST_SYN2, ST_SOH1, ST_TXT, ST_CRC1, ST_CRC2, ST_END };

struct acars_demod {
  // --- front end (I/Q input only) ---
  gboolean iq;
  double   in_rate, offset;
  int      dec, taps, fe_pos, fe_cnt;
  double  *h, *dI, *dQ;
  double   osc_r, osc_i, osc_dr, osc_di;
  int      osc_n;

  // --- envelope conditioning ---
  double   work;            // rate the MSK demod runs at
  double   dc, dc_a;        // DC follower (the AM carrier itself)
  double   lvl_ema;

  // --- MSK demod ---
  int      flen;
  double  *mf;              // matched filter, flen*MFLTOVER+1 taps
  double  *inb_r, *inb_i;   // ring of flen complex samples
  int      idx;
  double   phi, clk, df;
  unsigned mskS;
  double   lvlsum;
  int      lvlcnt;

  // --- framer ---
  uint8_t  outbits;
  int      nbits;
  int      state;
  uint8_t  txt[MAX_TXT + 4];
  int      len;
  uint8_t  crc[2];
  int      perr;
};

// ---------------------------------------------------------------------------
// Windowed-sinc low-pass.  fc is normalised (cycles/sample).
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

static void msk_init(acars_demod *d) {
  d->flen = (int)(d->work / 1200.0) + 1;
  int fleno = d->flen * MFLTOVER + 1;
  d->mf    = g_new0(double, fleno);
  d->inb_r = g_new0(double, d->flen);
  d->inb_i = g_new0(double, d->flen);
  // acarsdec's matched pulse: one half cycle of a 600 Hz cosine, negative lobes
  // clipped away — i.e. the MSK symbol shape, sampled MFLTOVER times finer than
  // the input so the bit decision can be taken at a fractional sample position.
  for (int i = 0; i < fleno; i++) {
    double v = cos(2.0 * M_PI * 600.0 / d->work / MFLTOVER * (i - (fleno - 1) / 2.0));
    d->mf[i] = v < 0.0 ? 0.0 : v;
  }
  d->idx = 0;
  d->phi = d->clk = d->df = 0.0;
  d->mskS = 0;
  d->dc_a = 2.0 * M_PI * 50.0 / d->work;    // DC follower: 50 Hz, far below 1200
  d->outbits = 0;
  d->nbits = 8;
  d->state = ST_WSYN;
}

static void fe_configure(acars_demod *d) {
  int dec = (int)(d->in_rate / WORK_TARGET);
  if (dec < 1) dec = 1;
  d->dec  = dec;
  d->work = d->in_rate / dec;
  // Scale the filter with the decimation: at 2.4 MS/s a fixed short filter has a
  // transition band a hundred kHz wide and would fold the neighbouring channels
  // straight into ours.  Cost is taps/dec MACs per input sample either way.
  int taps = 8 * dec + 1;
  if (taps < 63)   taps = 63;
  if (taps > 1023) taps = 1023;
  if ((taps & 1) == 0) taps++;
  d->taps = taps;
  d->h  = g_new0(double, taps);
  d->dI = g_new0(double, taps);
  d->dQ = g_new0(double, taps);
  double fc = CHAN_CUTOFF;
  if (fc > d->work * 0.45) fc = d->work * 0.45;
  fir_lowpass(d->h, taps, fc / d->in_rate);
  d->fe_pos = d->fe_cnt = 0;
  double w = -2.0 * M_PI * d->offset / d->in_rate;
  d->osc_dr = cos(w); d->osc_di = sin(w);
  d->osc_r  = 1.0;    d->osc_i  = 0.0;
  d->osc_n  = 0;
}

acars_demod *acars_demod_create(double in_rate, double offset_hz) {
  if (in_rate < 10000.0) return NULL;
  acars_demod *d = g_new0(acars_demod, 1);
  d->iq      = TRUE;
  d->in_rate = in_rate;
  d->offset  = offset_hz;
  fe_configure(d);
  msk_init(d);
  return d;
}

acars_demod *acars_demod_create_audio(double audio_rate) {
  // Measured floor: at 9600 (4 samples per bit) the matched filter is a whole
  // sample longer than the 1200 Hz period it is supposed to match and the bit
  // clock slips mid-block.  10 kHz up is clean, and the I/Q path never lands
  // below 12.5 kHz by construction.
  if (audio_rate < 10000.0) return NULL;
  acars_demod *d = g_new0(acars_demod, 1);
  d->iq   = FALSE;
  d->work = audio_rate;
  d->dec  = 1;
  msk_init(d);
  return d;
}

void acars_demod_destroy(acars_demod *d) {
  if (d == NULL) return;
  g_free(d->h); g_free(d->dI); g_free(d->dQ);
  g_free(d->mf); g_free(d->inb_r); g_free(d->inb_i);
  g_free(d);
}

void acars_demod_set_offset(acars_demod *d, double offset_hz) {
  if (d == NULL || !d->iq || offset_hz == d->offset) return;
  d->offset = offset_hz;
  // Keep the phasor where it is — retuning must not put a step in the phase.
  double w = -2.0 * M_PI * offset_hz / d->in_rate;
  d->osc_dr = cos(w); d->osc_di = sin(w);
}

double acars_demod_offset(const acars_demod *d) { return d ? d->offset : 0.0; }

double acars_demod_level_db(const acars_demod *d) {
  if (d == NULL || d->lvl_ema <= 1e-12) return -160.0;
  return 10.0 * log10(d->lvl_ema);
}

double acars_demod_work_rate(const acars_demod *d) { return d ? d->work : 0.0; }

// ---------------------------------------------------------------------------
// Block validation.
//
// Parity is odd per byte and the CRC (reflected CCITT, init 0) covers the block
// from the mode byte through the CRC pair, so a good block leaves zero.  When
// exactly one byte fails parity the damage is one bit in a known byte, so the
// eight candidates can simply be tried against the CRC — which is a far
// stronger test than the parity bit that pointed at it.

static uint16_t block_crc(const uint8_t *txt, int len, const uint8_t *crc) {
  uint16_t c = crc16_ccitt((uint8_t *)txt, (uint32_t)len, 0);
  return crc16_ccitt((uint8_t *)crc, 2, c);
}

static int parity_bad(uint8_t b) { return (__builtin_popcount(b) & 1) == 0; }

static gboolean repair(acars_demod *d, gboolean *corrected) {
  *corrected = FALSE;
  if (block_crc(d->txt, d->len, d->crc) == 0) return TRUE;

  // One bad-parity byte: try its eight bits.
  int bad = -1, nbad = 0;
  for (int i = 0; i < d->len; i++) if (parity_bad(d->txt[i])) { bad = i; nbad++; }
  if (nbad == 1) {
    uint8_t save = d->txt[bad];
    for (int b = 0; b < 8; b++) {
      d->txt[bad] = save ^ (uint8_t)(1 << b);
      if (block_crc(d->txt, d->len, d->crc) == 0) { *corrected = TRUE; return TRUE; }
    }
    d->txt[bad] = save;
    return FALSE;
  }
  if (nbad > 0) return FALSE;      // several bad bytes: out of our depth

  // Parity is clean everywhere, so the damage is in the CRC pair itself.
  for (int i = 0; i < 2; i++) {
    uint8_t save = d->crc[i];
    for (int b = 0; b < 8; b++) {
      d->crc[i] = save ^ (uint8_t)(1 << b);
      if (block_crc(d->txt, d->len, d->crc) == 0) { *corrected = TRUE; return TRUE; }
    }
    d->crc[i] = save;
  }
  return FALSE;
}

static void emit_block(acars_demod *d, acars_block_cb cb, void *ctx) {
  if (cb == NULL || d->len < 13) return;      // shorter than the fixed header

  acars_block_t blk;
  memset(&blk, 0, sizeof(blk));
  gboolean corrected = FALSE;
  blk.crc_ok    = repair(d, &corrected);
  blk.corrected = corrected;
  for (int i = 0; i < d->len; i++) if (parity_bad(d->txt[i])) blk.parity_errs++;
  blk.level_db  = (d->lvlcnt > 0) ? 10.0 * log10(d->lvlsum / d->lvlcnt + 1e-12) : -160.0;

  // Hand the parser the block in the shape it expects: SOH first, DEL last, the
  // CRC pair still in place and the parity bits untouched (it checks the CRC
  // over them itself before masking them off).
  int n = 0;
  blk.bytes[n++] = SOH;
  int take = d->len;
  if (take > ACARS_MAX_BLOCK - 4) take = ACARS_MAX_BLOCK - 4;
  memcpy(blk.bytes + n, d->txt, (size_t)take); n += take;
  blk.bytes[n++] = d->crc[0];
  blk.bytes[n++] = d->crc[1];
  blk.bytes[n++] = DEL;
  blk.len = n;
  cb(ctx, &blk);
}

// Bit-slip search: go back to hunting a SYN one bit at a time.
static void framer_reset(acars_demod *d) {
  d->state  = ST_WSYN;
  d->df     = 0.0;
  d->nbits  = 1;
}

// MACHPSDR_ACARS_BITS=1 prints every byte the framer looks at with the state it
// was in.  "Nothing decodes" has exactly two causes — the front end is not on
// the signal, or the bit stream is not what the framer expects — and this
// separates them in one run.
static gboolean bits_dbg = FALSE;
static gboolean bits_dbg_read = FALSE;

static void framer_byte(acars_demod *d, uint8_t r, acars_block_cb cb, void *ctx) {
  if (G_UNLIKELY(!bits_dbg_read)) {
    bits_dbg = (g_getenv("MACHPSDR_ACARS_BITS") != NULL);
    bits_dbg_read = TRUE;
  }
  if (G_UNLIKELY(bits_dbg))
    g_printerr("[ACARS bits] state %d byte %02x\n", d->state, r);
  switch (d->state) {
    case ST_WSYN:
      if (r == SYN)            { d->state = ST_SYN2; d->nbits = 8; return; }
      // MSK carries no absolute phase, so the whole bit stream can arrive
      // inverted.  A complemented SYN says exactly that; flip the decision
      // polarity from here on rather than throwing the block away.
      if (r == (uint8_t)~SYN)  { d->mskS ^= 2; d->state = ST_SYN2; d->nbits = 8; return; }
      d->nbits = 1;
      return;

    case ST_SYN2:
      if (r == SYN)            { d->state = ST_SOH1; d->nbits = 8; return; }
      if (r == (uint8_t)~SYN)  { d->mskS ^= 2; d->nbits = 8; return; }
      framer_reset(d);
      return;

    case ST_SOH1:
      if (r == SOH) {
        d->state  = ST_TXT;
        d->len    = 0;
        d->perr   = 0;
        d->nbits  = 8;
        d->lvlsum = 0.0;
        d->lvlcnt = 0;
        return;
      }
      framer_reset(d);
      return;

    case ST_TXT:
      d->txt[d->len++] = r;
      if (parity_bad(r) && ++d->perr > 4) { framer_reset(d); return; }
      if (r == ETX || r == ETB) { d->state = ST_CRC1; d->nbits = 8; return; }
      // A DEL well into the block means the end marker was missed: the last
      // three bytes are then the CRC pair and this DEL (acarsdec's recovery).
      if (d->len > 20 && r == DEL) {
        d->len -= 3;
        d->crc[0] = d->txt[d->len];
        d->crc[1] = d->txt[d->len + 1];
        emit_block(d, cb, ctx);
        d->state = ST_END;
        d->nbits = 8;
        return;
      }
      if (d->len > MAX_TXT) { framer_reset(d); return; }
      d->nbits = 8;
      return;

    case ST_CRC1:
      d->crc[0] = r;
      d->state  = ST_CRC2;
      d->nbits  = 8;
      return;

    case ST_CRC2:
      d->crc[1] = r;
      emit_block(d, cb, ctx);
      d->state = ST_END;
      d->nbits = 8;
      return;

    case ST_END:
    default:
      framer_reset(d);
      d->nbits = 8;
      return;
  }
}

// Bits arrive least-significant first within a byte.
static void putbit(acars_demod *d, double v, acars_block_cb cb, void *ctx) {
  d->outbits >>= 1;
  if (v > 0.0) d->outbits |= 0x80;
  if (--d->nbits <= 0) framer_byte(d, d->outbits, cb, ctx);
}

// One envelope sample through the MSK demodulator.
static void msk_sample(acars_demod *d, double in, acars_block_cb cb, void *ctx) {
  // Strip the AM carrier's DC so the matched filter sees the modulation only.
  d->dc += d->dc_a * (in - d->dc);
  double s_in = in - d->dc;
  d->lvl_ema += 0.0005 * (s_in * s_in - d->lvl_ema);

  double s = 2.0 * M_PI * 1800.0 / d->work + d->df;   // VCO step
  d->phi += s;
  if (d->phi >= 2.0 * M_PI) d->phi -= 2.0 * M_PI;

  d->inb_r[d->idx] =  s_in * cos(d->phi);
  d->inb_i[d->idx] = -s_in * sin(d->phi);
  d->idx = (d->idx + 1) % d->flen;

  // The bit clock is the carrier phase: 1800 Hz over 2400 bit/s is 3π/2 of
  // carrier per bit, so the same accumulator gives both.
  d->clk += s;
  if (d->clk < 1.5 * M_PI - s / 2.0) return;
  d->clk -= 1.5 * M_PI;

  int o = (int)(MFLTOVER * (d->clk / s + 0.5));
  if (o > MFLTOVER) o = MFLTOVER;
  if (o < 0) o = 0;
  double vr = 0.0, vi = 0.0;
  for (int j = 0; j < d->flen; j++, o += MFLTOVER) {
    int k = (j + d->idx) % d->flen;
    vr += d->mf[o] * d->inb_r[k];
    vi += d->mf[o] * d->inb_i[k];
  }
  double lvl = hypot(vr, vi);
  vr /= lvl + 1e-8;
  vi /= lvl + 1e-8;
  d->lvlsum += lvl * lvl / 4.0;
  d->lvlcnt++;

  // MSK alternates the data between the two quadratures; the sign pattern
  // repeats every four symbols, which is what mskS tracks.  dphi is the
  // orthogonal component — the timing/frequency error the PLL is driven by.
  double vo, dphi;
  if (d->mskS & 1) { vo = vi; dphi = (vo >= 0.0) ? -vr :  vr; }
  else             { vo = vr; dphi = (vo >= 0.0) ?  vi : -vi; }
  putbit(d, (d->mskS & 2) ? -vo : vo, cb, ctx);
  d->mskS++;

  d->df = PLLC * d->df + (1.0 - PLLC) * PLLG * dphi;
}

void acars_demod_process_audio(acars_demod *d, const double *samples, int nframes,
                               int stride, acars_block_cb cb, void *ctx) {
  if (d == NULL || samples == NULL || nframes <= 0) return;
  if (stride < 1) stride = 1;
  for (int i = 0; i < nframes; i++)
    msk_sample(d, samples[(size_t)i * stride], cb, ctx);
}

void acars_demod_process_iq(acars_demod *d, const double *iq, int nframes,
                            acars_block_cb cb, void *ctx) {
  if (d == NULL || !d->iq || iq == NULL || nframes <= 0) return;

  for (int i = 0; i < nframes; i++) {
    // (Q, I) per pair — the order WDSP reads the receiver's buffer in.  Build
    // the complex sample the other way round and the frequency axis mirrors
    // against the panadapter: a channel at +20 kHz gets hunted at −20 kHz.
    double I = iq[i * 2 + 1];
    double Q = iq[i * 2];

    double nr = d->osc_r * d->osc_dr - d->osc_i * d->osc_di;
    double ni = d->osc_r * d->osc_di + d->osc_i * d->osc_dr;
    d->osc_r = nr; d->osc_i = ni;
    if (++d->osc_n >= 1024) {
      double m = 1.0 / hypot(d->osc_r, d->osc_i);
      d->osc_r *= m; d->osc_i *= m; d->osc_n = 0;
    }

    d->dI[d->fe_pos] = I * d->osc_r - Q * d->osc_i;
    d->dQ[d->fe_pos] = I * d->osc_i + Q * d->osc_r;
    int p = d->fe_pos;
    d->fe_pos = (d->fe_pos + 1) % d->taps;

    if (++d->fe_cnt < d->dec) continue;
    d->fe_cnt = 0;

    double oi = 0.0, oq = 0.0;
    for (int k = 0; k < d->taps; k++) {
      int idx = p - k; if (idx < 0) idx += d->taps;
      oi += d->h[k] * d->dI[idx];
      oq += d->h[k] * d->dQ[idx];
    }
    // AM detection is the magnitude, which is also why nothing here cares about
    // a frequency error: mistuning rotates the phasor, and |z| does not see it.
    msk_sample(d, hypot(oi, oq), cb, ctx);
  }
}

// ---------------------------------------------------------------------------
// Modulator (self-test + offline harness).

int acars_demod_build_frame(uint8_t *out, const uint8_t *payload, int len) {
  int n = 0;
  out[n++] = 0xff;              // pre-key: the receiver's PLL needs something
  out[n++] = 0xff;
  out[n++] = SYN;
  out[n++] = SYN;
  out[n++] = SOH;
  // Odd parity per byte, computed here so a caller never has to think about it.
  uint8_t body[MAX_TXT + 4];
  for (int i = 0; i < len; i++) {
    uint8_t b = payload[i] & 0x7f;
    if ((__builtin_popcount(b) & 1) == 0) b |= 0x80;
    body[i] = b;
    out[n++] = b;
  }
  uint16_t crc = crc16_ccitt(body, (uint32_t)len, 0);
  out[n++] = (uint8_t)(crc & 0xff);
  out[n++] = (uint8_t)(crc >> 8);
  out[n++] = DEL;
  return n;
}

int acars_demod_modulate(double *out, int max, const uint8_t *frame, int len,
                         double rate) {
  // Continuous-phase MSK: 1800 ± 600 Hz, i.e. one full 2400 Hz cycle or one
  // half 1200 Hz cycle per 2400 bps bit.
  //
  // The data is DIFFERENTIALLY PRECODED (NRZI) onto that: the tone says whether
  // this data bit REPEATS the last one, not what it is.  It has to be — an MSK
  // receiver that slices the accumulated phase (which is what the coherent
  // demod above does, and what any MSK receiver does) recovers the running sum
  // of the frequency decisions, so only a precoded transmitter comes back out
  // as the data that went in.  Measured against the demodulator and confirmed
  // on acarsdec's own off-air test recording; getting this backwards produces a
  // bit stream that never syncs, which is exactly what the first version did.
  //
  // The waveform is evaluated at the exact sample instants rather than built by
  // stepping a phase accumulator and switching tone on a sample boundary: at a
  // rate that is not a whole number of samples per bit, snapping the tone
  // changes to the sample grid puts a systematic timing bias on the modulation
  // that the receiver's clock recovery reads as a rate error and eventually
  // slips on.  (That bias, not the demodulator, is what made the first version
  // of this test fail at 12.5 kHz while passing at 48 kHz.)
  int nbits = len * 8;
  if (nbits <= 0) return 0;
  double *ph = g_new0(double, (size_t)nbits + 1);   // phase at each bit boundary
  double *fr = g_new0(double, (size_t)nbits);       // tone during each bit
  int prev = 1;                                     // matches the pre-key of ones
  for (int i = 0, k = 0; i < len; i++) {
    for (int b = 0; b < 8; b++, k++) {              // LSB first
      int data = (frame[i] >> b) & 1;
      int bit  = (data == prev) ? 1 : 0;            // NRZI: 1 = no change
      prev = data;
      fr[k] = bit ? 2400.0 : 1200.0;
      ph[k + 1] = ph[k] + 2.0 * M_PI * fr[k] / 2400.0;
    }
  }
  int total = (int)(nbits * rate / 2400.0);
  if (total > max) total = max;
  for (int n = 0; n < total; n++) {
    double t = n / rate;                            // seconds into the frame
    int    k = (int)(t * 2400.0);
    if (k >= nbits) { total = n; break; }
    out[n] = cos(ph[k] + 2.0 * M_PI * fr[k] * (t - k / 2400.0));
  }
  g_free(ph); g_free(fr);
  return total;
}

// ---------------------------------------------------------------------------
// Self-test.

typedef struct { int n; acars_block_t last; } st_sink;

static void st_cb(void *ctx, const acars_block_t *blk) {
  st_sink *s = ctx;
  s->n++;
  s->last = *blk;
}

// A realistic downlink: mode, 7-char registration, ack, label, block id, STX,
// message number, flight id, text, ETX.
static int st_payload(uint8_t *p) {
  const char *hdr = "2.N839AW\x15" "H1" "4";       // mode, reg, NAK, label, blk
  int n = 0;
  memcpy(p, hdr, 12); n = 12;
  p[n++] = 0x02;                                   // STX
  const char *body = "M62AAF447/TESTMESSAGE";
  memcpy(p + n, body, strlen(body)); n += (int)strlen(body);
  p[n++] = 0x03;                                   // ETX
  return n;
}

static gboolean st_run_audio(const double *env, int n, double rate,
                             const uint8_t *want, int wantlen, const char *what) {
  acars_demod *d = acars_demod_create_audio(rate);
  st_sink s = { 0, { { 0 }, 0, FALSE, 0, FALSE, 0.0 } };
  acars_demod_process_audio(d, env, n, 1, st_cb, &s);
  gboolean ok = TRUE;
  if (s.n != 1) {
    g_printerr("[ACARS demod selftest] %s: %d block(s), expected 1\n", what, s.n);
    ok = FALSE;
  } else if (!s.last.crc_ok) {
    g_printerr("[ACARS demod selftest] %s: CRC failed\n", what);
    ok = FALSE;
  } else if (s.last.len != wantlen + 1 ||
             memcmp(s.last.bytes + 1, want, (size_t)wantlen) != 0) {
    g_printerr("[ACARS demod selftest] %s: payload mismatch (%d bytes, want %d)\n",
               what, s.last.len, wantlen + 1);
    ok = FALSE;
  }
  acars_demod_destroy(d);
  return ok;
}

// Modulate an envelope onto a carrier at `off` Hz and run the whole I/Q path.
static gboolean st_run_iq(const double *env, int n, double rate, double off,
                          double noise, gboolean conj,
                          const uint8_t *want, int wantlen, const char *what) {
  double *iq = g_new0(double, (size_t)2 * n);
  double ph = 0.0, dph = 2.0 * M_PI * off / rate;
  guint32 seed = 12345;
  for (int i = 0; i < n; i++) {
    double a = 0.5 + 0.4 * env[i];                 // AM: carrier + modulation
    double I = a * cos(ph), Q = a * sin(ph);
    ph += dph; if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
    if (noise > 0.0) {
      // Cheap deterministic uniform noise — the point is a disturbance of a
      // known size, not a Gaussian one.
      seed = seed * 1103515245u + 12345u;
      I += noise * (((seed >> 16) & 0x7fff) / 16384.0 - 1.0);
      seed = seed * 1103515245u + 12345u;
      Q += noise * (((seed >> 16) & 0x7fff) / 16384.0 - 1.0);
    }
    if (conj) Q = -Q;
    iq[2 * i]     = Q;      // the receiver's buffer order
    iq[2 * i + 1] = I;
  }
  acars_demod *d = acars_demod_create(rate, conj ? -off : off);
  st_sink s = { 0, { { 0 }, 0, FALSE, 0, FALSE, 0.0 } };
  acars_demod_process_iq(d, iq, n, st_cb, &s);
  gboolean ok = TRUE;
  if (s.n != 1) {
    g_printerr("[ACARS demod selftest] %s: %d block(s), expected 1\n", what, s.n);
    ok = FALSE;
  } else if (!s.last.crc_ok) {
    g_printerr("[ACARS demod selftest] %s: CRC failed\n", what);
    ok = FALSE;
  } else if (s.last.len != wantlen + 1 ||
             memcmp(s.last.bytes + 1, want, (size_t)wantlen) != 0) {
    g_printerr("[ACARS demod selftest] %s: payload mismatch (%d bytes, want %d)\n",
               what, s.last.len, wantlen + 1);
    ok = FALSE;
  }
  acars_demod_destroy(d);
  g_free(iq);
  return ok;
}

gboolean acars_demod_selftest(void) {
  gboolean ok = TRUE;
  uint8_t payload[MAX_TXT];
  int plen = st_payload(payload);
  uint8_t frame[MAX_TXT + 16];
  int flen = acars_demod_build_frame(frame, payload, plen);

  // The parity/CRC the receiver must reproduce: everything after SOH, minus the
  // pre-key, i.e. the on-wire bytes the framer will hand to the parser.
  const uint8_t *want = frame + 5;                 // past pre-key, SYN SYN SOH
  int wantlen = flen - 5;                          // body + CRC + DEL

  struct { double rate; const char *name; } audio[] = {
    { 12500.0, "audio 12.5k" }, { 48000.0, "audio 48k" },
  };
  for (int c = 0; c < (int)G_N_ELEMENTS(audio); c++) {
    int max = (int)(audio[c].rate * 2.0);
    double *env = g_new0(double, (size_t)max);
    int n = acars_demod_modulate(env, max, frame, flen, audio[c].rate);
    if (!st_run_audio(env, n, audio[c].rate, want, wantlen, audio[c].name)) ok = FALSE;
    // Inverted polarity: MSK carries no absolute phase, so the framer has to
    // recover from the whole bit stream arriving complemented.
    for (int i = 0; i < n; i++) env[i] = -env[i];
    if (!st_run_audio(env, n, audio[c].rate, want, wantlen, "audio inverted")) ok = FALSE;
    g_free(env);
  }

  // I/Q: the offsets are what prove the NCO sign and the (Q, I) buffer order —
  // a mirrored axis is invisible at the centre.
  struct { double rate, off, noise; gboolean conj; const char *name; } cases[] = {
    {  96000.0,      0.0, 0.00, FALSE, "iq 96k centre"      },
    { 192000.0,  30000.0, 0.00, FALSE, "iq 192k +30k"       },
    {  96000.0, -20000.0, 0.05, FALSE, "iq 96k -20k + noise" },
    { 192000.0,  30000.0, 0.00, TRUE,  "iq conjugated"      },
  };
  for (int c = 0; c < (int)G_N_ELEMENTS(cases); c++) {
    int max = (int)(cases[c].rate * 2.0);
    double *env = g_new0(double, (size_t)max);
    int n = acars_demod_modulate(env, max, frame, flen, cases[c].rate);
    if (!st_run_iq(env, n, cases[c].rate, cases[c].off, cases[c].noise,
                   cases[c].conj, want, wantlen, cases[c].name)) ok = FALSE;
    g_free(env);
  }

  // Pure noise must produce nothing.  A framer that finds blocks in noise is
  // worse than one that finds none (the CW decoder's original bug).
  {
    int n = 12500 * 5;
    double *env = g_new0(double, (size_t)n);
    guint32 seed = 999;
    for (int i = 0; i < n; i++) {
      seed = seed * 1103515245u + 12345u;
      env[i] = ((seed >> 16) & 0x7fff) / 16384.0 - 1.0;
    }
    acars_demod *d = acars_demod_create_audio(12500.0);
    st_sink s = { 0, { { 0 }, 0, FALSE, 0, FALSE, 0.0 } };
    acars_demod_process_audio(d, env, n, 1, st_cb, &s);
    if (s.n != 0) {
      g_printerr("[ACARS demod selftest] noise: %d block(s), expected 0\n", s.n);
      ok = FALSE;
    }
    acars_demod_destroy(d);
    g_free(env);
  }

  return ok;
}
