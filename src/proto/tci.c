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

// TCI binary data-stream constants (TCI Protocol.pdf v2). The 64-byte header is
// eight little-endian uint32 fields followed by 32 bytes of padding, then the
// interleaved float32 sample payload.
#define TCI_SAMPLE_FLOAT32   3    // TciSampleType::FLOAT32
#define TCI_STREAM_IQ        0    // TciStreamType::IQ_STREAM
#define TCI_HDR_BYTES        64

// One connected client. `send_mtx` serialises the byte stream to `fd` so control
// replies (client thread), state notifications (GTK thread) and IQ frames (audio
// thread) can never interleave partial WebSocket frames on the same socket. The
// GMutex lives in static storage, so it needs no g_mutex_init.
typedef struct {
  int    fd;       // -1 = free slot
  GMutex send_mtx; // serialise all sends to fd
  gint   iq_on;    // atomic: client subscribed to the IQ stream (iq_start)
} TCI_CLIENT;

static RADIO   *g_radio = NULL;
static volatile gint server_running = 0;      // atomic: accept loop alive
static GThread *server_thread = NULL;
static int      listen_socket = -1;
static int      listening_port = TCI_DEFAULT_PORT;

static GMutex     clients_mutex;              // guards the clients[] table
static TCI_CLIENT clients[TCI_MAX_CLIENTS];
static gint       iq_sub_count = 0;           // atomic: # clients with iq_on

static char     status_line[96] = "stopped";

// ---------------------------------------------------------------------------
// low-level socket helpers
// ---------------------------------------------------------------------------

// Read exactly n bytes (blocking) unless the peer closes / errors.
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

// Store a little-endian uint32 (TCI is LE; target hosts are LE, but stay explicit).
static inline void st32le(guint8 *p, guint32 v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

// ---------------------------------------------------------------------------
// minimal WebSocket framing (RFC 6455 — text/binary/close/ping, no extensions)
// ---------------------------------------------------------------------------

// Write a WebSocket frame header (server->client, unmasked) for `len` payload
// bytes into `hdr` (>=10 bytes). Returns the header length (2/4/10).
static size_t ws_write_header(guint8 *hdr, int opcode, size_t len) {
  hdr[0] = 0x80 | (opcode & 0x0f);            // FIN + opcode
  if (len < 126) { hdr[1] = (guint8)len; return 2; }
  if (len < 65536) {
    hdr[1] = 126; hdr[2] = (guint8)((len >> 8) & 0xff); hdr[3] = (guint8)(len & 0xff);
    return 4;
  }
  hdr[1] = 127;
  for (int i = 0; i < 8; i++) hdr[2 + i] = (guint8)(((guint64)len >> (56 - 8 * i)) & 0xff);
  return 10;
}

// Blocking send of one framed message on `fd` (caller holds the send lock). Used
// for handshake + control replies where delivery matters and the caller is the
// client's own thread.
static gboolean ws_send_blocking(int fd, int opcode, const void *payload, size_t len) {
  guint8 hdr[10];
  size_t hlen = ws_write_header(hdr, opcode, len);
  if (!send_all(fd, hdr, hlen)) return FALSE;
  if (len && !send_all(fd, payload, len)) return FALSE;
  return TRUE;
}

// Receive one WebSocket frame. Returns opcode (>=0) and a g_malloc'd, NUL-
// terminated payload in *out (caller frees). -1 on error/close.
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
  if (len > 65536) return -1;                 // no TCI control frame is this big
  guint8 mask[4] = {0,0,0,0};
  if (masked && !recv_all(fd, mask, 4)) return -1;
  char *buf = g_malloc(len + 1);
  if (len && !recv_all(fd, buf, len)) { g_free(buf); return -1; }
  if (masked) for (guint64 i = 0; i < len; i++) buf[i] ^= mask[i & 3];
  buf[len] = '\0';
  *out = buf; *out_len = (size_t)len;
  return opcode;
}

// HTTP Upgrade handshake: read the request, compute Sec-WebSocket-Accept via
// GLib SHA1+base64, reply 101. TRUE on success.
static gboolean ws_handshake(int fd) {
  char req[2048];
  size_t total = 0;
  while (total < sizeof(req) - 1) {
    ssize_t r = recv(fd, req + total, sizeof(req) - 1 - total, 0);
    if (r <= 0) return FALSE;
    total += (size_t)r;
    req[total] = '\0';
    if (strstr(req, "\r\n\r\n")) break;
  }
  req[total < sizeof(req) ? total : sizeof(req) - 1] = '\0';

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
// per-client serialised send
// ---------------------------------------------------------------------------

// Blocking text send from a client's own thread (delivery guaranteed).
static void client_send_text(TCI_CLIENT *c, const char *s) {
  g_mutex_lock(&c->send_mtx);
  if (c->fd >= 0) ws_send_blocking(c->fd, 0x1, s, strlen(s));
  g_mutex_unlock(&c->send_mtx);
}

// Best-effort non-blocking send of pre-framed on-wire bytes (a complete WS
// frame) from a foreign thread (GTK notify / audio IQ). Never blocks: if the
// send lock is contended it skips this client; on EAGAIN it drops the frame
// cleanly; a partial write would desync the stream, so the client is shut down.
static void client_send_framed_try(TCI_CLIENT *c, const guint8 *frame, size_t len) {
  if (!g_mutex_trylock(&c->send_mtx)) return;   // busy -> drop for this client
  int fd = c->fd;
  if (fd >= 0) {
    ssize_t w = send(fd, frame, len, TCI_SEND_FLAGS | MSG_DONTWAIT);
    if (w > 0 && (size_t)w != len) {
      // Partial write mid-frame: the WebSocket stream is now unrecoverable for
      // this client — drop it (its own thread will clean up on the recv error).
      shutdown(fd, SHUT_RDWR);
    }
    // w<=0 (EAGAIN/would-block or error): nothing sent, frame dropped cleanly.
  }
  g_mutex_unlock(&c->send_mtx);
}

// Broadcast a text line (ends with ';') to every client, best-effort. Builds the
// frame once and reuses it. Caller must NOT hold clients_mutex.
static void tci_broadcast_text(const char *line) {
  size_t len = strlen(line);
  guint8 frame[160 + 10];
  if (len > sizeof(frame) - 10) return;         // control lines are short
  size_t hlen = ws_write_header(frame, 0x1, len);
  memcpy(frame + hlen, line, len);
  size_t flen = hlen + len;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++)
    if (clients[i].fd >= 0) client_send_framed_try(&clients[i], frame, flen);
  g_mutex_unlock(&clients_mutex);
}

// ---------------------------------------------------------------------------
// TCI protocol — mode mapping
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// inbound command dispatch to the GTK main thread
// ---------------------------------------------------------------------------

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

// Enable/disable this client's IQ subscription and keep the global gate count.
static void client_set_iq(TCI_CLIENT *c, gboolean on) {
  gint was = g_atomic_int_get(&c->iq_on);
  if (on && !was) { g_atomic_int_set(&c->iq_on, 1); g_atomic_int_inc(&iq_sub_count); }
  else if (!on && was) {
    g_atomic_int_set(&c->iq_on, 0);
    g_atomic_int_add(&iq_sub_count, -1);
  }
}

// Handle one ';'-stripped command token from client `c`.
static void tci_handle_command(TCI_CLIENT *c, const char *token) {
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
    if (nargs >= 3) {
      dispatch_set_frequency(g_ascii_strtoll(args[2], NULL, 10));
    } else if (rx != NULL) {
      int ch = (nargs >= 2) ? atoi(args[1]) : 0;
      long long f = (ch == 1) ? (long long)rx->frequency_b : (long long)rx->frequency_a;
      char r[64];
      g_snprintf(r, sizeof(r), "vfo:0,%d,%lld;", ch, f);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "modulation") || !strcmp(name, "trx_mode")) {
    if (nargs >= 2) {
      int m = tci_to_mode(args[1]);
      if (m >= 0) dispatch_set_mode(m);
    } else if (rx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "modulation:0,%s;", mode_to_tci(rx->mode_a));
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "trx")) {
    if (nargs >= 2) {
      gboolean on = (!g_ascii_strcasecmp(args[1], "true") || !strcmp(args[1], "1"));
      dispatch_set_mox(on);
    } else if (g_radio != NULL) {
      char r[32];
      g_snprintf(r, sizeof(r), "trx:0,%s;", g_radio->mox ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "iq_start")) {
    client_set_iq(c, TRUE);
    log_info("tci: iq_start (fd=%d, subs=%d)\n", c->fd, g_atomic_int_get(&iq_sub_count));
  } else if (!strcmp(name, "iq_stop")) {
    client_set_iq(c, FALSE);
    log_info("tci: iq_stop (fd=%d, subs=%d)\n", c->fd, g_atomic_int_get(&iq_sub_count));
  } else if (!strcmp(name, "iq_samplerate") || !strcmp(name, "iq_sample_rate")) {
    // We stream at the receiver's native DDC rate; report it (a requested rate
    // is acknowledged but not honoured — the radio's rate is fixed here).
    int rate = (rx != NULL) ? rx->sample_rate : 48000;
    char r[48];
    g_snprintf(r, sizeof(r), "iq_samplerate:%d;", rate);
    client_send_text(c, r);
  } else {
    // Everything else (if, split_enable, rx_enable, mute, audio_*, start, stop,
    // …): echo as an ack so a request/response client doesn't hang. Phase A/B
    // carry no IF/RIT/audio state, so this is intentionally a stub.
    char r[160];
    g_snprintf(r, sizeof(r), "%s;", token);
    client_send_text(c, r);
  }
}

static void tci_process_text(TCI_CLIENT *c, char *text) {
  char *save = NULL;
  for (char *t = strtok_r(text, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
    char *cmd = g_strstrip(t);
    if (*cmd) tci_handle_command(c, cmd);
  }
}

static void tci_send_handshake(TCI_CLIENT *c) {
  RECEIVER *rx = (g_radio != NULL) ? g_radio->active_receiver : NULL;
  client_send_text(c, "protocol:ExpertSDR3,1.9;");
  client_send_text(c, "device:MacHPSDR;");
  client_send_text(c, g_radio && g_radio->can_transmit ? "receive_only:false;" : "receive_only:true;");
  client_send_text(c, "trx_count:1;");
  client_send_text(c, "channels_count:2;");
  client_send_text(c, "vfo_limits:0,6000000000;");
  client_send_text(c, "if_limits:-24000,24000;");
  client_send_text(c, "modulations_list:" TCI_MODLIST ";");
  if (rx != NULL) {
    char r[64];
    g_snprintf(r, sizeof(r), "iq_samplerate:%d;", rx->sample_rate);
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "vfo:0,0,%lld;", (long long)rx->frequency_a);
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "modulation:0,%s;", mode_to_tci(rx->mode_a));
    client_send_text(c, r);
  }
  client_send_text(c, "ready;");
}

// ---------------------------------------------------------------------------
// client & accept threads
// ---------------------------------------------------------------------------

static void clients_remove(TCI_CLIENT *c) {
  g_mutex_lock(&clients_mutex);
  client_set_iq(c, FALSE);   // keep iq_sub_count balanced
  c->fd = -1;
  g_mutex_unlock(&clients_mutex);
}

static gpointer tci_client_thread(gpointer data) {
  TCI_CLIENT *c = (TCI_CLIENT *)data;
  int fd = c->fd;

#ifdef __APPLE__
  { int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
  { int on = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on)); }
  // A generous send buffer keeps a full IQ frame atomic for MSG_DONTWAIT sends.
  { int sz = 1 << 20; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)); }

  if (!ws_handshake(fd)) {
    log_info("tci: websocket handshake failed for fd=%d\n", fd);
    clients_remove(c);
    close(fd);
    return NULL;
  }
  log_info("tci: client connected (fd=%d)\n", fd);
  tci_send_handshake(c);

  while (g_atomic_int_get(&server_running)) {
    char *payload = NULL;
    size_t plen = 0;
    int op = ws_recv_frame(fd, &payload, &plen);
    if (op < 0) break;
    if (op == 0x8) { g_free(payload); break; }          // close
    if (op == 0x9) {                                     // ping -> pong
      g_mutex_lock(&c->send_mtx);
      if (c->fd >= 0) ws_send_blocking(fd, 0xA, payload, plen);
      g_mutex_unlock(&c->send_mtx);
      g_free(payload);
      continue;
    }
    if (op == 0x1 && payload) tci_process_text(c, payload);   // text
    g_free(payload);
  }

  log_info("tci: client disconnected (fd=%d)\n", fd);
  clients_remove(c);
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
      if (!g_atomic_int_get(&server_running)) break;
      if (errno == EINTR) continue;
      break;
    }
    TCI_CLIENT *slot = NULL;
    g_mutex_lock(&clients_mutex);
    for (int i = 0; i < TCI_MAX_CLIENTS; i++)
      if (clients[i].fd < 0) { clients[i].fd = fd; g_atomic_int_set(&clients[i].iq_on, 0); slot = &clients[i]; break; }
    g_mutex_unlock(&clients_mutex);
    if (slot == NULL) {
      log_info("tci: too many clients, rejecting fd=%d\n", fd);
      close(fd);
      continue;
    }
    GThread *t = g_thread_new("tci-client", tci_client_thread, slot);
    if (t) g_thread_unref(t);
  }

  if (listen_socket >= 0) { close(listen_socket); listen_socket = -1; }
  return NULL;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void tci_init(RADIO *radio) {
  g_radio = radio;
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) clients[i].fd = -1;
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
  if (g_atomic_int_get(&server_running)) return;
  if (g_radio == NULL) return;
  listening_port = (g_radio->tci_port > 0) ? g_radio->tci_port : TCI_DEFAULT_PORT;
  g_atomic_int_set(&server_running, 1);
  g_snprintf(status_line, sizeof(status_line), "starting on :%d", listening_port);
  server_thread = g_thread_new("tci-server", tci_server_thread, NULL);
}

void tci_stop(void) {
  if (!g_atomic_int_get(&server_running) && server_thread == NULL) return;

  g_atomic_int_set(&server_running, 0);

  if (listen_socket >= 0) {
    shutdown(listen_socket, SHUT_RDWR);
    close(listen_socket);
    listen_socket = -1;
  }
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd >= 0) {
      shutdown(clients[i].fd, SHUT_RDWR);   // unblock the client recv; it self-cleans
    }
  }
  g_mutex_unlock(&clients_mutex);

  if (server_thread != NULL) {
    g_thread_join(server_thread);
    server_thread = NULL;
  }
  g_snprintf(status_line, sizeof(status_line), "stopped");
}

const char *tci_status(void) {
  if (!g_atomic_int_get(&server_running)) return status_line;
  static char buf[128];
  int n = 0;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) if (clients[i].fd >= 0) n++;
  g_mutex_unlock(&clients_mutex);
  int iq = g_atomic_int_get(&iq_sub_count);
  g_snprintf(buf, sizeof(buf), "%s (%d client%s%s)", status_line, n, n == 1 ? "" : "s",
             iq > 0 ? ", IQ streaming" : "");
  return buf;
}

// --- outbound notifications (GTK/audio thread) ------------------------------

void tci_notify_vfo(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  if (g_radio != NULL && rx != g_radio->active_receiver) return;
  char line[64];
  g_snprintf(line, sizeof(line), "vfo:0,0,%lld;", (long long)rx->frequency_a);
  tci_broadcast_text(line);
  g_snprintf(line, sizeof(line), "vfo:0,1,%lld;", (long long)rx->frequency_b);
  tci_broadcast_text(line);
}

void tci_notify_mode(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  if (g_radio != NULL && rx != g_radio->active_receiver) return;
  char line[48];
  g_snprintf(line, sizeof(line), "modulation:0,%s;", mode_to_tci(rx->mode_a));
  tci_broadcast_text(line);
}

void tci_notify_trx(gboolean mox) {
  if (!g_atomic_int_get(&server_running)) return;
  char line[32];
  g_snprintf(line, sizeof(line), "trx:0,%s;", mox ? "true" : "false");
  tci_broadcast_text(line);
}

// --- IQ stream (audio thread tap in receiver.c:full_rx_buffer) --------------

void tci_iq_feed(RECEIVER *rx, const double *iq, int nsamples, int sample_rate) {
  // Cheap gates first: server off, no subscribers, or not the active receiver
  // (Phase B streams the active RX as TCI trx 0). Costs one atomic read when idle.
  if (!g_atomic_int_get(&server_running)) return;
  if (g_atomic_int_get(&iq_sub_count) <= 0) return;
  if (rx == NULL || iq == NULL || nsamples <= 0) return;
  if (g_radio != NULL && rx != g_radio->active_receiver) return;

  // Build the complete on-wire WebSocket frame once: WS header + 64-byte TCI
  // header + interleaved float32 I/Q. length counts individual floats (2 per
  // complex sample); channels = 2; format = float32; type = IQ.
  guint32 nfloats = (guint32)nsamples * 2u;
  size_t  payload = TCI_HDR_BYTES + (size_t)nfloats * sizeof(float);
  guint8  wshdr[10];
  size_t  wshlen = ws_write_header(wshdr, 0x2 /*binary*/, payload);
  size_t  flen = wshlen + payload;
  guint8 *frame = g_malloc(flen);

  memcpy(frame, wshdr, wshlen);
  guint8 *th = frame + wshlen;                 // TCI header
  memset(th, 0, TCI_HDR_BYTES);
  st32le(th +  0, 0);                           // rx index (trx 0)
  st32le(th +  4, (guint32)sample_rate);        // sample_rate
  st32le(th +  8, TCI_SAMPLE_FLOAT32);          // data_format
  st32le(th + 12, 0);                           // codec
  st32le(th + 16, 0);                           // crc
  st32le(th + 20, nfloats);                     // length (float count)
  st32le(th + 24, TCI_STREAM_IQ);               // data_type
  st32le(th + 28, 2);                           // channels (I, Q)

  float *fp = (float *)(th + TCI_HDR_BYTES);
  for (int i = 0; i < nsamples * 2; i++) fp[i] = (float)iq[i];

  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++)
    if (clients[i].fd >= 0 && g_atomic_int_get(&clients[i].iq_on))
      client_send_framed_try(&clients[i], frame, flen);
  g_mutex_unlock(&clients_mutex);

  g_free(frame);
}
