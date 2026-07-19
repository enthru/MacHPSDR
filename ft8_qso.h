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
 * FT8 auto-QSO state machine (Phase 2).
 *
 * Polls the decoder every ~500 ms on the GTK main thread, drives the standard
 * WSJT-X exchange (CQ -> grid -> report -> R-report -> RR73 -> 73), keys TX via
 * ft8_encoder on the correct (opposite) UTC slot, and appends completed QSOs to
 * an ADIF log.  Works with standard callsigns (structured fields from the
 * decoder); non-standard/hashed calls are a follow-up.
 */

#ifndef _FT8_QSO_H
#define _FT8_QSO_H

#include <glib.h>
#include "ft8_decoder.h"

// Start the poll timer.  Call once at start-up (after ft8_decoder_init()).
extern void ft8_qso_init(void);

// Begin (auto-)calling CQ using the station call/grid and TX offset/slot from
// the global RADIO.  No-op if the station callsign is unset.
extern void ft8_qso_start_cq(void);

// Click-to-call: answer a decoded CQ (or reply to a message addressed to us).
// Copies what it needs from d.
extern void ft8_qso_answer(const FT8_DECODE *d);

// Abort any QSO/CQ, disarm TX, return to idle.
extern void ft8_qso_halt(void);

// TRUE while a QSO or CQ sequence is in progress.
extern gboolean ft8_qso_active(void);

// One-line human-readable status for the panel (e.g. "Calling CQ",
// "Working K1ABC: sent -12", "Idle").  Valid until the next call.
extern const char *ft8_qso_status(void);

// The message we will transmit on our next slot (empty string if none).
extern const char *ft8_qso_next_tx(void);

#endif
