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

#include <string.h>

#include "wsjtx_wire.h"

// QDataStream: big-endian integers; a string is a big-endian uint32 byte count
// followed by the UTF-8 bytes; a bool is one byte.  The writer refuses to run
// past `cap` and remembers that it did, so a builder checks once at the end.
typedef struct {
  uint8_t *buf;
  size_t   cap;
  size_t   off;
  int      overflow;
} WR;

static void put_bytes(WR *w, const void *p, size_t n) {
  if (w->overflow || n > w->cap - w->off) { w->overflow = 1; return; }
  memcpy(w->buf + w->off, p, n);
  w->off += n;
}

static void put_u8(WR *w, unsigned v) {
  uint8_t b = (uint8_t)v;
  put_bytes(w, &b, 1);
}

static void put_u32(WR *w, uint32_t v) {
  uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
  put_bytes(w, b, 4);
}

static void put_u64(WR *w, uint64_t v) {
  put_u32(w, (uint32_t)(v >> 32));
  put_u32(w, (uint32_t)v);
}

static void put_str(WR *w, const char *s) {
  if (s == NULL) s = "";
  size_t n = strlen(s);
  put_u32(w, (uint32_t)n);
  put_bytes(w, s, n);
}

long long wsjtx_julian_day(time_t t) {
  long long days = (long long)t / 86400;
  if ((long long)t % 86400 < 0) days--;
  return days + 2440588LL;   // 1970-01-01 is JD 2440588
}

static void put_datetime(WR *w, time_t t) {
  long long secs = (long long)t % 86400;
  if (secs < 0) secs += 86400;
  put_u64(w, (uint64_t)wsjtx_julian_day(t));
  put_u32(w, (uint32_t)(secs * 1000));
  put_u8(w, 1);   // Qt::UTC
}

static void header(WR *w, uint8_t *buf, size_t cap, uint32_t type, const char *id) {
  w->buf = buf; w->cap = cap; w->off = 0; w->overflow = 0;
  put_u32(w, WSJTX_MAGIC);
  put_u32(w, WSJTX_SCHEMA);
  put_u32(w, type);
  put_str(w, id);
}

static int finish(const WR *w) {
  return w->overflow ? -1 : (int)w->off;
}

int wsjtx_heartbeat(uint8_t *buf, size_t cap, const char *id,
                    const char *version, const char *revision) {
  WR w;
  header(&w, buf, cap, WSJTX_HEARTBEAT, id);
  put_u32(&w, WSJTX_MAX_SCHEMA);
  put_str(&w, version);
  put_str(&w, revision);
  return finish(&w);
}

int wsjtx_status(uint8_t *buf, size_t cap, const char *id, const WSJTX_STATUS_INFO *s) {
  WR w;
  header(&w, buf, cap, WSJTX_STATUS, id);
  put_u64(&w, (uint64_t)(s->dial_hz > 0 ? s->dial_hz : 0));
  put_str(&w, s->mode);
  put_str(&w, s->dx_call);
  put_str(&w, s->report);
  put_str(&w, s->tx_mode);
  put_u8(&w, s->tx_enabled ? 1 : 0);
  put_u8(&w, s->transmitting ? 1 : 0);
  put_u8(&w, s->decoding ? 1 : 0);
  put_u32(&w, s->rx_df);
  put_u32(&w, s->tx_df);
  put_str(&w, s->de_call);
  put_str(&w, s->de_grid);
  put_str(&w, s->dx_grid);
  put_u8(&w, 0);              // Tx watchdog
  put_str(&w, "");            // sub-mode
  put_u8(&w, 0);              // fast mode
  put_u8(&w, 0);              // special operation mode: none
  put_u32(&w, 0xffffffffU);   // frequency tolerance: not applicable
  put_u32(&w, 0xffffffffU);   // T/R period: not applicable
  put_str(&w, "Default");     // configuration name
  put_str(&w, s->tx_message);
  return finish(&w);
}

int wsjtx_qso_logged(uint8_t *buf, size_t cap, const char *id, const WSJTX_QSO_INFO *q) {
  WR w;
  header(&w, buf, cap, WSJTX_QSO_LOGGED, id);
  put_datetime(&w, q->time_off);
  put_str(&w, q->dx_call);
  put_str(&w, q->dx_grid);
  put_u64(&w, (uint64_t)(q->tx_hz > 0 ? q->tx_hz : 0));
  put_str(&w, q->mode);
  put_str(&w, q->rst_sent);
  put_str(&w, q->rst_rcvd);
  put_str(&w, "");            // Tx power
  put_str(&w, "");            // comments
  put_str(&w, "");            // name
  put_datetime(&w, q->time_on);
  put_str(&w, "");            // operator call
  put_str(&w, q->my_call);
  put_str(&w, q->my_grid);
  put_str(&w, "");            // exchange sent
  put_str(&w, "");            // exchange received
  put_str(&w, "");            // ADIF propagation mode
  return finish(&w);
}

int wsjtx_logged_adif(uint8_t *buf, size_t cap, const char *id, const char *adif_text) {
  WR w;
  header(&w, buf, cap, WSJTX_LOGGED_ADIF, id);
  put_str(&w, adif_text);
  return finish(&w);
}

int wsjtx_close(uint8_t *buf, size_t cap, const char *id) {
  WR w;
  header(&w, buf, cap, WSJTX_CLOSE, id);
  return finish(&w);
}
