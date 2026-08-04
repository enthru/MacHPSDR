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
 * ARINC-622 ATS applications carried inside ACARS message text — the layer that
 * turns "a page of hex" into an aircraft's actual position report.
 *
 * An ARINC-622 message is a ground-station address, an IMI naming the
 * application (.ADS / .DIS for ADS-C, .AT1 / .CR1 / .CC1 / .DR1 for FANS-1/A
 * CPDLC), the aircraft registration, and a hex-encoded binary payload with its
 * own CRC. Port of libacars' arinc.c (envelope) and the parts of adsc.c that
 * decode the downlink reports; the message text still prints raw as well, so
 * nothing is hidden if a parse goes wrong.
 *
 * CPDLC payloads are ASN.1 (PER) and are NOT decoded — libacars needs a
 * generated ASN.1 tree for them, which is the bulk of that library. They are
 * identified and their length reported, and the hex stays visible.
 *
 * Self-contained: GLib + the ACARS text, no GTK/RADIO. Audio-thread only, like
 * the rest of the HFDL application layer.
 */

#ifndef _HFDL_ARINC_H
#define _HFDL_ARINC_H

#include <glib.h>

// Look for an ARINC-622 message in `txt` (the decoded ACARS message text) and,
// if one is there, append its decode to `out` at `indent`. Returns TRUE if the
// text was recognised as ARINC-622 (whether or not every field parsed).
gboolean hfdl_arinc_decode(const char *txt, GString *out, int indent);

// Headless self-test: synthesises ARINC-622 envelopes (correct CRC) carrying
// ADS-C basic reports / flight IDs and asserts the rendered text, plus a
// corrupt-CRC case and a non-ARINC text that must not be claimed.
gboolean hfdl_arinc_selftest(void);

#endif
