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
 * MIAM — "Media Independent Aircraft Messaging", the ACARS file-transfer
 * application (labels MA and H1). Port of libacars' miam.c and miam-core.c.
 *
 * Two layers stacked on top of each other:
 *
 *   - the ACARS CF frame, named by the message's first character: a Single
 *     Transfer (T) carries one MIAM CORE PDU outright, while a File Transfer
 *     Request (F) / Accept (K) / Segment (S) / Abort (A) / Pause (Y) / Resume
 *     (X) exchange moves a file that is too large for one ACARS message. The
 *     segments are reassembled here, keyed on registration + file id, with the
 *     total length taken from the request that opened the transfer — a segment
 *     alone does not carry it.
 *
 *   - the MIAM CORE PDU: two padding digits, a BASE85 header, '|', and a BASE85
 *     (or plain) body. The header names the version (1 or 2) and PDU type
 *     (Data / Ack / Aloha / Aloha Reply); a Data body is usually DEFLATE and
 *     carries its own ARINC CRC over the decompressed content, which we check.
 *
 * Self-contained: GLib + zlib. Audio-thread only, like the rest of the HFDL
 * application layer.
 */

#ifndef _HFDL_MIAM_H
#define _HFDL_MIAM_H

#include <glib.h>

// Look for a MIAM message in `txt` (the decoded ACARS message text of a label
// MA or H1 block) and, if one is there, append its decode to `out` at `indent`.
// `reg` is the aircraft registration — it keys file-segment reassembly and may
// be NULL, in which case segments are decoded but never reassembled. Returns
// TRUE if the text was recognised as MIAM.
gboolean hfdl_miam_decode(const char *reg, const char *txt, GString *out, int indent);

// Drop all partial file transfers (called from hfdl_msg_reset()).
void hfdl_miam_reset(void);

// Test seam: pin the clock used for reassembly timeouts (0 = real clock).
void hfdl_miam_set_test_clock(gint64 now_us);

// Headless self-test: synthesises MIAM CORE v1 and v2 Data PDUs (deflate body +
// correct ARINC CRC), an Ack and an Aloha, a two-segment file transfer, and
// asserts the rendered text; plus non-MIAM text that must not be claimed and a
// body whose CRC is wrong, which must be reported.
gboolean hfdl_miam_selftest(void);

#endif
