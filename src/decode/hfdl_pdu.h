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
 * HFDL PDU header validation + description (parity 4.5 phase 3).
 *
 * The raw frame bytes from the framer are an HFDL PDU: either an SPDU (ground-
 * station squitter) or an MPDU (media-access PDU carrying link data), told apart
 * by bit 0 of the first byte (dumphfdl IS_MPDU). Each is protected by a CRC-16-
 * CCITT FCS over its header. This module ports dumphfdl's FCS check + the MPDU/
 * SPDU header parse (direction, ground-station / aircraft ids) so the decoder
 * can validate a decoded frame (reject garbage) and show a one-line summary —
 * WITHOUT the full libacars application layer (ACARS/CPDLC/ADS-C message text),
 * which is the remaining, real-signal-dependent piece.
 */

#ifndef _HFDL_PDU_H
#define _HFDL_PDU_H

#include <glib.h>
#include <stdint.h>

// Validate + describe a decoded HFDL frame. Writes a one-line human-readable
// summary into out (NUL-terminated). Returns TRUE if the frame's FCS checks out
// (a real, uncorrupted PDU), FALSE otherwise. GTK-independent.
gboolean hfdl_pdu_describe(const uint8_t *buf, int len, char *out, int outlen);

// FCS check: CRC-16-CCITT over the first hdr_len bytes against the little-endian
// FCS stored at buf[hdr_len..hdr_len+1] (dumphfdl hfdl_pdu_fcs_check). Every PDU
// layer — SPDU, MPDU header, LPDU — is protected the same way, so hfdl_msg.c
// uses this too.
gboolean hfdl_pdu_fcs_check(const uint8_t *buf, int hdr_len);

// Headless self-test: build a valid SPDU and a valid MPDU (correct FCS), confirm
// they validate + describe; corrupt a byte and confirm the FCS check fails.
gboolean hfdl_pdu_selftest(void);

#endif
