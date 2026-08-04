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
 * HFDL application layer: MPDU -> LPDU -> HFNPDU -> ACARS message text
 * (parity 4.5, phase 4).
 *
 * hfdl_pdu.c validates a decoded frame and summarises its header; this module
 * walks the whole protocol stack inside it and renders the human-readable
 * decode the operator actually wants to read — logon/logoff exchanges with ICAO
 * addresses, aircraft position/performance/frequency reports, and the ACARS
 * message text carried in "enveloped data" HFNPDUs.
 *
 * NATIVE PORT, not a libacars link. dumphfdl's lpdu.c/hfnpdu.c/acars.c are
 * coupled to libacars only for OUTPUT plumbing (la_vstring / la_json /
 * la_proto_node trees / la_dict lookups) — the bit-level parsing is dumphfdl's
 * own. So the parsing is ported faithfully and the output is rendered straight
 * into a GString, and the one piece libacars really did decode (the ACARS block
 * itself: SOH/mode/registration/label/block-id/STX/text/CRC/DEL) is ported from
 * libacars' la_acars_parse_and_reassemble. That keeps a ~3 MB vendored library
 * with an ASN.1 tree out of the repo.
 *
 * Deliberately NOT ported (all need a real signal to be worth having, and none
 * blocks reading a message): ACARS multi-block reassembly, the ARINC-622 /
 * CPDLC / ADS-C / MIAM / OHMA sub-decoders (libacars' actual bulk), and the
 * over-the-air system-table reassembly — the ground-station names/frequencies
 * come from an embedded snapshot instead.
 *
 * Threading: audio-thread only (called from hfdl_decoder.c's frame handler).
 * The aircraft-id cache is plain static state on that thread — no lock.
 */

#ifndef _HFDL_MSG_H
#define _HFDL_MSG_H

#include <glib.h>
#include <stdint.h>

// Decode a complete HFDL frame (raw PDU bytes from the framer) and append the
// human-readable decode to out. Returns TRUE if the frame's header FCS checked
// out (i.e. this is a real PDU, not garbage). GTK-independent.
gboolean hfdl_msg_decode(const uint8_t *buf, int len, GString *out);

// Drop the aircraft-id -> ICAO cache (called when the decoder resets).
void hfdl_msg_reset(void);

// Ground-station name from the embedded system-table snapshot, or NULL.
const char *hfdl_msg_gs_name(uint8_t gs_id);

// --- live activity tables ---------------------------------------------------
//
// The decode text scrolls away; these are the standing picture the operator
// wants next to it — which stations are up on which frequencies, and which
// aircraft have been heard. Filled in on the audio thread as frames are parsed,
// read by the panel on the GTK thread, so both list calls take an internal lock
// and hand back a snapshot.

#define HFDL_MAX_FREQS 20

typedef struct {
  uint8_t     id;
  const char *name;           // NULL when the station is not in the snapshot
  gboolean    learned;        // frequencies came from the over-the-air table
  gboolean    utc_sync;
  gboolean    have_pos;
  double      lat, lon;
  int         freq_cnt;
  uint32_t    freqs[HFDL_MAX_FREQS];   // kHz
  uint32_t    inuse_mask;     // bit n set = slot n reported in use
  gint64      last_heard_us;  // g_get_monotonic_time() of the last frame FROM it, 0 = never
  int         frames;
} HFDL_GS_INFO;

typedef struct {
  uint8_t   ac_id;
  uint32_t  icao;             // 0 = not seen in a logon exchange
  char      flight[8];
  gboolean  have_pos;
  double    lat, lon;
  gint64    last_heard_us;
  int       frames;
} HFDL_AC_INFO;

// Fill out[] with the known ground stations (every station in the table, heard
// or not) / the aircraft heard so far, most recent first. Returns the count.
int hfdl_msg_gs_list(HFDL_GS_INFO *out, int max);
int hfdl_msg_ac_list(HFDL_AC_INFO *out, int max);

// Version of the system table in use, or -1 if only the embedded snapshot is.
int hfdl_msg_systable_version(void);

// Headless self-test: synthesises valid frames (logon request, performance
// data, frequency data, an ACARS-bearing enveloped-data HFNPDU) with correct
// FCS/CRC and asserts the rendered text; also asserts corrupt input is rejected.
gboolean hfdl_msg_selftest(void);

#endif
