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
 * VHF ACARS receive decoder — orchestrator.
 *
 * The same aircraft data link as HFDL, but on VHF (129–137 MHz) and over a much
 * simpler radio layer: 2400 bps MSK on an AM carrier instead of an equalised
 * M-PSK burst on a fading HF path.  So this module is small — the physical
 * layer is acars_demod.c, and everything from the block upwards (message
 * header, multi-block reassembly, ARINC-622 / ADS-C / CPDLC, MIAM, OHMA) is the
 * layer HFDL already carries, reached through hfdl_msg_acars_block().  That
 * shared application layer is the entire reason this feature was cheap.
 *
 * Like HFDL and APT it takes RAW OFF-AIR I/Q rather than demodulated audio, so
 * the demod mode only decides what the operator hears.  Two consequences worth
 * knowing:
 *
 *  - Mistuning does not matter.  AM detection is |z|, and a carrier offset only
 *    rotates the phasor, so anything inside the ±4.5 kHz channel filter decodes
 *    identically.  (HFDL needed a seven-way coarse carrier search to survive a
 *    100 Hz error; here there is nothing to search for.)
 *  - An I/Q swap does not matter either, for the reason it does not matter for
 *    APT: the mirror moves the signal to the other side of centre and the
 *    panadapter mirrors with it, so pointing at what you see still works.
 *
 * Channels are 25 kHz apart and a receiver at 192 kHz or more holds several at
 * once, so — as with HFDL — the cursor channel is always decoded and "Scan
 * band" adds every known channel inside the passband.
 *
 * Threading: mirrors hfdl_decoder.c exactly.  set_enabled/add_iq run on the RX
 * audio thread and own the channels with no lock; only the published status and
 * the message/history rings are cross-thread, behind one GMutex.
 */

#ifndef _ACARS_DECODER_H
#define _ACARS_DECODER_H

#include <glib.h>

#define ACARS_MAX_CHANNELS 12

// --- known VHF ACARS channels ----------------------------------------------
// Used by "Scan band" and by the panel's Tune drop-down.  Deliberately NOT a
// list of everything in the air band: 136.9xx and up is mostly VDL Mode 2,
// which this decoder cannot read and which would only waste front-ends.
typedef struct {
  guint32     khz;
  const char *region;
} ACARS_CHANNEL_INFO;

int  acars_channel_count(void);
const ACARS_CHANNEL_INFO *acars_channel_at(int i);

// --- decoder ----------------------------------------------------------------

void acars_decoder_set_enabled(gboolean on);

// Raw off-air I/Q from the receiver ((Q, I) per pair).  centre_hz is the
// receiver centre this block was taken around, cursor_hz the channel the
// operator is pointing at (the CTUN/freetune cursor, else the centre).
void acars_decoder_add_iq(const double *iq, int nframes, int sample_rate,
                          long long centre_hz, long long cursor_hz);

// Already AM-demodulated audio at `rate` — the form ACARS recordings circulate
// in.  Offline harness only; the app always has I/Q.
void acars_decoder_add_audio(const double *samples, int nframes, int stride,
                             double rate);

void     acars_decoder_set_scan(gboolean on);
gboolean acars_decoder_get_scan(void);
int      acars_decoder_channel_count(void);

// Drain the decoded text (panel) / peek at the newest without consuming it
// (bottom Decode readout) — the same non-stealing split as CW and HFDL.
int  acars_decoder_get_messages(char *buf, int buflen);
int  acars_decoder_get_recent(char *buf, int buflen);

void   acars_decoder_get_status(gboolean *listening, int *sample_rate, glong *msgs);
double acars_decoder_get_level_db(void);
glong  acars_decoder_get_messages_count(void);
glong  acars_decoder_get_bad_count(void);
void   acars_decoder_get_tuned(long long *cursor_hz, double *offset_hz);
void   acars_decoder_reset(void);

// --- live aircraft table -----------------------------------------------------
// The decode scrolls away; this is the standing picture the panel shows next to
// it.  Filled on the audio thread, read on the GTK thread, so the list call
// takes an internal lock and hands back a snapshot.
typedef struct {
  char     reg[8];            // tail number as sent (leading '.' stripped)
  char     flight[8];
  char     label[3];
  guint32  khz;               // channel it was last heard on
  gint64   last_heard_us;
  int      msgs;
} ACARS_AC_INFO;

int acars_decoder_ac_list(ACARS_AC_INFO *out, int max);

// Headless self-test: the physical layer's own test plus a full-stack pass —
// modulate a real ACARS block, run it through the I/Q chain and assert the
// rendered message text.
gboolean acars_decoder_selftest(void);

#endif
