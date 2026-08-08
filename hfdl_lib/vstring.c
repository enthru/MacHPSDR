/*
 * la_vstring — the growable string the vendored asn1c tree prints into.
 *
 * NOT upstream libacars code: hfdl_lib/asn1/ is vendored verbatim and its
 * constr_TYPE.c calls la_vstring_append_buffer(), and the ported CPDLC
 * formatter (src/decode/hfdl_cpdlc.c) uses the LA_ISPRINTF macros from
 * hfdl_lib/libacars/vstring.h — which IS the upstream header. Rather than drag
 * in libacars' vstring.c and, behind it, its macros.h/util.h/config.h, this
 * implements the same small API over plain realloc. The struct layout and the
 * function semantics are the header's, so the vendored tree and the ported
 * formatter both compile unmodified.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libacars/vstring.h>

#define VSTR_INITIAL 256

static void vstr_grow(la_vstring *v, size_t need) {
  if (v->allocated_size >= need) return;
  size_t want = v->allocated_size ? v->allocated_size : VSTR_INITIAL;
  while (want < need) want *= 2;
  v->str = realloc(v->str, want);
  if (v->str == NULL) abort();          // same posture as libacars' LA_XCALLOC
  v->allocated_size = want;
}

la_vstring *la_vstring_new(void) {
  la_vstring *v = calloc(1, sizeof(la_vstring));
  if (v == NULL) abort();
  vstr_grow(v, VSTR_INITIAL);
  v->str[0] = '\0';
  v->len = 0;
  return v;
}

void la_vstring_destroy(la_vstring *v, bool destroy_buffer) {
  if (v == NULL) return;
  if (destroy_buffer) free(v->str);
  free(v);
}

void la_vstring_append_sprintf(la_vstring *v, char const *fmt, ...) {
  if (v == NULL || fmt == NULL) return;
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n > 0) {
    vstr_grow(v, v->len + (size_t)n + 1);
    vsnprintf(v->str + v->len, (size_t)n + 1, fmt, ap2);
    v->len += (size_t)n;
  }
  va_end(ap2);
}

void la_vstring_append_buffer(la_vstring *v, void const *buffer, size_t size) {
  if (v == NULL || buffer == NULL || size == 0) return;
  vstr_grow(v, v->len + size + 1);
  memcpy(v->str + v->len, buffer, size);
  v->len += size;
  v->str[v->len] = '\0';
}

// Append `txt` with every line indented — used by formatters that emit a block
// of free text inside an indented structure.
void la_isprintf_multiline_text(la_vstring *v, int indent, char const *txt) {
  if (v == NULL || txt == NULL) return;
  const char *p = txt;
  while (*p != '\0') {
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    la_vstring_append_sprintf(v, "%*s", indent, "");
    la_vstring_append_buffer(v, p, n);
    la_vstring_append_sprintf(v, "%s", "\n");
    if (nl == NULL) break;
    p = nl + 1;
  }
}
