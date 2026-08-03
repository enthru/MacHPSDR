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
 * HFDL framing primitives — parity 4.5 phase 3.
 *
 * Ported from dumphfdl (GPL-3.0) src/hfdl.c: the block deinterleaver and the
 * LFSR descrambler that sit between the M-PSK symbol demodulation and the
 * Viterbi FEC (hfdl_fec.c). Both are exact ports of dumphfdl's constants
 * (40-row deinterleaver with per-mode column shift; a liquid `msequence`
 * descrambler, 15-bit, 120-bit period). The full frame state machine (preamble
 * correlation, equalizer training, modulation selection, CRC) is built up on
 * top of these.
 *
 * Verified offline (no over-the-air signal): the deinterleaver is checked to be
 * a true bijection (a lossless permutation — catches any index-arithmetic bug),
 * and the descrambler is checked for its 120-bit periodicity + determinism. The
 * exact match to the HFDL spec is confirmed later against a reference frame /
 * dumphfdl-as-oracle.
 */

#ifndef _HFDL_FRAME_H
#define _HFDL_FRAME_H

#include <glib.h>

// HFDL modulation arities (bits/symbol), indexing hfdl_frame_params[].scheme.
enum { HFDL_M_BPSK = 1, HFDL_M_PSK4 = 2, HFDL_M_PSK8 = 3 };

#define HFDL_M_SHIFT_CNT   8    // number of frame (rate/slot) configurations
#define HFDL_DATA_FRAME_LEN 30  // data symbols per frame segment

// One frame configuration (300..1800 bps, single/double slot). Exact copy of
// dumphfdl's hfdl_frame_params[].
typedef struct {
  int scheme;                     // HFDL_M_BPSK / PSK4 / PSK8
  int data_segment_cnt;           // segments per slot (72 single, 168 double)
  int code_rate;                  // 2 (r=1/2) or 4 (r=1/4)
  int deinterleaver_push_column_shift;
} hfdl_params;

extern const hfdl_params hfdl_frame_params[HFDL_M_SHIFT_CNT];

// Headless self-test: deinterleaver bijection (over a few configs) + descrambler
// periodicity/determinism. Returns TRUE on pass. No GTK/RADIO deps.
gboolean hfdl_frame_selftest(void);

#endif
