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
#include <time.h>
#include <zlib.h>

#include "hfdl_miam.h"
#include "hfdl_util.h"
#include "log.h"

// --- the two ARINC CRCs MIAM CORE checks its bodies with --------------------
//
// Neither is one of the CRC-16s already in the tree: hfdl_crc.c carries the
// REFLECTED CCITT the HFDL FCS uses, hfdl_arinc.c the MSB-first CCITT-FALSE of
// ARINC-622. MIAM v2 uses a third combination — a non-reflected table driven by
// a reflected-style update — and MIAM v1 a 32-bit ARINC-665 CRC. Both tables
// are generated once instead of pasted as 256 constants: the same numbers, far
// less to mistype.

static uint16_t crc16_arinc(const uint8_t *d, gsize len, uint16_t crc) {
  static uint16_t table[256];
  static gboolean built = FALSE;
  if (!built) {
    for (int i = 0; i < 256; i++) {
      uint16_t c = (uint16_t)(i << 8);
      for (int b = 0; b < 8; b++)
        c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
      table[i] = c;
    }
    built = TRUE;
  }
  while (len-- > 0) crc = (uint16_t)((crc >> 8) ^ table[(crc ^ *d++) & 0xff]);
  return crc;
}

static uint32_t crc32_arinc665(const uint8_t *d, gsize len, uint32_t crc) {
  static uint32_t table[256];
  static gboolean built = FALSE;
  if (!built) {
    for (int i = 0; i < 256; i++) {
      uint32_t c = (uint32_t)i << 24;
      for (int b = 0; b < 8; b++)
        c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
      table[i] = c;
    }
    built = TRUE;
  }
  while (len-- > 0) crc = (crc << 8) ^ table[((crc >> 24) ^ *d++) & 0xff];
  return crc;
}

// --- BASE85 -----------------------------------------------------------------
//
// Five digits from '!' upwards encode one 32-bit word, big-endian; a lone 'z'
// stands for an all-zero word. Anything outside the alphabet means this is not
// a MIAM CORE header, which is how a plain-text message avoids being claimed.

typedef struct { uint8_t *buf; int len; gboolean ok; } B85;

static B85 base85_decode(const char *s, const char *end) {
  B85 r = { .buf = NULL, .len = 0, .ok = FALSE };
  if (s == NULL || end == NULL || end <= s) return r;

  static const uint32_t pow85[5] = { 85u*85u*85u*85u, 85u*85u*85u, 85u*85u, 85u, 1u };
  gsize cap = (gsize)((end - s) / 5 + 4) * 4;
  uint8_t *out = g_malloc0(cap);
  gsize n = 0;
  const char *p = s;

  while (end - p >= 5 || (p < end && *p == 'z')) {
    uint32_t v = 0;
    if (*p == 'z') {
      p++;
    } else {
      for (int i = 0; i < 5; i++, p++) {
        if (*p < 0x21 || *p > 0x7a) { g_free(out); return r; }
        v += (uint32_t)(*p - 0x21) * pow85[i];
      }
    }
    if (n + 4 > cap) { cap = cap + cap / 4 + 8; out = g_realloc(out, cap); }
    out[n++] = (uint8_t)(v >> 24);
    out[n++] = (uint8_t)(v >> 16);
    out[n++] = (uint8_t)(v >> 8);
    out[n++] = (uint8_t)v;
  }
  // A trailing partial group is what a truncated message looks like; keep what
  // decoded rather than throwing the whole PDU away.
  r.buf = out;
  r.len = (int)n;
  r.ok  = TRUE;
  return r;
}

// --- MIAM CORE PDUs ---------------------------------------------------------

#define MIAM_PDU_DATA 0
#define MIAM_PDU_ACK  1
#define MIAM_PDU_ALO  2
#define MIAM_PDU_ALR  3

#define MIAM_V1_CRC_LEN 4
#define MIAM_V2_CRC_LEN 2

#define MIAM_COMP_NONE    0x0
#define MIAM_COMP_DEFLATE 0x1

#define MIAM_APP_ACARS_2CHAR    0x0
#define MIAM_APP_ACARS_4CHAR    0x1
#define MIAM_APP_ACARS_6CHAR    0x2
#define MIAM_APP_NONACARS_6CHAR 0x3

static const char *name_or_null(const char *const *names, int n, unsigned v) {
  return (v < (unsigned)n) ? names[v] : NULL;
}

static void emit_enum(GString *out, int indent, const char *what,
                      const char *const *names, int n, unsigned v) {
  const char *s = name_or_null(names, n, v);
  if (s != NULL) hfdl_emit(out, indent, "%s: %s", what, s);
  else           hfdl_emit(out, indent, "%s: unknown (%u)", what, v);
}

static const char *const comp_names[] = { "none", "deflate" };
static const char *const enc_names[]  = { "ISO #5", "binary" };
static const char *const ack_result_names[] = {
  "ack", "nack", "time expiry", "peer abort", "local abort",
  "MIAM version not supported"
};

// Render the application id, whose meaning depends on the app type: an ACARS
// label, optionally with a sublabel and an MFI, or a free-form non-ACARS id.
static void emit_app_id(GString *out, int indent, unsigned app_type,
                        const char *app_id, int app_id_len) {
  switch (app_type) {
    case MIAM_APP_ACARS_2CHAR:
    case MIAM_APP_ACARS_4CHAR:
    case MIAM_APP_ACARS_6CHAR: {
      GString *l = g_string_new(NULL);
      g_string_append_printf(l, "Label: %c%c", app_id[0], app_id[1]);
      if (app_type == MIAM_APP_ACARS_4CHAR || app_type == MIAM_APP_ACARS_6CHAR)
        g_string_append_printf(l, "  Sublabel: %c%c", app_id[2], app_id[3]);
      if (app_type == MIAM_APP_ACARS_6CHAR)
        g_string_append_printf(l, "  MFI: %c%c", app_id[4], app_id[5]);
      hfdl_emit(out, indent, "ACARS:");
      hfdl_emit(out, indent + 1, "%s", l->str);
      g_string_free(l, TRUE);
      break;
    }
    case MIAM_APP_NONACARS_6CHAR:
    default:
      hfdl_emit(out, indent, "Non-ACARS payload:");
      hfdl_emit(out, indent + 1, "Application ID: %.*s", app_id_len, app_id);
      break;
  }
}

// The body of a Data PDU: DEFLATE or plain, then an ARINC CRC over whatever
// came out. `crc_len` picks v1 (32-bit ARINC-665) or v2 (16-bit ARINC).
static void emit_data_body(GString *out, int indent, unsigned compression,
                           const uint8_t *body, int body_len,
                           uint32_t crc_expect, int crc_len) {
  if (body == NULL || body_len <= 0) return;

  uint8_t *data = NULL;
  gsize data_len = 0;
  gboolean owned = FALSE, inflate_failed = FALSE, unsupported = FALSE;

  if (compression == MIAM_COMP_DEFLATE) {
    if (hfdl_inflate(body, body_len, &data, &data_len)) owned = TRUE;
    else inflate_failed = TRUE;
  } else if (compression == MIAM_COMP_NONE) {
    data = (uint8_t *)body;
    data_len = (gsize)body_len;
  } else {
    unsupported = TRUE;
  }

  if (data != NULL) {
    hfdl_emit(out, indent, "Message:");
    hfdl_emit_payload(out, indent + 1, data, data_len);

    uint32_t got = (crc_len == MIAM_V1_CRC_LEN)
                     ? ~crc32_arinc665(data, data_len, 0xFFFFFFFFu)
                     : crc16_arinc(data, data_len, 0xFFFFu);
    if (got != crc_expect)
      hfdl_emit(out, indent, "-- body CRC failed (got %0*x, want %0*x)",
                crc_len * 2, got, crc_len * 2, crc_expect);
  }
  if (inflate_failed) hfdl_emit(out, indent, "-- body decompression failed");
  if (unsupported)    hfdl_emit(out, indent, "-- unsupported body compression (%u)", compression);
  if (owned) g_free(data);
}

static void emit_v1_data(GString *out, int indent, const uint8_t *h, int hlen,
                         const uint8_t *body, int body_len) {
  if (hlen < 20) { hfdl_emit(out, indent, "-- header truncated"); return; }

  uint32_t pdu_len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
  h += 4; hlen -= 4;
  char aircraft_id[8];
  memcpy(aircraft_id, h, 7); aircraft_id[7] = '\0';
  h += 7; hlen -= 7;
  unsigned msg_num = (h[0] >> 1) & 0x7f, ack_option = h[0] & 1;
  h++; hlen--;
  unsigned compression = (unsigned)(((h[0] << 2) | ((h[1] >> 6) & 0x3)) & 0x7);
  unsigned encoding    = (h[1] >> 4) & 0x3;
  unsigned app_type    = h[1] & 0xf;
  h += 2; hlen -= 2;

  hfdl_emit(out, indent, "PDU length: %u", pdu_len);
  hfdl_emit(out, indent, "Aircraft ID: %s", g_strchomp(aircraft_id));
  hfdl_emit(out, indent, "Msg num: %u", msg_num);
  hfdl_emit(out, indent, "ACK: %srequired", ack_option ? "" : "not ");
  emit_enum(out, indent, "Compression", comp_names, G_N_ELEMENTS(comp_names), compression);
  emit_enum(out, indent, "Encoding", enc_names, G_N_ELEMENTS(enc_names), encoding);

  int app_id_len;
  switch (app_type) {
    case MIAM_APP_ACARS_2CHAR: app_id_len = 2; break;
    case MIAM_APP_ACARS_4CHAR: app_id_len = 4; break;
    case MIAM_APP_ACARS_6CHAR:
    case MIAM_APP_NONACARS_6CHAR: app_id_len = 6; break;
    default: hfdl_emit(out, indent, "-- unknown application type (%u)", app_type); return;
  }
  if (hlen < app_id_len + MIAM_V1_CRC_LEN) {
    hfdl_emit(out, indent, "-- header truncated");
    return;
  }
  char app_id[8] = "";
  memcpy(app_id, h, (gsize)app_id_len);
  h += app_id_len; hlen -= app_id_len;
  emit_app_id(out, indent, app_type, app_id, app_id_len);

  uint32_t crc = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                 ((uint32_t)h[2] << 8)  | h[3];
  if (pdu_len > (uint32_t)(hlen + body_len + 20))
    hfdl_emit(out, indent, "-- body truncated");
  emit_data_body(out, indent, compression, body, body_len, crc, MIAM_V1_CRC_LEN);
}

static void emit_v1_ack(GString *out, int indent, const uint8_t *h, int hlen) {
  if (hlen < 20) { hfdl_emit(out, indent, "-- header truncated"); return; }
  uint32_t pdu_len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
  h += 4;
  char aircraft_id[8];
  memcpy(aircraft_id, h, 7); aircraft_id[7] = '\0';
  h += 7;
  unsigned msg_ack_num = (h[0] >> 1) & 0x7f;
  h++;
  unsigned result = (h[0] >> 4) & 0xf;

  hfdl_emit(out, indent, "PDU length: %u", pdu_len);
  hfdl_emit(out, indent, "Aircraft ID: %s", g_strchomp(aircraft_id));
  hfdl_emit(out, indent, "Msg ack num: %u", msg_ack_num);
  emit_enum(out, indent, "Transfer result", ack_result_names,
            G_N_ELEMENTS(ack_result_names) - 1, result);   // v1 has no 0x5
}

static void emit_v2_data(GString *out, int indent, const uint8_t *h, int hlen,
                         const uint8_t *body, int body_len) {
  if (hlen < 7) { hfdl_emit(out, indent, "-- header truncated"); return; }
  h++; hlen--;                                   // version/type octet
  unsigned msg_num = (h[0] >> 1) & 0x7f, ack_option = h[0] & 1;
  h++; hlen--;
  unsigned compression = (unsigned)(((h[0] << 2) | ((h[1] >> 6) & 0x3)) & 0x7);
  unsigned encoding    = (h[1] >> 4) & 0x3;
  unsigned app_type    = h[1] & 0xf;
  h += 2; hlen -= 2;

  hfdl_emit(out, indent, "Msg num: %u", msg_num);
  hfdl_emit(out, indent, "ACK: %srequired", ack_option ? "" : "not ");
  emit_enum(out, indent, "Compression", comp_names, G_N_ELEMENTS(comp_names), compression);
  emit_enum(out, indent, "Encoding", enc_names, G_N_ELEMENTS(enc_names), encoding);

  int app_id_len;
  switch (app_type) {
    case MIAM_APP_ACARS_2CHAR: app_id_len = 2; break;
    case MIAM_APP_ACARS_4CHAR: app_id_len = 4; break;
    case MIAM_APP_ACARS_6CHAR:
    case MIAM_APP_NONACARS_6CHAR: app_id_len = 6; break;
    default:
      // v2 added variable-length ids: the top bit marks them and the low three
      // bits are the length less one. 0xd is reserved.
      if ((app_type & 0x8) != 0 && app_type != 0xd) app_id_len = (int)(app_type & 0x7) + 1;
      else { hfdl_emit(out, indent, "-- unknown application type (%u)", app_type); return; }
      break;
  }
  if (hlen < app_id_len + MIAM_V2_CRC_LEN) {
    hfdl_emit(out, indent, "-- header truncated");
    return;
  }
  char app_id[8] = "";
  memcpy(app_id, h, (gsize)app_id_len);
  h += app_id_len;
  emit_app_id(out, indent, app_type, app_id, app_id_len);

  uint32_t crc = ((uint32_t)h[0] << 8) | h[1];
  emit_data_body(out, indent, compression, body, body_len, crc, MIAM_V2_CRC_LEN);
}

static void emit_v2_ack(GString *out, int indent, const uint8_t *h, int hlen) {
  if (hlen < 8) { hfdl_emit(out, indent, "-- header truncated"); return; }
  h++;                                           // version/type octet
  unsigned msg_ack_num = (h[0] >> 1) & 0x7f;
  unsigned result = (unsigned)(((h[0] << 3) | (h[1] >> 5)) & 0xf);
  hfdl_emit(out, indent, "Msg ack num: %u", msg_ack_num);
  emit_enum(out, indent, "Transfer result", ack_result_names,
            G_N_ELEMENTS(ack_result_names), result);
}

static void emit_alo_alr(GString *out, int indent, const uint8_t *h, int hlen,
                         gboolean is_alo) {
  // 16 octets by the book, but the tail is unused, so short is not fatal.
  if (hlen < 13) { hfdl_emit(out, indent, "-- header truncated"); return; }
  uint32_t pdu_len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
  h += 4;
  char aircraft_id[8];
  memcpy(aircraft_id, h, 7); aircraft_id[7] = '\0';
  h += 7;
  unsigned compression = h[0], networks = h[1];

  static const char *const net_names[] = {
    "ACARS", "IP Middleware", "TCP/IP", "Satcom Data 3", "UDP"
  };
  hfdl_emit(out, indent, "PDU length: %u", pdu_len);
  hfdl_emit(out, indent, "Aircraft ID: %s", g_strchomp(aircraft_id));

  // Both fields are bitmasks: which algorithms / networks the sender supports
  // (Aloha) or has selected (Aloha Reply).
  const char *verb = is_alo ? "supported" : "selected";
  GString *l = g_string_new(NULL);
  if (compression & 1u) g_string_append(l, "deflate ");
  for (unsigned b = 1; b < 8; b++)
    if (compression & (1u << b)) g_string_append_printf(l, "unknown(%u) ", b);
  hfdl_emit(out, indent, "Compression %s: %s", verb, l->len ? g_strchomp(l->str) : "none");
  g_string_truncate(l, 0);
  for (unsigned b = 0; b < 8; b++) {
    if (!(networks & (1u << b))) continue;
    if (b < G_N_ELEMENTS(net_names)) g_string_append_printf(l, "%s ", net_names[b]);
    else                             g_string_append_printf(l, "unknown(%u) ", b);
  }
  hfdl_emit(out, indent, "Networks %s: %s", verb, l->len ? g_strchomp(l->str) : "none");
  g_string_free(l, TRUE);
}

// Decode a MIAM CORE PDU. Returns FALSE when the text is not one at all (so the
// caller can fall through to something else); a PDU that IS one but fails to
// parse renders its error and still returns TRUE.
static gboolean miam_core_decode(const char *txt, GString *out, int indent) {
  if (txt == NULL || strlen(txt) < 3) return FALSE;

  char bpad = txt[0], hpad_c = txt[1];
  txt += 2;
  if (!((bpad >= '0' && bpad <= '3') || bpad == '-' || bpad == '.')) return FALSE;
  if (!(hpad_c >= '0' && hpad_c <= '3')) return FALSE;
  int hpad = hpad_c - '0';

  const char *delim = strchr(txt, '|');
  if (delim == NULL || delim == txt) return FALSE;

  B85 header = base85_decode(txt, delim);
  if (!header.ok || header.len < hpad || header.len < 1) { g_free(header.buf); return FALSE; }

  B85 body = { 0 };
  const uint8_t *bodybuf = NULL;
  int bodylen = 0;
  if (delim[1] != '\0') {
    if (bpad >= '0' && bpad <= '3') {
      body = base85_decode(delim + 1, delim + 1 + strlen(delim + 1));
      int bp = bpad - '0';
      if (body.ok && body.len >= bp) body.len -= bp;   // strip padding octets
      bodybuf = body.buf;
      bodylen = body.len;
    } else if (bpad == '-') {
      bodybuf = (const uint8_t *)(delim + 1);          // body is not encoded
      bodylen = (int)strlen(delim + 1);
    }
  }

  header.len -= hpad;
  const uint8_t *h = header.buf;
  unsigned version  = h[0] & 0xf;
  unsigned pdu_type = (h[0] >> 4) & 0xf;

  static const char *const pdu_names[] = { "Data", "Ack", "Aloha", "Aloha Reply" };
  hfdl_emit(out, indent, "MIAM CORE v%u %s PDU:", version,
            (pdu_type < G_N_ELEMENTS(pdu_names)) ? pdu_names[pdu_type] : "unknown");
  indent++;

  if (version != 1 && version != 2) {
    hfdl_emit(out, indent, "-- unsupported MIAM version (%u)", version);
  } else if (pdu_type == MIAM_PDU_ALO || pdu_type == MIAM_PDU_ALR) {
    emit_alo_alr(out, indent, h, header.len, pdu_type == MIAM_PDU_ALO);
  } else if (pdu_type == MIAM_PDU_DATA) {
    if (version == 1) emit_v1_data(out, indent, h, header.len, bodybuf, bodylen);
    else              emit_v2_data(out, indent, h, header.len, bodybuf, bodylen);
  } else if (pdu_type == MIAM_PDU_ACK) {
    if (version == 1) emit_v1_ack(out, indent, h, header.len);
    else              emit_v2_ack(out, indent, h, header.len);
  } else {
    hfdl_emit(out, indent, "-- unknown PDU type (%u)", pdu_type);
  }

  g_free(header.buf);
  g_free(body.buf);
  return TRUE;
}

// --- file-transfer reassembly -----------------------------------------------
//
// A File Segment carries only a file id and a segment number — the total size
// arrives earlier, in the File Transfer Request that opened the transfer. So
// the request creates the entry and the segments fill it; without the request
// (missed, or the transfer started before we tuned in) segments still decode
// individually, they just never complete.

#define MIAM_XFER_MAX     4
#define MIAM_SEG_MAX     64
#define MIAM_XFER_TIMEOUT_US (900 * G_USEC_PER_SEC)

typedef struct {
  gboolean in_use;
  char     reg[16];
  int      file_id;
  gsize    total_len;
  gint64   first_us;
  char    *seg[MIAM_SEG_MAX];
} MIAM_XFER;

static MIAM_XFER miam_xfer[MIAM_XFER_MAX];
static gint64    miam_test_now_us = 0;

static gint64 miam_now(void) {
  return miam_test_now_us ? miam_test_now_us : g_get_monotonic_time();
}

void hfdl_miam_set_test_clock(gint64 now_us) { miam_test_now_us = now_us; }

static void miam_xfer_free(MIAM_XFER *e) {
  for (int i = 0; i < MIAM_SEG_MAX; i++) { g_free(e->seg[i]); e->seg[i] = NULL; }
  memset(e, 0, sizeof(*e));
}

void hfdl_miam_reset(void) {
  for (int i = 0; i < MIAM_XFER_MAX; i++) miam_xfer_free(&miam_xfer[i]);
}

static void miam_xfer_expire(gint64 now_us) {
  for (int i = 0; i < MIAM_XFER_MAX; i++)
    if (miam_xfer[i].in_use && now_us - miam_xfer[i].first_us > MIAM_XFER_TIMEOUT_US)
      miam_xfer_free(&miam_xfer[i]);
}

static MIAM_XFER *miam_xfer_lookup(const char *reg, int file_id) {
  for (int i = 0; i < MIAM_XFER_MAX; i++)
    if (miam_xfer[i].in_use && miam_xfer[i].file_id == file_id &&
        !strcmp(miam_xfer[i].reg, reg))
      return &miam_xfer[i];
  return NULL;
}

static void miam_xfer_open(const char *reg, int file_id, gsize total_len) {
  if (reg == NULL) return;
  gint64 now_us = miam_now();
  miam_xfer_expire(now_us);

  MIAM_XFER *e = miam_xfer_lookup(reg, file_id), *empty = NULL, *oldest = NULL;
  if (e == NULL) {
    for (int i = 0; i < MIAM_XFER_MAX; i++) {
      MIAM_XFER *c = &miam_xfer[i];
      if (!c->in_use) { if (empty == NULL) empty = c; }
      else if (oldest == NULL || c->first_us < oldest->first_us) oldest = c;
    }
    e = (empty != NULL) ? empty : oldest;
  }
  miam_xfer_free(e);
  e->in_use = TRUE;
  e->file_id = file_id;
  e->total_len = total_len;
  e->first_us = now_us;
  g_strlcpy(e->reg, reg, sizeof(e->reg));
}

typedef enum {
  MIAM_R_SKIPPED, MIAM_R_IN_PROGRESS, MIAM_R_COMPLETE, MIAM_R_DUPLICATE
} MIAM_XFER_STATUS;

// Add one segment. On completion *out is the concatenated file (caller frees).
static MIAM_XFER_STATUS miam_xfer_add(const char *reg, int file_id, int seg_id,
                                      const char *data, char **out) {
  *out = NULL;
  if (reg == NULL || data == NULL) return MIAM_R_SKIPPED;
  if (seg_id < 1 || seg_id > MIAM_SEG_MAX) return MIAM_R_SKIPPED;

  miam_xfer_expire(miam_now());
  MIAM_XFER *e = miam_xfer_lookup(reg, file_id);
  if (e == NULL) return MIAM_R_SKIPPED;          // never saw the request

  if (e->seg[seg_id - 1] != NULL) return MIAM_R_DUPLICATE;
  e->seg[seg_id - 1] = g_strdup(data);

  // Complete once a contiguous run from segment 1 holds at least the size the
  // request announced. A gap means a segment is still missing, whatever the
  // total says.
  gsize have = 0;
  for (int i = 0; i < MIAM_SEG_MAX; i++) {
    if (e->seg[i] == NULL) break;
    have += strlen(e->seg[i]);
  }
  if (e->total_len == 0 || have < e->total_len) return MIAM_R_IN_PROGRESS;

  GString *full = g_string_new(NULL);
  for (int i = 0; i < MIAM_SEG_MAX && e->seg[i] != NULL; i++)
    g_string_append(full, e->seg[i]);
  miam_xfer_free(e);
  *out = g_string_free(full, FALSE);
  return MIAM_R_COMPLETE;
}

// --- ACARS CF frames --------------------------------------------------------

// Read exactly `n` decimal digits. Returns -1 if they are not all digits.
static int read_uint(const char *s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

// YYMMDDhhmmss, as MIAM writes its transfer deadline.
static gboolean read_time(const char *s, struct tm *t) {
  int y = read_uint(s, 2), mo = read_uint(s + 2, 2), d = read_uint(s + 4, 2);
  int h = read_uint(s + 6, 2), mi = read_uint(s + 8, 2), se = read_uint(s + 10, 2);
  if (y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || se < 0) return FALSE;
  if (mo < 1 || mo > 12 || d > 31 || h > 23 || mi > 59 || se > 59) return FALSE;
  memset(t, 0, sizeof(*t));
  t->tm_year = y + 100; t->tm_mon = mo - 1; t->tm_mday = d;
  t->tm_hour = h; t->tm_min = mi; t->tm_sec = se;
  t->tm_isdst = -1;
  return TRUE;
}

// Length of the text with any trailing CR/LF ignored — the frame headers are
// fixed-width and a line ending would otherwise fail the length check.
static gsize chomped_len(const char *s) {
  gsize n = strlen(s);
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
  return n;
}

gboolean hfdl_miam_decode(const char *reg, const char *txt, GString *out, int indent) {
  if (txt == NULL || out == NULL || *txt == '\0') return FALSE;

  static const struct { char c; const char *name; } frames[] = {
    { 'T', "Single Transfer" },       { 'F', "File Transfer Request" },
    { 'K', "File Transfer Accept" },  { 'S', "File Segment" },
    { 'A', "File Transfer Abort" },   { 'Y', "File Transfer Pause" },
    { 'X', "File Transfer Resume" },
  };
  const char *frame_name = NULL;
  char fid = txt[0];
  for (gsize i = 0; i < G_N_ELEMENTS(frames); i++)
    if (frames[i].c == fid) { frame_name = frames[i].name; break; }
  if (frame_name == NULL) return FALSE;

  const char *p = txt + 1;
  gsize len = chomped_len(p);

  // Every frame but Single Transfer and File Segment is a fixed-width header,
  // so the length is what tells a real MIAM frame from a message that merely
  // starts with the same letter.
  GString *body = g_string_new(NULL);
  gboolean recognised = FALSE;

  switch (fid) {
    case 'T':
      recognised = miam_core_decode(p, body, indent + 1);
      break;

    case 'F': {
      if (len != 21) break;
      int file_id = read_uint(p, 3);
      int file_size = read_uint(p + 3, 6);
      struct tm t;
      if (file_id < 0 || file_size < 0 || !read_time(p + 9, &t)) break;
      recognised = TRUE;
      hfdl_emit(body, indent + 1, "File ID: %d", file_id);
      hfdl_emit(body, indent + 1, "File size: %d bytes", file_size);
      hfdl_emit(body, indent + 1, "Complete until: %04d-%02d-%02d %02d:%02d:%02d",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
      miam_xfer_open(reg, file_id, (gsize)file_size);
      break;
    }

    case 'K': {
      if (len != 10) break;
      int file_id = read_uint(p, 3);
      int seg_size;
      if      (p[3] >= '0' && p[3] <= '9') seg_size = p[3] - '0';
      else if (p[3] >= 'A' && p[3] <= 'F') seg_size = p[3] - 'A' + 10;
      else break;
      int onground = read_uint(p + 4, 3), inflight = read_uint(p + 7, 3);
      if (file_id < 0 || onground < 0 || inflight < 0) break;
      recognised = TRUE;
      hfdl_emit(body, indent + 1, "File ID: %d", file_id);
      hfdl_emit(body, indent + 1, "Segment size: %d", seg_size);
      hfdl_emit(body, indent + 1, "On-ground segment temporization: %d sec", onground);
      hfdl_emit(body, indent + 1, "In-flight segment temporization: %d sec", inflight);
      break;
    }

    case 'S': {
      if (len < 6) break;
      int file_id = read_uint(p, 3), seg_id = read_uint(p + 3, 3);
      if (file_id < 0 || seg_id < 0) break;
      recognised = TRUE;
      hfdl_emit(body, indent + 1, "File ID: %d", file_id);
      hfdl_emit(body, indent + 1, "Segment ID: %d", seg_id);

      char *whole = NULL;
      switch (miam_xfer_add(reg, file_id, seg_id, p + 6, &whole)) {
        case MIAM_R_IN_PROGRESS: hfdl_emit(body, indent + 1, "Reassembly: in progress"); break;
        case MIAM_R_COMPLETE:    hfdl_emit(body, indent + 1, "Reassembly: complete"); break;
        case MIAM_R_DUPLICATE:   hfdl_emit(body, indent + 1, "Reassembly: duplicate segment"); break;
        case MIAM_R_SKIPPED:     break;      // no request seen; decode what we have
      }
      // Decode the CORE PDU from the whole file if it is complete, else from
      // this segment alone — a first segment often already shows the header.
      miam_core_decode(whole ? whole : p + 6, body, indent + 1);
      g_free(whole);
      break;
    }

    case 'A': {
      if (len != 4) break;
      int file_id = read_uint(p, 3);
      if (file_id < 0 || p[3] < '0' || p[3] > '9') break;
      static const char *const reasons[] = {
        "file transfer request refused by receiver", "file segment out of context",
        "file transfer stopped by sender", "file transfer stopped by receiver",
        "file segment transmission failed"
      };
      recognised = TRUE;
      unsigned r = (unsigned)(p[3] - '0');
      hfdl_emit(body, indent + 1, "File ID: %d", file_id);
      emit_enum(body, indent + 1, "Reason", reasons, G_N_ELEMENTS(reasons), r);
      break;
    }

    case 'Y':
    case 'X': {
      gsize want = (fid == 'Y') ? 3 : 9;
      if (len != want) break;
      int file_id = (strncmp(p, "FFF", 3) == 0) ? 0xFFF : read_uint(p, 3);
      if (file_id < 0) break;
      int onground = 0, inflight = 0;
      if (fid == 'X') {
        onground = read_uint(p + 3, 3);
        inflight = read_uint(p + 6, 3);
        if (onground < 0 || inflight < 0) break;
      }
      recognised = TRUE;
      if (file_id == 0xFFF) hfdl_emit(body, indent + 1, "File ID: 0xFFF (all)");
      else                  hfdl_emit(body, indent + 1, "File ID: %d", file_id);
      if (fid == 'X') {
        hfdl_emit(body, indent + 1, "On-ground segment temporization: %d sec", onground);
        hfdl_emit(body, indent + 1, "In-flight segment temporization: %d sec", inflight);
      }
      break;
    }

    default: break;
  }

  if (!recognised) { g_string_free(body, TRUE); return FALSE; }

  hfdl_emit(out, indent, "MIAM: %s", frame_name);
  g_string_append_len(out, body->str, (gssize)body->len);
  g_string_free(body, TRUE);
  return TRUE;
}

// --- self-test --------------------------------------------------------------

// Encode octets as BASE85 the way MIAM CORE does, padding the tail to a whole
// 4-octet group; returns the padding count so the caller can put it in the
// header's padding digit.
static char *base85_encode(const uint8_t *buf, int len, int *pad_out) {
  int pad = (4 - (len % 4)) % 4;
  int n = len + pad;
  uint8_t *tmp = g_malloc0((gsize)n);
  memcpy(tmp, buf, (gsize)len);
  GString *s = g_string_new(NULL);
  for (int i = 0; i < n; i += 4) {
    uint32_t v = ((uint32_t)tmp[i] << 24) | ((uint32_t)tmp[i + 1] << 16) |
                 ((uint32_t)tmp[i + 2] << 8) | tmp[i + 3];
    char d[5];
    for (int k = 4; k >= 0; k--) { d[k] = (char)(0x21 + (v % 85)); v /= 85; }
    g_string_append_len(s, d, 5);
  }
  g_free(tmp);
  *pad_out = pad;
  return g_string_free(s, FALSE);
}

// DEFLATE without the zlib wrapper — what a MIAM CORE body carries.
static uint8_t *raw_deflate(const char *txt, gsize *out_len) {
  z_stream s;
  memset(&s, 0, sizeof(s));
  if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
  gsize cap = deflateBound(&s, (uLong)strlen(txt)) + 16;
  uint8_t *buf = g_malloc(cap);
  s.next_in = (Bytef *)txt;  s.avail_in = (uInt)strlen(txt);
  s.next_out = buf;          s.avail_out = (uInt)cap;
  int ret = deflate(&s, Z_FINISH);
  *out_len = s.total_out;
  deflateEnd(&s);
  if (ret != Z_STREAM_END) { g_free(buf); return NULL; }
  return buf;
}

// Build a whole MIAM Single Transfer around a Data PDU carrying `payload`.
static char *build_data_frame(int version, const char *label, const char *payload) {
  gsize comp_len = 0;
  uint8_t *comp = raw_deflate(payload, &comp_len);
  if (comp == NULL) return NULL;

  uint8_t hdr[32];
  memset(hdr, 0, sizeof(hdr));
  int hlen;
  if (version == 1) {
    hdr[0] = (uint8_t)((MIAM_PDU_DATA << 4) | 1);
    hdr[1] = 0; hdr[2] = 0; hdr[3] = 40;           // PDU length (not checked hard)
    memcpy(hdr + 4, "N12345 ", 7);
    hdr[11] = (uint8_t)((7 << 1) | 1);             // msg num 7, ack required
    hdr[12] = (uint8_t)(MIAM_COMP_DEFLATE >> 2);
    hdr[13] = (uint8_t)(((MIAM_COMP_DEFLATE & 0x3) << 6) | (0 << 4) | MIAM_APP_ACARS_2CHAR);
    hdr[14] = (uint8_t)label[0];
    hdr[15] = (uint8_t)label[1];
    uint32_t crc = ~crc32_arinc665((const uint8_t *)payload, strlen(payload), 0xFFFFFFFFu);
    hdr[16] = (uint8_t)(crc >> 24); hdr[17] = (uint8_t)(crc >> 16);
    hdr[18] = (uint8_t)(crc >> 8);  hdr[19] = (uint8_t)crc;
    hlen = 20;
  } else {
    hdr[0] = (uint8_t)((MIAM_PDU_DATA << 4) | 2);
    hdr[1] = (uint8_t)((3 << 1) | 0);              // msg num 3, no ack
    hdr[2] = (uint8_t)(MIAM_COMP_DEFLATE >> 2);
    hdr[3] = (uint8_t)(((MIAM_COMP_DEFLATE & 0x3) << 6) | (0 << 4) | MIAM_APP_ACARS_2CHAR);
    hdr[4] = (uint8_t)label[0];
    hdr[5] = (uint8_t)label[1];
    uint16_t crc = crc16_arinc((const uint8_t *)payload, strlen(payload), 0xFFFFu);
    hdr[6] = (uint8_t)(crc >> 8); hdr[7] = (uint8_t)crc;
    hlen = 8;
  }

  int hpad = 0, bpad = 0;
  char *h85 = base85_encode(hdr, hlen, &hpad);
  char *b85 = base85_encode(comp, (int)comp_len, &bpad);
  g_free(comp);
  char *frame = g_strdup_printf("T%d%d%s|%s", bpad, hpad, h85, b85);
  g_free(h85);
  g_free(b85);
  return frame;
}

static gboolean want(const char *what, const char *hay, const char *needle) {
  if (hay != NULL && strstr(hay, needle) != NULL) return TRUE;
  g_printerr("[HFDL miam selftest] %s: missing \"%s\" in:\n%s\n", what, needle,
             hay ? hay : "(null)");
  return FALSE;
}

gboolean hfdl_miam_selftest(void) {
  gboolean ok = TRUE;
  hfdl_miam_reset();
  hfdl_miam_set_test_clock(1000 * G_USEC_PER_SEC);

  // 1. v1 and v2 Data PDUs: deflated body, correct ARINC CRC.
  for (int version = 1; version <= 2; version++) {
    const char *payload = "WX REQUEST KJFK 1200Z";
    char *frame = build_data_frame(version, "H1", payload);
    GString *o = g_string_new(NULL);
    if (frame == NULL || !hfdl_miam_decode("N12345", frame, o, 0)) {
      g_printerr("[HFDL miam selftest] v%d data: not recognised as MIAM\n", version);
      ok = FALSE;
    } else {
      char want_hdr[64];
      g_snprintf(want_hdr, sizeof(want_hdr), "MIAM CORE v%d Data PDU:", version);
      ok &= want("data", o->str, "MIAM: Single Transfer");
      ok &= want("data", o->str, want_hdr);
      ok &= want("data", o->str, "Compression: deflate");
      ok &= want("data", o->str, "Label: H1");
      ok &= want("data", o->str, payload);
      if (strstr(o->str, "CRC failed") != NULL) {
        g_printerr("[HFDL miam selftest] v%d data: CRC wrongly rejected:\n%s\n", version, o->str);
        ok = FALSE;
      }
    }
    g_string_free(o, TRUE);
    g_free(frame);
  }

  // 2. A body whose CRC does not match must be reported, not passed off as good.
  {
    char *frame = build_data_frame(2, "H1", "GOOD PAYLOAD");
    // Flip a character of the BASE85 body so the decompressed text changes.
    if (frame != NULL) {
      char *bar = strchr(frame, '|');
      if (bar != NULL && bar[1] != '\0') bar[1] = (bar[1] == '!') ? '"' : '!';
    }
    GString *o = g_string_new(NULL);
    if (frame != NULL && hfdl_miam_decode("N12345", frame, o, 0)) {
      if (strstr(o->str, "CRC failed") == NULL &&
          strstr(o->str, "decompression failed") == NULL) {
        g_printerr("[HFDL miam selftest] corrupt body passed silently:\n%s\n", o->str);
        ok = FALSE;
      }
    }
    g_string_free(o, TRUE);
    g_free(frame);
  }

  // 3. The fixed-width control frames.
  {
    const struct { const char *txt; const char *want; } cases[] = {
      { "F001000012261231235959", "File size: 12 bytes" },
      { "K0014030060",            "Segment size: 4" },
      { "A0012",                  "file transfer stopped by sender" },
      { "YFFF",                   "File ID: 0xFFF (all)" },
      { "X001030060",             "In-flight segment temporization: 60 sec" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
      GString *o = g_string_new(NULL);
      if (!hfdl_miam_decode("N12345", cases[i].txt, o, 0)) {
        g_printerr("[HFDL miam selftest] control frame \"%s\" not recognised\n", cases[i].txt);
        ok = FALSE;
      } else {
        ok &= want("control", o->str, cases[i].want);
      }
      g_string_free(o, TRUE);
    }
  }

  // 4. A two-segment file transfer: the request opens it, the segments complete
  //    it, and the reassembled text is the CORE PDU built above.
  {
    hfdl_miam_reset();
    char *frame = build_data_frame(2, "H1", "SEGMENTED FILE CONTENT");
    gsize flen = frame ? strlen(frame + 1) : 0;      // without the 'T' frame id
    char req[32];
    g_snprintf(req, sizeof(req), "F001%06zu261231235959", (size_t)flen);
    GString *o = g_string_new(NULL);
    ok &= hfdl_miam_decode("N12345", req, o, 0);
    g_string_free(o, TRUE);

    gsize half = flen / 2;
    char *s1 = g_strdup_printf("S001001%.*s", (int)half, frame + 1);
    char *s2 = g_strdup_printf("S001002%s", frame + 1 + half);

    o = g_string_new(NULL);
    ok &= hfdl_miam_decode("N12345", s1, o, 0);
    ok &= want("segment", o->str, "Reassembly: in progress");
    g_string_free(o, TRUE);

    o = g_string_new(NULL);
    ok &= hfdl_miam_decode("N12345", s2, o, 0);
    ok &= want("segment", o->str, "Reassembly: complete");
    ok &= want("segment", o->str, "SEGMENTED FILE CONTENT");
    g_string_free(o, TRUE);

    g_free(s1); g_free(s2); g_free(frame);
  }

  // 5. Ordinary message text must not be claimed as MIAM. "TEST" starts with a
  //    valid frame id, which is exactly the false positive to guard against.
  {
    const char *texts[] = { "TEST MESSAGE", "OPS NORMAL", "F001", "SFO ARRIVAL" };
    for (gsize i = 0; i < G_N_ELEMENTS(texts); i++) {
      GString *o = g_string_new(NULL);
      if (hfdl_miam_decode("N12345", texts[i], o, 0)) {
        g_printerr("[HFDL miam selftest] plain text \"%s\" was claimed as MIAM:\n%s\n",
                   texts[i], o->str);
        ok = FALSE;
      }
      g_string_free(o, TRUE);
    }
  }

  hfdl_miam_set_test_clock(0);
  hfdl_miam_reset();
  if (ok) log_info("hfdl_miam selftest: PASS (CORE v1/v2 + CRC + control frames + file reassembly)\n");
  return ok;
}
