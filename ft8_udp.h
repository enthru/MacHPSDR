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
 * Sends the WSJT-X UDP "ADIF" message (schema 2, type 12) to a logger
 * (Log4OM, N1MM+, JTAlert, GridTracker, ...) so QSOs made in MacHPSDR appear
 * in the operator's logbook over the network, exactly as JTDX does.
 */

#ifndef _FT8_UDP_H
#define _FT8_UDP_H

// Broadcast one ADIF record (a single "<CALL:..>...<EOR>" line, as written to
// ft8_log.adi) to the configured host:port if radio->ft8_log_udp is enabled.
// Safe to call from the GTK main thread; never blocks meaningfully.
extern void ft8_udp_log(const char *adif_record);

#endif
