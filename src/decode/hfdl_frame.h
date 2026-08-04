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
#include <stdint.h>

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

// RF framer state machine: consumes carrier-recovered symbols (one per HFDL
// symbol), detects the A-preamble + M1 config, collects the data symbols and
// decodes them to frame bytes. Opaque; created per receiver.
typedef struct hfdl_framer hfdl_framer;
hfdl_framer *hfdl_framer_create(void);
void hfdl_framer_destroy(hfdl_framer *f);

// Feed one recovered symbol (interleaved I/Q). Returns the decoded byte count
// (>0) when a full frame completes — the bytes are then available from
// hfdl_framer_bytes() — else 0.
int hfdl_framer_push(hfdl_framer *f, float re, float im);

// Bytes of the frame most recently completed by hfdl_framer_push() (valid only
// on the call that returned >0). *nbytes gets the length.
const uint8_t *hfdl_framer_bytes(hfdl_framer *f, int *nbytes);

// Modulation arity the NEXT symbol will carry: 1 (BPSK) everywhere except the
// data segments, which use the frame's own modulation. The caller feeds this to
// hfdl_demod_set_slicer() after every symbol so the decision-directed carrier
// loop slices against what is actually being sent — without it, PSK4/PSK8
// frames are demodulated with a BPSK phase error and the carrier is dragged off.
int hfdl_framer_arity(const hfdl_framer *f);

// Headless self-test: deinterleaver bijection + descrambler periodicity +
// preamble/M1 correlators + whole data-path round-trip + full-frame end-to-end
// through the framer. Returns TRUE on pass. No GTK/RADIO deps.
gboolean hfdl_frame_selftest(void);

// --- test seam (self-tests only) -------------------------------------------
// Payload bits a BPSK r=1/2 frame of this modulation/slot config can carry, or
// 0 if the config isn't BPSK r=1/2.
int hfdl_frame_test_capacity(int m_shift);

// Synthesize a complete baseband frame carrying `bits` (zero-padded to the
// config's capacity), optionally through a 2-tap multipath channel `echo`, run
// it through a framer and return the decoded bytes in *out_bytes (caller frees
// with g_free) plus their count — 0 if the framer produced no frame. Lets a
// caller assert the whole chain, bits to symbols to framer to bytes.
int hfdl_frame_test_roundtrip(int m_shift, float echo, const uint8_t *bits, int nbits,
                              uint8_t **out_bytes);

#endif
