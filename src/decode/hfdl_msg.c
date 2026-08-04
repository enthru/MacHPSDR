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
 * HFDL application layer — see hfdl_msg.h for the design and what is (and is
 * not) ported. Bit-level parsing follows dumphfdl's spdu.c / mpdu.c / lpdu.c /
 * hfnpdu.c (GPL-3.0); the ACARS block parse follows libacars' acars.c (MIT).
 * Output rendering is our own.
 */

#include <glib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "hfdl_crc.h"      // vendored crc16_ccitt (hfdl_lib/) — same table as la_crc16_ccitt
#include "hfdl_frame.h"    // test seam: full-stack self-test through the RF chain
#include "hfdl_msg.h"
#include "hfdl_pdu.h"      // hfdl_pdu_fcs_check

// --- PDU / LPDU / HFNPDU type codes (dumphfdl) -----------------------------

#define SPDU_LEN                    66
#define SPDU_HDR                    64
#define GS_STATUS_CNT               3
#define GS_MAX_FREQ_CNT             20

#define LPDU_UNNUMBERED_DATA        0x0D
#define LPDU_UNNUMBERED_ACKED_DATA  0x1D
#define LPDU_LOGON_DENIED           0x2F
#define LPDU_LOGOFF_REQUEST         0x3F
#define LPDU_LOGON_RESUME           0x4F
#define LPDU_LOGON_RESUME_CONFIRM   0x5F
#define LPDU_LOGON_REQUEST_NORMAL   0x8F
#define LPDU_LOGON_CONFIRM          0x9F
#define LPDU_LOGON_REQUEST_DLS      0xBF

#define HFNPDU_SYSTEM_TABLE         0xD0
#define HFNPDU_PERFORMANCE_DATA     0xD1
#define HFNPDU_SYSTEM_TABLE_REQUEST 0xD2
#define HFNPDU_FREQUENCY_DATA       0xD5
#define HFNPDU_DELAYED_ECHO         0xDE
#define HFNPDU_ENVELOPED_DATA       0xFF

// ACARS control bytes (libacars acars.c)
#define ACARS_SOH 0x01
#define ACARS_STX 0x02
#define ACARS_ETX 0x03
#define ACARS_ETB 0x17
#define ACARS_ACK 0x06
#define ACARS_NAK 0x15
#define ACARS_DEL 0x7f
#define ACARS_PREAMBLE_LEN 16       // incl. CRC + DEL, excl. SOH
#define IS_DOWNLINK_BLK(b) ((b) >= '0' && (b) <= '9')

// --- ground-station table --------------------------------------------------
//
// Snapshot of dumphfdl's etc/systable.conf (version 52). The real table is
// broadcast over the air in System-table HFNPDUs; reassembling those needs a
// real signal to develop against, so until then this static copy supplies the
// station names and the per-station frequency slots that Performance-data and
// Frequency-data HFNPDUs address by index. Frequencies are kHz.

typedef struct {
  uint8_t     id;
  uint8_t     freq_cnt;
  const char *name;
  uint32_t    freqs[GS_MAX_FREQ_CNT];
} HFDL_GS;

static const HFDL_GS gs_table[] = {
  {  1,  8, "San Francisco, California",
    { 21934, 17919, 13276, 11327, 10081, 8927, 6559, 5508 } },
  {  2, 12, "Molokai, Hawaii",
    { 21937, 17919, 13324, 13312, 13276, 11348, 11312, 10027, 8936, 8912, 6565, 5514 } },
  {  3,  7, "Reykjavik, Iceland",
    { 17985, 15025, 11184, 8977, 6712, 5720, 3900 } },
  {  4,  7, "Riverhead, New York",
    { 21931, 17919, 13276, 11387, 8912, 6661, 5652 } },
  {  5,  6, "Auckland, New Zealand",
    { 17916, 13351, 10084, 8921, 6535, 5583 } },
  {  6,  7, "Hat Yai, Thailand",
    { 21949, 17928, 13270, 10066, 8825, 6535, 5655 } },
  {  7,  8, "Shannon, Ireland",
    { 11384, 10081, 8942, 8843, 6532, 5547, 3455, 2998 } },
  {  8,  8, "Johannesburg, South Africa",
    { 21949, 17922, 13321, 11321, 8834, 5529, 4681, 3016 } },
  {  9, 19, "Barrow, Alaska",
    { 21937, 21928, 17934, 17919, 11354, 10093, 10027, 8936, 8927, 6646,
      5544, 5538, 5529, 4687, 4654, 3497, 3007, 2992, 2944 } },
  { 10,  8, "Muan, South Korea",
    { 21931, 17958, 13342, 10060, 8939, 6619, 5502, 2941 } },
  { 11,  6, "Albrook, Panama",
    { 17901, 13264, 10063, 8894, 6589, 5589 } },
  { 13,  7, "Santa Cruz, Bolivia",
    { 21997, 17916, 13315, 11318, 8957, 6628, 4660 } },
  { 14,  7, "Krasnoyarsk, Russia",
    { 21990, 17912, 13321, 10087, 8886, 6596, 5622 } },
  { 15,  8, "Al Muharraq, Bahrain",
    { 21982, 17967, 13312, 10030, 8885, 6646, 5544, 2986 } },
  { 16,  7, "Agana, Guam",
    { 21928, 17919, 13312, 11306, 8927, 6652, 5451 } },
  { 17,  6, "Canarias, Spain",
    { 21955, 17928, 13303, 11348, 8948, 6529 } },
};

static const HFDL_GS *gs_lookup(uint8_t gs_id) {
  for (guint i = 0; i < G_N_ELEMENTS(gs_table); i++)
    if (gs_table[i].id == gs_id) return &gs_table[i];
  return NULL;
}

const char *hfdl_msg_gs_name(uint8_t gs_id) {
  const HFDL_GS *gs = gs_lookup(gs_id);
  return gs ? gs->name : NULL;
}

// kHz for a station's frequency slot, or 0 if unknown.
static uint32_t gs_freq(uint8_t gs_id, int slot) {
  const HFDL_GS *gs = gs_lookup(gs_id);
  if (gs == NULL || slot < 0 || slot >= gs->freq_cnt) return 0;
  return gs->freqs[slot];
}

// --- aircraft-id cache ------------------------------------------------------
//
// A downlink MPDU identifies the aircraft only by the 1-byte ID the ground
// station assigned it at logon; the ICAO address appears just in the logon
// exchange. dumphfdl keeps an (frequency, ac_id) -> ICAO cache for this; we
// decode one channel at a time, so ac_id alone is the key. Audio-thread only.

static uint32_t ac_cache[256];      // 0 = unknown

static void ac_cache_set(uint8_t ac_id, uint32_t icao) { ac_cache[ac_id] = icao; }
static void ac_cache_del(uint32_t icao) {
  for (int i = 0; i < 256; i++) if (ac_cache[i] == icao) ac_cache[i] = 0;
}
static uint32_t ac_cache_get(uint8_t ac_id) { return ac_cache[ac_id]; }

void hfdl_msg_reset(void) { memset(ac_cache, 0, sizeof(ac_cache)); }

// --- small field helpers (dumphfdl util.c) ---------------------------------

#define REVERSE_BYTE(x) \
  (uint8_t)((((x) * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32)

// The ICAO address is carried with each byte's bit order reversed.
static uint32_t parse_icao_hex(const uint8_t buf[3]) {
  uint32_t r = 0;
  for (int i = 0; i < 3; i++) r |= (uint32_t)REVERSE_BYTE(buf[i]) << (8 * (2 - i));
  return r;
}

// 20-bit signed coordinate, full scale = 180 degrees.
static double parse_coordinate(uint32_t c) {
  int32_t r = (c & 0x80000u) ? (int32_t)(c | 0xFFF00000u) : (int32_t)(c & 0xFFFFFu);
  return r * 180.0 / (double)0x7ffff;
}

static uint16_t u16le(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

// Seconds-of-day -> hh:mm:ss.
static void parse_utc(uint32_t t, int *h, int *m, int *s) {
  *h = (int)(t / 3600); *m = (int)(t % 3600 / 60); *s = (int)(t % 60);
}

static void indent_line(GString *out, int indent) {
  for (int i = 0; i < indent; i++) g_string_append(out, "  ");
}

G_GNUC_PRINTF(3, 4)
static void emit(GString *out, int indent, const char *fmt, ...) {
  va_list ap;
  indent_line(out, indent);
  va_start(ap, fmt);
  g_string_append_vprintf(out, fmt, ap);
  va_end(ap);
  g_string_append_c(out, '\n');
}

// "12 (3C6444)" — aircraft id plus its cached ICAO address, when known.
static void append_ac_id(GString *out, uint8_t ac_id) {
  uint32_t icao = ac_cache_get(ac_id);
  if (icao) g_string_append_printf(out, "%u (%06X)", ac_id, icao);
  else      g_string_append_printf(out, "%u", ac_id);
}

static void append_gs_id(GString *out, uint8_t gs_id) {
  const char *name = hfdl_msg_gs_name(gs_id);
  if (name) g_string_append_printf(out, "%u (%s)", gs_id, name);
  else      g_string_append_printf(out, "%u", gs_id);
}

// Bitmap of a station's frequency slots -> "13351, 8921 kHz" (slot indices if
// the station isn't in the embedded table).
static void append_freq_list(GString *out, uint8_t gs_id, uint32_t freqs) {
  gboolean first = TRUE, any_khz = FALSE;
  for (int i = 0; i < GS_MAX_FREQ_CNT; i++) {
    if (!((freqs >> i) & 1)) continue;
    uint32_t f = gs_freq(gs_id, i);
    g_string_append_printf(out, "%s", first ? "" : ", ");
    if (f) { g_string_append_printf(out, "%u", f); any_khz = TRUE; }
    else     g_string_append_printf(out, "#%d", i);
    first = FALSE;
  }
  if (first) g_string_append(out, "none");
  else if (any_khz) g_string_append(out, " kHz");
}

// --- ACARS block (libacars la_acars_parse_and_reassemble) -------------------
//
// buf starts at the SOH byte. Renders the message header + text. Multi-block
// reassembly is not implemented: each block is shown as it arrives (its block
// id / sequence tell the operator it is a fragment).

static gboolean acars_decode(const uint8_t *buf, int len, GString *out, int indent) {
  if (len < 1 || buf[0] != ACARS_SOH) return FALSE;
  buf++; len--;

  if (len < ACARS_PREAMBLE_LEN)   { emit(out, indent, "ACARS: truncated"); return FALSE; }
  if (buf[len - 1] != ACARS_DEL)  { emit(out, indent, "ACARS: no DEL"); return FALSE; }
  len--;

  // CRC-16-CCITT over the block including its own 2 CRC bytes must come to 0.
  gboolean crc_ok = (crc16_ccitt((uint8_t *)buf, (uint32_t)len, 0) == 0);
  len -= 2;

  // Strip the odd-parity bit off every byte.
  char *txt = g_malloc0((gsize)len + 1);
  for (int i = 0; i < len; i++) txt[i] = (char)(buf[i] & 0x7f);

  gboolean final_block;
  if      (txt[len - 1] == ACARS_ETX) final_block = TRUE;
  else if (txt[len - 1] == ACARS_ETB) final_block = FALSE;
  else { emit(out, indent, "ACARS: no ETX/ETB"); g_free(txt); return FALSE; }
  len--;

  char *p = txt;
  int remaining = len;

  char mode = *p++; remaining--;
  char reg[8];  memcpy(reg, p, 7); reg[7] = '\0'; p += 7; remaining -= 7;
  char ack = *p++; remaining--;
  if      (ack == ACARS_NAK) ack = '!';
  else if (ack == ACARS_ACK) ack = '^';
  char label[3] = { p[0], p[1], '\0' }; p += 2; remaining -= 2;
  if (label[1] == 0x7f) label[1] = 'd';
  char block_id = *p++; remaining--;
  if (block_id == 0) block_id = ' ';

  gboolean downlink = IS_DOWNLINK_BLK(block_id);
  char msg_num[5] = "";
  char flight_id[7] = "";

  if (remaining >= 1) {
    if (*p != ACARS_STX) { emit(out, indent, "ACARS: no STX"); g_free(txt); return FALSE; }
    p++; remaining--;
    for (int i = 0; i < remaining; i++) if (p[i] == 0) p[i] = '.';
    if (downlink) {
      if (remaining < 10) { emit(out, indent, "ACARS: downlink text too short"); g_free(txt); return FALSE; }
      memcpy(msg_num, p, 4); msg_num[4] = '\0';      // 3-char number + sequence char
      p += 4; remaining -= 4;
      memcpy(flight_id, p, 6); flight_id[6] = '\0';
      p += 6; remaining -= 6;
    }
  } else if (downlink) {
    emit(out, indent, "ACARS: no text in downlink"); g_free(txt); return FALSE;
  }

  emit(out, indent, "ACARS %s  Reg: %s  Label: %s  Blk: %c%s  Mode: %c  Ack: %c  CRC %s",
       downlink ? "downlink" : "uplink", g_strchomp(reg), label, block_id,
       final_block ? "" : " (more)", mode, ack, crc_ok ? "OK" : "FAIL");
  if (downlink)
    emit(out, indent + 1, "Flight: %s  Msg no: %s", g_strchomp(flight_id), msg_num);

  if (remaining > 0) {
    // Render the message text line by line, with non-printables shown as '.'.
    GString *line = g_string_new(NULL);
    for (int i = 0; i <= remaining; i++) {
      char c = (i < remaining) ? p[i] : '\n';
      if (c == '\r') continue;
      if (c == '\n') {
        if (line->len > 0) emit(out, indent + 1, "| %s", line->str);
        g_string_truncate(line, 0);
        continue;
      }
      g_string_append_c(line, g_ascii_isprint(c) ? c : '.');
    }
    g_string_free(line, TRUE);
  }

  g_free(txt);
  return TRUE;
}

// --- HFNPDU ----------------------------------------------------------------

static const char *hfnpdu_type_name(uint8_t t) {
  switch (t) {
    case HFNPDU_SYSTEM_TABLE:         return "System table (partial)";
    case HFNPDU_PERFORMANCE_DATA:     return "Performance data";
    case HFNPDU_SYSTEM_TABLE_REQUEST: return "System table request";
    case HFNPDU_FREQUENCY_DATA:       return "Frequency data";
    case HFNPDU_DELAYED_ECHO:         return "Delayed echo";
    case HFNPDU_ENVELOPED_DATA:       return "Enveloped data";
    default:                          return NULL;
  }
}

static const char *freq_change_cause(uint8_t code) {
  static const char *d[8] = {
    "First freq. search in this flight leg", "Too many NACKs",
    "SPDUs no longer received", "HFDL disabled", "GS frequency change",
    "GS down / channel down", "Poor uplink channel quality", "No change"
  };
  return d[code & 7];
}

// Flight ID / position / UTC prefix shared by performance- and frequency-data.
static void emit_flight_pos(GString *out, int indent, const uint8_t *buf) {
  char flight_id[7];
  memcpy(flight_id, buf + 2, 6); flight_id[6] = '\0';
  g_strchomp(flight_id);

  uint32_t c = buf[8] | (uint32_t)buf[9] << 8 | (uint32_t)(buf[10] & 0xF) << 16;
  double lat = parse_coordinate(c);
  c = (uint32_t)(buf[10] & 0xF0) >> 4 | (uint32_t)buf[11] << 4 | (uint32_t)buf[12] << 12;
  double lon = parse_coordinate(c);
  int h, m, s;
  parse_utc(2u * u16le(buf + 13), &h, &m, &s);

  // Locale-independent: with a comma decimal separator, "Pos: 44,9997, -29,9999"
  // is unreadable — and the app runs under the user's locale.
  char latbuf[G_ASCII_DTOSTR_BUF_SIZE], lonbuf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(latbuf, sizeof(latbuf), "%.4f", lat);
  g_ascii_formatd(lonbuf, sizeof(lonbuf), "%.4f", lon);
  emit(out, indent, "Flight: %s   Pos: %s, %s   Time: %02d:%02d:%02d",
       flight_id[0] ? flight_id : "(none)", latbuf, lonbuf, h, m, s);
}

#define PERFORMANCE_DATA_LEN 47
#define FREQUENCY_DATA_MIN_LEN 15
#define PROP_FREQ_DATA_LEN 6
#define PROP_FREQS_CNT_MAX 6

static void hfnpdu_decode(const uint8_t *buf, int len, GString *out, int indent) {
  if (len < 1) return;
  if (buf[0] != 0xFF) {
    emit(out, indent, "Non-HFNPDU payload (%d octets)", len);
    return;
  }
  if (len < 2) { emit(out, indent, "HFNPDU: truncated"); return; }

  uint8_t type = buf[1];
  const char *name = hfnpdu_type_name(type);
  if (name) emit(out, indent, "HFNPDU: %s", name);
  else      emit(out, indent, "HFNPDU: unknown type (0x%02x)", type);
  indent++;

  switch (type) {
    case HFNPDU_SYSTEM_TABLE:
      if (len < 5) { emit(out, indent, "truncated"); break; }
      emit(out, indent, "Version: %u   Part %u of %u",
           (unsigned)(buf[3] >> 4 | buf[4] << 4),
           (unsigned)((buf[2] & 0xF) + 1), (unsigned)((buf[2] >> 4) + 1));
      break;

    case HFNPDU_SYSTEM_TABLE_REQUEST:
      if (len < 4) { emit(out, indent, "truncated"); break; }
      emit(out, indent, "Request data: 0x%04x", u16le(buf + 2));
      break;

    case HFNPDU_PERFORMANCE_DATA: {
      if (len < PERFORMANCE_DATA_LEN) { emit(out, indent, "truncated"); break; }
      emit_flight_pos(out, indent, buf);
      uint8_t gs_id = buf[17] & 0x7F, freq_id = buf[18];
      uint32_t f = gs_freq(gs_id, freq_id);
      GString *g = g_string_new(NULL);
      append_gs_id(g, gs_id);
      if (f) emit(out, indent, "GS: %s   Freq: %u kHz   Leg: %u", g->str, f, buf[16]);
      else   emit(out, indent, "GS: %s   Freq slot: %u   Leg: %u", g->str, freq_id, buf[16]);
      g_string_free(g, TRUE);
      emit(out, indent, "MPDUs rx: %u/%u/%u/%u  tx: %u/%u/%u/%u (300/600/1200/1800 bps)",
           buf[30], buf[29], buf[28], buf[27], buf[41], buf[40], buf[39], buf[38]);
      emit(out, indent, "SPDUs rx: %u  missed: %u   Last freq change: %s",
           u16le(buf + 35), buf[37], freq_change_cause(buf[46] & 0xF));
      break;
    }

    case HFNPDU_FREQUENCY_DATA: {
      if (len < FREQUENCY_DATA_MIN_LEN) { emit(out, indent, "truncated"); break; }
      emit_flight_pos(out, indent, buf);
      for (int f = 0; f < PROP_FREQS_CNT_MAX; f++) {
        int pos = FREQUENCY_DATA_MIN_LEN + f * PROP_FREQ_DATA_LEN;
        if (pos + PROP_FREQ_DATA_LEN > len) break;
        uint8_t gs_id = buf[pos] & 0x7F;
        uint32_t heard = buf[pos + 1] | (uint32_t)buf[pos + 2] << 8 |
                         (uint32_t)(buf[pos + 3] & 0xF) << 16;
        uint32_t tuned = (uint32_t)(buf[pos + 3] & 0xF0) >> 4 |
                         (uint32_t)buf[pos + 4] << 4 | (uint32_t)buf[pos + 5] << 12;
        GString *g = g_string_new(NULL);
        append_gs_id(g, gs_id);
        emit(out, indent, "GS %s", g->str);
        g_string_truncate(g, 0);
        append_freq_list(g, gs_id, tuned);
        emit(out, indent + 1, "Listening on: %s", g->str);
        g_string_truncate(g, 0);
        append_freq_list(g, gs_id, heard);
        emit(out, indent + 1, "Heard on: %s", g->str);
        g_string_free(g, TRUE);
      }
      break;
    }

    case HFNPDU_ENVELOPED_DATA:
      if (!acars_decode(buf + 2, len - 2, out, indent))
        emit(out, indent, "Non-ACARS payload (%d octets)", len - 2);
      break;

    case HFNPDU_DELAYED_ECHO:
    default:
      break;
  }
}

// --- LPDU ------------------------------------------------------------------

static const char *lpdu_type_name(uint8_t t) {
  switch (t) {
    case LPDU_UNNUMBERED_DATA:       return "Unnumbered data";
    case LPDU_UNNUMBERED_ACKED_DATA: return "Unnumbered ack'ed data";
    case LPDU_LOGON_DENIED:          return "Logon denied";
    case LPDU_LOGOFF_REQUEST:        return "Logoff request";
    case LPDU_LOGON_RESUME:          return "Logon resume";
    case LPDU_LOGON_RESUME_CONFIRM:  return "Logon resume confirm";
    case LPDU_LOGON_REQUEST_NORMAL:  return "Logon request (normal)";
    case LPDU_LOGON_CONFIRM:         return "Logon confirm";
    case LPDU_LOGON_REQUEST_DLS:     return "Logon request (DLS)";
    default:                         return NULL;
  }
}

static const char *logoff_reason(uint8_t code) {
  switch (code) {
    case 0x01: return "Not within slot boundaries";
    case 0x02: return "Downlink set in uplink slot";
    case 0x03: return "RLS protocol error";
    case 0x04: return "Invalid aircraft ID";
    case 0x05: return "GS subsystem does not support RLS";
    case 0x06: return "Other";
    default:   return "Reserved";
  }
}

static const char *logon_denied_reason(uint8_t code) {
  switch (code) {
    case 0x01: return "Aircraft ID not available";
    case 0x02: return "GS subsystem does not support RLS";
    default:   return "Reserved";
  }
}

static void lpdu_decode(const uint8_t *buf, int len, GString *out, int indent) {
  if (len < 3) { emit(out, indent, "LPDU: truncated"); return; }

  len -= 2;                                   // strip the LPDU's own FCS
  if (!hfdl_pdu_fcs_check(buf, len)) { emit(out, indent, "LPDU: FCS FAIL"); return; }

  uint8_t type = buf[0];
  const char *name = lpdu_type_name(type);
  if (name) emit(out, indent, "LPDU: %s", name);
  else      emit(out, indent, "LPDU: unknown type (0x%02x)", type);

  int consumed = 0;
  switch (type) {
    case LPDU_UNNUMBERED_DATA:
    case LPDU_UNNUMBERED_ACKED_DATA:
      consumed = 1;                           // an HFNPDU follows
      break;

    case LPDU_LOGON_DENIED:
    case LPDU_LOGOFF_REQUEST: {
      if (len < 5) { emit(out, indent + 1, "truncated"); return; }
      uint32_t icao = parse_icao_hex(buf + 1);
      ac_cache_del(icao);
      emit(out, indent + 1, "ICAO: %06X   Reason: %u (%s)", icao, buf[4],
           type == LPDU_LOGON_DENIED ? logon_denied_reason(buf[4]) : logoff_reason(buf[4]));
      consumed = 5;
      break;
    }

    case LPDU_LOGON_CONFIRM:
    case LPDU_LOGON_RESUME_CONFIRM: {
      if (len < 8) { emit(out, indent + 1, "truncated"); return; }
      uint32_t icao = parse_icao_hex(buf + 1);
      ac_cache_set(buf[4], icao);             // the ID every later frame uses
      emit(out, indent + 1, "ICAO: %06X   Assigned AC ID: %u", icao, buf[4]);
      consumed = 8;
      break;
    }

    case LPDU_LOGON_RESUME:
    case LPDU_LOGON_REQUEST_NORMAL:
    case LPDU_LOGON_REQUEST_DLS: {
      if (len < 4) { emit(out, indent + 1, "truncated"); return; }
      emit(out, indent + 1, "ICAO: %06X", parse_icao_hex(buf + 1));
      consumed = 4;
      break;
    }

    default:
      consumed = len;                         // unknown: nothing more to parse
      break;
  }

  if (consumed > 0 && consumed < len)
    hfnpdu_decode(buf + consumed, len - consumed, out, indent + 1);
}

// Walk lpdu_cnt LPDUs whose sizes come from the MPDU header. Returns the number
// of data octets consumed, or -1 if an LPDU runs past the end of the frame.
static int lpdu_list_decode(const uint8_t *size_ptr, const uint8_t *data_ptr,
                            const uint8_t *end, int lpdu_cnt, GString *out, int indent) {
  int consumed = 0;
  for (int i = 0; i < lpdu_cnt; i++) {
    int lpdu_len = size_ptr[i] + 1;
    if (data_ptr + lpdu_len > end) return -1;
    lpdu_decode(data_ptr, lpdu_len, out, indent);
    data_ptr += lpdu_len;
    consumed  += lpdu_len;
  }
  return consumed;
}

// --- SPDU (ground-station squitter) ----------------------------------------

static void spdu_decode(const uint8_t *buf, GString *out) {
  uint8_t src = buf[1] & 0x7F;
  GString *g = g_string_new(NULL);
  append_gs_id(g, src);
  emit(out, 0, "HFDL SPDU (squitter) from GS %s", g->str);

  static const char *change_note[4] = {
    "None", "Channel down", "Upcoming frequency change", "Ground station down"
  };
  emit(out, 1, "Version: %u   RLS: %s   ISO-8208: %s   Change note: %s",
       (unsigned)((buf[0] >> 2) & 3), (buf[0] & 2) ? "yes" : "no",
       (buf[0] & 0x20) ? "yes" : "no", change_note[(buf[0] & 0xC0) >> 6]);
  emit(out, 1, "TDMA frame: index %u offset %u   Min priority: %u   Systable ver: %u",
       (unsigned)(buf[2] | ((buf[3] & 0xF) << 8)), (unsigned)(buf[3] >> 4),
       (unsigned)(buf[52] & 0xF), (unsigned)(buf[53] | ((buf[54] & 0xF) << 8)));

  struct { uint8_t id; gboolean utc; uint32_t freqs; } gs[GS_STATUS_CNT] = {
    { src,                            (buf[1] & 0x80) != 0,
      (uint32_t)buf[54] >> 4 | (uint32_t)buf[55] << 4 | (uint32_t)buf[56] << 12 },
    { (uint8_t)(buf[57] & 0x7F),      (buf[57] & 0x80) != 0,
      (uint32_t)buf[58] | (uint32_t)buf[59] << 8 | (uint32_t)(buf[60] & 0xF) << 16 },
    { (uint8_t)(buf[60] >> 4 | (buf[61] & 0x7) << 4), (buf[61] & 0x8) != 0,
      (uint32_t)buf[61] >> 4 | (uint32_t)buf[62] << 4 | (uint32_t)buf[63] << 12 },
  };
  emit(out, 1, "Ground station status:");
  for (int i = 0; i < GS_STATUS_CNT; i++) {
    g_string_truncate(g, 0);
    append_gs_id(g, gs[i].id);
    emit(out, 2, "GS %s   UTC sync: %s", g->str, gs[i].utc ? "yes" : "no");
    g_string_truncate(g, 0);
    append_freq_list(g, gs[i].id, gs[i].freqs);
    emit(out, 3, "Freqs in use: %s", g->str);
  }
  g_string_free(g, TRUE);
}

// --- MPDU / entry point -----------------------------------------------------

gboolean hfdl_msg_decode(const uint8_t *buf, int len, GString *out) {
  if (buf == NULL || out == NULL) return FALSE;
  if (len < 3) { emit(out, 0, "HFDL frame: too short (%d)", len); return FALSE; }

  if ((buf[0] & 1) == 0) {                    // SPDU
    if (len < SPDU_LEN)                 { emit(out, 0, "HFDL SPDU: too short (%d)", len); return FALSE; }
    if (!hfdl_pdu_fcs_check(buf, SPDU_HDR)) { emit(out, 0, "HFDL SPDU: FCS FAIL"); return FALSE; }
    spdu_decode(buf, out);
    return TRUE;
  }

  // MPDU: work out the header length, FCS-check it, then walk the LPDUs.
  gboolean downlink = (buf[0] & 0x2) != 0;
  int aircraft_cnt = 0, lpdu_cnt = 0, hdr_len;

  if (downlink) {
    lpdu_cnt = (buf[0] >> 2) & 0xF;
    hdr_len = 6 + lpdu_cnt;
  } else {
    aircraft_cnt = ((buf[0] & 0x70) >> 4) + 1;
    hdr_len = 2;
    for (int i = 0; i < aircraft_cnt; i++) {
      if (len < hdr_len + 2) { emit(out, 0, "HFDL MPDU uplink: too short (%d)", len); return FALSE; }
      hdr_len += 2 + (buf[hdr_len + 1] >> 4);
    }
  }
  if (len < hdr_len + 2) { emit(out, 0, "HFDL MPDU: too short (%d)", len); return FALSE; }
  if (!hfdl_pdu_fcs_check(buf, hdr_len)) { emit(out, 0, "HFDL MPDU: FCS FAIL"); return FALSE; }

  const uint8_t *end     = buf + len;
  const uint8_t *dataptr = buf + hdr_len + 2;   // first LPDU data octet
  GString *g = g_string_new(NULL);

  if (downlink) {
    uint8_t src_ac = buf[2], dst_gs = buf[1] & 0x7f;
    append_ac_id(g, src_ac);
    g_string_append(g, "  ->  GS ");
    append_gs_id(g, dst_gs);
    emit(out, 0, "HFDL downlink MPDU (air->gnd)  AC %s", g->str);
    if (lpdu_list_decode(buf + 6, dataptr, end, lpdu_cnt, out, 1) < 0)
      emit(out, 1, "LPDU list truncated");
  } else {
    uint8_t src_gs = buf[1] & 0x7f;
    append_gs_id(g, src_gs);
    emit(out, 0, "HFDL uplink MPDU (gnd->air)  GS %s  %d aircraft", g->str, aircraft_cnt);
    const uint8_t *hdrptr = buf + 2;            // first AC ID octet
    for (int i = 0; i < aircraft_cnt; i++) {
      uint8_t dst_ac = *hdrptr++;
      lpdu_cnt = (*hdrptr++ >> 4) & 0xF;
      g_string_truncate(g, 0);
      append_ac_id(g, dst_ac);
      emit(out, 1, "To AC %s  (%d LPDU)", g->str, lpdu_cnt);
      int consumed = lpdu_list_decode(hdrptr, dataptr, end, lpdu_cnt, out, 2);
      if (consumed < 0) { emit(out, 2, "LPDU list truncated"); break; }
      hdrptr  += lpdu_cnt;
      dataptr += consumed;
    }
  }
  g_string_free(g, TRUE);
  return TRUE;
}

// --- self-test --------------------------------------------------------------
//
// Builds real frames byte by byte (correct FCS at every layer, correct ACARS
// CRC) and asserts the rendered text. This is what makes the app layer
// verifiable without an off-air recording: the framer's synthetic frames carry
// random bytes, so the structure has to be synthesised here.

// Append the little-endian FCS over the first hdr_len bytes.
static void fcs_append(uint8_t *buf, int hdr_len) {
  uint16_t c = (uint16_t)(crc16_ccitt(buf, (uint32_t)hdr_len, 0xFFFFu) ^ 0xFFFFu);
  buf[hdr_len]     = (uint8_t)(c & 0xff);
  buf[hdr_len + 1] = (uint8_t)(c >> 8);
}

// Wrap one LPDU (already complete, FCS included) in a 1-LPDU downlink MPDU.
static int build_downlink_mpdu(uint8_t *out, uint8_t src_ac, uint8_t dst_gs,
                               const uint8_t *lpdu, int lpdu_len) {
  out[0] = (uint8_t)(0x01 | 0x02 | (1 << 2));   // MPDU + downlink + 1 LPDU
  out[1] = dst_gs;
  out[2] = src_ac;
  out[3] = out[4] = out[5] = 0;
  out[6] = (uint8_t)(lpdu_len - 1);             // LPDU size octet
  fcs_append(out, 7);
  memcpy(out + 9, lpdu, (size_t)lpdu_len);
  return 9 + lpdu_len;
}

// LPDU type octet + payload, with the LPDU FCS appended.
static int build_lpdu(uint8_t *out, uint8_t type, const uint8_t *payload, int plen) {
  out[0] = type;
  if (plen > 0) memcpy(out + 1, payload, (size_t)plen);
  fcs_append(out, 1 + plen);
  return 1 + plen + 2;
}

static void icao_encode(uint8_t *out, uint32_t icao) {
  for (int i = 0; i < 3; i++) out[i] = REVERSE_BYTE((icao >> (8 * (2 - i))) & 0xff);
}

// A complete ACARS block: SOH .. DEL, with a valid CRC.
static int build_acars(uint8_t *out, const char *reg, const char *label,
                       char block_id, const char *flight, const char *msg_num,
                       const char *text) {
  int n = 0;
  out[n++] = ACARS_SOH;
  int crc_start = n;
  out[n++] = '2';                               // mode
  memcpy(out + n, reg, 7); n += 7;
  out[n++] = 0x15;                              // NAK (no ack)
  out[n++] = (uint8_t)label[0];
  out[n++] = (uint8_t)label[1];
  out[n++] = (uint8_t)block_id;
  out[n++] = ACARS_STX;
  if (IS_DOWNLINK_BLK(block_id)) {
    memcpy(out + n, msg_num, 4); n += 4;        // 3-char number + sequence char
    memcpy(out + n, flight, 6);  n += 6;
  }
  size_t tlen = strlen(text);
  memcpy(out + n, text, tlen); n += (int)tlen;
  out[n++] = ACARS_ETX;
  uint16_t crc = crc16_ccitt(out + crc_start, (uint32_t)(n - crc_start), 0);
  out[n++] = (uint8_t)(crc & 0xff);             // LE, so the check comes to 0
  out[n++] = (uint8_t)(crc >> 8);
  out[n++] = ACARS_DEL;
  return n;
}

// Check one rendered decode. With MACHPSDR_HFDL_MSG_DEBUG set, every step's
// output is dumped so a failing expectation can be read straight off.
static gboolean check(GString *out, const char *step, const char *const *needles) {
  gboolean ok = TRUE;
  for (int i = 0; needles[i] != NULL; i++) {
    if (strstr(out->str, needles[i]) == NULL) {
      ok = FALSE;
      g_printerr("[HFDL msg selftest] %s: missing \"%s\"\n", step, needles[i]);
    }
  }
  if (!ok || g_getenv("MACHPSDR_HFDL_MSG_DEBUG"))
    g_printerr("--- %s ---\n%s", step, out->str);
  return ok;
}

gboolean hfdl_msg_selftest(void) {
  gboolean ok = TRUE;
  uint8_t lpdu[512], frame[600];
  GString *out = g_string_new(NULL);

  hfdl_msg_reset();

  // (1) Logon request: ICAO must decode, and the frame must validate.
  uint8_t icao_buf[3];
  icao_encode(icao_buf, 0x3C6444);
  int lp = build_lpdu(lpdu, LPDU_LOGON_REQUEST_NORMAL, icao_buf, 3);
  int fl = build_downlink_mpdu(frame, 12, 3, lpdu, lp);
  if (!hfdl_msg_decode(frame, fl, out)) ok = FALSE;
  // GS 3's name comes from the embedded table.
  if (!check(out, "logon request",
             (const char *[]){ "3C6444", "Logon request", "Reykjavik", NULL })) ok = FALSE;

  // (2) Logon confirm caches the aircraft ID -> a later frame shows the ICAO.
  g_string_truncate(out, 0);
  uint8_t confirm[8] = {0};
  icao_encode(confirm, 0x3C6444);
  confirm[3] = 42;                              // assigned AC ID (LPDU octet 4)
  lp = build_lpdu(lpdu, LPDU_LOGON_CONFIRM, confirm, 7);
  fl = build_downlink_mpdu(frame, 12, 3, lpdu, lp);
  if (!hfdl_msg_decode(frame, fl, out)) ok = FALSE;
  if (!check(out, "logon confirm",
             (const char *[]){ "Assigned AC ID: 42", NULL })) ok = FALSE;

  // (3) Performance data: flight id, position, time, station frequency.
  g_string_truncate(out, 0);
  uint8_t hfnpdu[64] = {0};
  hfnpdu[0] = 0xFF; hfnpdu[1] = HFNPDU_PERFORMANCE_DATA;
  memcpy(hfnpdu + 2, "DLH441", 6);
  // 20-bit two's-complement coordinates (via int32_t — casting a negative
  // double straight to an unsigned type is undefined).
  uint32_t lat = (uint32_t)(int32_t)( 45.0 / 180.0 * (double)0x7ffff) & 0xFFFFF;
  uint32_t lon = (uint32_t)(int32_t)(-30.0 / 180.0 * (double)0x7ffff) & 0xFFFFF;
  hfnpdu[8]  = lat & 0xff; hfnpdu[9] = (lat >> 8) & 0xff;
  hfnpdu[10] = (uint8_t)(((lat >> 16) & 0xF) | ((lon & 0xF) << 4));
  hfnpdu[11] = (uint8_t)((lon >> 4) & 0xff);
  hfnpdu[12] = (uint8_t)((lon >> 12) & 0xff);
  uint16_t t2 = (uint16_t)((12 * 3600 + 34 * 60 + 56) / 2);    // 12:34:56
  hfnpdu[13] = t2 & 0xff; hfnpdu[14] = t2 >> 8;
  hfnpdu[17] = 3;                               // GS 3 (Reykjavik)
  hfnpdu[18] = 1;                               // freq slot 1 -> 15025 kHz
  lp = build_lpdu(lpdu, LPDU_UNNUMBERED_DATA, hfnpdu, PERFORMANCE_DATA_LEN);
  fl = build_downlink_mpdu(frame, 42, 3, lpdu, lp);
  if (!hfdl_msg_decode(frame, fl, out)) ok = FALSE;
  // "(3C6444)" = AC 42 resolved from the logon-confirm cache. The coordinates
  // come back a fraction under 45 / -30 because the 20-bit encoding quantises.
  if (!check(out, "performance data",
             (const char *[]){ "(3C6444)", "DLH441", "12:34:56", "15025 kHz",
                               "Pos: 44.99", "-29.99", NULL })) ok = FALSE;

  // (4) Enveloped data carrying an ACARS downlink block.
  g_string_truncate(out, 0);
  uint8_t env[256];
  env[0] = 0xFF; env[1] = HFNPDU_ENVELOPED_DATA;
  int alen = build_acars(env + 2, ".N123AB", "H1", '4', "DLH441", "M01A",
                         "POS/ALT370/OVERHEAD");
  lp = build_lpdu(lpdu, LPDU_UNNUMBERED_DATA, env, 2 + alen);
  fl = build_downlink_mpdu(frame, 42, 3, lpdu, lp);
  if (!hfdl_msg_decode(frame, fl, out)) ok = FALSE;
  if (!check(out, "acars",
             (const char *[]){ "ACARS downlink", "N123AB", "Label: H1",
                               "CRC OK", "POS/ALT370/OVERHEAD", NULL })) ok = FALSE;

  // (5) A corrupted ACARS block must be flagged, not silently accepted. The
  // corruption goes in BEFORE the LPDU is built, so the outer FCS layers still
  // check out and only the ACARS CRC catches it.
  g_string_truncate(out, 0);
  env[2 + alen - 8] ^= 0xff;                    // a byte of the message text
  lp = build_lpdu(lpdu, LPDU_UNNUMBERED_DATA, env, 2 + alen);
  fl = build_downlink_mpdu(frame, 42, 3, lpdu, lp);
  if (!hfdl_msg_decode(frame, fl, out)) ok = FALSE;   // outer FCS still fine
  if (!check(out, "acars corrupt", (const char *[]){ "CRC FAIL", NULL })) ok = FALSE;

  // (6) SPDU squitter.
  g_string_truncate(out, 0);
  uint8_t spdu[SPDU_LEN];
  memset(spdu, 0, sizeof(spdu));
  spdu[0] = 0x02;                               // SPDU (bit0=0), RLS in use
  spdu[1] = 5 | 0x80;                           // GS 5 (Auckland), UTC synced
  spdu[54] = 0x20;                              // GS 5 freqs-in-use bit 1 -> 13351
  fcs_append(spdu, SPDU_HDR);
  if (!hfdl_msg_decode(spdu, SPDU_LEN, out)) ok = FALSE;
  if (!check(out, "spdu",
             (const char *[]){ "squitter", "Auckland", "13351",
                               "UTC sync: yes", NULL })) ok = FALSE;

  // (7) A corrupt MPDU header must be rejected outright.
  g_string_truncate(out, 0);
  uint8_t saved = frame[3];
  frame[3] ^= 0xff;
  if (hfdl_msg_decode(frame, fl, out)) {
    ok = FALSE;
    g_printerr("[HFDL msg selftest] corrupt MPDU header was accepted\n");
  }
  frame[3] = saved;

  // (8) FULL STACK: push the ACARS-bearing frame through the real RF chain
  // (convolutional encode -> interleave -> scramble -> BPSK symbols -> framer ->
  // deinterleave -> Viterbi -> bytes) and decode the message text off the other
  // end. This is the one test that ties the DSP/FEC layers to the app layer.
  g_string_truncate(out, 0);
  alen = build_acars(env + 2, ".N123AB", "H1", '4', "DLH441", "M01A",
                     "POS/ALT370/OVERHEAD");      // rebuild the uncorrupted block
  lp = build_lpdu(lpdu, LPDU_UNNUMBERED_DATA, env, 2 + alen);
  fl = build_downlink_mpdu(frame, 42, 3, lpdu, lp);
  int cap = hfdl_frame_test_capacity(1);            // m=1: 600 bps single slot
  if (cap < fl * 8) {
    g_printerr("[HFDL msg selftest] full stack: frame too big (%d > %d bits)\n", fl * 8, cap);
    ok = FALSE;
  } else {
    uint8_t *bits = g_new0(uint8_t, cap);
    // LSB-first within each octet — the decoder bit-reverses every octet coming
    // out of the Viterbi (HFDL octet numbering), so the transmit side has to
    // present the payload the same way round.
    for (int i = 0; i < fl * 8; i++) bits[i] = (frame[i >> 3] >> (i & 7)) & 1;
    uint8_t *decoded = NULL;
    int nd = hfdl_frame_test_roundtrip(1, 0.0f, bits, cap, &decoded);
    if (nd < fl || decoded == NULL || memcmp(decoded, frame, (size_t)fl) != 0) {
      g_printerr("[HFDL msg selftest] full stack: framer returned %d bytes, payload mismatch\n", nd);
      ok = FALSE;
    } else if (!hfdl_msg_decode(decoded, fl, out)) {
      g_printerr("[HFDL msg selftest] full stack: decode rejected the recovered frame\n");
      ok = FALSE;
    } else if (!check(out, "full stack",
                      (const char *[]){ "ACARS downlink", "N123AB",
                                        "POS/ALT370/OVERHEAD", NULL })) {
      ok = FALSE;
    }
    g_free(decoded);
    g_free(bits);
  }

  g_string_free(out, TRUE);
  hfdl_msg_reset();
  return ok;
}
