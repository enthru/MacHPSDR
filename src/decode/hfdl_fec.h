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
 * HFDL FEC — thin wrapper over the vendored libfec (Phil Karn KA9Q, LGPL) r=1/2
 * K=7 Viterbi decoder (hfdl_lib/libfec/), parity 4.5 phase 3.
 *
 * HFDL data is protected by an r=1/2, K=7 convolutional code (polynomials
 * V27POLYA=0x6d / V27POLYB=0x4f), the same code Karn's viterbi27 decodes. This
 * wraps the decoder for the framer and provides a self-contained round-trip
 * self-test (a matching convolutional encoder → Viterbi decode → bit-exact
 * recovery) so the port is verified offline — no over-the-air signal needed.
 */

#ifndef _HFDL_FEC_H
#define _HFDL_FEC_H

#include <glib.h>
#include <stdint.h>

// Viterbi-decode `nbits_out` bits from `soft` (2*nbits_out soft symbols, 0..255,
// 255 = strong 1), assuming encoder start AND end state 0 (the caller must have
// appended 6 zero tail bits before encoding, so nbits_out includes them). The
// decoded bits are packed MSB-first into `out` (⌈nbits_out/8⌉ bytes). Returns 0
// on success, -1 on error. Not thread-safe against itself (creates/uses a
// per-call decoder context).
int hfdl_fec_viterbi_decode(const uint8_t *soft, int nbits_out, uint8_t *out);

// Headless round-trip self-test: convolutionally encode random data (matching
// Karn's convention) + 6 tail bits, hard-map to soft symbols, Viterbi-decode,
// and assert bit-exact recovery (incl. a run with a few injected bit errors the
// code must correct). Returns TRUE on pass. No GTK/RADIO deps.
gboolean hfdl_fec_selftest(void);

#endif
