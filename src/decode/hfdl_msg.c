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

#include "hfdl_arinc.h"   // ARINC-622 / ADS-C inside the ACARS text
#include "hfdl_crc.h"      // vendored crc16_ccitt (hfdl_lib/) — same table as la_crc16_ccitt
#include "hfdl_frame.h"    // test seam: full-stack self-test through the RF chain
#include "hfdl_miam.h"     // MIAM file transfer (labels MA / H1)
#include "hfdl_msg.h"
#include "hfdl_ohma.h"     // OHMA messaging (label H1)
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
// Snapshot of dumphfdl's etc/systable.conf (version 52) — the fallback, and the
// only source of station NAMES (the over-the-air table carries none). The live
// table is broadcast in System-table HFNPDUs and reassembled below into
// `gs_learned`, which takes precedence for frequencies once a complete table has
// been received. Frequencies are kHz.

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

// --- over-the-air system table ----------------------------------------------
//
// Port of dumphfdl's systable.c, minus its libconfig save-file: a ground station
// broadcasts the current table split across several System-table HFNPDUs, each
// carrying the table version, its own index and the set size. Collect a complete
// set, concatenate, and parse a list of per-station records. That table is the
// authority on which frequencies a station's slot indices mean — the embedded
// snapshot goes stale as stations move frequency, and a stale slot map renders
// every "freq slot N" in a Performance/Frequency-data HFNPDU as the wrong kHz.
//
// Names are NOT in the broadcast (dumphfdl copies them across from its previous
// config for the same reason), so a learned station keeps the snapshot's name.

// Defined further down with the other field helpers / renderers; this block
// sits here because gs_freq() below it is what the rest of the file calls.
static double parse_coordinate(uint32_t c);
G_GNUC_PRINTF(3, 4) static void emit(GString *out, int indent, const char *fmt, ...);

#define SYSTABLE_MAX_PDUS       16
#define SYSTABLE_PDU_MAX_LEN    1024
#define SYSTABLE_HFNPDU_HDR_LEN 5    // HFNPDU type .. table version, in every PDU
#define SYSTABLE_VERSION_MAX  4095
#define SYSTABLE_GS_MIN_LEN   8      // GS id .. master slot offset, sans frequencies
#define GS_LEARNED_MAX        128    // gs_id is 7 bits

typedef struct {
  gboolean valid;
  gboolean utc_sync;
  double   lat, lon;
  uint8_t  spdu_version;
  int      freq_cnt;
  uint32_t freqs[GS_MAX_FREQ_CNT];      // kHz
  uint8_t  slots[GS_MAX_FREQ_CNT];      // master frame slot
} HFDL_GS_LEARNED;

static HFDL_GS_LEARNED gs_learned[GS_LEARNED_MAX];
static int gs_learned_version = -1;     // version of the table in gs_learned

// The PDU set being collected (one at a time, as in dumphfdl).
static struct {
  gboolean in_use;
  int      version;
  int      len;                          // total PDUs in the set
  int      have;                         // how many collected
  int      frag_len[SYSTABLE_MAX_PDUS];
  uint8_t  frag[SYSTABLE_MAX_PDUS][SYSTABLE_PDU_MAX_LEN];
} systable_set;

static void systable_set_reset(void) { memset(&systable_set, 0, sizeof(systable_set)); }

// The 12-bit version wraps; a value less than half the space behind the current
// one is a wrap forward, not an older table (dumphfdl systable_is_newer).
static gboolean systable_is_newer(int v_old, int v_new) {
  if (v_old < 0 && v_new >= 0) return TRUE;
  if (v_new < 0 && v_old >= 0) return FALSE;
  if (v_new == v_old)          return FALSE;
  return v_new > v_old ||
         v_new + SYSTABLE_VERSION_MAX - v_old < (SYSTABLE_VERSION_MAX + 1) >> 1;
}

// 3 octets of packed BCD, 100 Hz per unit -> kHz.
static uint32_t systable_decode_frequency(const uint8_t buf[3]) {
  uint32_t hz = 100u        * (buf[0] & 0xF) +
                1000u       * ((buf[0] >> 4) & 0xF) +
                10000u      * (buf[1] & 0xF) +
                100000u     * ((buf[1] >> 4) & 0xF) +
                1000000u    * (buf[2] & 0xF) +
                10000000u   * ((buf[2] >> 4) & 0xF);
  return hz / 1000u;
}

// One station record; returns the octets consumed, or -1 on a malformed record.
static int systable_decode_gs(const uint8_t *buf, int len, HFDL_GS_LEARNED *out,
                              uint8_t *gs_id_out) {
  if (len < SYSTABLE_GS_MIN_LEN) return -1;

  uint8_t gs_id = buf[0] & 0x7F;
  HFDL_GS_LEARNED gs;
  memset(&gs, 0, sizeof(gs));
  gs.utc_sync = (buf[0] & 0x80) != 0;

  uint32_t coord = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)(buf[3] & 0xF) << 16);
  gs.lat = parse_coordinate(coord);
  coord = (uint32_t)(buf[3] >> 4) | ((uint32_t)buf[4] << 4) | ((uint32_t)buf[5] << 12);
  gs.lon = parse_coordinate(coord);

  gs.spdu_version = buf[6] & 7;
  gs.freq_cnt = (buf[6] >> 3) & 0x1F;
  if (gs.freq_cnt > GS_MAX_FREQ_CNT) return -1;

  int consumed = SYSTABLE_GS_MIN_LEN - 1;               // 7: header, sans frequencies
  for (int f = 0; f < gs.freq_cnt; f++) {
    int pos = SYSTABLE_GS_MIN_LEN - 1 + f * 4;          // 3 octets freq + 1 slot
    if (pos + 4 > len) return -1;
    gs.freqs[f] = systable_decode_frequency(buf + pos);
    gs.slots[f] = buf[pos + 3] & 0xF;
    consumed += 4;
  }
  gs.valid = TRUE;
  *out = gs;
  *gs_id_out = gs_id;
  return consumed;
}

// Parse a reassembled table. Returns the station count, or -1 if malformed;
// on success the stations are written into gs_learned.
static int systable_decode(const uint8_t *buf, int len, int version, GString *out, int indent) {
  HFDL_GS_LEARNED parsed[GS_LEARNED_MAX];
  uint8_t ids[GS_LEARNED_MAX];
  int n = 0;

  while (len >= SYSTABLE_GS_MIN_LEN && n < GS_LEARNED_MAX) {
    uint8_t id;
    HFDL_GS_LEARNED gs;
    int used = systable_decode_gs(buf, len, &gs, &id);
    if (used <= 0) return -1;
    parsed[n] = gs; ids[n] = id; n++;
    buf += used; len -= used;
  }
  if (n == 0) return -1;

  // Only commit a table that is actually newer, so a stale retransmission of an
  // older version cannot roll the frequency map back.
  if (!systable_is_newer(gs_learned_version, version)) {
    emit(out, indent, "System table version %d received (keeping version %d)",
         version, gs_learned_version);
    return n;
  }
  memset(gs_learned, 0, sizeof(gs_learned));
  for (int i = 0; i < n; i++) gs_learned[ids[i]] = parsed[i];
  gs_learned_version = version;

  emit(out, indent, "System table version %d complete — %d ground stations", version, n);
  for (int i = 0; i < n; i++) {
    const char *name = hfdl_msg_gs_name(ids[i]);
    GString *fl = g_string_new(NULL);
    for (int f = 0; f < parsed[i].freq_cnt; f++)
      g_string_append_printf(fl, "%s%u", f ? ", " : "", parsed[i].freqs[f]);
    char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(lat, sizeof(lat), "%.4f", parsed[i].lat);
    g_ascii_formatd(lon, sizeof(lon), "%.4f", parsed[i].lon);
    emit(out, indent + 1, "GS %u%s%s%s  %s, %s  [%s kHz]", ids[i],
         name ? " (" : "", name ? name : "", name ? ")" : "", lat, lon,
         fl->len ? fl->str : "none");
    g_string_free(fl, TRUE);
  }
  return n;
}

// Store one fragment; when the set completes, decode and adopt it.
static void systable_store_pdu(int version, int idx, int total,
                               const uint8_t *buf, int len, GString *out, int indent) {
  if (total < 1 || total > SYSTABLE_MAX_PDUS || idx < 0 || idx >= total) return;
  if (len <= 0 || len > SYSTABLE_PDU_MAX_LEN) return;

  // A version or set-size change means the sender started a different table;
  // whatever we had collected belongs to the old one.
  if (systable_set.in_use &&
      (systable_set.version != version || systable_set.len != total))
    systable_set_reset();

  if (!systable_set.in_use) {
    systable_set.in_use  = TRUE;
    systable_set.version = version;
    systable_set.len     = total;
  }
  if (systable_set.frag_len[idx] == 0) systable_set.have++;
  systable_set.frag_len[idx] = len;
  memcpy(systable_set.frag[idx], buf, (size_t)len);

  emit(out, indent, "System table v%d: part %d of %d (%d/%d collected)",
       version, idx + 1, total, systable_set.have, total);
  if (systable_set.have < total) return;

  uint8_t whole[SYSTABLE_MAX_PDUS * SYSTABLE_PDU_MAX_LEN];
  int pos = 0;
  for (int i = 0; i < total; i++) {
    memcpy(whole + pos, systable_set.frag[i], (size_t)systable_set.frag_len[i]);
    pos += systable_set.frag_len[i];
  }
  systable_set_reset();

  if (systable_decode(whole, pos, version, out, indent) < 0)
    emit(out, indent, "System table v%d: malformed, discarded", version);
}

// kHz for a station's frequency slot, or 0 if unknown. The over-the-air table
// wins when we have one — it is current, the snapshot is not.
static uint32_t gs_freq(uint8_t gs_id, int slot) {
  if (slot < 0 || slot >= GS_MAX_FREQ_CNT) return 0;
  if (gs_id < GS_LEARNED_MAX && gs_learned[gs_id].valid) {
    if (slot < gs_learned[gs_id].freq_cnt) return gs_learned[gs_id].freqs[slot];
    return 0;                            // learned table is authoritative
  }
  const HFDL_GS *gs = gs_lookup(gs_id);
  if (gs == NULL || slot >= gs->freq_cnt) return 0;
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

// --- live activity tables ---------------------------------------------------
//
// What the panel's Stations/Aircraft views show. Written here on the audio
// thread as frames are parsed and read from the GTK thread, so the two arrays
// (and only they) are behind a mutex; everything else in this file stays
// audio-thread-only.

static GMutex activity_lock;

typedef struct {
  gboolean seen;              // reported by a squitter or heard directly
  gboolean utc_sync;
  uint32_t inuse_mask;
  gint64   last_us;           // last frame FROM this station
  int      frames;
} GS_ACTIVITY;

typedef struct {
  gboolean seen;
  char     flight[8];
  gboolean have_pos;
  double   lat, lon;
  gint64   last_us;
  int      frames;
} AC_ACTIVITY;

static GS_ACTIVITY gs_act[GS_LEARNED_MAX];
static AC_ACTIVITY ac_act[256];

// The aircraft the MPDU currently being walked belongs to, so an HFNPDU nested
// several layers down can attribute its flight id / position without threading
// the id through every parse function. Audio-thread only.
static int current_ac_id = -1;

static void activity_clear(void) {
  g_mutex_lock(&activity_lock);
  memset(gs_act, 0, sizeof(gs_act));
  memset(ac_act, 0, sizeof(ac_act));
  g_mutex_unlock(&activity_lock);
}

static void gs_heard(uint8_t id) {
  if (id >= GS_LEARNED_MAX) return;
  g_mutex_lock(&activity_lock);
  gs_act[id].seen = TRUE;
  gs_act[id].last_us = g_get_monotonic_time();
  gs_act[id].frames++;
  g_mutex_unlock(&activity_lock);
}

static void gs_status(uint8_t id, gboolean utc_sync, uint32_t inuse) {
  if (id >= GS_LEARNED_MAX) return;
  g_mutex_lock(&activity_lock);
  gs_act[id].seen = TRUE;
  gs_act[id].utc_sync = utc_sync;
  gs_act[id].inuse_mask = inuse;
  g_mutex_unlock(&activity_lock);
}

static void ac_heard(uint8_t id) {
  g_mutex_lock(&activity_lock);
  ac_act[id].seen = TRUE;
  ac_act[id].last_us = g_get_monotonic_time();
  ac_act[id].frames++;
  g_mutex_unlock(&activity_lock);
}

static void ac_seen(uint8_t id) {          // addressed by a ground station
  g_mutex_lock(&activity_lock);
  ac_act[id].seen = TRUE;
  g_mutex_unlock(&activity_lock);
}

static void ac_set_flight(uint8_t id, const char *flight, gboolean have_pos,
                          double lat, double lon) {
  g_mutex_lock(&activity_lock);
  ac_act[id].seen = TRUE;
  if (flight && flight[0]) g_strlcpy(ac_act[id].flight, flight, sizeof(ac_act[id].flight));
  if (have_pos) { ac_act[id].have_pos = TRUE; ac_act[id].lat = lat; ac_act[id].lon = lon; }
  g_mutex_unlock(&activity_lock);
}

// --- ACARS multi-block reassembly ------------------------------------------
//
// Port of libacars' reassembly.c driven with its ACARS profile (acars.c), cut
// down to what ACARS actually needs: in-order delivery only, so a fragment list
// is just a growing string and "have we got them all" is "did the final block
// arrive". Out-of-order delivery exists in libacars solely for OHMA.
//
// An ACARS message longer than one block is split by the sender, every block
// but the last terminated with ETB instead of ETX. Without this the operator
// sees each block on its own, cut mid-word — the long ones (flight plans,
// weather, free text) are exactly the interesting ones.
//
//   key   = registration + label + message number  (the message number is the
//           3-char field, NOT including its trailing sequence character)
//   seq   = downlink: the message number's sequence char, 'A' -> 0
//           uplink:   the block id, 'A' -> 0, wrapping after 'X'
//   first = downlink: 0 (a downlink always starts at 'A')
//           uplink:   unknown (any block id may start a message)
//   final = the block ended with ETX rather than ETB
//
// Timeouts are ARINC 618's HFGT4/HFAT4 (dumphfdl selects LA_ACARS_BEARER_HFDL).

#define ACARS_REASM_MAX      16                 // messages in flight at once
#define ACARS_REASM_MAX_LEN  (32 * 1024)        // per-message cap
#define ACARS_SEQ_UNINIT     (-2)
#define ACARS_SEQ_NONE       (-1)               // "no such value" for first/wrap
#define ACARS_SEQ_WRAP_UP    ('X' - 'A')
#define ACARS_TMO_DOWN_US    (1260LL * 1000000) // HFGT4
#define ACARS_TMO_UP_US      (370LL  * 1000000) // HFAT4

typedef enum {
  ACARS_REASM_SKIPPED,      // not fragmented (or only the last block survived)
  ACARS_REASM_IN_PROGRESS,
  ACARS_REASM_COMPLETE,
  ACARS_REASM_DUPLICATE,
  ACARS_REASM_OUT_OF_SEQ,   // a block was lost — reassembly abandoned
} ACARS_REASM_STATUS;

typedef struct {
  gboolean in_use;
  char     reg[8], label[3], msg_num[4];
  gboolean downlink;
  int      prev_seq;
  gint64   first_us;        // arrival of the first block (for the timeout)
  int      frags;
  GString *text;
} ACARS_REASM;

static ACARS_REASM acars_reasm[ACARS_REASM_MAX];

// Test seam: the self-test drives the timeout path on a mock clock, since no
// test can wait 21 minutes for HFGT4 to expire.
static gint64 reasm_test_now_us = 0;
static gint64 reasm_now(void) {
  return reasm_test_now_us ? reasm_test_now_us : g_get_monotonic_time();
}

static void acars_reasm_free(ACARS_REASM *e) {
  if (e->text) { g_string_free(e->text, TRUE); e->text = NULL; }
  e->in_use = FALSE;
}

static void acars_reasm_clear(void) {
  for (int i = 0; i < ACARS_REASM_MAX; i++) acars_reasm_free(&acars_reasm[i]);
}

static gint64 acars_reasm_timeout(gboolean downlink) {
  return downlink ? ACARS_TMO_DOWN_US : ACARS_TMO_UP_US;
}

// Reap entries whose reassembly timer expired. Also the only thing keeping the
// table from filling up with messages whose remaining blocks never arrive.
static void acars_reasm_expire(gint64 now_us) {
  for (int i = 0; i < ACARS_REASM_MAX; i++) {
    ACARS_REASM *e = &acars_reasm[i];
    if (e->in_use && now_us - e->first_us > acars_reasm_timeout(e->downlink))
      acars_reasm_free(e);
  }
}

static ACARS_REASM *acars_reasm_lookup(const char *reg, const char *label,
                                       const char *msg_num) {
  for (int i = 0; i < ACARS_REASM_MAX; i++) {
    ACARS_REASM *e = &acars_reasm[i];
    if (e->in_use && !strcmp(e->reg, reg) && !strcmp(e->label, label) &&
        !strcmp(e->msg_num, msg_num))
      return e;
  }
  return NULL;
}

// Take a free slot, or recycle the oldest in-flight one if the table is full.
static ACARS_REASM *acars_reasm_alloc(void) {
  ACARS_REASM *oldest = NULL;
  for (int i = 0; i < ACARS_REASM_MAX; i++) {
    if (!acars_reasm[i].in_use) return &acars_reasm[i];
    if (oldest == NULL || acars_reasm[i].first_us < oldest->first_us)
      oldest = &acars_reasm[i];
  }
  acars_reasm_free(oldest);
  return oldest;
}

// Add one block. On COMPLETE, *full points at the assembled text (owned by the
// caller, which must free it); otherwise *full is left NULL.
static ACARS_REASM_STATUS acars_reasm_add(const char *reg, const char *label,
                                          const char *msg_num, gboolean downlink,
                                          int seq, gboolean final,
                                          const char *data, int dlen,
                                          char **full) {
  gint64 now_us = reasm_now();
  int seq_first = downlink ? 0 : ACARS_SEQ_NONE;
  int seq_wrap  = downlink ? ACARS_SEQ_NONE : ACARS_SEQ_WRAP_UP;

  *full = NULL;
  acars_reasm_expire(now_us);

  ACARS_REASM *e = acars_reasm_lookup(reg, label, msg_num);
  if (e != NULL && now_us - e->first_us > acars_reasm_timeout(downlink)) {
    // Expired between the sweep above and now only if the direction differs;
    // treat this block as the start of a new message either way.
    acars_reasm_free(e);
    e = NULL;
  }

  if (e == NULL) {
    // Don't start a message on a block we know cannot be its first one...
    if (seq_first != ACARS_SEQ_NONE && seq != seq_first) return ACARS_REASM_OUT_OF_SEQ;
    // ...and don't bother tracking one that is already complete. This is the
    // overwhelmingly common case: a single-block message.
    if (final) return ACARS_REASM_SKIPPED;

    e = acars_reasm_alloc();
    e->in_use   = TRUE;
    e->downlink = downlink;
    e->prev_seq = ACARS_SEQ_UNINIT;
    e->first_us = now_us;
    e->frags    = 0;
    e->text     = g_string_new(NULL);
    g_strlcpy(e->reg,     reg,     sizeof(e->reg));
    g_strlcpy(e->label,   label,   sizeof(e->label));
    g_strlcpy(e->msg_num, msg_num, sizeof(e->msg_num));
  }

  // Uplink block ids run A..X and then start over; rebase so the increment
  // check below still sees a step of one.
  if (seq_wrap != ACARS_SEQ_NONE && seq == 0 && e->prev_seq == seq_wrap)
    e->prev_seq = -1;

  if (e->prev_seq == seq || (seq_wrap == ACARS_SEQ_NONE && seq < e->prev_seq))
    return ACARS_REASM_DUPLICATE;

  if (e->prev_seq != ACARS_SEQ_UNINIT && seq != e->prev_seq + 1) {
    acars_reasm_free(e);            // a block was lost; the rest is unusable
    return ACARS_REASM_OUT_OF_SEQ;
  }

  if (dlen > 0) g_string_append_len(e->text, data, dlen);
  if (e->text->len > ACARS_REASM_MAX_LEN) {
    acars_reasm_free(e);
    return ACARS_REASM_OUT_OF_SEQ;
  }
  e->prev_seq = seq;
  e->frags++;

  if (!final) return ACARS_REASM_IN_PROGRESS;

  *full = g_string_free(e->text, FALSE);
  e->text = NULL;
  acars_reasm_free(e);
  return ACARS_REASM_COMPLETE;
}

void hfdl_msg_reset(void) {
  memset(ac_cache, 0, sizeof(ac_cache));
  acars_reasm_clear();
  hfdl_miam_reset();        // partial file transfers
  hfdl_ohma_reset();        // partial OHMA conversations
  activity_clear();
  current_ac_id = -1;
  systable_set_reset();     // partial table: worthless once the stream restarts
  // gs_learned is deliberately kept: it is the current world-wide station list,
  // not per-session state, and re-learning it costs another full PDU set.
}

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
// buf starts at the SOH byte. Renders the message header + text, feeding
// multi-block messages through the reassembly table above: a fragment shows its
// header plus a "Reassembly:" line, and the block that completes the message
// prints the whole text.

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

  g_strchomp(reg);

  // Reassembly. The key is the message number WITHOUT its trailing sequence
  // character (that character is the sequence itself); an uplink has no message
  // number at all, so its key is registration+label and its sequence is the
  // block id.
  char msg_num_key[4] = "";
  int  seq = -1;
  if (downlink) {
    if (msg_num[0] != '\0') {
      memcpy(msg_num_key, msg_num, 3); msg_num_key[3] = '\0';
      seq = msg_num[3] - 'A';
    }
  } else {
    seq = block_id - 'A';
  }

  char *reasm_txt = NULL;
  ACARS_REASM_STATUS reasm = ACARS_REASM_SKIPPED;
  if (seq >= 0)
    reasm = acars_reasm_add(reg, label, msg_num_key, downlink, seq, final_block,
                            p, remaining, &reasm_txt);

  emit(out, indent, "ACARS %s  Reg: %s  Label: %s  Blk: %c%s  Mode: %c  Ack: %c  CRC %s",
       downlink ? "downlink" : "uplink", reg, label, block_id,
       final_block ? "" : " (more)", mode, ack, crc_ok ? "OK" : "FAIL");
  if (downlink)
    emit(out, indent + 1, "Flight: %s  Msg no: %s", g_strchomp(flight_id), msg_num);
  switch (reasm) {
    case ACARS_REASM_IN_PROGRESS:
      emit(out, indent + 1, "Reassembly: in progress"); break;
    case ACARS_REASM_COMPLETE:
      emit(out, indent + 1, "Reassembly: complete"); break;
    case ACARS_REASM_DUPLICATE:
      emit(out, indent + 1, "Reassembly: duplicate block"); break;
    case ACARS_REASM_OUT_OF_SEQ:
      emit(out, indent + 1, "Reassembly: block(s) lost — cannot reassemble"); break;
    case ACARS_REASM_SKIPPED:
      break;                                    // not fragmented; nothing to say
  }

  // A completed message prints in full; anything else prints just this block.
  const char *body     = reasm_txt ? reasm_txt : p;
  int         body_len = reasm_txt ? (int)strlen(reasm_txt) : remaining;

  // Applications carried inside the message text, decoded BEFORE the raw text
  // is printed so the interpretation leads and the hex backs it up. Only on a
  // complete message — half an envelope has neither a valid CRC nor a parsable
  // payload.
  //
  // ARINC-622 (ADS-C position reports, FANS-1/A CPDLC) is tried on every label:
  // its envelope match is anchored at the start of the text, which is a strong
  // enough test on its own, and that is the behaviour verified on air. MIAM and
  // OHMA are gated on the labels that carry them (libacars' acars.c), because
  // their frame ids are single characters — "TEST MESSAGE" opens exactly like a
  // MIAM Single Transfer, and only the label keeps that apart.
  if (body_len > 0 && reasm != ACARS_REASM_IN_PROGRESS) {
    char *b0 = g_strndup(body, (gsize)body_len);
    gboolean claimed = hfdl_arinc_decode(b0, downlink, out, indent + 1);
    if (!claimed && !strcmp(label, "H1")) {
      if (!hfdl_miam_decode(reg, b0, out, indent + 1))
        hfdl_ohma_decode(reg, b0, out, indent + 1);
    } else if (!claimed && !strcmp(label, "MA")) {
      hfdl_miam_decode(reg, b0, out, indent + 1);
    }
    g_free(b0);
  }

  if (body_len > 0) {
    // Render the message text line by line, with non-printables shown as '.'.
    GString *line = g_string_new(NULL);
    for (int i = 0; i <= body_len; i++) {
      char c = (i < body_len) ? body[i] : '\n';
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

  g_free(reasm_txt);
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
  // "No fix" has two encodings and neither is a place: all-zero (0,0 is not the
  // Gulf of Guinea) and all-ones, which decodes to 180,180 — impossible, since
  // latitude cannot exceed 90. Printing those as coordinates reads like a real
  // position report, so say what it is.
  gboolean pos_ok = (lat != 0.0 || lon != 0.0) && lat <= 90.0 && lat >= -90.0;
  char latbuf[G_ASCII_DTOSTR_BUF_SIZE], lonbuf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(latbuf, sizeof(latbuf), "%.4f", lat);
  g_ascii_formatd(lonbuf, sizeof(lonbuf), "%.4f", lon);
  if (pos_ok)
    emit(out, indent, "Flight: %s   Pos: %s, %s   Time: %02d:%02d:%02d",
         flight_id[0] ? flight_id : "(none)", latbuf, lonbuf, h, m, s);
  else
    emit(out, indent, "Flight: %s   Pos: no fix   Time: %02d:%02d:%02d",
         flight_id[0] ? flight_id : "(none)", h, m, s);

  // Feed the Aircraft view. Two encodings mean "no fix" rather than a place:
  // all-zero (0,0 — not the Gulf of Guinea) and all-ones, which comes out as
  // 180,180 and is impossible anyway (latitude cannot exceed 90). The decode
  // text above still prints whatever was sent; only the table filters.
  if (current_ac_id >= 0)
    ac_set_flight((uint8_t)current_ac_id, flight_id, pos_ok, lat, lon);
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
    case HFNPDU_SYSTEM_TABLE: {
      // The 5-octet header (type .. table version) is repeated in every PDU of
      // the set; everything after it is a slice of the table itself.
      if (len < SYSTABLE_HFNPDU_HDR_LEN) { emit(out, indent, "truncated"); break; }
      int version = buf[3] >> 4 | buf[4] << 4;
      int idx     = buf[2] & 0xF;
      int total   = (buf[2] >> 4) + 1;
      if (len > SYSTABLE_HFNPDU_HDR_LEN)
        systable_store_pdu(version, idx, total, buf + SYSTABLE_HFNPDU_HDR_LEN,
                           len - SYSTABLE_HFNPDU_HDR_LEN, out, indent);
      else
        emit(out, indent, "Version: %d   Part %d of %d (no data)", version, idx + 1, total);
      break;
    }

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
      // A logon confirm is addressed to the broadcast id, not to the aircraft
      // being logged on, so without this the newly identified aircraft would
      // not appear in the Aircraft view until it transmitted something.
      ac_seen(buf[4]);
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
  gs_heard(src);
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
    gs_status(gs[i].id, gs[i].utc, gs[i].freqs);
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
    ac_heard(src_ac);
    current_ac_id = src_ac;
    append_ac_id(g, src_ac);
    g_string_append(g, "  ->  GS ");
    append_gs_id(g, dst_gs);
    emit(out, 0, "HFDL downlink MPDU (air->gnd)  AC %s", g->str);
    if (lpdu_list_decode(buf + 6, dataptr, end, lpdu_cnt, out, 1) < 0)
      emit(out, 1, "LPDU list truncated");
  } else {
    uint8_t src_gs = buf[1] & 0x7f;
    gs_heard(src_gs);
    append_gs_id(g, src_gs);
    emit(out, 0, "HFDL uplink MPDU (gnd->air)  GS %s  %d aircraft", g->str, aircraft_cnt);
    const uint8_t *hdrptr = buf + 2;            // first AC ID octet
    for (int i = 0; i < aircraft_cnt; i++) {
      uint8_t dst_ac = *hdrptr++;
      ac_seen(dst_ac);
      current_ac_id = dst_ac;
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

// --- activity snapshots for the panel ---------------------------------------

int hfdl_msg_systable_version(void) { return gs_learned_version; }

int hfdl_msg_gs_list(HFDL_GS_INFO *out, int max) {
  if (out == NULL || max <= 0) return 0;
  int n = 0;
  g_mutex_lock(&activity_lock);
  for (int id = 0; id < GS_LEARNED_MAX && n < max; id++) {
    const HFDL_GS *snap = gs_lookup((uint8_t)id);
    const HFDL_GS_LEARNED *lrn = gs_learned[id].valid ? &gs_learned[id] : NULL;
    // A station is worth listing if we know it from either table or have heard
    // it (an unknown id we have actually heard matters more than a known one).
    if (snap == NULL && lrn == NULL && !gs_act[id].seen) continue;

    HFDL_GS_INFO *o = &out[n++];
    memset(o, 0, sizeof(*o));
    o->id      = (uint8_t)id;
    o->name    = snap ? snap->name : NULL;
    o->learned = lrn != NULL;
    if (lrn) {
      o->freq_cnt = MIN(lrn->freq_cnt, HFDL_MAX_FREQS);
      for (int f = 0; f < o->freq_cnt; f++) o->freqs[f] = lrn->freqs[f];
      o->have_pos = (lrn->lat != 0.0 || lrn->lon != 0.0);
      o->lat = lrn->lat; o->lon = lrn->lon;
    } else if (snap) {
      o->freq_cnt = MIN(snap->freq_cnt, HFDL_MAX_FREQS);
      for (int f = 0; f < o->freq_cnt; f++) o->freqs[f] = snap->freqs[f];
    }
    o->utc_sync      = gs_act[id].utc_sync;
    o->inuse_mask    = gs_act[id].inuse_mask;
    o->last_heard_us = gs_act[id].last_us;
    o->frames        = gs_act[id].frames;
  }
  g_mutex_unlock(&activity_lock);
  return n;
}

int hfdl_msg_ac_list(HFDL_AC_INFO *out, int max) {
  if (out == NULL || max <= 0) return 0;
  int n = 0;
  g_mutex_lock(&activity_lock);
  for (int id = 0; id < 256 && n < max; id++) {
    if (!ac_act[id].seen) continue;
    HFDL_AC_INFO *o = &out[n++];
    memset(o, 0, sizeof(*o));
    o->ac_id = (uint8_t)id;
    o->icao  = ac_cache[id];
    g_strlcpy(o->flight, ac_act[id].flight, sizeof(o->flight));
    o->have_pos      = ac_act[id].have_pos;
    o->lat           = ac_act[id].lat;
    o->lon           = ac_act[id].lon;
    o->last_heard_us = ac_act[id].last_us;
    o->frames        = ac_act[id].frames;
  }
  g_mutex_unlock(&activity_lock);

  // Most recently heard first; never-heard entries (addressed but silent) last.
  for (int i = 1; i < n; i++) {
    HFDL_AC_INFO key = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].last_heard_us < key.last_heard_us) { out[j + 1] = out[j]; j--; }
    out[j + 1] = key;
  }
  return n;
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

// kHz -> the 3-octet packed-BCD frequency the system table carries (100 Hz per
// unit), i.e. the inverse of systable_decode_frequency().
static void st_freq(uint8_t *out, uint32_t khz) {
  uint32_t hz = khz * 1000u;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) { d[i] = (uint8_t)((hz / 100u) % 10u); hz /= 10u; }
  out[0] = (uint8_t)(d[0] | (d[1] << 4));
  out[1] = (uint8_t)(d[2] | (d[3] << 4));
  out[2] = (uint8_t)(d[4] | (d[5] << 4));
}

static void icao_encode(uint8_t *out, uint32_t icao) {
  for (int i = 0; i < 3; i++) out[i] = REVERSE_BYTE((icao >> (8 * (2 - i))) & 0xff);
}

// A complete ACARS block: SOH .. DEL, with a valid CRC. final=FALSE ends the
// block with ETB instead of ETX, i.e. "more blocks follow".
static int build_acars_blk(uint8_t *out, const char *reg, const char *label,
                           char block_id, const char *flight, const char *msg_num,
                           const char *text, gboolean final) {
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
  out[n++] = final ? ACARS_ETX : ACARS_ETB;
  uint16_t crc = crc16_ccitt(out + crc_start, (uint32_t)(n - crc_start), 0);
  out[n++] = (uint8_t)(crc & 0xff);             // LE, so the check comes to 0
  out[n++] = (uint8_t)(crc >> 8);
  out[n++] = ACARS_DEL;
  return n;
}

static int build_acars(uint8_t *out, const char *reg, const char *label,
                       char block_id, const char *flight, const char *msg_num,
                       const char *text) {
  return build_acars_blk(out, reg, label, block_id, flight, msg_num, text, TRUE);
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

  // (9) ACARS multi-block reassembly. Each block goes through the whole stack
  // (MPDU -> LPDU -> enveloped-data HFNPDU -> ACARS) exactly as a real one does.
  g_string_truncate(out, 0);
  hfdl_msg_reset();

  // Push one ACARS block and return the rendered decode in `out`.
  #define PUSH_BLK(blkid, msgnum, text, final)                                   \
    do {                                                                         \
      g_string_truncate(out, 0);                                                 \
      env[0] = 0xFF; env[1] = HFNPDU_ENVELOPED_DATA;                             \
      int _al = build_acars_blk(env + 2, ".N123AB", "H1", (blkid), "DLH441",     \
                                (msgnum), (text), (final));                      \
      int _lp = build_lpdu(lpdu, LPDU_UNNUMBERED_DATA, env, 2 + _al);            \
      int _fl = build_downlink_mpdu(frame, 42, 3, lpdu, _lp);                    \
      if (!hfdl_msg_decode(frame, _fl, out)) ok = FALSE;                         \
    } while (0)

  // (9a) Three-block downlink: 'A' and 'B' end in ETB, 'C' in ETX. Only the
  // last one may print the text, and it must print ALL of it.
  PUSH_BLK('4', "M01A", "FIRST/", FALSE);
  if (!check(out, "reasm blk 1", (const char *[]){ "Reassembly: in progress", NULL })) ok = FALSE;
  if (strstr(out->str, "| FIRST/") == NULL) {   // fragment text is still shown
    g_printerr("[HFDL msg selftest] reasm blk 1: fragment text missing\n"); ok = FALSE;
  }
  PUSH_BLK('4', "M01B", "SECOND/", FALSE);
  if (!check(out, "reasm blk 2", (const char *[]){ "Reassembly: in progress", NULL })) ok = FALSE;
  PUSH_BLK('4', "M01C", "THIRD", TRUE);
  if (!check(out, "reasm blk 3",
             (const char *[]){ "Reassembly: complete", "FIRST/SECOND/THIRD", NULL })) ok = FALSE;

  // (9b) A repeated block is recognised as a duplicate, not appended twice.
  hfdl_msg_reset();
  PUSH_BLK('4', "M02A", "ONE/", FALSE);
  PUSH_BLK('4', "M02A", "ONE/", FALSE);
  if (!check(out, "reasm duplicate", (const char *[]){ "duplicate block", NULL })) ok = FALSE;
  PUSH_BLK('4', "M02B", "TWO", TRUE);
  if (!check(out, "reasm after duplicate",
             (const char *[]){ "Reassembly: complete", "ONE/TWO", NULL })) ok = FALSE;
  if (strstr(out->str, "ONE/ONE/") != NULL) {
    g_printerr("[HFDL msg selftest] reasm: duplicate block was appended\n"); ok = FALSE;
  }

  // (9c) A lost block abandons the message rather than splicing a hole.
  hfdl_msg_reset();
  PUSH_BLK('4', "M03A", "START/", FALSE);
  PUSH_BLK('4', "M03C", "END", TRUE);           // 'B' never arrived
  if (!check(out, "reasm gap", (const char *[]){ "cannot reassemble", NULL })) ok = FALSE;
  if (strstr(out->str, "START/END") != NULL) {
    g_printerr("[HFDL msg selftest] reasm: spliced across a lost block\n"); ok = FALSE;
  }

  // (9d) Uplink: no message number, so the sequence is the block id itself and
  // any block id may start a message.
  hfdl_msg_reset();
  PUSH_BLK('M', "", "UP-ONE/", FALSE);
  if (!check(out, "reasm uplink blk 1",
             (const char *[]){ "ACARS uplink", "Reassembly: in progress", NULL })) ok = FALSE;
  PUSH_BLK('N', "", "UP-TWO", TRUE);
  if (!check(out, "reasm uplink blk 2",
             (const char *[]){ "Reassembly: complete", "UP-ONE/UP-TWO", NULL })) ok = FALSE;

  // (9e) The reassembly timer. HFGT4 is 21 minutes, so this runs on the mock
  // clock: a second block arriving after it expires starts a new message (and
  // for a downlink, a block that is not 'A' cannot start one).
  hfdl_msg_reset();
  reasm_test_now_us = 1000000;
  PUSH_BLK('4', "M04A", "STALE/", FALSE);
  reasm_test_now_us += (ACARS_TMO_DOWN_US + 1000000);
  PUSH_BLK('4', "M04B", "LATE", TRUE);
  if (!check(out, "reasm timeout", (const char *[]){ "cannot reassemble", NULL })) ok = FALSE;
  if (strstr(out->str, "STALE/LATE") != NULL) {
    g_printerr("[HFDL msg selftest] reasm: timed-out message was still joined\n"); ok = FALSE;
  }
  reasm_test_now_us = 0;
  #undef PUSH_BLK

  // (10) Over-the-air system table. Build a two-PDU set carrying two station
  // records and check that it is reassembled, adopted, and then actually used
  // to resolve a frequency slot.
  hfdl_msg_reset();
  gs_learned_version = -1;
  memset(gs_learned, 0, sizeof(gs_learned));

  // One station record: id/utc-sync, lat, lon, spdu ver + freq count, then
  // (frequency, slot) pairs. Frequencies are packed BCD, 100 Hz per unit.
  #define ST_GS(dst, id, f1, f2)                                                 \
    do {                                                                         \
      uint8_t *_d = (dst);                                                       \
      _d[0] = (uint8_t)((id) | 0x80);        /* UTC synced */                    \
      _d[1] = _d[2] = _d[3] = _d[4] = _d[5] = 0;   /* lat/lon 0 */               \
      _d[6] = (uint8_t)(0x01 | (2 << 3));    /* spdu ver 1, 2 frequencies */     \
      st_freq(_d + 7,  (f1)); _d[10] = 1;                                        \
      st_freq(_d + 11, (f2)); _d[14] = 2;                                        \
    } while (0)

  uint8_t table[64];
  ST_GS(table,      4, 11387, 6661);
  ST_GS(table + 15, 5, 13351, 8921);
  int tlen = 30;

  // Split across two HFNPDUs, each repeating the 5-octet header.
  g_string_truncate(out, 0);
  uint8_t st_pdu[64];
  int split = 14;
  for (int part = 0; part < 2; part++) {
    int off  = part ? split : 0;
    int plen = part ? tlen - split : split;
    st_pdu[0] = 0xFF; st_pdu[1] = HFNPDU_SYSTEM_TABLE;
    st_pdu[2] = (uint8_t)((1 << 4) | part);          // 2 PDUs total, index `part`
    st_pdu[3] = (uint8_t)((60 & 0xF) << 4);          // version 60, low nibble
    st_pdu[4] = (uint8_t)(60 >> 4);
    memcpy(st_pdu + 5, table + off, (size_t)plen);
    lp = build_lpdu(lpdu, LPDU_UNNUMBERED_DATA, st_pdu, 5 + plen);
    fl = build_downlink_mpdu(frame, 42, 3, lpdu, lp);
    g_string_truncate(out, 0);
    if (!hfdl_msg_decode(frame, fl, out)) ok = FALSE;
  }
  if (!check(out, "systable complete",
             (const char *[]){ "System table version 60 complete", "2 ground stations",
                               "GS 4 (Riverhead, New York)", "11387, 6661", NULL })) ok = FALSE;

  // The learned table must now drive slot -> kHz. Slot 1 of GS 4 is 6661 in the
  // table just received; in the embedded snapshot it is 17919, so this only
  // passes if the over-the-air table won.
  if (gs_freq(4, 1) != 6661) {
    g_printerr("[HFDL msg selftest] systable: slot 1 of GS 4 resolved to %u, expected 6661\n",
               gs_freq(4, 1));
    ok = FALSE;
  }

  // An older version must not roll the table back.
  g_string_truncate(out, 0);
  ST_GS(table, 4, 21931, 17919);
  st_pdu[0] = 0xFF; st_pdu[1] = HFNPDU_SYSTEM_TABLE;
  st_pdu[2] = 0x00;                                   // single-PDU set
  st_pdu[3] = (uint8_t)((59 & 0xF) << 4);
  st_pdu[4] = (uint8_t)(59 >> 4);
  memcpy(st_pdu + 5, table, 15);
  lp = build_lpdu(lpdu, LPDU_UNNUMBERED_DATA, st_pdu, 5 + 15);
  fl = build_downlink_mpdu(frame, 42, 3, lpdu, lp);
  if (!hfdl_msg_decode(frame, fl, out)) ok = FALSE;
  if (!check(out, "systable older", (const char *[]){ "keeping version 60", NULL })) ok = FALSE;
  if (gs_freq(4, 1) != 6661) {
    g_printerr("[HFDL msg selftest] systable: an older table overwrote the current one\n");
    ok = FALSE;
  }
  #undef ST_GS
  gs_learned_version = -1;
  memset(gs_learned, 0, sizeof(gs_learned));

  g_string_free(out, TRUE);
  hfdl_msg_reset();
  return ok;
}
