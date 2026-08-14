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

#include <stdarg.h>
#include <string.h>
#include <zlib.h>

#include "hfdl_util.h"

void hfdl_emit(GString *out, int indent, const char *fmt, ...) {
  va_list ap;
  for (int i = 0; i < indent; i++) g_string_append(out, "  ");
  va_start(ap, fmt);
  g_string_append_vprintf(out, fmt, ap);
  va_end(ap);
  g_string_append_c(out, '\n');
}

// Raw deflate, growing the output as inflate() runs out of room. Port of
// libacars' la_inflate(): inflateInit2(-15) selects "no zlib wrapper", which is
// what both MIAM CORE bodies and (after their two-octet CMF/FLG) OHMA payloads
// carry.
gboolean hfdl_inflate(const uint8_t *in, int in_len, uint8_t **out, gsize *out_len) {
  if (out != NULL) *out = NULL;
  if (out_len != NULL) *out_len = 0;
  if (in == NULL || in_len <= 0 || out == NULL || out_len == NULL) return FALSE;

  z_stream s;
  memset(&s, 0, sizeof(s));
  if (inflateInit2(&s, -15) != Z_OK) return FALSE;

  int chunk = 4 * in_len;
  if (chunk < 1024) chunk = 1024;
  int cap = chunk;
  uint8_t *buf = g_malloc0((gsize)cap + 1);   // +1: room for the NUL below

  s.next_in  = (uint8_t *)in;
  s.avail_in = (uInt)in_len;
  s.next_out = buf;
  s.avail_out = (uInt)cap;

  int ret;
  gboolean overflow = FALSE;
  uLong last_in = 0, last_out = 0;
  while ((ret = inflate(&s, Z_FINISH)) == Z_BUF_ERROR) {
    // Z_BUF_ERROR with input left AND output space left should not be
    // reachable -- zlib returns it only when it cannot progress -- and neither
    // this loop nor libacars', which it is a port of, has a branch for it: the
    // loop would just call inflate() again on the same state, for ever, on the
    // RX audio thread, driven by a byte sequence that arrived off the air.
    // One no-progress iteration is enough to say the stream is going nowhere.
    if (s.total_in == last_in && s.total_out == last_out) { ret = Z_DATA_ERROR; break; }
    last_in = s.total_in; last_out = s.total_out;
    if (s.avail_out == 0) {
      if (cap > HFDL_INFLATE_MAX - chunk) { overflow = TRUE; break; }
      int ncap = cap + chunk;
      buf = g_realloc(buf, (gsize)ncap + 1);
      s.next_out  = buf + cap;
      s.avail_out = (uInt)chunk;
      cap = ncap;
    } else if (s.avail_in == 0) {
      break;                                  // truncated input
    }
  }

  gsize produced = (gsize)s.total_out;
  (void)inflateEnd(&s);

  if (ret != Z_STREAM_END || overflow) { g_free(buf); return FALSE; }
  buf[produced] = '\0';
  *out = buf;
  *out_len = produced;
  return TRUE;
}

// Plain ASCII text plus the whitespace controls libacars allows (7..13).
static gboolean is_ascii_text(const uint8_t *buf, gsize len) {
  for (gsize i = 0; i < len; i++) {
    uint8_t c = buf[i];
    if ((c >= 7 && c <= 13) || (c >= 32 && c <= 126)) continue;
    return FALSE;
  }
  return TRUE;
}

gboolean hfdl_is_printable(const uint8_t *buf, gsize len) {
  if (buf == NULL || len == 0) return FALSE;
  if (is_ascii_text(buf, len)) return TRUE;

  // Deliberate deviation from libacars, whose is_printable() is ASCII-only: an
  // OHMA payload is JSON and therefore UTF-8 by definition, so one accented
  // character would otherwise turn the whole message into a hexdump. Valid
  // UTF-8 with no control characters beyond the ones above is accepted —
  // binary data essentially never satisfies both tests at once.
  if (!g_utf8_validate((const char *)buf, (gssize)len, NULL)) return FALSE;
  for (gsize i = 0; i < len; i++) {
    uint8_t c = buf[i];
    if (c < 0x80 && !((c >= 7 && c <= 13) || (c >= 32 && c <= 126))) return FALSE;
  }
  return TRUE;
}

void hfdl_emit_text(GString *out, int indent, const uint8_t *buf, gsize len) {
  // Non-ASCII bytes survive only when the whole buffer is valid UTF-8;
  // otherwise they become '.', as they always did.
  gboolean utf8 = g_utf8_validate((const char *)buf, (gssize)len, NULL);
  GString *line = g_string_new(NULL);
  for (gsize i = 0; i <= len; i++) {
    char c = (i < len) ? (char)buf[i] : '\n';
    if (c == '\r') continue;
    if (c == '\n') {
      if (line->len > 0) hfdl_emit(out, indent, "| %s", line->str);
      g_string_truncate(line, 0);
      continue;
    }
    if (g_ascii_isprint(c))                    g_string_append_c(line, c);
    else if (utf8 && (uint8_t)c >= 0x80)       g_string_append_c(line, c);
    else                                       g_string_append_c(line, '.');
  }
  g_string_free(line, TRUE);
}

void hfdl_emit_hexdump(GString *out, int indent, const uint8_t *buf, gsize len) {
  for (gsize off = 0; off < len; off += 16) {
    GString *l = g_string_new(NULL);
    g_string_append_printf(l, "%04zx  ", (size_t)off);
    for (gsize i = 0; i < 16; i++) {
      if (off + i < len) g_string_append_printf(l, "%02x ", buf[off + i]);
      else               g_string_append(l, "   ");
      if (i == 7) g_string_append_c(l, ' ');
    }
    g_string_append_c(l, '|');
    for (gsize i = 0; i < 16 && off + i < len; i++) {
      char c = (char)buf[off + i];
      g_string_append_c(l, g_ascii_isprint(c) ? c : '.');
    }
    g_string_append_c(l, '|');
    hfdl_emit(out, indent, "%s", l->str);
    g_string_free(l, TRUE);
  }
}

void hfdl_emit_payload(GString *out, int indent, const uint8_t *buf, gsize len) {
  if (buf == NULL || len == 0) return;
  if (hfdl_is_printable(buf, len)) hfdl_emit_text(out, indent, buf, len);
  else                             hfdl_emit_hexdump(out, indent, buf, len);
}
