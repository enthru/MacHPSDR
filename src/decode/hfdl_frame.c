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
#include <liquid/liquid.h>

#include "hfdl_frame.h"
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

/* ---------------- Self-test ---------------- */

// A block deinterleaver must be a bijection over its table: each of the N input
// (push) positions maps to a distinct output (pop) position, covering all N.
// Probe one input at a time (a lone 1 among 0s) and record where it pops out; a
// collision or a gap means the row/col arithmetic is wrong.
static gboolean deinterleaver_is_bijection(int m_shift) {
  deinterleaver *d = deinterleaver_create(m_shift);
  int N = deinterleaver_table_size(d);
  gboolean ok = TRUE;
  int *seen = g_new0(int, N);          // output-position hit count
  uint8_t *out = g_new0(uint8_t, N);
  for (int k = 0; k < N && ok; k++) {
    deinterleaver_reset(d);
    for (int i = 0; i < DEINTERLEAVER_ROW_CNT; i++)
      memset(d->table[i], 0, d->column_cnt);
    for (int i = 0; i < N; i++) deinterleaver_push(d, i == k ? 1 : 0);
    deinterleaver_reset(d);
    int at = -1, hits = 0;
    for (int i = 0; i < N; i++) { out[i] = deinterleaver_pop(d); if (out[i]) { at = i; hits++; } }
    if (hits != 1) { ok = FALSE; break; }   // marker lost or duplicated
    seen[at]++;
  }
  if (ok) for (int i = 0; i < N; i++) if (seen[i] != 1) { ok = FALSE; break; }
  g_free(seen); g_free(out);
  deinterleaver_destroy(d);
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

  if (ok)
    log_info("hfdl_frame selftest: PASS (deinterleaver bijection + descrambler 120-periodic)\n");
  return ok;
}
