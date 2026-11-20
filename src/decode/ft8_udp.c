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

#include <stdio.h>
#include "log.h"
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "net_compat.h"   // must precede gtk.h: winsock2 before windows.h
#include <gtk/gtk.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"

#include "ft8_udp.h"
#include "ft8_qso.h"
#include "version.h"
#include "wsjtx_wire.h"

// What a logger lists us as.  WSJT-X uses "WSJT-X", JTDX "JTDX": any unique id
// works, and naming ourselves keeps a real WSJT-X beside us distinguishable.
#define UDP_ID "MacHPSDR"
#define HEARTBEAT_US (15 * G_USEC_PER_SEC)   // WSJT-X's own heartbeat interval

// One socket for the life of the destination: the logger keys a client on the
// (address, port) it hears from, so a fresh ephemeral port per datagram would
// look like a new client every time.  Everything here runs on the GTK thread
// (the tick below and ft8_qso's 500 ms poll), so there is no lock.
static int udp_fd = -1;
static struct sockaddr_storage udp_dest;
static socklen_t udp_dest_len = 0;
static char udp_host[64] = "";
static int udp_port = 0;
static gboolean udp_resolve_failed = FALSE;   // logged once per destination

static gboolean announced = FALSE;   // a Heartbeat went to the current destination
static gint64 last_heartbeat = 0;
static gint64 last_status = 0;
static uint8_t last_status_buf[1024];
static int last_status_len = 0;
static guint tick_id = 0;

static void udp_drop_socket(void) {
  if (udp_fd >= 0) closesocket(udp_fd);
  udp_fd = -1;
  udp_dest_len = 0;
}

// The socket for the configured host:port, (re)made when either changes.
static gboolean udp_ready(void) {
  if (radio->ft8_log_udp_host[0] == '\0' || radio->ft8_log_udp_port <= 0) return FALSE;
  if (udp_fd >= 0 && udp_port == radio->ft8_log_udp_port &&
      strcmp(udp_host, radio->ft8_log_udp_host) == 0) return TRUE;
  if (udp_port != radio->ft8_log_udp_port || strcmp(udp_host, radio->ft8_log_udp_host) != 0) {
    g_strlcpy(udp_host, radio->ft8_log_udp_host, sizeof(udp_host));
    udp_port = radio->ft8_log_udp_port;
    udp_resolve_failed = FALSE;
    announced = FALSE;
    last_status_len = 0;
  }
  udp_drop_socket();

  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", udp_port);
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(udp_host, portstr, &hints, &res) != 0 || res == NULL) {
    if (!udp_resolve_failed) log_error("ft8-udp: cannot resolve %s:%s\n", udp_host, portstr);
    udp_resolve_failed = TRUE;
    return FALSE;
  }
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0 || res->ai_addrlen > sizeof(udp_dest)) {
    if (!udp_resolve_failed) net_perror("ft8-udp: socket");
    udp_resolve_failed = TRUE;
    if (fd >= 0) closesocket(fd);
    freeaddrinfo(res);
    return FALSE;
  }
  // Allow broadcast destinations (e.g. 255.255.255.255) to work too.
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
  memcpy(&udp_dest, res->ai_addr, res->ai_addrlen);
  udp_dest_len = (socklen_t)res->ai_addrlen;
  udp_fd = fd;
  freeaddrinfo(res);
  log_info("ft8-udp: sending to %s:%d as \"%s\" (WSJT-X protocol)\n", udp_host, udp_port, UDP_ID);
  return TRUE;
}

static gboolean udp_send(const uint8_t *buf, int len, const char *what) {
  if (len < 0) { log_error("ft8-udp: %s does not fit a datagram\n", what); return FALSE; }
  if (!udp_ready()) return FALSE;
  if (sendto(udp_fd, (const char *)buf, len, 0, (struct sockaddr *)&udp_dest, udp_dest_len) < 0) {
    log_error("ft8-udp: sendto %s:%d (%s) failed\n", udp_host, udp_port, what);
    return FALSE;
  }
  return TRUE;
}

static void send_close(void) {
  uint8_t buf[64];
  udp_send(buf, wsjtx_close(buf, sizeof(buf), UDP_ID), "close");
  announced = FALSE;
  last_status_len = 0;
}

static gboolean ft8_decoding(void) {
  return radio->decode_mode == DECODE_FT8 || radio->decode_mode == DECODE_FT4;
}

// Status: what the logger shows as our frequency and mode.  Sent while an FT8/FT4
// decoder runs, whenever it changes, and with every heartbeat.
static int build_status(uint8_t *buf, size_t cap) {
  const char *mode = radio->decode_mode == DECODE_FT4 ? "FT4" : "FT8";
  WSJTX_STATUS_INFO s;
  memset(&s, 0, sizeof(s));
  s.dial_hz = radio->active_receiver ? radio->active_receiver->frequency_a : 0;
  s.mode = mode;
  s.tx_mode = mode;
  s.dx_call = ft8_qso_dx_call();
  s.tx_enabled = ft8_qso_tx_enabled();
  s.transmitting = isTransmitting(radio);
  s.decoding = TRUE;
  s.rx_df = s.tx_df = radio->ft8_tx_offset > 0 ? (unsigned)radio->ft8_tx_offset : 0;
  s.de_call = radio->station_call;
  s.de_grid = radio->station_grid;
  s.tx_message = ft8_qso_next_tx();
  return wsjtx_status(buf, cap, UDP_ID, &s);
}

static gboolean udp_tick(gpointer data) {
  (void)data;
  if (radio == NULL) return G_SOURCE_CONTINUE;
  if (!radio->ft8_log_udp) {
    if (announced) send_close();
    udp_drop_socket();
    return G_SOURCE_CONTINUE;
  }

  gint64 now = g_get_monotonic_time();
  uint8_t buf[1024];
  // udp_ready() first: a changed destination clears `announced`, so the new
  // logger hears a heartbeat at once rather than up to 15 s later.
  if (!udp_ready()) return G_SOURCE_CONTINUE;
  gboolean heartbeat = !announced || now - last_heartbeat >= HEARTBEAT_US;
  if (heartbeat) {
    if (udp_send(buf, wsjtx_heartbeat(buf, sizeof(buf), UDP_ID, version, build_version), "heartbeat")) {
      if (!announced) log_info("ft8-udp: heartbeat to %s:%d\n", udp_host, udp_port);
      announced = TRUE;
      last_heartbeat = now;
    }
  }

  if (ft8_decoding()) {
    int len = build_status(buf, sizeof(buf));
    gboolean changed = len != last_status_len || (len > 0 && memcmp(buf, last_status_buf, len) != 0);
    if (changed || heartbeat || now - last_status >= HEARTBEAT_US) {
      if (udp_send(buf, len, "status") && len > 0) {
        memcpy(last_status_buf, buf, len);
        last_status_len = len;
        last_status = now;
      }
    }
  } else {
    last_status_len = 0;
  }
  return G_SOURCE_CONTINUE;
}

void ft8_udp_init(void) {
  if (tick_id == 0) tick_id = g_timeout_add_seconds(1, udp_tick, NULL);
}

void ft8_udp_shutdown(void) {
  if (radio != NULL && announced) send_close();
  udp_drop_socket();
}

void ft8_udp_log(const WSJTX_QSO_INFO *qso, const char *adif_record) {
  if (radio == NULL || !radio->ft8_log_udp) return;
  if (qso == NULL || adif_record == NULL || adif_record[0] == '\0') return;

  // WSJT-X sends both: QSO Logged with the fields, then the ADIF record. Loggers
  // built for WSJT-X expect the pair and log once; some read only one of them.
  uint8_t buf[1400];
  int n5 = wsjtx_qso_logged(buf, sizeof(buf), UDP_ID, qso);
  gboolean ok5 = udp_send(buf, n5, "QSO logged");

  // WSJT-X wraps the record in a minimal ADIF document (header + record).
  char adif[1024];
  snprintf(adif, sizeof(adif),
           "\n<adif_ver:5>3.1.0\n<programid:8>MacHPSDR\n<EOH>\n%s", adif_record);
  int n12 = wsjtx_logged_adif(buf, sizeof(buf), UDP_ID, adif);
  gboolean ok12 = udp_send(buf, n12, "logged ADIF");

  if (ok5 || ok12)
    log_info("ft8-udp: logged QSO with %s to %s:%d\n", qso->dx_call, udp_host, udp_port);
}
