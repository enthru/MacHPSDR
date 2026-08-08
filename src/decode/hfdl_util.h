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
 * Small pieces the HFDL application layer needs in more than one place: the
 * indented emit() every decoder renders through, a raw-deflate decompressor
 * (MIAM CORE bodies and OHMA payloads are both DEFLATE), and the printable /
 * hexdump rendering both fall back to.
 *
 * GLib + zlib only — no GTK, no RADIO, so this links into the headless
 * harnesses like the rest of the decode chain.
 */

#ifndef _HFDL_UTIL_H
#define _HFDL_UTIL_H

#include <glib.h>
#include <stdint.h>

// Append one indented line (two spaces per level) plus a newline.
G_GNUC_PRINTF(3, 4) void hfdl_emit(GString *out, int indent, const char *fmt, ...);

// Raw DEFLATE (RFC 1951 — no zlib CMF/FLG header; strip those first). On
// success returns a NUL-terminated buffer the caller frees with g_free() and
// its length in *out_len; the terminator is past *out_len, so text payloads can
// be printed directly. Bounded by HFDL_INFLATE_MAX so a malformed stream can't
// be told to allocate the world.
#define HFDL_INFLATE_MAX (1 << 20)
gboolean hfdl_inflate(const uint8_t *in, int in_len, uint8_t **out, gsize *out_len);

// TRUE when every octet is printable ASCII, tab, CR or LF — i.e. worth showing
// as text rather than as a hexdump.
gboolean hfdl_is_printable(const uint8_t *buf, gsize len);

// Render `len` octets of `buf` as text lines ("| ..."), non-printables as '.'.
void hfdl_emit_text(GString *out, int indent, const uint8_t *buf, gsize len);

// Render `len` octets of `buf` as a classic 16-per-line hexdump.
void hfdl_emit_hexdump(GString *out, int indent, const uint8_t *buf, gsize len);

// Whichever of the two above suits the data.
void hfdl_emit_payload(GString *out, int indent, const uint8_t *buf, gsize len);

#endif
