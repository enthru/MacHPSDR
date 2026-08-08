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
 * The rendering layer over the vendored asn1c tree (`hfdl_lib/asn1/`).
 *
 * asn1c gives us a decoded C structure, not text: a FANS-1/A message comes out
 * as a tree of CHOICEs, SEQUENCEs and integers with no idea that uM20 means
 * "CLIMB TO AND MAINTAIN". Turning that back into the controller's phrase is a
 * table keyed by ASN.1 type descriptor — one formatter per type — which is how
 * libacars does it (`asn1-util.c`, `asn1-format-common.c`) and what this ports,
 * text output only.
 *
 * Everything here writes into an `la_vstring`, the growable string the vendored
 * tree itself prints into (see `hfdl_lib/vstring.c`); `hfdl_cpdlc.c` moves the
 * result into the decoder's GString at the end.
 */

#ifndef _HFDL_ASN1_H
#define _HFDL_ASN1_H

#include <stdint.h>

#include <asn_application.h>
#include <libacars/vstring.h>

// A value -> label table. Same shape as libacars' la_dict, kept because the
// ported CPDLC tables are written in terms of it.
typedef struct { int id; const char *val; } HFDL_DICT;
const char *hfdl_dict_search(const HFDL_DICT *list, int id);

// What every formatter is handed: where to write, what to call this field, and
// which ASN.1 value it is looking at.
typedef struct {
  la_vstring *vstr;
  const char *label;
  asn_TYPE_descriptor_t *td;
  const void *sptr;
  int indent;
} ASN1_FMT_PARAMS;

typedef void (*ASN1_FMT_FUNC)(ASN1_FMT_PARAMS);

#define HFDL_ASN1_FORMATTER(x) void x(ASN1_FMT_PARAMS p)

typedef struct {
  asn_TYPE_descriptor_t *type;
  ASN1_FMT_FUNC          format;
  const char            *label;
} ASN1_FORMATTER;

// Decode `buf` as `td` using unaligned PER. Returns 0 on success.
int hfdl_asn1_decode_as(asn_TYPE_descriptor_t *td, void **struct_ptr,
                        const uint8_t *buf, int size);

// Find the formatter for p.td in the table and run it. A type with no entry is
// silently skipped — the table deliberately omits the wrapper types whose only
// job is to hold one member.
void hfdl_asn1_output(ASN1_FMT_PARAMS p, const ASN1_FORMATTER *table, size_t table_len);

// --- generic formatters shared by the CPDLC tables --------------------------

const char *hfdl_asn1_value2enum(asn_TYPE_descriptor_t *td, long value);

void hfdl_fmt_INTEGER_with_unit(ASN1_FMT_PARAMS p, const char *unit,
                                double multiplier, int decimal_places);
void hfdl_fmt_INTEGER_as_ENUM(ASN1_FMT_PARAMS p, const HFDL_DICT *value_labels);
void hfdl_fmt_CHOICE(ASN1_FMT_PARAMS p, const HFDL_DICT *choice_labels, ASN1_FMT_FUNC cb);
void hfdl_fmt_SEQUENCE(ASN1_FMT_PARAMS p, ASN1_FMT_FUNC cb);
void hfdl_fmt_SEQUENCE_OF(ASN1_FMT_PARAMS p, ASN1_FMT_FUNC cb);
void hfdl_fmt_BIT_STRING(ASN1_FMT_PARAMS p, const HFDL_DICT *bit_labels);

HFDL_ASN1_FORMATTER(hfdl_fmt_any);
HFDL_ASN1_FORMATTER(hfdl_fmt_label_only);
HFDL_ASN1_FORMATTER(hfdl_fmt_ENUM);

#endif
