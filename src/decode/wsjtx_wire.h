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
 * The WSJT-X UDP message format (NetworkMessage.hpp), as bytes and nothing
 * else: no socket, no RADIO, no GTK, so tools/ft8_offline.c can pin it.
 *
 * A logger (RUMlogNG, Log4OM, JTAlert, GridTracker, N1MM+) lists a WSJT-X
 * client only once it has heard a Heartbeat, and shows its frequency/mode from
 * Status; a QSO arrives as QSO Logged (type 5) AND Logged ADIF (type 12), the
 * pair WSJT-X itself sends -- some loggers read only one of them.
 *
 * Every builder returns the datagram length, or -1 if it does not fit `cap`.
 * A NULL string is written as an empty one.
 */

#ifndef _WSJTX_WIRE_H
#define _WSJTX_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define WSJTX_MAGIC         0xadbccbdaU
#define WSJTX_SCHEMA        2U
#define WSJTX_MAX_SCHEMA    3U

#define WSJTX_HEARTBEAT     0U
#define WSJTX_STATUS        1U
#define WSJTX_QSO_LOGGED    5U
#define WSJTX_CLOSE         6U
#define WSJTX_LOGGED_ADIF   12U

typedef struct {
  long long   dial_hz;
  const char *mode;          // "FT8" / "FT4"
  const char *dx_call;
  const char *report;
  const char *tx_mode;
  int         tx_enabled;
  int         transmitting;
  int         decoding;
  unsigned    rx_df;         // audio offset, Hz
  unsigned    tx_df;
  const char *de_call;
  const char *de_grid;
  const char *dx_grid;
  const char *tx_message;
} WSJTX_STATUS_INFO;

typedef struct {
  time_t      time_on;
  time_t      time_off;
  const char *dx_call;
  const char *dx_grid;
  long long   tx_hz;         // RF frequency of the QSO
  const char *mode;
  const char *rst_sent;
  const char *rst_rcvd;
  const char *my_call;
  const char *my_grid;
} WSJTX_QSO_INFO;

extern int wsjtx_heartbeat(uint8_t *buf, size_t cap, const char *id,
                           const char *version, const char *revision);
extern int wsjtx_status(uint8_t *buf, size_t cap, const char *id,
                        const WSJTX_STATUS_INFO *s);
extern int wsjtx_qso_logged(uint8_t *buf, size_t cap, const char *id,
                            const WSJTX_QSO_INFO *q);
extern int wsjtx_logged_adif(uint8_t *buf, size_t cap, const char *id,
                             const char *adif_text);
extern int wsjtx_close(uint8_t *buf, size_t cap, const char *id);

// QDateTime as WSJT-X streams it (Qt_5_4): qint64 Julian day, quint32 ms since
// midnight, quint8 timespec (1 = UTC).  Exposed for the harness.
extern long long wsjtx_julian_day(time_t t);

#endif
