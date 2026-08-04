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
 * HFDL PDU header validation + description. See hfdl_pdu.h. FCS + header parse
 * ported from dumphfdl (pdu.c / mpdu.c / spdu.c); crc16_ccitt is vendored under
 * hfdl_lib/hfdl_crc.c.
 */

#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "hfdl_crc.h"      // vendored crc16_ccitt (hfdl_lib/)
#include "hfdl_pdu.h"

#define SPDU_LEN 66        // dumphfdl SPDU_LEN
#define SPDU_HDR 64        // FCS covers the first 64 bytes of an SPDU

// FCS = CRC-16-CCITT over the first hdr_len bytes, stored little-endian at
// buf[hdr_len..hdr_len+1] (dumphfdl hfdl_pdu_fcs_check).
gboolean hfdl_pdu_fcs_check(const uint8_t *buf, int hdr_len) {
  uint16_t check = (uint16_t)(buf[hdr_len] | (buf[hdr_len + 1] << 8));
  uint16_t comp  = (uint16_t)(crc16_ccitt((uint8_t *)buf, (uint32_t)hdr_len, 0xFFFFu) ^ 0xFFFFu);
  return check == comp;
}
#define fcs_ok hfdl_pdu_fcs_check

gboolean hfdl_pdu_describe(const uint8_t *buf, int len, char *out, int outlen) {
  if (out == NULL || outlen <= 0) return FALSE;
  if (buf == NULL || len < 3) { g_strlcpy(out, "HFDL frame: too short", outlen); return FALSE; }

  if ((buf[0] & 1) == 0) {
    // SPDU — ground-station squitter.
    if (len < SPDU_LEN) { g_snprintf(out, outlen, "HFDL SPDU: too short (%d)", len); return FALSE; }
    gboolean ok = fcs_ok(buf, SPDU_HDR);
    g_snprintf(out, outlen, "HFDL SPDU (ground-station squitter)  FCS %s",
               ok ? "OK" : "FAIL");
    return ok;
  }

  // MPDU — determine the header length, then FCS-check it.
  int hdr_len;
  int downlink = (buf[0] & 0x2) != 0;
  if (downlink) {
    int lpdu_cnt = (buf[0] >> 2) & 0xF;
    hdr_len = 6 + lpdu_cnt;
    if (len < hdr_len + 2) { g_snprintf(out, outlen, "HFDL MPDU: too short (%d)", len); return FALSE; }
    gboolean ok = fcs_ok(buf, hdr_len);
    g_snprintf(out, outlen, "HFDL MPDU downlink (air->gnd)  AC=0x%02x GS=%u  %d LPDU  FCS %s",
               buf[2], buf[1] & 0x7f, lpdu_cnt, ok ? "OK" : "FAIL");
    return ok;
  } else {
    int aircraft_cnt = ((buf[0] & 0x70) >> 4) + 1;
    hdr_len = 2;
    for (int i = 0; i < aircraft_cnt; i++) {
      if (len < hdr_len + 2) { g_snprintf(out, outlen, "HFDL MPDU uplink: too short (%d)", len); return FALSE; }
      int lpdu_cnt = buf[hdr_len + 1] >> 4;
      hdr_len += 2 + lpdu_cnt;
    }
    if (len < hdr_len + 2) { g_snprintf(out, outlen, "HFDL MPDU uplink: too short (%d)", len); return FALSE; }
    gboolean ok = fcs_ok(buf, hdr_len);
    g_snprintf(out, outlen, "HFDL MPDU uplink (gnd->air)  GS=%u  %d aircraft  FCS %s",
               buf[1] & 0x7f, aircraft_cnt, ok ? "OK" : "FAIL");
    return ok;
  }
}

// Append the little-endian FCS over the first hdr_len bytes (inverse of fcs_ok),
// so the synthetic frame validates. Test only.
static void fcs_append(uint8_t *buf, int hdr_len) {
  uint16_t comp = (uint16_t)(crc16_ccitt(buf, (uint32_t)hdr_len, 0xFFFFu) ^ 0xFFFFu);
  buf[hdr_len]     = (uint8_t)(comp & 0xff);
  buf[hdr_len + 1] = (uint8_t)(comp >> 8);
}

gboolean hfdl_pdu_selftest(void) {
  gboolean ok = TRUE;
  char desc[128];

  // (1) SPDU: 66 bytes, bit0=0, FCS over the first 64.
  uint8_t spdu[SPDU_LEN];
  uint32_t rng = 0x77aa33ffu;
  for (int i = 0; i < SPDU_LEN; i++) { rng = rng * 1103515245u + 12345u; spdu[i] = (uint8_t)(rng >> 16); }
  spdu[0] &= ~1u;                       // mark as SPDU
  fcs_append(spdu, SPDU_HDR);
  if (!hfdl_pdu_describe(spdu, SPDU_LEN, desc, sizeof(desc))) ok = FALSE;  // valid must pass
  spdu[5] ^= 0xff;                       // corrupt the header -> FCS must fail
  if (hfdl_pdu_describe(spdu, SPDU_LEN, desc, sizeof(desc))) ok = FALSE;

  // (2) MPDU downlink: bit0=1, bit1=1, lpdu_cnt in bits 2..5. hdr_len = 6+lpdu_cnt.
  int lpdu_cnt = 2;
  uint8_t mpdu[32];
  for (int i = 0; i < (int)sizeof(mpdu); i++) { rng = rng * 1103515245u + 12345u; mpdu[i] = (uint8_t)(rng >> 16); }
  mpdu[0] = (uint8_t)(0x01 | 0x02 | (lpdu_cnt << 2));   // MPDU + downlink + lpdu_cnt
  int hdr = 6 + lpdu_cnt;
  fcs_append(mpdu, hdr);
  if (!hfdl_pdu_describe(mpdu, hdr + 2, desc, sizeof(desc))) ok = FALSE;
  mpdu[3] ^= 0xff;                       // corrupt -> FCS must fail
  if (hfdl_pdu_describe(mpdu, hdr + 2, desc, sizeof(desc))) ok = FALSE;

  return ok;
}
