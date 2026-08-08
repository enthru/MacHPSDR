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
 * OHMA — the Collins/ARINC "over-the-horizon" messaging application carried in
 * ACARS label H1. Port of libacars' ohma.c.
 *
 * Wire format: an optional ground/uplink prefix, the literal "OHMA" or "RYKO",
 * then BASE64 of a zlib stream (RFC 1950 header + DEFLATE) whose plaintext is a
 * JSON envelope: version, conversation id, the payload itself, and — when the
 * conversation does not fit one ACARS message — a msg_seq / msg_total pair that
 * fragments it. The payload is usually further encrypted end-to-end (the
 * envelope carries sym_key / iv / signature), so what we can show is the
 * envelope plus whatever the payload turns out to be.
 *
 * Two things differ from libacars deliberately: the JSON is read by a small
 * flat-object scanner instead of a Jansson dependency (the envelope is a flat
 * object — six members we care about), and OHMA fragment reassembly is its own
 * fixed table here rather than the generic engine, because OHMA is the one
 * ACARS application that genuinely delivers fragments out of order.
 *
 * Self-contained: GLib + zlib. Audio-thread only, like the rest of the HFDL
 * application layer.
 */

#ifndef _HFDL_OHMA_H
#define _HFDL_OHMA_H

#include <glib.h>

// Look for an OHMA message in `txt` (the decoded ACARS message text of a label
// H1 block) and, if one is there, append its decode to `out` at `indent`.
// `reg` is the aircraft registration — it keys fragment reassembly and may be
// NULL, in which case fragments are decoded but never reassembled. Returns TRUE
// if the text was recognised as OHMA (whether or not every field parsed).
gboolean hfdl_ohma_decode(const char *reg, const char *txt, GString *out, int indent);

// Drop all partial reassemblies (called from hfdl_msg_reset()).
void hfdl_ohma_reset(void);

// Test seam: pin the clock used for reassembly timeouts (0 = real clock).
void hfdl_ohma_set_test_clock(gint64 now_us);

// Headless self-test: synthesises OHMA envelopes (JSON -> deflate -> base64),
// single and fragmented-out-of-order, and asserts the rendered text; plus
// non-OHMA text that must not be claimed and a corrupt stream that must be
// reported rather than crash.
gboolean hfdl_ohma_selftest(void);

#endif
