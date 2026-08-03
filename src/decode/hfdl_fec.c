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
 * HFDL FEC wrapper + round-trip self-test. See hfdl_fec.h. Uses the vendored
 * libfec Viterbi (hfdl_lib/libfec/fec.h).
 */

#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "fec.h"           // vendored libfec (hfdl_lib/libfec)
#include "hfdl_fec.h"
#include "log.h"

#define HFDL_CONV_POLYA 0x6d   // V27POLYA
#define HFDL_CONV_POLYB 0x4f   // V27POLYB
#define HFDL_CONV_TAIL  6      // K-1 flush bits to terminate at state 0

static inline int parity7(unsigned int x) {
  x &= 0x7f;
  x ^= x >> 4;
  x ^= x >> 2;
  x ^= x >> 1;
  return x & 1;
}

int hfdl_fec_viterbi_decode(const uint8_t *soft, int nbits_out, uint8_t *out) {
  if (soft == NULL || out == NULL || nbits_out <= 0) return -1;
  void *v = create_viterbi27(nbits_out);
  if (v == NULL) return -1;
  init_viterbi27(v, 0);
  update_viterbi27_blk(v, (unsigned char *)soft, nbits_out);
  chainback_viterbi27(v, out, (unsigned int)nbits_out, 0);
  delete_viterbi27(v);
  return 0;
}

// r=1/2 K=7 convolutional encoder matching Karn's viterbi27 (parity over
// sr&POLYA then sr&POLYB, newest bit shifted into the LSB; data MSB-first). Used
// only by the self-test — the live path is decode-only.
static void conv_encode(const uint8_t *data, int nbits, uint8_t *soft_out) {
  unsigned int sr = 0;
  int total = nbits + HFDL_CONV_TAIL;
  for (int i = 0; i < total; i++) {
    int b = (i < nbits) ? ((data[i >> 3] >> (7 - (i & 7))) & 1) : 0;  // tail = 0
    sr = (sr << 1) | (unsigned int)b;
    soft_out[2 * i]     = parity7(sr & HFDL_CONV_POLYA) ? 255 : 0;
    soft_out[2 * i + 1] = parity7(sr & HFDL_CONV_POLYB) ? 255 : 0;
  }
}

gboolean hfdl_fec_selftest(void) {
  enum { NDATA = 400,                       // data bits
         NBITS = NDATA + HFDL_CONV_TAIL };  // decoded bits (incl. tail)
  uint8_t data[(NDATA + 7) / 8];
  uint8_t soft[2 * NBITS];
  uint8_t dec[(NBITS + 7) / 8];

  // deterministic PRBS
  uint32_t lcg = 0x51ed2718u;
  for (unsigned i = 0; i < sizeof(data); i++) {
    lcg = lcg * 1103515245u + 12345u;
    data[i] = (uint8_t)(lcg >> 16);
  }

  conv_encode(data, NDATA, soft);

  gboolean ok = TRUE;

  // (1) clean round-trip: exact recovery of all NDATA bits.
  memset(dec, 0, sizeof(dec));
  hfdl_fec_viterbi_decode(soft, NBITS, dec);
  int errs = 0;
  for (int i = 0; i < NDATA; i++) {
    int tb = (data[i >> 3] >> (7 - (i & 7))) & 1;
    int rb = (dec[i >> 3]  >> (7 - (i & 7))) & 1;
    if (tb != rb) errs++;
  }
  if (errs != 0) {
    log_error("hfdl_fec selftest: clean round-trip FAIL (%d/%d bit errors)\n", errs, NDATA);
    ok = FALSE;
  }

  // (2) error correction: flip a handful of channel bits; the code must still
  // recover the data exactly (well within its correcting capability).
  uint8_t soft2[2 * NBITS];
  memcpy(soft2, soft, sizeof(soft2));
  int flip[] = { 10, 11, 80, 205, 400, 611 };
  for (unsigned k = 0; k < sizeof(flip) / sizeof(flip[0]); k++)
    soft2[flip[k]] = (uint8_t)(255 - soft2[flip[k]]);   // invert that channel symbol
  memset(dec, 0, sizeof(dec));
  hfdl_fec_viterbi_decode(soft2, NBITS, dec);
  int errs2 = 0;
  for (int i = 0; i < NDATA; i++) {
    int tb = (data[i >> 3] >> (7 - (i & 7))) & 1;
    int rb = (dec[i >> 3]  >> (7 - (i & 7))) & 1;
    if (tb != rb) errs2++;
  }
  if (errs2 != 0) {
    log_error("hfdl_fec selftest: error-correction FAIL (%d/%d bit errors after 6 flips)\n", errs2, NDATA);
    ok = FALSE;
  }

  if (ok)
    log_info("hfdl_fec selftest: PASS (Viterbi round-trip clean + corrects 6 bit errors)\n");
  return ok;
}
