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
 * Reporting of received FT8 spots to the PSK Reporter network
 * (report.pskreporter.info), the same reception-report service WSJT-X/JTDX
 * feed.  Each decode of another station becomes a spot record; the operator's
 * own callsign + grid identify the receiver.
 *
 * The wire format is PSK Reporter's IPFIX-based UDP protocol (RFC 5101 style,
 * private enterprise number 30351), built to match WSJT-X's PSKReporter.cpp so
 * the spots parse on the server.
 *
 * Gated by radio->ft8_pskr AND a non-empty radio->station_call + station_grid
 * (PSK Reporter needs both to attribute the reception) — nothing is sent
 * unless all three are present.
 */

#ifndef _FT8_PSKREPORTER_H
#define _FT8_PSKREPORTER_H

#include <time.h>
#include "ft8_decoder.h"

// Report one 15 s slot's worth of decodes as PSK Reporter reception reports.
// Reads the receiver's dial frequency from radio->active_receiver to turn each
// decode's audio offset into an RF frequency.  slot_time is the UTC time the
// slot was received.  Safe to call from the decoder worker thread; never blocks
// meaningfully.  A no-op unless enabled and the station call/grid are set.
extern void ft8_pskreporter_report(const FT8_DECODE *decodes, int n, time_t slot_time);

#endif
