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
 * Network logging of completed FT8 QSOs, WSJT-X/JTDX compatible.
 *
 * Speaks the WSJT-X UDP protocol to a logger (RUMlogNG, Log4OM, N1MM+, JTAlert,
 * GridTracker, ...): a Heartbeat every 15 s so the logger lists us as a client,
 * Status (dial frequency, FT8/FT4, Tx state) while an FT8/FT4 decoder runs, and
 * for each completed QSO the pair WSJT-X sends -- QSO Logged and Logged ADIF.
 * Close when the option is switched off or the app exits.
 */

#ifndef _FT8_UDP_H
#define _FT8_UDP_H

#include "wsjtx_wire.h"

// Start the 1 s tick that sends Heartbeat/Status while radio->ft8_log_udp is on.
extern void ft8_udp_init(void);

// Tell the logger we are gone (WSJT-X "Close") and drop the socket.
extern void ft8_udp_shutdown(void);

// Send one completed QSO: its fields, and the ADIF record (a single
// "<CALL:..>...<EOR>" line, as written to ft8_log.adi).  GTK thread only.
extern void ft8_udp_log(const WSJTX_QSO_INFO *qso, const char *adif_record);

#endif
