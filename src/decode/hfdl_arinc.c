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
 * ARINC-622 envelope + ADS-C decode. See hfdl_arinc.h for what is and is not
 * ported. Field layouts and scaling follow libacars' arinc.c / adsc.c (MIT);
 * the rendering is our own.
 */

#include <ctype.h>
#include <glib.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "hfdl_arinc.h"
#include "hfdl_cpdlc.h"

#define IMI_LEN      3
#define AIR_REG_LEN  7
#define ARINC_CRC_LEN 2
#define CRC_ARINC_GOOD 0x1D0Fu

typedef enum {
  IMI_UNKNOWN = 0,
  IMI_AT1, IMI_CR1, IMI_CC1, IMI_DR1, IMI_ADS, IMI_DIS
} ARINC_IMI;

static const struct { const char *tag; ARINC_IMI imi; const char *desc; } imi_map[] = {
  { ".AT1", IMI_AT1, "FANS-1/A CPDLC message" },
  { ".CR1", IMI_CR1, "FANS-1/A CPDLC connect request" },
  { ".CC1", IMI_CC1, "FANS-1/A CPDLC connect confirm" },
  { ".DR1", IMI_DR1, "FANS-1/A CPDLC disconnect request" },
  { ".ADS", IMI_ADS, "ADS-C message" },
  { ".DIS", IMI_DIS, "ADS-C disconnect request" },
};

// CRC-16-CCITT, MSB-first, init 0xFFFF ("CCITT-FALSE"). NOT the reflected
// variant hfdl_crc.c provides for the ACARS block — different polynomial
// direction, different good value.
static uint16_t crc16_arinc(const uint8_t *d, int len, uint16_t crc) {
  for (int i = 0; i < len; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  return -1;
}

// Decode as much leading hex as the string holds. Returns the byte count.
static int slurp_hex(const char *s, uint8_t *out, int max) {
  int n = 0;
  while (n < max) {
    int hi = hexval(s[2 * n]), lo = hi < 0 ? -1 : hexval(s[2 * n + 1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

// --- bit reader (libacars bitstream.c, MSB-first) ---------------------------

typedef struct { const uint8_t *buf; int len; int pos; } BITS;

static uint32_t bits_get(BITS *b, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    int byte = b->pos >> 3, bit = 7 - (b->pos & 7);
    uint32_t x = (byte < b->len) ? ((b->buf[byte] >> bit) & 1u) : 0u;
    v = (v << 1) | x;
    b->pos++;
  }
  return v;
}

// --- ADS-C field scaling (libacars adsc.c) ----------------------------------

// 21-bit signed, MSB weight 90 degrees.
static double adsc_coord(uint32_t c) {
  int32_t r = (c & 0x100000u) ? (int32_t)(c | 0xFFE00000u) : (int32_t)c;
  return (180.0 - 90.0 / 524288.0) * (double)r / (double)0xfffff;
}
static int adsc_alt(uint32_t a) {
  int32_t r = (a & 0x8000u) ? (int32_t)(a | 0xFFFF0000u) : (int32_t)a;
  return r * 4;
}
static double adsc_time(uint32_t t)    { return (double)t * 0.125; }
static double adsc_speed(uint32_t s)   { return (double)s / 2.0; }
static int    adsc_vspeed(uint32_t v) {
  int32_t r = (v & 0x800u) ? (int32_t)(v | 0xFFFFF000u) : (int32_t)v;
  return r * 16;
}
static double adsc_heading(uint32_t h) {
  int32_t r = (h & 0x800u) ? (int32_t)(h | 0xFFFFF000u) : (int32_t)h;
  return (double)r * 180.0 / 2048.0;
}
static double adsc_wind_dir(uint32_t d)  { return (double)d * 360.0 / 512.0; }
static double adsc_temperature(uint32_t t) {
  int32_t r = (t & 0x800u) ? (int32_t)(t | 0xFFFFF000u) : (int32_t)t;
  return (double)r * 0.25;
}
static double adsc_distance(uint32_t d)  { return (double)d / 8.0; }

// Locale-safe "%.4f" — the app runs under the operator's locale and a comma
// decimal makes a coordinate unreadable (the same trap hfdl_msg.c hit).
static void fmt4(char *out, gsize len, double v) {
  g_ascii_formatd(out, (gint)len, "%.4f", v);
}

// --- ADS-C tags -------------------------------------------------------------
//
// Downlink (aircraft -> ground) tag table. Every tag has a fixed length, so an
// unhandled one can still be stepped over; length 0 means "variable", which
// stops the walk (guessing would turn the rest of the report into nonsense).

typedef enum { T_NONE, T_BASIC, T_FLIGHT_ID, T_ROUTE, T_EARTH_REF, T_AIR_REF,
               T_METEO, T_AIRFRAME, T_INTERMED, T_FIXED, T_U8 } ADSC_KIND;

static const struct { uint8_t id; int len; ADSC_KIND kind; const char *label; } adsc_tags[] = {
  {   3,  1, T_U8,        "Acknowledgement" },
  {   4,  0, T_NONE,      "Negative acknowledgement" },
  {   5,  0, T_NONE,      "Noncompliance notification" },
  {   6,  0, T_NONE,      "Cancel emergency mode" },
  {   7, 10, T_BASIC,     "Basic report" },
  {   9, 10, T_BASIC,     "Emergency basic report" },
  {  10, 10, T_BASIC,     "Lateral deviation change event" },
  {  12,  6, T_FLIGHT_ID, "Flight ID" },
  {  13, 17, T_ROUTE,     "Predicted route" },
  {  14,  5, T_EARTH_REF, "Earth reference data" },
  {  15,  5, T_AIR_REF,   "Air reference data" },
  {  16,  4, T_METEO,     "Meteo data" },
  {  17,  3, T_AIRFRAME,  "Airframe ID" },
  {  18, 10, T_BASIC,     "Vertical rate change event" },
  {  19, 10, T_BASIC,     "Altitude range event" },
  {  20, 10, T_BASIC,     "Waypoint change event" },
  {  22,  8, T_INTERMED,  "Intermediate projection" },
  {  23,  9, T_FIXED,     "Fixed projection" },
};

static void ind(GString *out, int indent) {
  for (int i = 0; i < indent; i++) g_string_append(out, "  ");
}

G_GNUC_PRINTF(3, 4)
static void emit(GString *out, int indent, const char *fmt, ...) {
  va_list ap;
  ind(out, indent);
  va_start(ap, fmt);
  g_string_append_vprintf(out, fmt, ap);
  va_end(ap);
  g_string_append_c(out, '\n');
}

static void adsc_emit_pos(GString *out, int indent, const char *what,
                          double lat, double lon, int alt) {
  char la[G_ASCII_DTOSTR_BUF_SIZE], lo[G_ASCII_DTOSTR_BUF_SIZE];
  fmt4(la, sizeof(la), lat);
  fmt4(lo, sizeof(lo), lon);
  emit(out, indent, "%s: %s, %s   Alt: %d ft", what, la, lo, alt);
}

static void adsc_tag_render(GString *out, int indent, ADSC_KIND kind,
                            const uint8_t *b, int len) {
  BITS bs = { b, len, 0 };
  switch (kind) {
    case T_BASIC: {
      double lat = adsc_coord(bits_get(&bs, 21));
      double lon = adsc_coord(bits_get(&bs, 21));
      int    alt = adsc_alt(bits_get(&bs, 16));
      double ts  = adsc_time(bits_get(&bs, 15));
      uint32_t f = bits_get(&bs, 7);
      adsc_emit_pos(out, indent, "Pos", lat, lon, alt);
      // The timestamp is seconds into the current hour.
      emit(out, indent, "Time: %02d:%06.3f   Accuracy: %u   TCAS: %s",
           (int)(ts / 60.0), fmod(ts, 60.0), (f >> 1) & 0x7,
           ((f >> 4) & 1) ? "ok" : "unavailable");
      break;
    }
    case T_FLIGHT_ID: {
      char id[9];
      for (int i = 0; i < 8; i++) {
        uint32_t c = bits_get(&bs, 6);       // ISO-5 on 6 bits: A-Z, 0-9, space
        if ((c & 0x20) == 0) c += 0x40;
        id[i] = (char)c;
      }
      id[8] = '\0';
      emit(out, indent, "Flight ID: %s", g_strchomp(id));
      break;
    }
    case T_ROUTE: {
      double lat = adsc_coord(bits_get(&bs, 21));
      double lon = adsc_coord(bits_get(&bs, 21));
      int    alt = adsc_alt(bits_get(&bs, 16));
      bits_get(&bs, 14);                     // ETA to the next waypoint
      double lat2 = adsc_coord(bits_get(&bs, 21));
      double lon2 = adsc_coord(bits_get(&bs, 21));
      int    alt2 = adsc_alt(bits_get(&bs, 16));
      adsc_emit_pos(out, indent, "Next waypoint", lat, lon, alt);
      adsc_emit_pos(out, indent, "Then", lat2, lon2, alt2);
      break;
    }
    case T_EARTH_REF:
    case T_AIR_REF: {
      bits_get(&bs, 1);                      // true/magnetic flag
      double hdg = adsc_heading(bits_get(&bs, 12));
      double spd = adsc_speed(bits_get(&bs, 13));
      int    vs  = adsc_vspeed(bits_get(&bs, 12));
      char h[G_ASCII_DTOSTR_BUF_SIZE], s[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(h, sizeof(h), "%.1f", hdg);
      g_ascii_formatd(s, sizeof(s), "%.1f", spd);
      emit(out, indent, "%s: %s deg   %s kt   V/S %d ft/min",
           kind == T_EARTH_REF ? "Track/ground speed" : "Heading/airspeed", h, s, vs);
      break;
    }
    case T_METEO: {
      double ws  = adsc_speed(bits_get(&bs, 9));
      bits_get(&bs, 1);
      double wd  = adsc_wind_dir(bits_get(&bs, 9));
      double tmp = adsc_temperature(bits_get(&bs, 12));
      char a[G_ASCII_DTOSTR_BUF_SIZE], b2[G_ASCII_DTOSTR_BUF_SIZE], c[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(a,  sizeof(a),  "%.1f", ws);
      g_ascii_formatd(b2, sizeof(b2), "%.1f", wd);
      g_ascii_formatd(c,  sizeof(c),  "%.1f", tmp);
      emit(out, indent, "Wind: %s kt from %s deg   Temp: %s C", a, b2, c);
      break;
    }
    case T_AIRFRAME:
      emit(out, indent, "Airframe ID: %02X%02X%02X", b[0], b[1], b[2]);
      break;
    case T_INTERMED: {
      double d = adsc_distance(bits_get(&bs, 16));
      bits_get(&bs, 1);
      double t = adsc_heading(bits_get(&bs, 12));
      int  alt = adsc_alt(bits_get(&bs, 16));
      char a[G_ASCII_DTOSTR_BUF_SIZE], b2[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(a,  sizeof(a),  "%.1f", d);
      g_ascii_formatd(b2, sizeof(b2), "%.1f", t);
      emit(out, indent, "Projected: %s NM on track %s deg   Alt: %d ft", a, b2, alt);
      break;
    }
    case T_FIXED: {
      double lat = adsc_coord(bits_get(&bs, 21));
      double lon = adsc_coord(bits_get(&bs, 21));
      int    alt = adsc_alt(bits_get(&bs, 16));
      adsc_emit_pos(out, indent, "Fixed projection", lat, lon, alt);
      break;
    }
    case T_U8:
      emit(out, indent, "Contract number: %u", b[0]);
      break;
    default:
      break;
  }
}

static void adsc_decode(const uint8_t *buf, int len, GString *out, int indent) {
  int pos = 0;
  while (pos < len) {
    uint8_t tag = buf[pos++];
    const char *label = NULL;
    int taglen = -1;
    ADSC_KIND kind = T_NONE;
    for (gsize i = 0; i < G_N_ELEMENTS(adsc_tags); i++)
      if (adsc_tags[i].id == tag) {
        label = adsc_tags[i].label; taglen = adsc_tags[i].len; kind = adsc_tags[i].kind;
        break;
      }
    if (label == NULL) {
      emit(out, indent, "Tag %u: unknown, %d octets left undecoded", tag, len - pos);
      return;
    }
    if (taglen == 0) {                       // variable length: cannot step over it
      emit(out, indent, "%s (%d octets left undecoded)", label, len - pos);
      return;
    }
    if (pos + taglen > len) {
      emit(out, indent, "%s: truncated", label);
      return;
    }
    emit(out, indent, "%s", label);
    adsc_tag_render(out, indent + 1, kind, buf + pos, taglen);
    pos += taglen;
  }
}

// --- ARINC-622 envelope -----------------------------------------------------

static gboolean is_addr(const char *s, int len) {
  for (int i = 0; i < len; i++)
    if (!(g_ascii_isupper(s[i]) || g_ascii_isdigit(s[i]))) return FALSE;
  return TRUE;
}

gboolean hfdl_arinc_decode(const char *txt, gboolean downlink, GString *out, int indent) {
  if (txt == NULL || out == NULL) return FALSE;

  // The envelope is ANCHORED: the ground address is the first thing in the
  // message text (optionally after a leading '/', or after an H1 sublabel/MFI
  // prefix), and the IMI comes straight after it. libacars does the same, and
  // it matters — searching the whole string for ".ADS" happily "finds" one in
  // ordinary free text like "...OVERHEAD.ADS..." and then reads the preceding
  // seven letters as a ground station.
  const char *p = txt;
  if (p[0] == '/') p++;
  // H1 sublabel: "#XXB" downlink, "- #XX" uplink, each optionally followed by
  // an MFI "/XX ".
  if (p[0] == '#' && p[1] && p[2] && p[3] == 'B')             p += 4;
  else if (p[0] == '-' && p[1] == ' ' && p[2] == '#' && p[3]) p += 5;
  if (p[0] == '/' && p[1] && p[2] && p[3] == ' ')             p += 4;

  const char *imi_ptr = NULL;
  const char *gs_addr = NULL;
  int gs_len = 0;
  ARINC_IMI imi = IMI_UNKNOWN;
  const char *desc = NULL;
  for (gsize i = 0; i < G_N_ELEMENTS(imi_map) && imi_ptr == NULL; i++) {
    const char *tag = imi_map[i].tag;
    // Seven-character ground address ("AKLCDYA.AT1…") or four ("EDYY.ADS…").
    if (strlen(p) > 7 && strncmp(p + 7, tag, IMI_LEN + 1) == 0 && is_addr(p, 7)) {
      imi_ptr = p + 7; gs_addr = p; gs_len = 7;
    } else if (strlen(p) > 4 && strncmp(p + 4, tag, IMI_LEN + 1) == 0 && is_addr(p, 4)) {
      imi_ptr = p + 4; gs_addr = p; gs_len = 4;
    }
    if (imi_ptr != NULL) { imi = imi_map[i].imi; desc = imi_map[i].desc; }
  }
  if (imi_ptr == NULL) return FALSE;

  char gs[8];
  memcpy(gs, gs_addr, (size_t)gs_len); gs[gs_len] = '\0';

  // From here on the message is ARINC-622 whatever happens, so every exit
  // reports rather than declining.
  const char *payload = imi_ptr + 1;            // skip the '.', keep the IMI
  int plen = (int)strlen(payload);
  if (plen < IMI_LEN + AIR_REG_LEN + ARINC_CRC_LEN * 2) {
    emit(out, indent, "ARINC-622 %s from %s: truncated", desc, gs);
    return TRUE;
  }

  char reg[AIR_REG_LEN + 1];
  memcpy(reg, payload + IMI_LEN, AIR_REG_LEN); reg[AIR_REG_LEN] = '\0';

  uint8_t bin[512];
  int nbin = slurp_hex(payload + IMI_LEN + AIR_REG_LEN, bin, (int)sizeof(bin));
  if (nbin < ARINC_CRC_LEN) {
    emit(out, indent, "ARINC-622 %s   GS: %s   Reg: %s   (no binary payload)", desc, gs, reg);
    return TRUE;
  }

  // CRC covers IMI + registration + the binary payload including its own CRC.
  uint8_t crcbuf[IMI_LEN + AIR_REG_LEN + 512];
  memcpy(crcbuf, payload, IMI_LEN + AIR_REG_LEN);
  memcpy(crcbuf + IMI_LEN + AIR_REG_LEN, bin, (size_t)nbin);
  gboolean crc_ok = (crc16_arinc(crcbuf, IMI_LEN + AIR_REG_LEN + nbin, 0xFFFFu) == CRC_ARINC_GOOD);
  int datalen = nbin - ARINC_CRC_LEN;

  emit(out, indent, "ARINC-622 %s   GS: %s   Reg: %s   CRC %s",
       desc, gs, g_strchomp(reg), crc_ok ? "OK" : "FAIL");

  switch (imi) {
    case IMI_ADS:
    case IMI_DIS:
      if (datalen > 0) adsc_decode(bin, datalen, out, indent + 1);
      break;
    case IMI_AT1:
    case IMI_CR1:
    case IMI_CC1:
    case IMI_DR1:
      // FANS-1/A CPDLC. Direction is not in the envelope and it decides the
      // whole message: the same octets are a clearance as an uplink and a
      // request as a downlink, so it comes from the HFDL frame that carried it.
      if (datalen > 0) hfdl_cpdlc_decode(bin, datalen, !downlink, out, indent + 1);
      break;
    default:
      break;
  }
  return TRUE;
}

// --- self-test --------------------------------------------------------------

// Bit packer, MSB-first — the transmit-side inverse of bits_get(). A basic
// report is 80 bits, so this cannot be an accumulator in a 64-bit integer
// (which is exactly the bug the first version of this test had).
typedef struct { uint8_t *buf; int pos; } BITW;

static void bits_put(BITW *w, uint32_t v, int n) {
  for (int i = n - 1; i >= 0; i--) {
    int byte = w->pos >> 3, bit = 7 - (w->pos & 7);
    if ((v >> i) & 1u) w->buf[byte] |= (uint8_t)(1u << bit);
    else               w->buf[byte] &= (uint8_t)~(1u << bit);
    w->pos++;
  }
}

// Build "<gs>.<IMI><reg><hex payload + CRC>" with a valid ARINC CRC.
static char *build_arinc(const char *gs, const char *imi, const char *reg,
                         const uint8_t *data, int dlen) {
  uint8_t crcbuf[IMI_LEN + AIR_REG_LEN + 256];
  memcpy(crcbuf, imi, IMI_LEN);
  memcpy(crcbuf + IMI_LEN, reg, AIR_REG_LEN);
  memcpy(crcbuf + IMI_LEN + AIR_REG_LEN, data, (size_t)dlen);
  // ARINC-622 transmits the COMPLEMENT of the CRC, MSB first; that is what makes
  // a recomputation over message+CRC come to the 0x1D0F residue the receiver
  // checks for (appending the plain CRC would give 0x0000 instead).
  uint16_t crc = (uint16_t)~crc16_arinc(crcbuf, IMI_LEN + AIR_REG_LEN + dlen, 0xFFFFu);

  GString *s = g_string_new(NULL);
  g_string_append_printf(s, "%s.%s%s", gs, imi, reg);
  for (int i = 0; i < dlen; i++) g_string_append_printf(s, "%02X", data[i]);
  g_string_append_printf(s, "%02X%02X", (crc >> 8) & 0xff, crc & 0xff);
  return g_string_free(s, FALSE);
}

static gboolean want(GString *out, const char *step, const char *const *needles) {
  for (int i = 0; needles[i] != NULL; i++)
    if (strstr(out->str, needles[i]) == NULL) {
      g_printerr("[HFDL arinc selftest] %s: missing \"%s\" in:\n%s\n",
                 step, needles[i], out->str);
      return FALSE;
    }
  return TRUE;
}

gboolean hfdl_arinc_selftest(void) {
  gboolean ok = TRUE;
  GString *out = g_string_new(NULL);

  // (1) ADS-C basic report. Fields are packed exactly as the parser reads them:
  // 21-bit lat, 21-bit lon, 16-bit altitude, 15-bit timestamp, 7-bit flags.
  {
    double lat = 51.5, lon = -0.25;
    int alt_ft = 37000;
    double ts = 1234.5;                       // seconds into the hour
    uint32_t lat_f = (uint32_t)llround(lat * (double)0xfffff / (180.0 - 90.0 / 524288.0));
    uint32_t lon_f = (uint32_t)llround(lon * (double)0xfffff / (180.0 - 90.0 / 524288.0));
    uint8_t data[11] = { 7 };                 // tag: basic report
    BITW w = { data + 1, 0 };
    bits_put(&w, lat_f, 21);
    bits_put(&w, lon_f, 21);
    bits_put(&w, (uint32_t)(alt_ft / 4), 16);
    bits_put(&w, (uint32_t)llround(ts / 0.125), 15);
    bits_put(&w, 0x10, 7);                    // TCAS ok

    char *msg = build_arinc("EGGXCDA", "ADS", ".G-ABCD", data, 11);
    g_string_truncate(out, 0);
    if (!hfdl_arinc_decode(msg, TRUE, out, 0)) {
      g_printerr("[HFDL arinc selftest] basic report: not recognised as ARINC-622\n");
      ok = FALSE;
    } else if (!want(out, "basic report",
                     (const char *[]){ "ARINC-622 ADS-C message", "EGGXCDA", ".G-ABCD",
                                       "CRC OK", "Basic report", "51.5000", "-0.249",   // lon quantises
                                       "37000 ft", NULL })) {
      ok = FALSE;
    }
    // A single flipped payload octet must show up as a CRC failure.
    char *bad = g_strdup(msg);
    char *hex = strstr(bad, ".G-ABCD") + 7;
    hex[0] = (hex[0] == '0') ? '1' : '0';
    g_string_truncate(out, 0);
    hfdl_arinc_decode(bad, TRUE, out, 0);
    if (!want(out, "corrupt", (const char *[]){ "CRC FAIL", NULL })) ok = FALSE;
    g_free(bad);
    g_free(msg);
  }

  // (2) Flight ID tag: 8 characters of 6-bit ISO-5.
  {
    const char *fid = "BAW123  ";
    uint8_t data[7] = { 12 };
    BITW w = { data + 1, 0 };
    for (int i = 0; i < 8; i++) {
      uint32_t c = (uint8_t)fid[i];
      c = (c == ' ') ? 0x20 : (c & 0x3f);     // A-Z/0-9 keep their low 6 bits
      bits_put(&w, c, 6);
    }
    char *msg = build_arinc("KZAKCDA", "ADS", ".N123AB", data, 7);
    g_string_truncate(out, 0);
    hfdl_arinc_decode(msg, TRUE, out, 0);
    if (!want(out, "flight id",
              (const char *[]){ "CRC OK", "Flight ID: BAW123", NULL })) ok = FALSE;
    g_free(msg);
  }

  // (3) CPDLC: the envelope is recognised and the payload handed to the ASN.1
  //     decoder. These four octets happen to be a valid downlink saying AFFIRM,
  //     which is what makes them a usable end-to-end check here; the real CPDLC
  //     vectors live in hfdl_cpdlc_selftest().
  {
    uint8_t data[4] = { 0x01, 0x02, 0x03, 0x04 };
    char *msg = build_arinc("EDYYCDA", "AT1", ".D-AIBC", data, 4);
    g_string_truncate(out, 0);
    hfdl_arinc_decode(msg, TRUE, out, 0);
    if (!want(out, "cpdlc",
              (const char *[]){ "FANS-1/A CPDLC message", "CRC OK",
                                "CPDLC Downlink Message", "AFFIRM", NULL })) ok = FALSE;
    g_free(msg);
  }

  // (4) Ordinary ACARS text must NOT be claimed as ARINC-622.
  {
    g_string_truncate(out, 0);
    if (hfdl_arinc_decode("0EH1441532SH", TRUE, out, 0) ||
        hfdl_arinc_decode("POS/ALT370/OVERHEAD.ADSXYZ", TRUE, out, 0)) {
      g_printerr("[HFDL arinc selftest] plain text was claimed as ARINC-622\n");
      ok = FALSE;
    }
  }

  g_string_free(out, TRUE);
  if (ok) g_printerr("[INFO] hfdl_arinc selftest: PASS (envelope + CRC + ADS-C tags)\n");
  return ok;
}
