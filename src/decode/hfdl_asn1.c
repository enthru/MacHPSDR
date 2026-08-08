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
 * Port of libacars' asn1-util.c and the text half of asn1-format-common.c.
 * See hfdl_asn1.h for what this layer is for.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <asn_SET_OF.h>
#include <BIT_STRING.h>
#include <constr_CHOICE.h>
#include <INTEGER.h>
#include <OCTET_STRING.h>
#include <per_decoder.h>

#include "hfdl_asn1.h"

const char *hfdl_dict_search(const HFDL_DICT *list, int id) {
  if (list == NULL) return NULL;
  for (const HFDL_DICT *p = list; p->val != NULL; p++)
    if (p->id == id) return p->val;
  return NULL;
}

int hfdl_asn1_decode_as(asn_TYPE_descriptor_t *td, void **struct_ptr,
                        const uint8_t *buf, int size) {
  asn_dec_rval_t rval = uper_decode_complete(0, td, struct_ptr, buf, (size_t)size);
  if (rval.code != RC_OK) return -1;
  // Trailing octets are reported but not fatal: the message decoded, and
  // dropping it because a sender padded would lose real traffic.
  if (rval.consumed < (size_t)size) return (int)((size_t)size - rval.consumed);
  return 0;
}

void hfdl_asn1_output(ASN1_FMT_PARAMS p, const ASN1_FORMATTER *table, size_t table_len) {
  if (p.td == NULL || p.sptr == NULL) return;
  for (size_t i = 0; i < table_len; i++) {
    if (table[i].type != p.td) continue;
    // A NULL formatter means "known type, deliberately not printed" — the
    // wrapper types whose label would only add a level of nesting.
    if (table[i].format != NULL) {
      p.label = table[i].label;
      table[i].format(p);
    }
    return;
  }
  // No entry at all: silently skipped. libacars can dump the raw ASN.1 here;
  // we do not, because the CPDLC table covers the whole FANS-1/A tree and a
  // dump in the middle of a controller message is noise, not information.
}

const char *hfdl_asn1_value2enum(asn_TYPE_descriptor_t *td, long value) {
  if (td == NULL) return NULL;
  const asn_INTEGER_enum_map_t *m = INTEGER_map_value2enum(td->specifics, value);
  return (m == NULL) ? NULL : m->enum_name;
}

void hfdl_fmt_INTEGER_with_unit(ASN1_FMT_PARAMS p, const char *unit,
                                double multiplier, int decimal_places) {
  const long *val = p.sptr;
  LA_ISPRINTF(p.vstr, p.indent, "%s: %.*f%s\n", p.label, decimal_places,
              (double)(*val) * multiplier, unit);
}

void hfdl_fmt_INTEGER_as_ENUM(ASN1_FMT_PARAMS p, const HFDL_DICT *value_labels) {
  const long *val = p.sptr;
  const char *lbl = hfdl_dict_search(value_labels, (int)(*val));
  if (lbl != NULL) LA_ISPRINTF(p.vstr, p.indent, "%s: %s\n", p.label, lbl);
  else             LA_ISPRINTF(p.vstr, p.indent, "%s: %ld (unknown)\n", p.label, *val);
}

void hfdl_fmt_CHOICE(ASN1_FMT_PARAMS p, const HFDL_DICT *choice_labels, ASN1_FMT_FUNC cb) {
  asn_CHOICE_specifics_t *specs = p.td->specifics;
  int present = _fetch_present_idx(p.sptr, specs->pres_offset, specs->pres_size);
  if (p.label != NULL) {
    LA_ISPRINTF(p.vstr, p.indent, "%s:\n", p.label);
    p.indent++;
  }
  if (choice_labels != NULL) {
    const char *descr = hfdl_dict_search(choice_labels, present);
    if (descr != NULL) LA_ISPRINTF(p.vstr, p.indent, "%s\n", descr);
    else               LA_ISPRINTF(p.vstr, p.indent, "<no description for CHOICE value %d>\n", present);
    p.indent++;
  }
  if (present > 0 && present <= p.td->elements_count) {
    asn_TYPE_member_t *elm = &p.td->elements[present - 1];
    const void *memb_ptr;
    if (elm->flags & ATF_POINTER) {
      memb_ptr = *(const void *const *)((const char *)p.sptr + elm->memb_offset);
      if (memb_ptr == NULL) {
        LA_ISPRINTF(p.vstr, p.indent, "%s: <not present>\n", elm->name);
        return;
      }
    } else {
      memb_ptr = (const void *)((const char *)p.sptr + elm->memb_offset);
    }
    p.td = elm->type;
    p.sptr = memb_ptr;
    cb(p);
  } else {
    LA_ISPRINTF(p.vstr, p.indent, "-- %s: value %d out of range\n", p.td->name, present);
  }
}

void hfdl_fmt_SEQUENCE(ASN1_FMT_PARAMS p, ASN1_FMT_FUNC cb) {
  if (p.label != NULL) {
    LA_ISPRINTF(p.vstr, p.indent, "%s:\n", p.label);
    p.indent++;
  }
  ASN1_FMT_PARAMS cb_p = p;
  for (int edx = 0; edx < p.td->elements_count; edx++) {
    asn_TYPE_member_t *elm = &p.td->elements[edx];
    const void *memb_ptr;
    if (elm->flags & ATF_POINTER) {
      memb_ptr = *(const void *const *)((const char *)p.sptr + elm->memb_offset);
      if (memb_ptr == NULL) continue;         // OPTIONAL field, absent
    } else {
      memb_ptr = (const void *)((const char *)p.sptr + elm->memb_offset);
    }
    cb_p.td = elm->type;
    cb_p.sptr = memb_ptr;
    cb(cb_p);
  }
}

void hfdl_fmt_SEQUENCE_OF(ASN1_FMT_PARAMS p, ASN1_FMT_FUNC cb) {
  if (p.label != NULL) {
    LA_ISPRINTF(p.vstr, p.indent, "%s:\n", p.label);
    p.indent++;
  }
  asn_TYPE_member_t *elm = p.td->elements;
  const asn_anonymous_set_ *list = _A_CSET_FROM_VOID(p.sptr);
  for (int i = 0; i < list->count; i++) {
    const void *memb_ptr = list->array[i];
    if (memb_ptr == NULL) continue;
    p.td = elm->type;
    p.sptr = memb_ptr;
    cb(p);
  }
}

// Reverse the low `numbits` bits of v (libacars util.c).
static uint32_t bit_reverse(uint32_t v, int numbits) {
  uint32_t r = v;
  int s = (int)(sizeof(v) * CHAR_BIT) - 1;
  for (v >>= 1; v; v >>= 1) { r <<= 1; r |= v & 1; s--; }
  r <<= s;
  r >>= 32 - numbits;
  return r;
}

// Bit strings up to 32 bits. Dictionary keys are bit numbers from 0, bit 0
// being the MSB of the first octet.
void hfdl_fmt_BIT_STRING(ASN1_FMT_PARAMS p, const HFDL_DICT *bit_labels) {
  const BIT_STRING_t *bs = p.sptr;
  uint32_t val = 0;
  int truncated = 0;
  int len = bs->size;
  int bits_unused = bs->bits_unused;

  if (len > (int)sizeof(val)) {
    truncated = len - (int)sizeof(val);
    len = (int)sizeof(val);
    bits_unused = 0;
  }
  if (p.label != NULL) LA_ISPRINTF(p.vstr, p.indent, "%s: ", p.label);
  for (int i = 0; i < len; val = (val << 8) | bs->buf[i++]);
  val &= (~0u << bits_unused);
  if (val == 0) {
    la_vstring_append_sprintf(p.vstr, "none\n");
  } else {
    val = bit_reverse(val, len * 8);
    bool first = true;
    for (const HFDL_DICT *ptr = bit_labels; ptr != NULL && ptr->val != NULL; ptr++) {
      if ((val >> (uint32_t)ptr->id) & 1) {
        la_vstring_append_sprintf(p.vstr, "%s%s", first ? "" : ", ", ptr->val);
        first = false;
      }
    }
    LA_EOL(p.vstr);
  }
  if (truncated > 0)
    LA_ISPRINTF(p.vstr, p.indent,
                "-- Warning: bit string too long (%d bits), truncated to %d bits\n",
                bs->size * 8 - bs->bits_unused, len * 8);
}

HFDL_ASN1_FORMATTER(hfdl_fmt_any) {
  if (p.label != NULL) LA_ISPRINTF(p.vstr, p.indent, "%s: ", p.label);
  else                 LA_ISPRINTF(p.vstr, p.indent, "%s", "");
  asn_sprintf(p.vstr, p.td, p.sptr, 1);
  LA_EOL(p.vstr);
}

HFDL_ASN1_FORMATTER(hfdl_fmt_label_only) {
  if (p.label != NULL) LA_ISPRINTF(p.vstr, p.indent, "%s\n", p.label);
}

HFDL_ASN1_FORMATTER(hfdl_fmt_ENUM) {
  long value = *(const long *)p.sptr;
  const char *s = hfdl_asn1_value2enum(p.td, value);
  if (s != NULL) LA_ISPRINTF(p.vstr, p.indent, "%s: %s\n", p.label, s);
  else           LA_ISPRINTF(p.vstr, p.indent, "%s: %ld\n", p.label, value);
}
