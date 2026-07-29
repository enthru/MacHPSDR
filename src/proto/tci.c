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

#include <gtk/gtk.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "mode.h"
#include "ext.h"
#include "log.h"
#include "tci.h"

// Suppress SIGPIPE on a broken client socket: macOS uses the SO_NOSIGPIPE
// sockopt, Linux uses the MSG_NOSIGNAL send flag.
#ifdef __APPLE__
  #define TCI_SEND_FLAGS 0
#else
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif
  #define TCI_SEND_FLAGS MSG_NOSIGNAL
#endif

#define TCI_MAX_CLIENTS   8
#define TCI_WS_GUID       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define TCI_MODLIST       "am,sam,dsb,lsb,usb,cw,nfm,digl,digu,wfm,drm,spec"

static RADIO   *g_radio = NULL;
static volatile gint server_running = 0;      // atomic: accept loop alive
static GThread *server_thread = NULL;
static int      listen_socket = -1;
static int      listening_port = TCI_DEFAULT_PORT;

// Connected client fds, mutex-guarded (accept thread adds, client threads
// remove, notify broadcasts iterate). -1 = free slot.
static GMutex   clients_mutex;
static int      clients[TCI_MAX_CLIENTS];

static char     status_line[96] = "stopped";

// ---------------------------------------------------------------------------
// low-level socket helpers
// ---------------------------------------------------------------------------

// Read exactly n bytes (blocking) unless the peer closes / errors. Returns
// TRUE only when all n bytes were read.
static gboolean recv_all(int fd, void *buf, size_t n) {
  guint8 *p = (guint8 *)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, p + got, n - got, 0);
    if (r <= 0) return FALSE;
    got += (size_t)r;
  }
  return TRUE;
}

// Send a full buffer (blocking). Returns TRUE on success.
static gboolean send_all(int fd, const void *buf, size_t n) {
  const guint8 *p = (const guint8 *)buf;
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = send(fd, p + sent, n - sent, TCI_SEND_FLAGS);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return FALSE;
    }
    sent += (size_t)w;
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// minimal WebSocket framing (RFC 6455 — text/close/ping only, no extensions)
// ---------------------------------------------------------------------------

// Send one unmasked text frame. `flags` lets callers add MSG_DONTWAIT for
// best-effort broadcasts. Returns TRUE on success.
static gboolean ws_send_text_fd(int fd, const char *text, int extra_flags) {
  size_t len = strlen(text);
  guint8 hdr[10];
  size_t hlen;
  hdr[0] = 0x81;                       // FIN + text opcode
  if (len < 126) {
    hdr[1] = (guint8)len;
    hlen = 2;
  } else if (len < 65536) {
    hdr[1] = 126;
    hdr[2] = (guint8)((len >> 8) & 0xff);
    hdr[3] = (guint8)(len & 0xff);
    hlen = 4;
  } else {
    // Phase-A payloads never reach 64 KiB; refuse rather than truncate.
    return FALSE;
  }
  // One send() for the header, one for the payload — with an added-flags path
  // for non-blocking broadcast (partial writes there are simply dropped).
  if (extra_flags) {
    if (send(fd, hdr, hlen, TCI_SEND_FLAGS | extra_flags) != (ssize_t)hlen) return FALSE;
    if (len && send(fd, text, len, TCI_SEND_FLAGS | extra_flags) != (ssize_t)len) return FALSE;
    return TRUE;
  }
  if (!send_all(fd, hdr, hlen)) return FALSE;
  if (len && !send_all(fd, text, len)) return FALSE;
  return TRUE;
}

// Receive one WebSocket frame. Returns opcode (>=0) and fills *out (a
// g_malloc'd, NUL-terminated payload the caller frees) with *out_len bytes.
// Returns -1 on connection error/close. Fragmented frames are not expected
// from TCI clients (control commands are tiny), so only single frames handled.
static int ws_recv_frame(int fd, char **out, size_t *out_len) {
  guint8 h[2];
  *out = NULL; *out_len = 0;
  if (!recv_all(fd, h, 2)) return -1;
  int opcode = h[0] & 0x0f;
  int masked = (h[1] & 0x80) != 0;
  guint64 len = h[1] & 0x7f;
  if (len == 126) {
    guint8 e[2];
    if (!recv_all(fd, e, 2)) return -1;
    len = ((guint64)e[0] << 8) | e[1];
  } else if (len == 127) {
    guint8 e[8];
    if (!recv_all(fd, e, 8)) return -1;
    len = 0;
    for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
  }
  // Sanity cap — no legitimate TCI control frame is anywhere near this.
  if (len > 65536) return -1;
  guint8 mask[4] = {0,0,0,0};
  if (masked && !recv_all(fd, mask, 4)) return -1;
  char *buf = g_malloc(len + 1);
  if (len && !recv_all(fd, buf, len)) { g_free(buf); return -1; }
  if (masked) for (guint64 i = 0; i < len; i++) buf[i] ^= mask[i & 3];
  buf[len] = '\0';
  *out = buf;
  *out_len = (size_t)len;
  return opcode;
}

// Perform the HTTP Upgrade handshake. Reads the request, computes the
// Sec-WebSocket-Accept via GLib SHA1+base64, and replies 101. TRUE on success.
static gboolean ws_handshake(int fd) {
  char req[2048];
  size_t total = 0;
  // Read until the header terminator (or buffer full / peer gone).
  while (total < sizeof(req) - 1) {
    ssize_t r = recv(fd, req + total, sizeof(req) - 1 - total, 0);
    if (r <= 0) return FALSE;
    total += (size_t)r;
    req[total] = '\0';
    if (strstr(req, "\r\n\r\n")) break;
  }
  req[total < sizeof(req) ? total : sizeof(req) - 1] = '\0';

  // Extract the Sec-WebSocket-Key header value (case-insensitive header name).
  const char *key_hdr = NULL;
  for (char *p = req; *p; p++) {
    if (g_ascii_strncasecmp(p, "Sec-WebSocket-Key:", 18) == 0) { key_hdr = p + 18; break; }
  }
  if (!key_hdr) return FALSE;
  while (*key_hdr == ' ' || *key_hdr == '\t') key_hdr++;
  char key[128];
  int ki = 0;
  while (*key_hdr && *key_hdr != '\r' && *key_hdr != '\n' && ki < (int)sizeof(key) - 1)
    key[ki++] = *key_hdr++;
  key[ki] = '\0';

  // accept = base64( SHA1( key + GUID ) )
  char concat[256];
  g_snprintf(concat, sizeof(concat), "%s%s", key, TCI_WS_GUID);
  GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(ck, (const guchar *)concat, strlen(concat));
  guint8 digest[20];
  gsize dlen = sizeof(digest);
  g_checksum_get_digest(ck, digest, &dlen);
  char *accept = g_base64_encode(digest, dlen);
  g_checksum_free(ck);

  char resp[256];
  int rl = g_snprintf(resp, sizeof(resp),
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
  g_free(accept);
  return send_all(fd, resp, (size_t)rl);
}

// ---------------------------------------------------------------------------
// TCI protocol
// ---------------------------------------------------------------------------

// internal mode enum -> TCI modulation name
static const char *mode_to_tci(int m) {
  switch (m) {
    case LSB:  return "lsb";
    case USB:  return "usb";
    case DSB:  return "dsb";
    case CWL:  return "cw";
    case CWU:  return "cw";
    case FMN:  return "nfm";
    case AM:   return "am";
    case DIGU: return "digu";
    case SPEC: return "spec";
    case DIGL: return "digl";
    case SAM:  return "sam";
    case DRM:  return "drm";
    case WFM:  return "wfm";
    default:   return "usb";
  }
}

// TCI modulation name -> internal mode enum, or -1 if unknown. "cw" maps to
// CWU (TCI has a single "cw"; the sideband isn't carried in the protocol).
static int tci_to_mode(const char *s) {
  if (!g_ascii_strcasecmp(s, "lsb"))  return LSB;
  if (!g_ascii_strcasecmp(s, "usb"))  return USB;
  if (!g_ascii_strcasecmp(s, "dsb"))  return DSB;
  if (!g_ascii_strcasecmp(s, "cw"))   return CWU;
  if (!g_ascii_strcasecmp(s, "cwu"))  return CWU;
  if (!g_ascii_strcasecmp(s, "cwl"))  return CWL;
  if (!g_ascii_strcasecmp(s, "nfm"))  return FMN;
  if (!g_ascii_strcasecmp(s, "fm"))   return FMN;
  if (!g_ascii_strcasecmp(s, "am"))   return AM;
  if (!g_ascii_strcasecmp(s, "digu")) return DIGU;
  if (!g_ascii_strcasecmp(s, "digl")) return DIGL;
  if (!g_ascii_strcasecmp(s, "spec")) return SPEC;
  if (!g_ascii_strcasecmp(s, "sam"))  return SAM;
  if (!g_ascii_strcasecmp(s, "drm"))  return DRM;
  if (!g_ascii_strcasecmp(s, "wfm"))  return WFM;
  return -1;
}

// Broadcast a NUL-terminated TCI line (must already end with ';') to all
// connected clients, best-effort and non-blocking.
static void tci_broadcast(const char *line) {
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i] >= 0) ws_send_text_fd(clients[i], line, MSG_DONTWAIT);
  }
  g_mutex_unlock(&clients_mutex);
}

// --- inbound command dispatch to the GTK main thread ------------------------

// Set the active RX's dial frequency (VFO A). Reuses ext.c's idle wrapper.
static void dispatch_set_frequency(long long hz) {
  if (g_radio == NULL || g_radio->active_receiver == NULL) return;
  RX_FREQUENCY *f = g_new0(RX_FREQUENCY, 1);
  f->rx = g_radio->active_receiver;
  f->frequency = hz;
  g_idle_add(ext_set_frequency_a, f);
}

static void dispatch_set_mode(int mode) {
  if (g_radio == NULL || g_radio->active_receiver == NULL) return;
  MODE *m = g_new0(MODE, 1);
  m->rx = g_radio->active_receiver;
  m->mode_a = mode;   // ext_set_mode assigns rx->mode_a = m->mode_a on the GTK thread
  g_idle_add(ext_set_mode, m);
}

static void dispatch_set_mox(gboolean state) {
  if (g_radio == NULL || !g_radio->can_transmit) return;
  MOX_STATE *m = g_new0(MOX_STATE, 1);
  m->radio = g_radio;
  m->state = state;
  g_idle_add(ext_set_mox, m);
}

// Handle one ';'-stripped command token. `fd` is the originating client (for
// getter replies). Reads live radio state directly for getters/replies —
// scalar reads racing the GTK thread are harmless here.
static void tci_handle_command(int fd, const char *token) {
  // Split "name:arg,arg,arg" into a lowercased name and up to 4 args.
  char name[32];
  const char *colon = strchr(token, ':');
  size_t nlen = colon ? (size_t)(colon - token) : strlen(token);
  if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
  for (size_t i = 0; i < nlen; i++) name[i] = g_ascii_tolower(token[i]);
  name[nlen] = '\0';

  char *args[4] = {NULL,NULL,NULL,NULL};
  int nargs = 0;
  char argbuf[128];
  if (colon) {
    g_strlcpy(argbuf, colon + 1, sizeof(argbuf));
    char *save = NULL;
    for (char *t = strtok_r(argbuf, ",", &save); t && nargs < 4; t = strtok_r(NULL, ",", &save))
      args[nargs++] = g_strstrip(t);
  }

  RECEIVER *rx = (g_radio != NULL) ? g_radio->active_receiver : NULL;

  if (!strcmp(name, "vfo") || !strcmp(name, "dds")) {
    // set: vfo:trx,channel,freq;   get: vfo:trx,channel;
    if (nargs >= 3) {
      dispatch_set_frequency(g_ascii_strtoll(args[2], NULL, 10));
    } else if (rx != NULL) {
      int ch = (nargs >= 2) ? atoi(args[1]) : 0;
      long long f = (ch == 1) ? (long long)rx->frequency_b : (long long)rx->frequency_a;
      char r[64];
      g_snprintf(r, sizeof(r), "vfo:0,%d,%lld;", ch, f);
      ws_send_text_fd(fd, r, 0);
    }
  } else if (!strcmp(name, "modulation") || !strcmp(name, "trx_mode")) {
    // set: modulation:trx,mode;   get: modulation:trx;
    if (nargs >= 2) {
      int m = tci_to_mode(args[1]);
      if (m >= 0) dispatch_set_mode(m);
    } else if (rx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "modulation:0,%s;", mode_to_tci(rx->mode_a));
      ws_send_text_fd(fd, r, 0);
    }
  } else if (!strcmp(name, "trx")) {
    // set: trx:trx,true/false[,signal];   get: trx:trx;
    if (nargs >= 2) {
      gboolean on = (!g_ascii_strcasecmp(args[1], "true") || !strcmp(args[1], "1"));
      dispatch_set_mox(on);
    } else if (g_radio != NULL) {
      char r[32];
      g_snprintf(r, sizeof(r), "trx:0,%s;", g_radio->mox ? "true" : "false");
      ws_send_text_fd(fd, r, 0);
    }
  } else {
    // Everything else (if, split_enable, rx_enable, mute, start, stop, …):
    // acknowledge by echoing so a client's request/response wait doesn't hang.
    // Phase A carries no IF/RIT/stream state, so this is intentionally a stub.
    char r[160];
    g_snprintf(r, sizeof(r), "%s;", token);
    ws_send_text_fd(fd, r, 0);
  }
}

// Split a received text frame (may hold several ';'-terminated commands) and
// dispatch each.
static void tci_process_text(int fd, char *text) {
  char *save = NULL;
  for (char *t = strtok_r(text, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
    char *cmd = g_strstrip(t);
    if (*cmd) tci_handle_command(fd, cmd);
  }
}

// Send the on-connect handshake so the client learns our capabilities.
static void tci_send_handshake(int fd) {
  RECEIVER *rx = (g_radio != NULL) ? g_radio->active_receiver : NULL;
  ws_send_text_fd(fd, "protocol:ExpertSDR3,1.9;", 0);
  ws_send_text_fd(fd, "device:MacHPSDR;", 0);
  ws_send_text_fd(fd, g_radio && g_radio->can_transmit ? "receive_only:false;" : "receive_only:true;", 0);
  ws_send_text_fd(fd, "trx_count:1;", 0);
  ws_send_text_fd(fd, "channels_count:2;", 0);
  ws_send_text_fd(fd, "vfo_limits:0,6000000000;", 0);
  ws_send_text_fd(fd, "if_limits:-24000,24000;", 0);
  ws_send_text_fd(fd, "modulations_list:" TCI_MODLIST ";", 0);
  if (rx != NULL) {
    char r[64];
    g_snprintf(r, sizeof(r), "vfo:0,0,%lld;", (long long)rx->frequency_a);
    ws_send_text_fd(fd, r, 0);
    g_snprintf(r, sizeof(r), "modulation:0,%s;", mode_to_tci(rx->mode_a));
    ws_send_text_fd(fd, r, 0);
  }
  ws_send_text_fd(fd, "ready;", 0);
}

// ---------------------------------------------------------------------------
// client & accept threads
// ---------------------------------------------------------------------------

static void clients_remove(int fd) {
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++)
    if (clients[i] == fd) { clients[i] = -1; break; }
  g_mutex_unlock(&clients_mutex);
}

static gpointer tci_client_thread(gpointer data) {
  int fd = GPOINTER_TO_INT(data);

#ifdef __APPLE__
  { int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
  { int on = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on)); }

  if (!ws_handshake(fd)) {
    log_info("tci: websocket handshake failed for fd=%d\n", fd);
    clients_remove(fd);
    close(fd);
    return NULL;
  }
  log_info("tci: client connected (fd=%d)\n", fd);
  tci_send_handshake(fd);

  while (g_atomic_int_get(&server_running)) {
    char *payload = NULL;
    size_t plen = 0;
    int op = ws_recv_frame(fd, &payload, &plen);
    if (op < 0) break;                       // closed / error
    if (op == 0x8) { g_free(payload); break; }   // close frame
    if (op == 0x9) {                              // ping -> pong
      guint8 pong[2] = { 0x8a, 0x00 };
      send_all(fd, pong, 2);
      g_free(payload);
      continue;
    }
    if (op == 0x1 && payload) tci_process_text(fd, payload);   // text
    g_free(payload);
  }

  log_info("tci: client disconnected (fd=%d)\n", fd);
  clients_remove(fd);
  close(fd);
  return NULL;
}

static gpointer tci_server_thread(gpointer data) {
  (void)data;
  int on = 1;

  listen_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_socket < 0) {
    log_error("tci: socket() failed: %s\n", strerror(errno));
    g_atomic_int_set(&server_running, 0);
    return NULL;
  }
  setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
  setsockopt(listen_socket, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(listening_port);
  if (bind(listen_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    log_error("tci: bind(:%d) failed: %s\n", listening_port, strerror(errno));
    close(listen_socket);
    listen_socket = -1;
    g_atomic_int_set(&server_running, 0);
    g_snprintf(status_line, sizeof(status_line), "bind :%d failed", listening_port);
    return NULL;
  }
  if (listen(listen_socket, 4) < 0) {
    log_error("tci: listen() failed: %s\n", strerror(errno));
    close(listen_socket);
    listen_socket = -1;
    g_atomic_int_set(&server_running, 0);
    return NULL;
  }
  g_snprintf(status_line, sizeof(status_line), "listening on :%d", listening_port);
  log_info("tci: %s\n", status_line);

  while (g_atomic_int_get(&server_running)) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int fd = accept(listen_socket, (struct sockaddr *)&caddr, &clen);
    if (fd < 0) {
      if (!g_atomic_int_get(&server_running)) break;   // stopped: expected
      if (errno == EINTR) continue;
      break;
    }
    // Find a free client slot; reject if full.
    int slot = -1;
    g_mutex_lock(&clients_mutex);
    for (int i = 0; i < TCI_MAX_CLIENTS; i++)
      if (clients[i] < 0) { clients[i] = fd; slot = i; break; }
    g_mutex_unlock(&clients_mutex);
    if (slot < 0) {
      log_info("tci: too many clients, rejecting fd=%d\n", fd);
      close(fd);
      continue;
    }
    GThread *t = g_thread_new("tci-client", tci_client_thread, GINT_TO_POINTER(fd));
    if (t) g_thread_unref(t);   // detached; self-cleans on disconnect
  }

  if (listen_socket >= 0) { close(listen_socket); listen_socket = -1; }
  return NULL;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void tci_init(RADIO *radio) {
  g_radio = radio;
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) clients[i] = -1;
  // Dev/verify hook: MACHPSDR_TCI[=port] force-enables the server (optionally on
  // a chosen port) so the faker can exercise it headlessly.
  const char *env = getenv("MACHPSDR_TCI");
  if (env != NULL) {
    radio->tci_enable = TRUE;
    int p = atoi(env);
    if (p > 0) radio->tci_port = p;
  }
  if (radio != NULL && radio->tci_enable) tci_start();
}

void tci_start(void) {
  if (g_atomic_int_get(&server_running)) return;   // already running
  if (g_radio == NULL) return;
  listening_port = (g_radio->tci_port > 0) ? g_radio->tci_port : TCI_DEFAULT_PORT;
  g_atomic_int_set(&server_running, 1);
  g_snprintf(status_line, sizeof(status_line), "starting on :%d", listening_port);
  server_thread = g_thread_new("tci-server", tci_server_thread, NULL);
}

void tci_stop(void) {
  if (!g_atomic_int_get(&server_running) && server_thread == NULL) return;

  g_atomic_int_set(&server_running, 0);

  // Unblock accept() by closing the listen socket.
  if (listen_socket >= 0) {
    shutdown(listen_socket, SHUT_RDWR);
    close(listen_socket);
    listen_socket = -1;
  }
  // Unblock every client recv() and drop them.
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i] >= 0) {
      shutdown(clients[i], SHUT_RDWR);
      // The client thread closes its own fd; just detach it from the table so
      // a late broadcast can't touch a soon-to-be-closed socket.
      clients[i] = -1;
    }
  }
  g_mutex_unlock(&clients_mutex);

  if (server_thread != NULL) {
    g_thread_join(server_thread);
    server_thread = NULL;
  }
  g_strlcpy(status_line, "stopped", sizeof(status_line));
}

const char *tci_status(void) {
  if (!g_atomic_int_get(&server_running)) return status_line;
  // Append the live client count.
  static char buf[128];
  int n = 0;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) if (clients[i] >= 0) n++;
  g_mutex_unlock(&clients_mutex);
  g_snprintf(buf, sizeof(buf), "%s (%d client%s)", status_line, n, n == 1 ? "" : "s");
  return buf;
}

// --- outbound notifications (GTK/audio thread) ------------------------------

void tci_notify_vfo(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  if (g_radio != NULL && rx != g_radio->active_receiver) return;   // report trx 0 only
  char line[64];
  g_snprintf(line, sizeof(line), "vfo:0,0,%lld;", (long long)rx->frequency_a);
  tci_broadcast(line);
  g_snprintf(line, sizeof(line), "vfo:0,1,%lld;", (long long)rx->frequency_b);
  tci_broadcast(line);
}

void tci_notify_mode(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  if (g_radio != NULL && rx != g_radio->active_receiver) return;
  char line[48];
  g_snprintf(line, sizeof(line), "modulation:0,%s;", mode_to_tci(rx->mode_a));
  tci_broadcast(line);
}

void tci_notify_trx(gboolean mox) {
  if (!g_atomic_int_get(&server_running)) return;
  char line[32];
  g_snprintf(line, sizeof(line), "trx:0,%s;", mox ? "true" : "false");
  tci_broadcast(line);
}
