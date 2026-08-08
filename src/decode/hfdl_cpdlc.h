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
 * FANS-1/A CPDLC — controller-pilot data link, the ATC conversation itself.
 *
 * This is the last of the ARINC-622 applications to be decoded. It sits behind
 * the IMIs `.AT1` / `.CR1` / `.CC1` / `.DR1` that `hfdl_arinc.c` already
 * recognises, and unlike ADS-C it is not a fixed-length tag walk: the payload
 * is ASN.1 in unaligned PER, so it needs the generated FANS-1/A type tree.
 * That tree is vendored under `hfdl_lib/asn1/` (libacars' `libacars/asn1/`,
 * itself asn1c output plus the asn1c runtime — 490 files that compile as-is and
 * are never edited by hand). `hfdl_asn1.c` is the rendering layer over it, and
 * this file is the message-element dictionaries and per-type formatter table,
 * ported from libacars' `asn1-format-cpdlc-text.c`.
 *
 * Direction decides everything: the same octets mean different things as an
 * uplink (ATCUplinkMessage — clearances and instructions) and as a downlink
 * (ATCDownlinkMessage — requests and reports), so the caller must say which.
 */

#ifndef _HFDL_CPDLC_H
#define _HFDL_CPDLC_H

#include <glib.h>
#include <stdint.h>

// Decode `len` octets of ARINC-622 CPDLC payload (the binary body, with its
// trailing ARINC CRC already stripped) and append the rendered message to
// `out` at `indent`. `uplink` is TRUE for a ground-to-air message. Returns
// FALSE only when the payload would not decode at all — the reason is reported
// either way, since by this point the IMI has already said it is CPDLC.
gboolean hfdl_cpdlc_decode(const uint8_t *buf, int len, gboolean uplink,
                           GString *out, int indent);

// Headless self-test: decodes real CPDLC messages taken from libacars' CI and
// usage examples — deliberately from outside this codebase — and asserts the
// rendered text, then re-encodes one of them through the PER encoder and
// requires the original octets back.
gboolean hfdl_cpdlc_selftest(void);

#endif
