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

#include <stdint.h>
#include <string.h>
#include <zlib.h>

#include "hfdl_ohma.h"
#include "hfdl_util.h"
#include "log.h"

// --- a small flat-object JSON reader ----------------------------------------
//
// The OHMA envelope is a flat JSON object and we want six of its members. That
// does not justify a JSON library dependency, but it does need a real parser:
// a member we skip may itself be an object, an array or a string containing
// braces, so skipping has to be structural, not a search for the next comma.

typedef struct { const char *p; } JSON;

static void js_ws(JSON *j) {
  while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

// Parse a JSON string token into a newly allocated UTF-8 string. Returns NULL
// (leaving the cursor untouched) if the cursor is not on a string.
static char *js_string(JSON *j) {
  js_ws(j);
  if (*j->p != '"') return NULL;
  const char *p = j->p + 1;
  GString *s = g_string_new(NULL);
  while (*p != '\0' && *p != '"') {
    if (*p != '\\') { g_string_append_c(s, *p++); continue; }
    p++;
    switch (*p) {
      case '"':  g_string_append_c(s, '"');  p++; break;
      case '\\': g_string_append_c(s, '\\'); p++; break;
      case '/':  g_string_append_c(s, '/');  p++; break;
      case 'b':  g_string_append_c(s, '\b'); p++; break;
      case 'f':  g_string_append_c(s, '\f'); p++; break;
      case 'n':  g_string_append_c(s, '\n'); p++; break;
      case 'r':  g_string_append_c(s, '\r'); p++; break;
      case 't':  g_string_append_c(s, '\t'); p++; break;
      case 'u': {
        gunichar cp = 0;
        int i = 0;
        for (p++; i < 4 && g_ascii_isxdigit(p[i]); i++)
          cp = (cp << 4) | (gunichar)g_ascii_xdigit_value(p[i]);
        if (i < 4) { g_string_free(s, TRUE); return NULL; }
        p += 4;
        // A surrogate pair encodes one astral character; a lone surrogate is
        // not valid UTF-8, so it becomes U+FFFD rather than a broken string.
        if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
          gunichar lo = 0;
          int k = 0;
          for (int m = 0; m < 4 && g_ascii_isxdigit(p[2 + m]); m++, k++)
            lo = (lo << 4) | (gunichar)g_ascii_xdigit_value(p[2 + m]);
          if (k == 4 && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        g_string_append_unichar(s, cp);
        break;
      }
      default: g_string_free(s, TRUE); return NULL;
    }
  }
  if (*p != '"') { g_string_free(s, TRUE); return NULL; }
  j->p = p + 1;
  return g_string_free(s, FALSE);
}

static gboolean js_skip_value(JSON *j, int depth);

static gboolean js_skip_container(JSON *j, char open, char close, int depth) {
  j->p++;                                     // step over the opening bracket
  js_ws(j);
  if (*j->p == close) { j->p++; return TRUE; }
  for (;;) {
    if (open == '{') {
      char *k = js_string(j);
      if (k == NULL) return FALSE;
      g_free(k);
      js_ws(j);
      if (*j->p != ':') return FALSE;
      j->p++;
    }
    if (!js_skip_value(j, depth + 1)) return FALSE;
    js_ws(j);
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == close) { j->p++; return TRUE; }
    return FALSE;
  }
}

// Step the cursor over one complete value. `depth` bounds nesting so a
// pathological input can't recurse the stack away.
static gboolean js_skip_value(JSON *j, int depth) {
  if (depth > 32) return FALSE;
  js_ws(j);
  switch (*j->p) {
    case '"': { char *s = js_string(j); if (s == NULL) return FALSE; g_free(s); return TRUE; }
    case '{': return js_skip_container(j, '{', '}', depth);
    case '[': return js_skip_container(j, '[', ']', depth);
    case 't': if (strncmp(j->p, "true",  4) == 0) { j->p += 4; return TRUE; } return FALSE;
    case 'f': if (strncmp(j->p, "false", 5) == 0) { j->p += 5; return TRUE; } return FALSE;
    case 'n': if (strncmp(j->p, "null",  4) == 0) { j->p += 4; return TRUE; } return FALSE;
    default: {
      const char *start = j->p;
      if (*j->p == '-' || *j->p == '+') j->p++;
      while (g_ascii_isdigit(*j->p) || *j->p == '.' || *j->p == 'e' || *j->p == 'E' ||
             *j->p == '+' || *j->p == '-') j->p++;
      return j->p > start;
    }
  }
}

// The members of the envelope we understand.
typedef struct {
  char   *version, *convo_id, *message, *sym_key, *iv, *signature;
  gint64  msg_seq, msg_total;
  gboolean parsed;
} OHMA_JSON;

static void ohma_json_free(OHMA_JSON *o) {
  g_free(o->version);  g_free(o->convo_id); g_free(o->message);
  g_free(o->sym_key);  g_free(o->iv);       g_free(o->signature);
  memset(o, 0, sizeof(*o));
}

static gboolean ohma_json_parse(const char *json, OHMA_JSON *o) {
  memset(o, 0, sizeof(*o));
  JSON j = { .p = json };
  js_ws(&j);
  if (*j.p != '{') return FALSE;
  j.p++;
  js_ws(&j);
  if (*j.p == '}') { o->parsed = TRUE; return TRUE; }

  for (;;) {
    char *key = js_string(&j);
    if (key == NULL) { ohma_json_free(o); return FALSE; }
    js_ws(&j);
    if (*j.p != ':') { g_free(key); ohma_json_free(o); return FALSE; }
    j.p++;
    js_ws(&j);

    char **str_dst = NULL;
    gint64 *int_dst = NULL;
    if      (!strcmp(key, "version"))   str_dst = &o->version;
    else if (!strcmp(key, "convo_id"))  str_dst = &o->convo_id;
    else if (!strcmp(key, "message"))   str_dst = &o->message;
    else if (!strcmp(key, "sym_key"))   str_dst = &o->sym_key;
    else if (!strcmp(key, "iv"))        str_dst = &o->iv;
    else if (!strcmp(key, "signature")) str_dst = &o->signature;
    else if (!strcmp(key, "msg_seq"))   int_dst = &o->msg_seq;
    else if (!strcmp(key, "msg_total")) int_dst = &o->msg_total;
    g_free(key);

    if (str_dst != NULL && *j.p == '"') {
      char *v = js_string(&j);
      if (v == NULL) { ohma_json_free(o); return FALSE; }
      g_free(*str_dst);
      *str_dst = v;
    } else if (int_dst != NULL && (g_ascii_isdigit(*j.p) || *j.p == '-')) {
      *int_dst = g_ascii_strtoll(j.p, NULL, 10);
      if (!js_skip_value(&j, 0)) { ohma_json_free(o); return FALSE; }
    } else if (!js_skip_value(&j, 0)) {
      ohma_json_free(o);
      return FALSE;
    }

    js_ws(&j);
    if (*j.p == ',') { j.p++; js_ws(&j); continue; }
    if (*j.p == '}') { j.p++; break; }
    ohma_json_free(o);
    return FALSE;
  }
  o->parsed = TRUE;
  return TRUE;
}

// --- fragment reassembly ----------------------------------------------------
//
// Keyed on registration + conversation id. Unlike ACARS block reassembly (which
// is strictly in order — see hfdl_msg.c), OHMA fragments really do arrive out
// of order, so each fragment goes into its own slot and the message completes
// when every slot 1..msg_total is filled.

#define OHMA_REASM_MAX   8
#define OHMA_FRAG_MAX   32
#define OHMA_REASM_TIMEOUT_US (1200 * G_USEC_PER_SEC)

typedef struct {
  gboolean in_use;
  char     reg[16];
  char     convo_id[64];
  int      total;
  gint64   first_us;
  char    *frag[OHMA_FRAG_MAX];
} OHMA_REASM;

static OHMA_REASM ohma_reasm[OHMA_REASM_MAX];
static gint64     ohma_test_now_us = 0;

typedef enum {
  OHMA_R_SKIPPED, OHMA_R_IN_PROGRESS, OHMA_R_COMPLETE, OHMA_R_DUPLICATE, OHMA_R_ERROR
} OHMA_REASM_STATUS;

static gint64 ohma_now(void) {
  return ohma_test_now_us ? ohma_test_now_us : g_get_monotonic_time();
}

void hfdl_ohma_set_test_clock(gint64 now_us) { ohma_test_now_us = now_us; }

static void ohma_reasm_free(OHMA_REASM *e) {
  for (int i = 0; i < OHMA_FRAG_MAX; i++) { g_free(e->frag[i]); e->frag[i] = NULL; }
  memset(e, 0, sizeof(*e));
}

void hfdl_ohma_reset(void) {
  for (int i = 0; i < OHMA_REASM_MAX; i++) ohma_reasm_free(&ohma_reasm[i]);
}

static void ohma_reasm_expire(gint64 now_us) {
  for (int i = 0; i < OHMA_REASM_MAX; i++)
    if (ohma_reasm[i].in_use && now_us - ohma_reasm[i].first_us > OHMA_REASM_TIMEOUT_US)
      ohma_reasm_free(&ohma_reasm[i]);
}

// Add one fragment; on completion *out is the concatenated message (caller frees).
static OHMA_REASM_STATUS ohma_reasm_add(const char *reg, const char *convo_id,
                                        int seq, int total, const char *data,
                                        char **out) {
  *out = NULL;
  if (reg == NULL || convo_id == NULL || data == NULL) return OHMA_R_ERROR;
  if (seq < 1 || seq > OHMA_FRAG_MAX) return OHMA_R_ERROR;

  gint64 now_us = ohma_now();
  ohma_reasm_expire(now_us);

  OHMA_REASM *e = NULL, *empty = NULL, *oldest = NULL;
  for (int i = 0; i < OHMA_REASM_MAX; i++) {
    OHMA_REASM *c = &ohma_reasm[i];
    if (c->in_use && !strcmp(c->reg, reg) && !strcmp(c->convo_id, convo_id)) { e = c; break; }
    if (!c->in_use) { if (empty == NULL) empty = c; }
    else if (oldest == NULL || c->first_us < oldest->first_us) oldest = c;
  }
  if (e == NULL) {
    // No entry for this conversation: take a free slot, else evict the oldest.
    e = (empty != NULL) ? empty : oldest;
    ohma_reasm_free(e);
    e->in_use = TRUE;
    e->first_us = now_us;
    g_strlcpy(e->reg, reg, sizeof(e->reg));
    g_strlcpy(e->convo_id, convo_id, sizeof(e->convo_id));
  }
  // msg_total travels only in the first fragment, so remember it whenever it
  // shows up and never let a later 0 erase it.
  if (total > 0 && total <= OHMA_FRAG_MAX) e->total = total;

  if (e->frag[seq - 1] != NULL) return OHMA_R_DUPLICATE;
  e->frag[seq - 1] = g_strdup(data);

  if (e->total <= 0) return OHMA_R_IN_PROGRESS;
  for (int i = 0; i < e->total; i++)
    if (e->frag[i] == NULL) return OHMA_R_IN_PROGRESS;

  GString *full = g_string_new(NULL);
  for (int i = 0; i < e->total; i++) g_string_append(full, e->frag[i]);
  ohma_reasm_free(e);
  *out = g_string_free(full, FALSE);
  return OHMA_R_COMPLETE;
}

// --- envelope ---------------------------------------------------------------

static gboolean is_base64(const char *s, gsize len) {
  for (gsize i = 0; i < len; i++) {
    char c = s[i];
    if (!(g_ascii_isalnum(c) || c == '+' || c == '/' || c == '=')) return FALSE;
  }
  return len > 0;
}

// Find `needle` (nlen octets) inside `hay` (hlen octets). memmem() is a GNU
// extension; the strings here are short, so a plain scan is fine.
static const char *find_mem(const char *hay, gsize hlen, const char *needle, gsize nlen) {
  if (nlen == 0 || nlen > hlen) return NULL;
  for (gsize i = 0; i + nlen <= hlen; i++)
    if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
  return NULL;
}

gboolean hfdl_ohma_decode(const char *reg, const char *txt, GString *out, int indent) {
  if (txt == NULL || out == NULL) return FALSE;

  // Envelope recognition (libacars ohma.c): downlinks start with "OHMA"/"RYKO",
  // optionally behind a '/' + 7-char ground address + '.', uplinks behind a
  // '/' + 2 characters + '.'.
  const char *ptr = txt;
  gsize len = 0;
  for (int guard = 0; ; guard++) {
    len = strlen(ptr);
    if      (len >= 13 && ptr[0] == '/' && ptr[8] == '.') { ptr += 9; len -= 9; }
    else if (len >=  8 && ptr[0] == '/' && ptr[3] == '.') { ptr += 4; len -= 4; }
    if (len >= 4 && (strncmp(ptr, "OHMA", 4) == 0 || strncmp(ptr, "RYKO", 4) == 0)) {
      ptr += 4; len -= 4;
    } else {
      return FALSE;
    }
    // Some senders repeat the first ACARS block verbatim in the second, with
    // the sequence number correctly incremented — so ACARS reassembly cannot
    // see it as a duplicate and we get the header twice. If the prefix we just
    // stepped over occurs again in what is left, skip to that copy.
    if (guard >= 4) break;
    const char *dup = find_mem(ptr, len, txt, (gsize)(ptr - txt));
    if (dup == NULL) break;
    log_debug("hfdl ohma: duplicate first fragment, skipping %ld octets\n", (long)(dup - txt));
    ptr = dup;
  }

  // Trailing CR/LF is common on uplinks and is not BASE64.
  while (len > 0 && (ptr[len - 1] == '\r' || ptr[len - 1] == '\n')) len--;
  if (!is_base64(ptr, len)) return FALSE;    // almost certainly not OHMA

  gsize raw_len = 0;
  char *b64 = g_strndup(ptr, len);
  guchar *raw = g_base64_decode(b64, &raw_len);
  g_free(b64);
  if (raw == NULL || raw_len < 3) { g_free(raw); return FALSE; }

  hfdl_emit(out, indent, "OHMA message:");
  indent++;

  // RFC 1950 zlib stream: CMF low nibble is the compression method, 8 = DEFLATE.
  if ((raw[0] & 0x0f) != 8) {
    hfdl_emit(out, indent, "-- unknown compression algorithm (%u)", raw[0] & 0x0f);
    g_free(raw);
    return TRUE;
  }

  uint8_t *plain = NULL;
  gsize plain_len = 0;
  // inflate() wants the DEFLATE stream, not the two-octet zlib header.
  gboolean ok = hfdl_inflate(raw + 2, (int)(raw_len - 2), &plain, &plain_len);
  g_free(raw);
  if (!ok) {
    hfdl_emit(out, indent, "-- decompression failed");
    g_free(plain);
    return TRUE;
  }

  OHMA_JSON js;
  if (!ohma_json_parse((const char *)plain, &js)) {
    // Not JSON (or malformed). Print what we have — it is still more than hex.
    hfdl_emit(out, indent, "-- unexpected JSON structure");
    hfdl_emit_payload(out, indent, plain, plain_len);
    g_free(plain);
    return TRUE;
  }

  if (js.version   != NULL) hfdl_emit(out, indent, "Version: %s", js.version);
  if (js.convo_id  != NULL) hfdl_emit(out, indent, "Msg ID: %s", js.convo_id);
  if (js.msg_seq   >  0)    hfdl_emit(out, indent, "Msg seq: %" G_GINT64_FORMAT, js.msg_seq);
  if (js.msg_total >  0)    hfdl_emit(out, indent, "Msg total: %" G_GINT64_FORMAT, js.msg_total);
  if (js.sym_key   != NULL) hfdl_emit(out, indent, "Sym key: %s", js.sym_key);
  if (js.iv        != NULL) hfdl_emit(out, indent, "IV: %s", js.iv);
  if (js.signature != NULL) hfdl_emit(out, indent, "Signature: %s", js.signature);

  char *reassembled = NULL;
  if (js.msg_seq > 0 && js.message != NULL) {
    if (js.convo_id == NULL) {
      hfdl_emit(out, indent, "Reassembly: msg_seq set but no conversation id");
    } else if (js.msg_seq == 1 && js.msg_total <= 0) {
      hfdl_emit(out, indent, "Reassembly: first fragment without a total");
    } else if (reg == NULL) {
      hfdl_emit(out, indent, "Reassembly: skipped (no registration)");
    } else {
      switch (ohma_reasm_add(reg, js.convo_id, (int)js.msg_seq, (int)js.msg_total,
                             js.message, &reassembled)) {
        case OHMA_R_IN_PROGRESS: hfdl_emit(out, indent, "Reassembly: in progress"); break;
        case OHMA_R_COMPLETE:    hfdl_emit(out, indent, "Reassembly: complete"); break;
        case OHMA_R_DUPLICATE:   hfdl_emit(out, indent, "Reassembly: duplicate fragment"); break;
        default:                 hfdl_emit(out, indent, "Reassembly: failed"); break;
      }
    }
  }

  const char *body = reassembled ? reassembled : js.message;
  if (body != NULL) {
    hfdl_emit(out, indent, "Message:");
    hfdl_emit_payload(out, indent + 1, (const uint8_t *)body, strlen(body));
  } else if (js.message == NULL) {
    hfdl_emit_payload(out, indent, plain, plain_len);
  }

  g_free(reassembled);
  ohma_json_free(&js);
  g_free(plain);
  return TRUE;
}

// --- self-test --------------------------------------------------------------

// Build the wire form of one OHMA message: JSON -> zlib stream -> BASE64,
// behind the given prefix. The inverse of what the decoder does, so a
// round-trip proves the whole envelope rather than just the JSON reader.
static char *ohma_build(const char *prefix, const char *json) {
  gsize jlen = strlen(json);
  uLongf clen = compressBound((uLong)jlen);
  Bytef *comp = g_malloc(clen);
  // Z_DEFAULT_COMPRESSION over the zlib wrapper: compress2() emits exactly the
  // RFC 1950 CMF/FLG the decoder expects to strip.
  if (compress2(comp, &clen, (const Bytef *)json, (uLong)jlen, 6) != Z_OK) {
    g_free(comp);
    return NULL;
  }
  char *b64 = g_base64_encode(comp, clen);
  g_free(comp);
  char *msg = g_strconcat(prefix, b64, NULL);
  g_free(b64);
  return msg;
}

static gboolean want(const char *what, const char *hay, const char *needle) {
  if (hay != NULL && strstr(hay, needle) != NULL) return TRUE;
  g_printerr("[HFDL ohma selftest] %s: missing \"%s\" in:\n%s\n", what, needle,
             hay ? hay : "(null)");
  return FALSE;
}

gboolean hfdl_ohma_selftest(void) {
  gboolean ok = TRUE;
  hfdl_ohma_reset();
  hfdl_ohma_set_test_clock(1000 * G_USEC_PER_SEC);

  // 1. A whole message in one block.
  {
    char *wire = ohma_build("/RTNBOCR.OHMA",
        "{\"version\":\"1.0\",\"convo_id\":\"abc-123\","
        "\"message\":\"HELLO FROM THE FLIGHT DECK\",\"iv\":\"AAAA\"}");
    GString *o = g_string_new(NULL);
    if (wire == NULL || !hfdl_ohma_decode("N999XX", wire, o, 0)) {
      g_printerr("[HFDL ohma selftest] single: not recognised as OHMA\n");
      ok = FALSE;
    } else {
      ok &= want("single", o->str, "OHMA message:");
      ok &= want("single", o->str, "Version: 1.0");
      ok &= want("single", o->str, "Msg ID: abc-123");
      ok &= want("single", o->str, "IV: AAAA");
      ok &= want("single", o->str, "HELLO FROM THE FLIGHT DECK");
    }
    g_string_free(o, TRUE);
    g_free(wire);
  }

  // 2. Three fragments delivered 2, 3, 1 — the case ACARS block reassembly
  //    cannot handle and OHMA's own sequencing must.
  {
    const char *frags[3] = {
      "{\"version\":\"1.0\",\"convo_id\":\"zz-9\",\"msg_seq\":2,\"message\":\"MIDDLE-\"}",
      "{\"version\":\"1.0\",\"convo_id\":\"zz-9\",\"msg_seq\":3,\"message\":\"END\"}",
      "{\"version\":\"1.0\",\"convo_id\":\"zz-9\",\"msg_seq\":1,\"msg_total\":3,\"message\":\"START-\"}"
    };
    const char *expect_status[3] = { "Reassembly: in progress", "Reassembly: in progress",
                                     "Reassembly: complete" };
    for (int i = 0; i < 3; i++) {
      char *wire = ohma_build("OHMA", frags[i]);
      GString *o = g_string_new(NULL);
      if (wire == NULL || !hfdl_ohma_decode("N777YY", wire, o, 0)) {
        g_printerr("[HFDL ohma selftest] frag %d: not recognised\n", i);
        ok = FALSE;
      } else {
        ok &= want("fragmented", o->str, expect_status[i]);
        if (i == 2) ok &= want("fragmented", o->str, "START-MIDDLE-END");
      }
      g_string_free(o, TRUE);
      g_free(wire);
    }
  }

  // 3. Plain text must not be claimed as OHMA.
  {
    GString *o = g_string_new(NULL);
    if (hfdl_ohma_decode("N111ZZ", "OPS NORMAL, ETA 1430Z", o, 0)) {
      g_printerr("[HFDL ohma selftest] plain text was claimed as OHMA\n");
      ok = FALSE;
    }
    g_string_free(o, TRUE);
  }

  // 4. A recognised envelope whose payload is garbage must be reported, not
  //    crash and not be silently dropped.
  {
    GString *o = g_string_new(NULL);
    if (!hfdl_ohma_decode("N111ZZ", "OHMAeJxrZ2BgYGRgYGJgYGZgYGFgYGVgYGNg", o, 0)) {
      g_printerr("[HFDL ohma selftest] corrupt payload: envelope not recognised\n");
      ok = FALSE;
    } else if (strstr(o->str, "OHMA message:") == NULL) {
      g_printerr("[HFDL ohma selftest] corrupt payload: no output\n");
      ok = FALSE;
    }
    g_string_free(o, TRUE);
  }

  hfdl_ohma_set_test_clock(0);
  hfdl_ohma_reset();
  if (ok) log_info("hfdl_ohma selftest: PASS (envelope + JSON + out-of-order reassembly)\n");
  return ok;
}
