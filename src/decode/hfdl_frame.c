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
 * HFDL framing primitives (deinterleaver + descrambler). See hfdl_frame.h.
 * Ported from dumphfdl src/hfdl.c.
 */

#include <glib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <liquid/liquid.h>

#include "hfdl_frame.h"
#include "hfdl_fec.h"
#include "log.h"

#define DATA_FRAME_CNT_SINGLE_SLOT 72
#define DATA_FRAME_CNT_DOUBLE_SLOT 168
#define DEINTERLEAVER_ROW_CNT      40
#define DEINTERLEAVER_POP_ROW_SHIFT 9

// Frame configurations — verbatim from dumphfdl hfdl_frame_params[].
const hfdl_params hfdl_frame_params[HFDL_M_SHIFT_CNT] = {
  { HFDL_M_BPSK, DATA_FRAME_CNT_SINGLE_SLOT, 4, 17 },  // 300 bps single
  { HFDL_M_BPSK, DATA_FRAME_CNT_SINGLE_SLOT, 2, 17 },  // 600 bps single
  { HFDL_M_PSK4, DATA_FRAME_CNT_SINGLE_SLOT, 2, 17 },  // 1200 bps single
  { HFDL_M_PSK8, DATA_FRAME_CNT_SINGLE_SLOT, 2, 17 },  // 1800 bps single
  { HFDL_M_BPSK, DATA_FRAME_CNT_DOUBLE_SLOT, 4, 23 },  // 300 bps double
  { HFDL_M_BPSK, DATA_FRAME_CNT_DOUBLE_SLOT, 2, 23 },  // 600 bps double
  { HFDL_M_PSK4, DATA_FRAME_CNT_DOUBLE_SLOT, 2, 23 },  // 1200 bps double
  { HFDL_M_PSK8, DATA_FRAME_CNT_DOUBLE_SLOT, 2, 23 },  // 1800 bps double
};

/* ---------------- Deinterleaver (block, 40 rows) ---------------- */

typedef struct {
  uint8_t **table;
  int row, col;
  int column_cnt;
  int push_column_shift;
} deinterleaver;

static int deinterleaver_table_size(const deinterleaver *d) {
  return d->column_cnt * DEINTERLEAVER_ROW_CNT;
}

static deinterleaver *deinterleaver_create(int m_shift) {
  const hfdl_params *p = &hfdl_frame_params[m_shift];
  deinterleaver *d = g_new0(deinterleaver, 1);
  d->column_cnt = p->data_segment_cnt * HFDL_DATA_FRAME_LEN * p->scheme / DEINTERLEAVER_ROW_CNT;
  d->push_column_shift = p->deinterleaver_push_column_shift;
  d->table = g_new0(uint8_t *, DEINTERLEAVER_ROW_CNT);
  for (int i = 0; i < DEINTERLEAVER_ROW_CNT; i++)
    d->table[i] = g_new0(uint8_t, d->column_cnt);
  d->row = d->col = 0;
  return d;
}

static void deinterleaver_destroy(deinterleaver *d) {
  if (d == NULL) return;
  for (int i = 0; i < DEINTERLEAVER_ROW_CNT; i++) g_free(d->table[i]);
  g_free(d->table);
  g_free(d);
}

static void deinterleaver_reset(deinterleaver *d) { d->row = d->col = 0; }

static void deinterleaver_push(deinterleaver *d, uint8_t val) {
  d->table[d->row][d->col] = val;
  d->row++;
  if (d->row == DEINTERLEAVER_ROW_CNT) { d->row = 0; d->col++; }
  d->col -= d->push_column_shift;
  if (d->col < 0) d->col += d->column_cnt;
}

static uint8_t deinterleaver_pop(deinterleaver *d) {
  uint8_t ret = d->table[d->row][d->col];
  d->row = (d->row + DEINTERLEAVER_POP_ROW_SHIFT) % DEINTERLEAVER_ROW_CNT;
  if (d->row == 0) d->col++;
  return ret;
}

/* ---------------- Descrambler (LFSR / liquid msequence) ---------------- */

typedef struct {
  msequence ms;
  uint32_t len;   // sequence period before reset (120)
  uint32_t pos;
} descrambler;

static descrambler *hfdl_descrambler_create(void) {
  descrambler *d = g_new0(descrambler, 1);
  uint32_t genpoly, init;
  // liquid changed msequence_create's argument convention across versions
  // (see dumphfdl): 1.0.0..1.6.0 used one set, ≥1.6.0 the reversed set.
  if (liquid_libversion_number() > 1000000 && liquid_libversion_number() < 1006000) {
    genpoly = 0x8002u; init = 0x6959u;
  } else {
    genpoly = 0x4001u; init = 0x4d4bu;   // 0x6959 reversed
  }
  d->ms = msequence_create(15, genpoly, init);
  d->len = 120;
  d->pos = 0;
  return d;
}

static void hfdl_descrambler_destroy(descrambler *d) {
  if (d == NULL) return;
  msequence_destroy(d->ms);
  g_free(d);
}

static uint32_t descrambler_advance(descrambler *d) {
  if (d->pos == d->len) { d->pos = 0; msequence_reset(d->ms); }
  d->pos++;
  return msequence_advance(d->ms);
}

/* ---------------- Preamble correlator (A-sequence) ---------------- */

#define HFDL_A_LEN 127
#define HFDL_CORR_THRESHOLD_A1 0.36f   // dumphfdl CORR_THRESHOLD_A1

// The 127-bit HFDL "A" preamble sync sequence (verbatim from dumphfdl A_octets).
static const unsigned char hfdl_A_octets[16] = {
  0x5b, 0xbc, 0x74, 0x57, 0x03, 0xd9, 0x89, 0x39,
  0xf2, 0x08, 0xd5, 0x36, 0x94, 0x2c, 0x32, 0xfe
};

typedef struct {
  bsequence ref;   // the known A-sequence
  bsequence win;   // sliding window of the last A_LEN demodulated bits
} hfdl_preamble;

static hfdl_preamble *hfdl_preamble_create(void) {
  hfdl_preamble *p = g_new0(hfdl_preamble, 1);
  p->ref = bsequence_create(HFDL_A_LEN);
  bsequence_init(p->ref, (unsigned char *)hfdl_A_octets);
  p->win = bsequence_create(HFDL_A_LEN);
  return p;
}

static void hfdl_preamble_destroy(hfdl_preamble *p) {
  if (p == NULL) return;
  bsequence_destroy(p->ref);
  bsequence_destroy(p->win);
  g_free(p);
}

// Push one demodulated hard bit into the sliding window and return the
// normalised correlation against the A-sequence, in [-1, 1]. |corr| exceeding
// HFDL_CORR_THRESHOLD_A1 marks the preamble; its sign resolves the BPSK 180°
// phase ambiguity (this is why the real decoder needs no differential coding).
static float hfdl_preamble_push(hfdl_preamble *p, int bit) {
  bsequence_push(p->win, bit);
  int c = bsequence_correlate(p->ref, p->win);
  return 2.0f * (float)c / (float)HFDL_A_LEN - 1.0f;
}

/* ---------------- M1 mode correlator (modulation/slot select) ---------------- */

#define HFDL_M1_LEN 127
#define HFDL_CORR_THRESHOLD_M1 0.30f   // dumphfdl CORR_THRESHOLD_M1

// The M1 base sequence + the 8 per-mode cyclic shifts (verbatim from dumphfdl).
// Each M-shift is one frame configuration (see hfdl_frame_params[]); the M1
// field after the A-preamble identifies which, correlated against these 8
// rotations of M1_bits.
static const uint8_t hfdl_M1_bits[HFDL_M1_LEN] = {
  0,1,1,1,0,1,1,0,1,1,1,1,0,1,0,0,0,1,0,1,1,0,0,
  1,0,1,1,1,1,1,0,0,0,1,0,0,0,0,0,0,1,1,0,0,1,1,0,1,1,
  0,0,0,1,1,1,0,0,1,1,1,0,1,0,1,1,1,0,0,0,0,1,0,0,1,1,
  0,0,0,0,0,1,0,1,0,1,0,1,1,0,1,0,0,1,0,0,1,0,1,0,0,1,
  1,1,1,0,0,1,0,0,0,1,1,0,1,0,1,0,0,0,0,1,1,1,1,1,1,1
};
static const int hfdl_M_shifts[HFDL_M_SHIFT_CNT] = { 72, 82, 113, 123, 61, 103, 93, 9 };

typedef struct {
  bsequence tmpl[HFDL_M_SHIFT_CNT];   // the 8 M1 rotations
  bsequence win;                      // sliding 127-bit window
} hfdl_mode;

static hfdl_mode *hfdl_mode_create(void) {
  hfdl_mode *m = g_new0(hfdl_mode, 1);
  for (int s = 0; s < HFDL_M_SHIFT_CNT; s++) {
    m->tmpl[s] = bsequence_create(HFDL_M1_LEN);
    for (int j = 0; j < HFDL_M1_LEN; j++)
      bsequence_push(m->tmpl[s], hfdl_M1_bits[(hfdl_M_shifts[s] + j) % HFDL_M1_LEN]);
  }
  m->win = bsequence_create(HFDL_M1_LEN);
  return m;
}

static void hfdl_mode_destroy(hfdl_mode *m) {
  if (m == NULL) return;
  for (int s = 0; s < HFDL_M_SHIFT_CNT; s++) bsequence_destroy(m->tmpl[s]);
  bsequence_destroy(m->win);
  g_free(m);
}

static void hfdl_mode_push(hfdl_mode *m, int bit) { bsequence_push(m->win, bit); }

// Best-matching M-shift (frame config) for the current window; *corr gets the
// (absolute) normalised correlation of the winner. dumphfdl match_sequence().
static int hfdl_mode_match(hfdl_mode *m, float *corr) {
  float best = 0.f; int best_i = -1;
  for (int s = 0; s < HFDL_M_SHIFT_CNT; s++) {
    float c = fabsf(2.0f * (float)bsequence_correlate(m->tmpl[s], m->win) / (float)HFDL_M1_LEN - 1.0f);
    if (c > best) { best = c; best_i = s; }
  }
  *corr = best;
  return best_i;
}

/* ---------------- Data-frame decode (symbols -> bytes) ---------------- */

// Decode one HFDL data frame: `nsym` collected data symbols (interleaved float
// I/Q) for configuration `m_shift`, mirroring dumphfdl decode_user_data():
// per-symbol descramble + soft M-PSK demod → block-deinterleave → (r=1/4 chip
// averaging) → r=1/2 Viterbi → `out` bytes (MSB-first, ⌈K/8⌉). Returns the
// decoded bit count K (data + 6 conv tail bits), or -1 on error. `bitmask` is
// the BPSK-polarity flag from the preamble (0 or 1); the caller passes what the
// A-correlator resolved. GTK-independent.
static int hfdl_frame_decode_data(const float *sym_iq, int nsym, int m_shift,
                                  int bitmask, uint8_t *out) {
  const hfdl_params *p = &hfdl_frame_params[m_shift];
  int arity = p->scheme;
  int num_encoded_bits = nsym * arity;
  deinterleaver *di = deinterleaver_create(m_shift);
  if (num_encoded_bits != deinterleaver_table_size(di)) { deinterleaver_destroy(di); return -1; }
  descrambler *ds = hfdl_descrambler_create();
  modem m = modem_create(arity == 1 ? LIQUID_MODEM_BPSK :
                         arity == 2 ? LIQUID_MODEM_PSK4 : LIQUID_MODEM_PSK8);
  uint8_t soft[8];
  float bmflip = (bitmask & 1) ? -1.f : 1.f;
  for (int i = 0; i < nsym; i++) {
    float complex s = sym_iq[2 * i] + sym_iq[2 * i + 1] * I;
    uint32_t dbit = descrambler_advance(ds);
    float flip = (dbit ? -1.f : 1.f) * bmflip;
    unsigned int hard;
    modem_demodulate_soft(m, s * flip, &hard, soft);
    for (int j = 0; j < arity; j++) deinterleaver_push(di, soft[j]);
  }
  int vin_len = (p->code_rate == 4) ? num_encoded_bits / 2 : num_encoded_bits;
  uint8_t *vin = g_new(uint8_t, vin_len);
  if (p->code_rate == 4) {
    for (int i = 0; i < vin_len; i++) {
      uint8_t a = deinterleaver_pop(di), b = deinterleaver_pop(di);
      vin[i] = (uint8_t)((a & b) + ((a ^ b) >> 1));   // average without overflow
    }
  } else {
    for (int i = 0; i < vin_len; i++) vin[i] = deinterleaver_pop(di);
  }
  int K = vin_len / 2;   // decoded bits (incl. the 6 conv tail bits)
  hfdl_fec_viterbi_decode(vin, K, out);
  g_free(vin);
  modem_destroy(m);
  hfdl_descrambler_destroy(ds);
  deinterleaver_destroy(di);
  return K;
}

/* ---------------- RF framer state machine ---------------- */

#define HFDL_M2_LEN   15
#define HFDL_T_LEN    15
#define HFDL_A2_THRESH 0.30f
#define HFDL_MAX_SEARCH_RETRIES 3
#define HFDL_DATA_SYM_MAX (168 * HFDL_DATA_FRAME_LEN)   // double-slot worst case

// framer states (dumphfdl framer_state)
enum { FR_A1 = 1, FR_A2, FR_M1, FR_M2_SKIP, FR_EQ_TRAIN, FR_DATA_1, FR_DATA_2 };
// sampler states (what happens to each recovered symbol)
enum { SMP_BITS = 1, SMP_SYMBOLS, SMP_SKIP };

typedef struct hfdl_framer {
  hfdl_preamble *pre;      // A correlator (own bit window)
  hfdl_mode     *mode;     // M1 correlator (own bit window, fed the same bits)
  modem          bpsk;     // search-symbol slicer
  int state, sampler;
  int bitmask;             // BPSK polarity resolved by the A1 peak (0/1)
  int symbols_wanted;
  int m_shift;             // detected config (-1 until M1)
  int data_arity, data_segment_cnt, eq_train_seq_cnt, search_retries;
  float last_corr;         // A correlation from the most recent search bit
  gboolean collecting_data;
  float *data_sym;         // collected data symbols (interleaved I/Q)
  int data_n;
  uint8_t frame_bytes[HFDL_DATA_SYM_MAX];  // decoded output (K/8 <= this)
  int frame_nbytes;
} hfdl_framer;

static void framer_reset(hfdl_framer *f) {
  f->state = FR_A1;
  f->sampler = SMP_BITS;
  f->symbols_wanted = 1;
  f->search_retries = 0;
  f->m_shift = -1;
  f->data_n = 0;
  f->collecting_data = FALSE;
}

hfdl_framer *hfdl_framer_create(void) {
  hfdl_framer *f = g_new0(hfdl_framer, 1);
  f->pre = hfdl_preamble_create();
  f->mode = hfdl_mode_create();
  f->bpsk = modem_create(LIQUID_MODEM_BPSK);
  f->data_sym = g_new0(float, 2 * HFDL_DATA_SYM_MAX);
  framer_reset(f);
  return f;
}

void hfdl_framer_destroy(hfdl_framer *f) {
  if (f == NULL) return;
  hfdl_preamble_destroy(f->pre);
  hfdl_mode_destroy(f->mode);
  modem_destroy(f->bpsk);
  g_free(f->data_sym);
  g_free(f);
}

// Feed one carrier-recovered symbol. Returns the decoded byte count (>0) when a
// full frame completes (bytes in f->frame_bytes), else 0. Faithful port of
// dumphfdl's per-symbol framer loop, minus the LMS equalizer (a clean channel
// needs none; it's a real-fading refinement verified only on-air).
int hfdl_framer_push(hfdl_framer *f, float re, float im) {
  float complex s = re + im * I;

  if (f->sampler == SMP_BITS) {
    unsigned int bit;
    modem_demodulate(f->bpsk, s, &bit);
    bit ^= (unsigned int)(f->bitmask & 1);
    f->last_corr = hfdl_preamble_push(f->pre, (int)bit);
    hfdl_mode_push(f->mode, (int)bit);
  } else if (f->sampler == SMP_SYMBOLS && f->collecting_data) {
    if (f->data_n < HFDL_DATA_SYM_MAX) {
      f->data_sym[2 * f->data_n]     = re;
      f->data_sym[2 * f->data_n + 1] = im;
      f->data_n++;
    }
  }
  // Symbols are consumed until the slot boundary; the switch runs only then.
  if (f->symbols_wanted > 1) { f->symbols_wanted--; return 0; }

  switch (f->state) {
  case FR_A1:
    if (fabsf(f->last_corr) > HFDL_CORR_THRESHOLD_A1) {
      f->bitmask = f->last_corr > 0.f ? 0 : 1;   // sign resolves BPSK 180°
      f->symbols_wanted = HFDL_A_LEN;
      f->state = FR_A2;
    }
    break;
  case FR_A2:
    if (fabsf(f->last_corr) > HFDL_A2_THRESH) {
      f->symbols_wanted = HFDL_M1_LEN;
      f->search_retries = 0;
      f->state = FR_M1;
    } else if (++f->search_retries >= HFDL_MAX_SEARCH_RETRIES) {
      framer_reset(f);
    }
    break;
  case FR_M1: {
    float mc; int mk = hfdl_mode_match(f->mode, &mc);
    if (mk >= 0 && mc > HFDL_CORR_THRESHOLD_M1) {
      f->m_shift = mk;
      f->data_segment_cnt = hfdl_frame_params[mk].data_segment_cnt;
      f->data_arity = hfdl_frame_params[mk].scheme;
      f->symbols_wanted = HFDL_M2_LEN;
      f->state = FR_M2_SKIP;
      f->sampler = SMP_SKIP;
    } else {
      framer_reset(f);
    }
    break;
  }
  case FR_M2_SKIP:
    f->symbols_wanted = HFDL_T_LEN;
    f->eq_train_seq_cnt = 9;
    f->state = FR_EQ_TRAIN;
    f->sampler = SMP_SYMBOLS;
    f->collecting_data = FALSE;   // training symbols (ignored — no equalizer)
    break;
  case FR_EQ_TRAIN:
    if (f->eq_train_seq_cnt > 1) {
      f->eq_train_seq_cnt--;
      f->symbols_wanted = HFDL_T_LEN;
    } else if (f->data_segment_cnt > 0) {
      f->symbols_wanted = HFDL_DATA_FRAME_LEN / 2;
      f->state = FR_DATA_1;
      f->collecting_data = TRUE;
    } else {
      // end of frame: decode the collected data symbols.
      int K = hfdl_frame_decode_data(f->data_sym, f->data_n, f->m_shift,
                                     f->bitmask, f->frame_bytes);
      framer_reset(f);
      f->frame_nbytes = (K > 0) ? (K + 7) / 8 : 0;
      return f->frame_nbytes;
    }
    break;
  case FR_DATA_1:
    f->symbols_wanted = HFDL_DATA_FRAME_LEN / 2;
    f->state = FR_DATA_2;
    break;
  case FR_DATA_2:
    f->data_segment_cnt--;
    f->symbols_wanted = HFDL_T_LEN;
    f->state = FR_EQ_TRAIN;
    f->eq_train_seq_cnt = 1;
    f->collecting_data = FALSE;
    break;
  }
  return 0;
}

const uint8_t *hfdl_framer_bytes(hfdl_framer *f, int *nbytes) {
  if (nbytes) *nbytes = f ? f->frame_nbytes : 0;
  return f ? f->frame_bytes : NULL;
}

/* ---------------- Self-test ---------------- */

// Deinterleaver permutation: perm[i] = pop-position of the value pushed i-th.
// O(N^2) probe — test/encode only (the live decode uses push/pop directly).
static void frame_perm(int m_shift, int *perm, int N) {
  deinterleaver *d = deinterleaver_create(m_shift);
  for (int k = 0; k < N; k++) {
    deinterleaver_reset(d);
    for (int i = 0; i < DEINTERLEAVER_ROW_CNT; i++) memset(d->table[i], 0, d->column_cnt);
    for (int i = 0; i < N; i++) deinterleaver_push(d, i == k ? 1 : 0);
    deinterleaver_reset(d);
    perm[k] = -1;
    for (int i = 0; i < N; i++) if (deinterleaver_pop(d)) { perm[k] = i; break; }
  }
  deinterleaver_destroy(d);
}

// A valid block deinterleaver is a bijection: perm covers 0..N-1 exactly once.
static gboolean deinterleaver_is_bijection(int m_shift) {
  deinterleaver *d = deinterleaver_create(m_shift);
  int N = deinterleaver_table_size(d);
  deinterleaver_destroy(d);
  int *perm = g_new(int, N);
  int *seen = g_new0(int, N);
  frame_perm(m_shift, perm, N);
  gboolean ok = TRUE;
  for (int i = 0; i < N; i++) {
    if (perm[i] < 0 || perm[i] >= N || seen[perm[i]]) { ok = FALSE; break; }
    seen[perm[i]] = 1;
  }
  g_free(perm); g_free(seen);
  return ok;
}

// r=1/2 K=7 convolutional encoder (Karn convention; matches hfdl_fec's decoder),
// appending 6 tail zero bits. out_bits holds 2*(ndata+6) hard bits (0/1).
static void frame_conv_encode(const uint8_t *data_bits, int ndata, uint8_t *out_bits) {
  unsigned int sr = 0;
  int total = ndata + 6;
  for (int i = 0; i < total; i++) {
    int b = (i < ndata) ? (data_bits[i] & 1) : 0;
    sr = (sr << 1) | (unsigned int)b;
    unsigned int a = sr & 0x6d, c = sr & 0x4f;
    a ^= a >> 4; a ^= a >> 2; a ^= a >> 1;
    c ^= c >> 4; c ^= c >> 2; c ^= c >> 1;
    out_bits[2 * i]     = (uint8_t)(a & 1);
    out_bits[2 * i + 1] = (uint8_t)(c & 1);
  }
}

// End-to-end data-frame round-trip for a BPSK config (arity 1): random user
// bits → conv-encode(+tail) → interleave (inverse of the deinterleaver perm) →
// per-symbol scramble + BPSK-modulate → hfdl_frame_decode_data → assert the
// user bits come back bit-exact. Exercises the whole data path (interleave +
// scramble + M-PSK + Viterbi) as one self-consistent chain.
static gboolean dataframe_roundtrip_bpsk(int m_shift) {
  const hfdl_params *p = &hfdl_frame_params[m_shift];
  if (p->scheme != HFDL_M_BPSK || p->code_rate != 2) return TRUE;   // BPSK r=1/2 only
  int nsym = p->data_segment_cnt * HFDL_DATA_FRAME_LEN;   // arity 1 -> pushes = nsym
  int N = nsym;                                           // encoded bits = table size
  int K = N / 2;                                          // decoded bits (incl tail)
  int ndata = K - 6;

  uint8_t *dbits = g_new(uint8_t, ndata);
  uint8_t *enc = g_new(uint8_t, N);
  int *perm = g_new(int, N);
  float *sym = g_new(float, 2 * nsym);
  uint8_t *out = g_new0(uint8_t, (K + 7) / 8);

  uint32_t rng = 0x2468aceu;
  for (int i = 0; i < ndata; i++) { rng = rng * 1103515245u + 12345u; dbits[i] = (rng >> 19) & 1u; }
  frame_conv_encode(dbits, ndata, enc);
  frame_perm(m_shift, perm, N);

  // TX: pushed[i] (soft bit for symbol i) = enc[perm[i]] so the RX pop yields enc
  // in order; scramble + BPSK-modulate so RX descramble+demod recovers it.
  modem mtx = modem_create(LIQUID_MODEM_BPSK);
  descrambler *dtx = hfdl_descrambler_create();
  for (int i = 0; i < nsym; i++) {
    unsigned int b = enc[perm[i]];
    float complex s; modem_modulate(mtx, b, &s);
    uint32_t dbit = descrambler_advance(dtx);
    if (dbit) s = -s;                                     // pre-apply scramble flip
    sym[2 * i] = crealf(s); sym[2 * i + 1] = cimagf(s);
  }
  modem_destroy(mtx); hfdl_descrambler_destroy(dtx);

  int Kout = hfdl_frame_decode_data(sym, nsym, m_shift, 0, out);
  gboolean ok = (Kout == K);
  int errs = 0;
  if (ok) for (int i = 0; i < ndata; i++) {
    int rb = (out[i >> 3] >> (7 - (i & 7))) & 1;
    if (rb != dbits[i]) errs++;
  }
  if (!ok || errs != 0) {
    log_error("hfdl_frame selftest: data-frame[%d] round-trip FAIL (Kout=%d/%d, %d/%d bit errors)\n",
              m_shift, Kout, K, errs, ndata);
    ok = FALSE;
  }
  g_free(dbits); g_free(enc); g_free(perm); g_free(sym); g_free(out);
  return ok;
}

// Feed one symbol to the framer, latching a completed frame's bytes.
static void framer_feed(hfdl_framer *f, float re, float im,
                        int *got, const uint8_t **fb, int *fn) {
  int r = hfdl_framer_push(f, re, im);
  if (r > 0) { *got = r; *fb = hfdl_framer_bytes(f, fn); }
}

// Full-frame end-to-end: synthesize a complete baseband HFDL frame (prekey + A +
// A + M1 + M2 + 9 training + [data + training] per segment) carrying known user
// bytes, feed the symbols to the framer, and assert it syncs, selects the mode,
// collects the data and decodes back to the same bytes. BPSK r=1/2 configs.
static gboolean framer_e2e_bpsk(int m_shift) {
  const hfdl_params *p = &hfdl_frame_params[m_shift];
  if (p->scheme != HFDL_M_BPSK || p->code_rate != 2) return TRUE;
  int nsym = p->data_segment_cnt * HFDL_DATA_FRAME_LEN;
  int N = nsym, K = N / 2, ndata = K - 6;

  uint8_t *dbits = g_new(uint8_t, ndata);
  uint8_t *enc = g_new(uint8_t, N);
  int *perm = g_new(int, N);
  float *dsym = g_new(float, 2 * nsym);
  uint32_t rng = 0x13572468u;
  for (int i = 0; i < ndata; i++) { rng = rng * 1103515245u + 12345u; dbits[i] = (rng >> 19) & 1u; }
  frame_conv_encode(dbits, ndata, enc);
  frame_perm(m_shift, perm, N);

  modem mm = modem_create(LIQUID_MODEM_BPSK);
  descrambler *dtx = hfdl_descrambler_create();
  for (int i = 0; i < nsym; i++) {
    unsigned int b = enc[perm[i]];
    float complex s; modem_modulate(mm, b, &s);
    if (descrambler_advance(dtx)) s = -s;
    dsym[2 * i] = crealf(s); dsym[2 * i + 1] = cimagf(s);
  }
  hfdl_descrambler_destroy(dtx);

  hfdl_framer *f = hfdl_framer_create();
  int got = 0, fn = 0; const uint8_t *fb = NULL;

  // Sync fields are EMIT_BITS (demodulated + correlated) — BPSK-modulate each bit.
  float complex bs;
  for (int i = 0; i < 140; i++) { modem_modulate(mm, 0, &bs); framer_feed(f, crealf(bs), cimagf(bs), &got, &fb, &fn); }  // prekey
  for (int r = 0; r < 2; r++)                                          // A + A
    for (int j = 0; j < HFDL_A_LEN; j++) {
      modem_modulate(mm, (hfdl_A_octets[j >> 3] >> (7 - (j & 7))) & 1, &bs);
      framer_feed(f, crealf(bs), cimagf(bs), &got, &fb, &fn);
    }
  for (int j = 0; j < HFDL_M1_LEN; j++) {                              // M1
    modem_modulate(mm, hfdl_M1_bits[(hfdl_M_shifts[m_shift] + j) % HFDL_M1_LEN], &bs);
    framer_feed(f, crealf(bs), cimagf(bs), &got, &fb, &fn);
  }
  // From M2 on the framer collects raw symbols (no demod): M2 skipped, T ignored,
  // data collected. Push M2(15) + 9 T(15) then [30 data + 15 T] per segment.
  for (int i = 0; i < HFDL_M2_LEN; i++)          framer_feed(f, 0.f, 0.f, &got, &fb, &fn);
  for (int i = 0; i < 9 * HFDL_T_LEN; i++)       framer_feed(f, 0.f, 0.f, &got, &fb, &fn);
  int di = 0;
  for (int seg = 0; seg < p->data_segment_cnt; seg++) {
    for (int i = 0; i < HFDL_DATA_FRAME_LEN; i++, di++)
      framer_feed(f, dsym[2 * di], dsym[2 * di + 1], &got, &fb, &fn);
    for (int i = 0; i < HFDL_T_LEN; i++)         framer_feed(f, 0.f, 0.f, &got, &fb, &fn);
  }
  modem_destroy(mm);

  gboolean ok = TRUE;
  int errs = 0;
  if (got <= 0 || fb == NULL) {
    log_error("hfdl_frame selftest: framer[%d] produced no frame\n", m_shift);
    ok = FALSE;
  } else {
    for (int i = 0; i < ndata; i++) {
      int rb = (fb[i >> 3] >> (7 - (i & 7))) & 1;
      if (rb != dbits[i]) errs++;
    }
    if (errs != 0) {
      log_error("hfdl_frame selftest: framer[%d] decoded %d/%d bit errors\n", m_shift, errs, ndata);
      ok = FALSE;
    }
  }
  hfdl_framer_destroy(f);
  g_free(dbits); g_free(enc); g_free(perm); g_free(dsym);
  return ok;
}

gboolean hfdl_frame_selftest(void) {
  gboolean ok = TRUE;

  // (1) Deinterleaver bijection over a single-slot (m=1) and a double-slot (m=6)
  // config (different column_cnt + push shift).
  if (!deinterleaver_is_bijection(1)) {
    log_error("hfdl_frame selftest: deinterleaver[1] not a bijection\n"); ok = FALSE;
  }
  if (!deinterleaver_is_bijection(6)) {
    log_error("hfdl_frame selftest: deinterleaver[6] not a bijection\n"); ok = FALSE;
  }

  // (2) Descrambler: 120-bit periodicity + determinism + not stuck.
  descrambler *d = hfdl_descrambler_create();
  uint8_t a[120], b[120];
  int ones = 0;
  for (int i = 0; i < 120; i++) { a[i] = (uint8_t)descrambler_advance(d); ones += a[i]; }
  for (int i = 0; i < 120; i++) b[i] = (uint8_t)descrambler_advance(d);
  if (memcmp(a, b, 120) != 0) {
    log_error("hfdl_frame selftest: descrambler not 120-periodic\n"); ok = FALSE;
  }
  if (ones == 0 || ones == 120) {
    log_error("hfdl_frame selftest: descrambler stuck (%d/120 ones)\n", ones); ok = FALSE;
  }
  hfdl_descrambler_destroy(d);

  // (3) Preamble correlator: feed a random run (no false peak above threshold),
  // then the A-sequence (a sharp peak at |corr|=1 when the window fills), then
  // the inverted A-sequence (peak of the opposite sign — the polarity that
  // resolves BPSK's 180° ambiguity).
  int abits[HFDL_A_LEN];
  for (int i = 0; i < HFDL_A_LEN; i++)
    abits[i] = (hfdl_A_octets[i >> 3] >> (7 - (i & 7))) & 1;   // MSB-first

  hfdl_preamble *pr = hfdl_preamble_create();
  uint32_t rng = 0x0badf00du;
  float max_noise = 0.f;
  for (int i = 0; i < 300; i++) {
    rng = rng * 1103515245u + 12345u;
    float c = hfdl_preamble_push(pr, (int)((rng >> 20) & 1u));
    if (fabsf(c) > max_noise) max_noise = fabsf(c);
  }
  if (max_noise >= HFDL_CORR_THRESHOLD_A1) {
    log_error("hfdl_frame selftest: preamble false-triggered on noise (max |corr|=%.3f)\n", max_noise);
    ok = FALSE;
  }
  float peak = 0.f;
  for (int i = 0; i < HFDL_A_LEN; i++) peak = hfdl_preamble_push(pr, abits[i]);
  if (peak < 0.99f) {
    log_error("hfdl_frame selftest: A-sequence peak too low (corr=%.3f)\n", peak);
    ok = FALSE;
  }
  float peak_inv = 0.f;
  for (int i = 0; i < HFDL_A_LEN; i++) peak_inv = hfdl_preamble_push(pr, abits[i] ^ 1);
  if (peak_inv > -0.99f) {
    log_error("hfdl_frame selftest: inverted A-sequence peak wrong (corr=%.3f)\n", peak_inv);
    ok = FALSE;
  }
  hfdl_preamble_destroy(pr);

  // (4) M1 mode correlator: feed the exact bits of shift k; hfdl_mode_match must
  // return k with a full peak, and not mis-pick another shift.
  hfdl_mode *mo = hfdl_mode_create();
  for (int test_k = 0; test_k < HFDL_M_SHIFT_CNT; test_k++) {
    for (int j = 0; j < HFDL_M1_LEN; j++)
      hfdl_mode_push(mo, hfdl_M1_bits[(hfdl_M_shifts[test_k] + j) % HFDL_M1_LEN]);
    float mc; int mk = hfdl_mode_match(mo, &mc);
    if (mk != test_k || mc < 0.99f) {
      log_error("hfdl_frame selftest: M1 mode match failed (shift %d -> %d, corr %.3f)\n",
                test_k, mk, mc);
      ok = FALSE;
    }
  }
  hfdl_mode_destroy(mo);

  // (5) Whole data-path round-trip (interleave + scramble + BPSK + Viterbi) for a
  // single-slot (m=1) and a double-slot (m=5) BPSK r=1/2 config.
  if (!dataframe_roundtrip_bpsk(1)) ok = FALSE;
  if (!dataframe_roundtrip_bpsk(5)) ok = FALSE;

  // (6) Full-frame end-to-end through the RF framer (sync + mode + collect +
  // decode) on a synthesized baseband frame.
  if (!framer_e2e_bpsk(1)) ok = FALSE;
  if (!framer_e2e_bpsk(5)) ok = FALSE;

  if (ok)
    log_info("hfdl_frame selftest: PASS (correlators %.2f/%.2f + M1 mode + data round-trip + full-frame framer)\n",
             peak, peak_inv);
  return ok;
}
