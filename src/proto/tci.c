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
#include <math.h>
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
#include "vfo.h"
#include "log.h"
#include "tci.h"
#ifdef SSTV
  #include "cw_encoder.h"   // cw_tx_send_text / cw_tx_abort (CW keyer, SSTV-flag)
#endif

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
#define TCI_STREAM_RX_AUDIO  1    // TciStreamType::RX_AUDIO_STREAM
#define TCI_STREAM_TX_AUDIO  2    // TciStreamType::TX_AUDIO_STREAM
#define TCI_HDR_BYTES        64
#define TCI_AUDIO_RATE       48000 // RX/TX audio streamed at the native AF rate
#define TCI_TX_RING          48000 // TX-audio jitter ring: 1 s of mono float
#define TCI_TX_ACTIVE_US     250000 // TX-audio idle timeout (µs) -> release mic

// One connected client. `send_mtx` serialises the byte stream to `fd` so control
// replies (client thread), state notifications (GTK thread) and IQ frames (audio
// thread) can never interleave partial WebSocket frames on the same socket. The
// GMutex lives in static storage, so it needs no g_mutex_init.
typedef struct {
  int    fd;        // -1 = free slot
  GMutex send_mtx;  // serialise all sends to fd
  gint   iq_mask;    // atomic: bitmask of rx indices subscribed to IQ (iq_start:<rx>)
  gint   audio_mask; // atomic: bitmask of rx indices subscribed to RX audio (audio_start:<rx>)
} TCI_CLIENT;

static RADIO   *g_radio = NULL;
static volatile gint server_running = 0;      // atomic: accept loop alive
static GThread *server_thread = NULL;
static int      listen_socket = -1;
static int      listening_port = TCI_DEFAULT_PORT;

static GMutex     clients_mutex;              // guards the clients[] table
static TCI_CLIENT clients[TCI_MAX_CLIENTS];
static gint       iq_sub_count = 0;           // atomic: # active (client,rx) IQ subscriptions
static gint       audio_sub_count = 0;        // atomic: # active (client,rx) audio subscriptions

// TX audio ingest: a mono float ring filled by client threads (from inbound TCI
// TX-audio binary frames) and drained by the TX thread via tci_tx_next_sample().
static GMutex   tx_ring_mutex;
static float    tx_ring[TCI_TX_RING];
static int      tx_ring_head = 0;             // next write
static int      tx_ring_tail = 0;             // next read
static gint64   tx_audio_last_us = 0;         // monotonic time of last TX-audio frame

static char     status_line[96] = "stopped";

// Requested RX/TX audio stream rate (Hz). 48000 = native fast path (no resample).
// A client changes it with audio_samplerate:<rate>; the stream is shared, so the
// last writer wins. RX-out is resampled 48k->this; TX-in is resampled this->48k.
static volatile gint audio_stream_rate = TCI_AUDIO_RATE;

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
  if (len > 262144) return -1;                // bounded: control frames tiny, TX-audio blocks small
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
// multi-RX index helpers — a TCI "trx" is a *visible* receiver (show_rx). Hidden
// receivers (diversity / PureSignal feedback) are not exposed. Indices are the
// receiver's position among the visible ones (0-based), stable in array order.
// ---------------------------------------------------------------------------

static int tci_trx_count(void) {
  if (g_radio == NULL) return 1;
  int n = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++)
    if (g_radio->receiver[i] != NULL && g_radio->receiver[i]->show_rx) n++;
  return n > 0 ? n : 1;
}

// TCI index of a receiver, or -1 if it is hidden / not found.
static int tci_rx_index(RECEIVER *rx) {
  if (g_radio == NULL || rx == NULL) return -1;
  int idx = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    RECEIVER *r = g_radio->receiver[i];
    if (r == NULL || !r->show_rx) continue;
    if (r == rx) return idx;
    idx++;
  }
  return -1;
}

// TCI index of the receiver the (single) transmitter is attached to. This app
// has one transmitter, but it is not necessarily bound to receiver 0, and TCI
// addresses PTT per trx — so "which trx transmits" is a real question and the
// answer used to be hard-coded to 0. Falls back to 0 when the transmitter's
// receiver is hidden or absent, which keeps a plain single-RX client working.
static int tci_tx_index(void) {
  if (g_radio == NULL || g_radio->transmitter == NULL) return 0;
  int idx = tci_rx_index(g_radio->transmitter->rx);
  return idx >= 0 ? idx : 0;
}

// The visible receiver at TCI index `idx`, or NULL.
static RECEIVER *tci_rx_at(int idx) {
  if (g_radio == NULL || idx < 0) return NULL;
  int n = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    RECEIVER *r = g_radio->receiver[i];
    if (r == NULL || !r->show_rx) continue;
    if (n == idx) return r;
    n++;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// inbound command dispatch to the GTK main thread
// ---------------------------------------------------------------------------

static void dispatch_set_frequency(RECEIVER *rx, long long hz) {
  if (rx == NULL) return;
  RX_FREQUENCY *f = g_new0(RX_FREQUENCY, 1);
  f->rx = rx;
  f->frequency = hz;
  g_idle_add(ext_set_frequency_a, f);
}

static void dispatch_set_frequency_b(RECEIVER *rx, long long hz) {
  if (rx == NULL) return;
  RX_FREQUENCY *f = g_new0(RX_FREQUENCY, 1);
  f->rx = rx;
  f->frequency = hz;
  g_idle_add(ext_set_frequency_b, f);
}

static void dispatch_set_mode(RECEIVER *rx, int mode) {
  if (rx == NULL) return;
  MODE *m = g_new0(MODE, 1);
  m->rx = rx;
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

// TCI booleans are "true"/"false" (also accept "1"/"0").
static gboolean tci_argbool(const char *s) {
  return (!g_ascii_strcasecmp(s, "true") || !strcmp(s, "1"));
}

// Deferred RIT/XIT/split/IF state changes: mutated on the GTK main thread (like
// ext.c's wrappers) since they touch RECEIVER/TRANSMITTER + WDSP + the VFO.
typedef enum {
  TCI_OP_RIT_ENABLE, TCI_OP_RIT_OFFSET,
  TCI_OP_XIT_ENABLE, TCI_OP_XIT_OFFSET,
  TCI_OP_SPLIT,      TCI_OP_IF
} TCI_OP;

typedef struct { TCI_OP op; int rx_index; gboolean b; gint64 v; } TCI_STATE_CMD;

static gboolean tci_apply_state_idle(gpointer data) {
  TCI_STATE_CMD *c = (TCI_STATE_CMD *)data;
  RADIO *r = g_radio;
  // Resolve the target receiver on the GTK thread (race-free — delete_receiver
  // also runs here); fall back to the active RX if the index is gone.
  RECEIVER *rx = (r != NULL) ? tci_rx_at(c->rx_index) : NULL;
  if (rx == NULL && r != NULL) rx = r->active_receiver;
  if (r != NULL && rx != NULL) {
    g_mutex_lock(&rx->mutex);
    switch (c->op) {
      case TCI_OP_RIT_ENABLE:
        rx->rit_enabled = c->b;
        frequency_changed(rx);
        break;
      case TCI_OP_RIT_OFFSET:
        rx->rit = c->v;
        frequency_changed(rx);
        break;
      case TCI_OP_XIT_ENABLE:
        if (r->transmitter != NULL) r->transmitter->xit_enabled = c->b;
        break;
      case TCI_OP_XIT_OFFSET:
        if (r->transmitter != NULL) r->transmitter->xit = c->v;
        break;
      case TCI_OP_SPLIT:
        rx->split = c->b ? SPLIT_ON : SPLIT_OFF;
        if (r->transmitter != NULL)
          transmitter_set_mode(r->transmitter, c->b ? rx->mode_b : rx->mode_a);
        break;
      case TCI_OP_IF:
        // Place the demod point c->v Hz off the panorama centre (CTUN offset).
        // A non-zero offset with plain tuning implies CTUN, so enable it.
        if (c->v != 0 && !rx->ctun && !rx->freetune) rx->ctun = TRUE;
        rx->ctun_frequency = rx->frequency_a + c->v;
        frequency_changed(rx);
        break;
    }
    update_vfo(rx);
    g_mutex_unlock(&rx->mutex);
  }
  g_free(c);
  return G_SOURCE_REMOVE;
}

static void tci_dispatch_state(int rx_index, TCI_OP op, gboolean b, gint64 v) {
  if (g_radio == NULL) return;
  TCI_STATE_CMD *c = g_new0(TCI_STATE_CMD, 1);
  c->op = op; c->rx_index = rx_index; c->b = b; c->v = v;
  g_idle_add(tci_apply_state_idle, c);
}

// CW keyer integration (only when the CW encoder is compiled in — SSTV flag).
// cw_tx_send_text() keys MOX and starts a GLib timeout, so it must run on the
// GTK main thread; dispatch a strdup'd copy there.
#ifdef SSTV
static gboolean tci_cw_send_idle(gpointer data) {
  char *text = (char *)data;
  cw_tx_send_text(text);
  g_free(text);
  return G_SOURCE_REMOVE;
}
static gboolean tci_cw_stop_idle(gpointer data) {
  (void)data;
  cw_tx_abort();
  return G_SOURCE_REMOVE;
}
#endif

// Queue a CW message for transmission (no-op if the CW encoder isn't built).
static void tci_dispatch_cw_send(const char *text) {
#ifdef SSTV
  g_idle_add(tci_cw_send_idle, g_strdup(text));
#else
  (void)text;
#endif
}
static void tci_dispatch_cw_stop(void) {
#ifdef SSTV
  g_idle_add(tci_cw_stop_idle, NULL);
#endif
}

// Set/clear this client's IQ subscription for one rx index (or all rx when
// rx_index < 0), keeping the global gate count = total set bits across clients.
static void client_set_iq(TCI_CLIENT *c, int rx_index, gboolean on) {
  gint old = g_atomic_int_get(&c->iq_mask);
  gint bits = (rx_index < 0) ? ~0 : (1 << rx_index);
  gint nw = on ? (old | bits) : (old & ~bits);
  if (nw == old) return;
  g_atomic_int_set(&c->iq_mask, nw);
  g_atomic_int_add(&iq_sub_count, __builtin_popcount((guint)(nw ^ old)) * (on ? 1 : -1));
}

// Same for the RX audio subscription.
static void client_set_audio(TCI_CLIENT *c, int rx_index, gboolean on) {
  gint old = g_atomic_int_get(&c->audio_mask);
  gint bits = (rx_index < 0) ? ~0 : (1 << rx_index);
  gint nw = on ? (old | bits) : (old & ~bits);
  if (nw == old) return;
  g_atomic_int_set(&c->audio_mask, nw);
  g_atomic_int_add(&audio_sub_count, __builtin_popcount((guint)(nw ^ old)) * (on ? 1 : -1));
}

// --- arbitrary-ratio audio resampler (windowed-sinc) ------------------------
// Used ONLY when a client asks for an RX/TX audio rate other than 48 kHz; the
// 48 kHz path never touches this (zero cost, zero risk to the tested fast path).
// Each channel keeps its FIR history + fractional read pointer across blocks so
// there is no click at block boundaries. Quality is fine for monitoring audio.
#define TCI_RS_HALF 16                       // sinc half-width -> 32 taps
#define TCI_RS_HIST (2 * TCI_RS_HALF)

typedef struct {
  int    rin, rout;
  double step;                               // rin/rout (input samples per output)
  double rp;                                 // read pointer into [hist | new]
  float  hist[TCI_RS_HIST];
} TCI_RESAMP;

static void tci_resamp_reset(TCI_RESAMP *s, int rin, int rout) {
  s->rin = rin; s->rout = rout;
  s->step = (double)rin / (double)rout;
  s->rp = (double)TCI_RS_HIST;               // history is zero-padded at start
  memset(s->hist, 0, sizeof(s->hist));
}

static inline double tci_sinc(double x) {
  if (x < 1e-9 && x > -1e-9) return 1.0;
  double p = G_PI * x;
  return sin(p) / p;
}

// Resample one channel (rin->rout). in[n_in] -> out[], returns sample count.
// out_cap must be >= ceil(n_in*rout/rin)+2. n_in is a small audio block.
static int tci_resamp_run(TCI_RESAMP *s, const float *in, int n_in, float *out, int out_cap) {
  int W = TCI_RS_HIST + n_in;
  float *w = g_newa(float, W);
  memcpy(w, s->hist, TCI_RS_HIST * sizeof(float));
  memcpy(w + TCI_RS_HIST, in, (size_t)n_in * sizeof(float));

  double fc   = 0.5 * ((s->rout < s->rin) ? (double)s->rout / (double)s->rin : 1.0);
  double gain = 2.0 * fc;
  double limit = (double)(W - 1 - TCI_RS_HALF);   // need HALF future samples

  int n_out = 0;
  double rp = s->rp;
  while (rp <= limit && n_out < out_cap) {
    int i0 = (int)floor(rp);
    double acc = 0.0;
    for (int k = i0 - TCI_RS_HALF + 1; k <= i0 + TCI_RS_HALF; k++) {
      double x   = rp - k;
      double wnd = 0.5 + 0.5 * cos(G_PI * x / (double)TCI_RS_HALF);   // Hann
      double h   = gain * tci_sinc(2.0 * fc * x) * wnd;
      float  smp = (k >= 0 && k < W) ? w[k] : 0.0f;
      acc += smp * h;
    }
    out[n_out++] = (float)acc;
    rp += s->step;
  }

  memcpy(s->hist, w + (W - TCI_RS_HIST), TCI_RS_HIST * sizeof(float));
  s->rp = rp - n_in;                         // reindex after dropping n_in front samples
  if (s->rp < 0.0) s->rp = 0.0;
  return n_out;
}

// Per-rx stereo RX-audio resamplers (touched only by that rx's audio thread) and
// one mono TX-audio resampler (guarded by tx_ring_mutex).
static TCI_RESAMP rs_rx[MAX_RECEIVERS][2];
static TCI_RESAMP rs_tx;

// Ingest one inbound TCI binary frame from a client. Only TX-audio (type 2,
// float32) is consumed: its left channel is pushed into the mono TX ring for
// add_mic_sample() to drain. Everything else is ignored.
static void tci_ingest_binary(const guint8 *buf, size_t len) {
  if (len < TCI_HDR_BYTES + 4) return;
  guint32 srate   = ((guint32)buf[ 4]) | ((guint32)buf[ 5]<<8) | ((guint32)buf[ 6]<<16) | ((guint32)buf[ 7]<<24);
  guint32 fmt     = ((guint32)buf[ 8]) | ((guint32)buf[ 9]<<8) | ((guint32)buf[10]<<16) | ((guint32)buf[11]<<24);
  guint32 dtype   = ((guint32)buf[24]) | ((guint32)buf[25]<<8) | ((guint32)buf[26]<<16) | ((guint32)buf[27]<<24);
  guint32 channels= ((guint32)buf[28]) | ((guint32)buf[29]<<8) | ((guint32)buf[30]<<16) | ((guint32)buf[31]<<24);
  if (dtype != TCI_STREAM_TX_AUDIO) return;
  if (fmt != TCI_SAMPLE_FLOAT32) return;              // only float32 supported
  if (channels < 1) channels = 1;

  const float *s = (const float *)(buf + TCI_HDR_BYTES);
  size_t nfloats = (len - TCI_HDR_BYTES) / sizeof(float);
  size_t stride  = channels;                          // take the left channel
  size_t nmono   = nfloats / stride;
  if (nmono == 0) return;

  // Extract the left (mono) channel, then resample srate -> 48 kHz if needed.
  // rs_tx + the ring are both guarded by tx_ring_mutex (client threads may race).
  float *mono = g_new(float, nmono);
  for (size_t i = 0, j = 0; j < nmono; i += stride, j++) mono[j] = s[i];

  const float *push = mono;
  int npush = (int)nmono;
  float *res = NULL;
  // Clamp the client-supplied sample rate: an unvalidated tiny srate makes the
  // resampler's output capacity (nmono*48000/srate) overflow int and abort the
  // whole app in g_new(). srate==0 falls back to the native 48 kHz fast path.
  int want_rate = (srate > 0) ? (int)srate : TCI_AUDIO_RATE;
  if(want_rate < 8000)   want_rate = 8000;
  if(want_rate > 192000) want_rate = 192000;

  g_mutex_lock(&tx_ring_mutex);
  if (want_rate != TCI_AUDIO_RATE) {
    if (rs_tx.rin != want_rate || rs_tx.rout != TCI_AUDIO_RATE)
      tci_resamp_reset(&rs_tx, want_rate, TCI_AUDIO_RATE);
    int cap = (int)((gint64)nmono * TCI_AUDIO_RATE / want_rate) + 4;
    res = g_new(float, cap);
    npush = tci_resamp_run(&rs_tx, mono, (int)nmono, res, cap);
    push = res;
  }
  // Single-consumer (audio thread) reads head/tail lock-free, so publish the
  // slot write BEFORE advancing head (g_atomic_int_set is a full barrier).
  // Producers are serialised by tx_ring_mutex, so head increments don't race.
  int head = tx_ring_head;
  for (int i = 0; i < npush; i++) {
    int next = (head + 1) % TCI_TX_RING;
    if (next == g_atomic_int_get(&tx_ring_tail)) break;  // ring full: drop the rest
    tx_ring[head] = push[i];
    g_atomic_int_set(&tx_ring_head, next);
    head = next;
  }
  g_mutex_unlock(&tx_ring_mutex);

  g_free(mono);
  g_free(res);
  tx_audio_last_us = g_get_monotonic_time();
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

  // Most commands carry a 0-based rx (trx) index in their first field. Resolve
  // the addressed receiver. Commands whose first field is NOT an rx index
  // (cw_macros_speed = wpm) simply ignore these.
  //   req_index : the raw requested index (clamped to a valid bit range) — used
  //               by stream subscriptions, which must NOT fall back to rx 0.
  //   addr      : the exact addressed receiver, or NULL if it does not exist.
  //   rx_index/trx : for control commands, which fall back to the active RX
  //               (labelled index 0) when the addressed rx is absent.
  int req_index = (nargs >= 1 && args[0] != NULL) ? atoi(args[0]) : 0;
  if (req_index < 0 || req_index >= MAX_RECEIVERS) req_index = 0;
  RECEIVER *addr = tci_rx_at(req_index);
  int rx_index = (addr != NULL) ? req_index : 0;
  RECEIVER *trx = (addr != NULL) ? addr
                                 : ((g_radio != NULL) ? g_radio->active_receiver : NULL);

  if (!strcmp(name, "vfo") || !strcmp(name, "dds")) {
    if (nargs >= 3) {
      int ch = atoi(args[1]);
      long long hz = g_ascii_strtoll(args[2], NULL, 10);
      if (ch == 0)      dispatch_set_frequency(trx, hz);
      else if (ch == 1) dispatch_set_frequency_b(trx, hz);
    } else if (trx != NULL) {
      int ch = (nargs >= 2) ? atoi(args[1]) : 0;
      long long f = (ch == 1) ? (long long)trx->frequency_b : (long long)trx->frequency_a;
      char r[64];
      g_snprintf(r, sizeof(r), "vfo:%d,%d,%lld;", rx_index, ch, f);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "modulation") || !strcmp(name, "trx_mode")) {
    if (nargs >= 2) {
      int m = tci_to_mode(args[1]);
      if (m >= 0) dispatch_set_mode(trx, m);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "modulation:%d,%s;", rx_index, mode_to_tci(trx->mode_a));
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "trx")) {
    // There is one transmitter and it belongs to one trx, so PTT addressed to
    // any other trx is a no-op — the rule iq_start/audio_start already follow
    // for an absent rx. The alternative is that `trx:1,true` transmits on rx 0
    // without saying so.
    //
    // Matched on the RAW req_index, not the shared rx_index: that one falls
    // back to 0 for a receiver that does not exist, which would make every
    // bogus index look like the transmitting one and key the radio anyway.
    // (Caught by exactly this case in the faker probe.) A single-RX client is
    // unaffected: it addresses trx 0, which is the transmitting one.
    gboolean is_tx = (addr != NULL) && (req_index == tci_tx_index());
    if (nargs >= 2) {
      gboolean on = (!g_ascii_strcasecmp(args[1], "true") || !strcmp(args[1], "1"));
      if (is_tx) dispatch_set_mox(on);
    } else if (g_radio != NULL) {
      char r[32];
      g_snprintf(r, sizeof(r), "trx:%d,%s;", req_index,
                 (is_tx && g_radio->mox) ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "iq_start")) {
    if (addr != NULL) client_set_iq(c, req_index, TRUE);   // subscribe only to an existing rx
    log_info("tci: iq_start rx=%d%s (fd=%d, subs=%d)\n", req_index,
             addr ? "" : " (no such rx)", c->fd, g_atomic_int_get(&iq_sub_count));
  } else if (!strcmp(name, "iq_stop")) {
    client_set_iq(c, (nargs >= 1) ? req_index : -1, FALSE);
    log_info("tci: iq_stop rx=%d (fd=%d, subs=%d)\n", req_index, c->fd, g_atomic_int_get(&iq_sub_count));
  } else if (!strcmp(name, "iq_samplerate") || !strcmp(name, "iq_sample_rate")) {
    // We stream at the receiver's native DDC rate; report it (a requested rate
    // is acknowledged but not honoured — the radio's rate is fixed here).
    int rate = (trx != NULL) ? trx->sample_rate : 48000;
    char r[48];
    g_snprintf(r, sizeof(r), "iq_samplerate:%d;", rate);
    client_send_text(c, r);
  } else if (!strcmp(name, "audio_start")) {
    if (addr != NULL) client_set_audio(c, req_index, TRUE);
    log_info("tci: audio_start rx=%d%s (fd=%d, subs=%d)\n", req_index,
             addr ? "" : " (no such rx)", c->fd, g_atomic_int_get(&audio_sub_count));
  } else if (!strcmp(name, "audio_stop")) {
    client_set_audio(c, (nargs >= 1) ? req_index : -1, FALSE);
    log_info("tci: audio_stop rx=%d (fd=%d, subs=%d)\n", req_index, c->fd, g_atomic_int_get(&audio_sub_count));
  } else if (!strcmp(name, "audio_samplerate") || !strcmp(name, "audio_sample_rate")) {
    // RX/TX audio stream rate. A requested rate is honoured via a resampler
    // (the native AF path is 48 kHz); 48000 is the zero-cost fast path.
    if (nargs >= 1 && args[0] != NULL && atoi(args[0]) > 0) {
      int req = atoi(args[0]);
      if (req < 8000)  req = 8000;
      if (req > 192000) req = 192000;
      g_atomic_int_set(&audio_stream_rate, req);
    }
    char r[48];
    g_snprintf(r, sizeof(r), "audio_samplerate:%d;", g_atomic_int_get(&audio_stream_rate));
    client_send_text(c, r);
  } else if (!strcmp(name, "rit_enable")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_RIT_ENABLE, tci_argbool(args[1]), 0);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "rit_enable:%d,%s;", rx_index, trx->rit_enabled ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "rit_offset")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_RIT_OFFSET, FALSE, g_ascii_strtoll(args[1], NULL, 10));
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "rit_offset:%d,%lld;", rx_index, (long long)trx->rit);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "xit_enable")) {
    // XIT lives on the single transmitter; the rx index is echoed as-is.
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_XIT_ENABLE, tci_argbool(args[1]), 0);
    } else {
      gboolean en = (g_radio != NULL && g_radio->transmitter != NULL)
                    ? g_radio->transmitter->xit_enabled : FALSE;
      char r[48];
      g_snprintf(r, sizeof(r), "xit_enable:%d,%s;", rx_index, en ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "xit_offset")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_XIT_OFFSET, FALSE, g_ascii_strtoll(args[1], NULL, 10));
    } else {
      long long v = (g_radio != NULL && g_radio->transmitter != NULL)
                    ? (long long)g_radio->transmitter->xit : 0;
      char r[48];
      g_snprintf(r, sizeof(r), "xit_offset:%d,%lld;", rx_index, v);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "split_enable")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_SPLIT, tci_argbool(args[1]), 0);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "split_enable:%d,%s;", rx_index, (trx->split != SPLIT_OFF) ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "if")) {
    // if:<rx>,<sub_rx>,<offset_hz> — demod offset within the panorama (CTUN).
    // Only the main channel (sub_rx 0) is applied; a sub-rx request is acked.
    if (nargs >= 3) {
      int chan = atoi(args[1]);
      gint64 off = g_ascii_strtoll(args[2], NULL, 10);
      gint64 lim = (trx != NULL && trx->sample_rate > 0) ? trx->sample_rate / 2 : 24000;
      if (off >  lim) off =  lim;
      if (off < -lim) off = -lim;
      if (chan == 0) {
        tci_dispatch_state(rx_index, TCI_OP_IF, FALSE, off);
      } else {
        char r[64];
        g_snprintf(r, sizeof(r), "if:%d,%d,%lld;", rx_index, chan, (long long)off);
        client_send_text(c, r);
      }
    } else if (trx != NULL) {
      int chan = (nargs >= 2) ? atoi(args[1]) : 0;
      long long off = (chan == 0 && (trx->ctun || trx->freetune))
                      ? (long long)(trx->ctun_frequency - trx->frequency_a) : 0;
      char r[64];
      g_snprintf(r, sizeof(r), "if:%d,%d,%lld;", rx_index, chan, off);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "cw_msg")) {
    // cw_msg:<rx>,<before>,<after>,<text> — transmit <text> via the CW encoder.
    // Fields can be empty (before/after usually are) and <text> may itself
    // contain commas, so parse positionally from the raw token rather than the
    // comma-collapsed args[]. before/after (callsign-substitution markers) are
    // not implemented — the literal text is sent (use the %C macro for own call).
    const char *p = strchr(token, ':');
    if (p != NULL) {
      int commas = 0;
      const char *text = NULL;
      for (const char *q = p + 1; *q; q++) {
        if (*q == ',') { if (++commas == 3) { text = q + 1; break; } }
      }
      if (text != NULL && *text) tci_dispatch_cw_send(text);
    }
  } else if (!strcmp(name, "cw_macros_stop") || !strcmp(name, "cw_stop")) {
    tci_dispatch_cw_stop();
  } else if (!strcmp(name, "cw_macros_speed")) {
    if (nargs >= 1 && args[0] != NULL) {
      int wpm = atoi(args[0]);
      if (wpm >= 5 && wpm <= 60 && g_radio != NULL) g_radio->cw_keyer_speed = wpm;
    } else if (g_radio != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "cw_macros_speed:%d;", g_radio->cw_keyer_speed);
      client_send_text(c, r);
    }
  } else {
    // Everything else (rx_enable, mute, drive, audio_*, start, stop, …): echo as
    // an ack so a request/response client doesn't hang. Those fields aren't
    // wired to radio state here, so this is intentionally a stub.
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
  {
    char r[32];
    g_snprintf(r, sizeof(r), "trx_count:%d;", tci_trx_count());
    client_send_text(c, r);
  }
  client_send_text(c, "channels_count:2;");
  client_send_text(c, "vfo_limits:0,6000000000;");
  if (rx != NULL && rx->sample_rate > 0) {
    char r[48];
    g_snprintf(r, sizeof(r), "if_limits:%d,%d;", -rx->sample_rate / 2, rx->sample_rate / 2);
    client_send_text(c, r);
  } else {
    client_send_text(c, "if_limits:-24000,24000;");
  }
  client_send_text(c, "modulations_list:" TCI_MODLIST ";");
  {
    char r[48];
    g_snprintf(r, sizeof(r), "audio_samplerate:%d;", g_atomic_int_get(&audio_stream_rate));
    client_send_text(c, r);
  }
  if (rx != NULL) {
    char r[48];
    g_snprintf(r, sizeof(r), "iq_samplerate:%d;", rx->sample_rate);
    client_send_text(c, r);
  }
  // Per-rx initial state so a connecting (multi-RX) logger starts fully in sync.
  int ntrx = tci_trx_count();
  for (int idx = 0; idx < ntrx; idx++) {
    RECEIVER *t = tci_rx_at(idx);
    if (t == NULL) continue;
    char r[64];
    g_snprintf(r, sizeof(r), "vfo:%d,0,%lld;", idx, (long long)t->frequency_a);
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "vfo:%d,1,%lld;", idx, (long long)t->frequency_b);
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "modulation:%d,%s;", idx, mode_to_tci(t->mode_a));
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "rit_enable:%d,%s;", idx, t->rit_enabled ? "true" : "false");
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "rit_offset:%d,%lld;", idx, (long long)t->rit);
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "split_enable:%d,%s;", idx, (t->split != SPLIT_OFF) ? "true" : "false");
    client_send_text(c, r);
  }
  if (g_radio != NULL && g_radio->transmitter != NULL) {
    // Transmitter state is announced against the trx the transmitter is on, not
    // a hard-coded 0 — an unsolicited "xit_enable:0" tells a multi-RX client the
    // wrong thing when the transmitter is not on receiver 0.
    int txi = tci_tx_index();
    char r[48];
    g_snprintf(r, sizeof(r), "xit_enable:%d,%s;", txi, g_radio->transmitter->xit_enabled ? "true" : "false");
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "xit_offset:%d,%lld;", txi, (long long)g_radio->transmitter->xit);
    client_send_text(c, r);
    // Initial PTT state, which the handshake never sent at all.
    g_snprintf(r, sizeof(r), "trx:%d,%s;", txi, g_radio->mox ? "true" : "false");
    client_send_text(c, r);
  }
  client_send_text(c, "ready;");
}

// ---------------------------------------------------------------------------
// client & accept threads
// ---------------------------------------------------------------------------

static void clients_remove(TCI_CLIENT *c) {
  g_mutex_lock(&clients_mutex);
  client_set_iq(c, -1, FALSE);      // clear all rx bits, keep iq_sub_count balanced
  client_set_audio(c, -1, FALSE);   // clear all rx bits, keep audio_sub_count balanced
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
    if (op == 0x1 && payload) tci_process_text(c, payload);          // text command
    else if (op == 0x2 && payload) tci_ingest_binary((guint8 *)payload, plen);  // TX audio
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
      if (clients[i].fd < 0) {
        clients[i].fd = fd;
        g_atomic_int_set(&clients[i].iq_mask, 0);
        g_atomic_int_set(&clients[i].audio_mask, 0);
        slot = &clients[i];
        break;
      }
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
  int au = g_atomic_int_get(&audio_sub_count);
  g_snprintf(buf, sizeof(buf), "%s (%d client%s%s%s)", status_line, n, n == 1 ? "" : "s",
             iq > 0 ? ", IQ" : "", au > 0 ? ", audio" : "");
  return buf;
}

// --- outbound notifications (GTK/audio thread) ------------------------------

void tci_notify_vfo(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;                         // hidden receiver: not a TCI trx
  char line[64];
  g_snprintf(line, sizeof(line), "vfo:%d,0,%lld;", idx, (long long)rx->frequency_a);
  tci_broadcast_text(line);
  g_snprintf(line, sizeof(line), "vfo:%d,1,%lld;", idx, (long long)rx->frequency_b);
  tci_broadcast_text(line);
}

void tci_notify_mode(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;
  char line[48];
  g_snprintf(line, sizeof(line), "modulation:%d,%s;", idx, mode_to_tci(rx->mode_a));
  tci_broadcast_text(line);
}

void tci_notify_trx(gboolean mox) {
  if (!g_atomic_int_get(&server_running)) return;
  char line[32];
  g_snprintf(line, sizeof(line), "trx:%d,%s;", tci_tx_index(), mox ? "true" : "false");
  tci_broadcast_text(line);
}

// RIT / split (RECEIVER) + XIT (TRANSMITTER) changed locally — push the full
// small state set so a following logger stays synced. Called from actions.c.
void tci_notify_state(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;
  char line[64];
  g_snprintf(line, sizeof(line), "rit_enable:%d,%s;", idx, rx->rit_enabled ? "true" : "false");
  tci_broadcast_text(line);
  g_snprintf(line, sizeof(line), "rit_offset:%d,%lld;", idx, (long long)rx->rit);
  tci_broadcast_text(line);
  g_snprintf(line, sizeof(line), "split_enable:%d,%s;", idx, (rx->split != SPLIT_OFF) ? "true" : "false");
  tci_broadcast_text(line);
  if (g_radio != NULL && g_radio->transmitter != NULL) {
    g_snprintf(line, sizeof(line), "xit_enable:%d,%s;", idx, g_radio->transmitter->xit_enabled ? "true" : "false");
    tci_broadcast_text(line);
    g_snprintf(line, sizeof(line), "xit_offset:%d,%lld;", idx, (long long)g_radio->transmitter->xit);
    tci_broadcast_text(line);
  }
}

// --- outbound binary streams (audio thread taps in receiver.c) --------------

// Build the complete on-wire WS frame once (WS binary header + 64-byte TCI
// header + interleaved float32 down-cast from `interleaved`) and broadcast to
// every client subscribed to this rx's stream (`audio` picks audio_mask vs
// iq_mask; only clients with rx_index's bit set receive it). `data` may be a
// float source (`fsrc`) or double source (`interleaved`) — exactly one is used.
static void tci_stream_broadcast(int data_type, int rx_index, int sample_rate, int channels,
                                 const double *interleaved, const float *fsrc,
                                 guint32 nfloats, gboolean audio) {
  if (rx_index < 0 || rx_index >= TCI_MAX_CLIENTS * 4) return;   // bit index sanity
  guint  rxbit = 1u << rx_index;

  // Cheap pre-scan: if no connected client is subscribed to THIS rx's stream,
  // skip building the frame entirely (the malloc + double->float conversion of a
  // whole IQ/audio block). The global sub-count gate already handled "nobody
  // subscribed to anything"; this covers the multi-RX case where clients follow
  // other receivers. A client subscribing between this check and the send below
  // just misses one block — harmless.
  gboolean any = FALSE;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd < 0) continue;
    gint mask = audio ? g_atomic_int_get(&clients[i].audio_mask)
                      : g_atomic_int_get(&clients[i].iq_mask);
    if ((guint)mask & rxbit) { any = TRUE; break; }
  }
  g_mutex_unlock(&clients_mutex);
  if (!any) return;

  size_t  payload = TCI_HDR_BYTES + (size_t)nfloats * sizeof(float);
  guint8  wshdr[10];
  size_t  wshlen = ws_write_header(wshdr, 0x2 /*binary*/, payload);
  size_t  flen = wshlen + payload;
  guint8 *frame = g_malloc(flen);

  memcpy(frame, wshdr, wshlen);
  guint8 *th = frame + wshlen;                 // 64-byte TCI header
  memset(th, 0, TCI_HDR_BYTES);
  st32le(th +  0, (guint32)rx_index);           // rx index
  st32le(th +  4, (guint32)sample_rate);        // sample_rate
  st32le(th +  8, TCI_SAMPLE_FLOAT32);          // data_format
  st32le(th + 12, 0);                           // codec
  st32le(th + 16, 0);                           // crc
  st32le(th + 20, nfloats);                     // length (float count)
  st32le(th + 24, (guint32)data_type);          // data_type
  st32le(th + 28, (guint32)channels);           // channels

  float *fp = (float *)(th + TCI_HDR_BYTES);
  if (fsrc != NULL) memcpy(fp, fsrc, (size_t)nfloats * sizeof(float));
  else for (guint32 i = 0; i < nfloats; i++) fp[i] = (float)interleaved[i];

  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd < 0) continue;
    gint mask = audio ? g_atomic_int_get(&clients[i].audio_mask)
                      : g_atomic_int_get(&clients[i].iq_mask);
    if ((guint)mask & rxbit) client_send_framed_try(&clients[i], frame, flen);
  }
  g_mutex_unlock(&clients_mutex);

  g_free(frame);
}

// Phase B: off-air I/Q (interleaved doubles, `nsamples` complex pairs). Gated so
// it costs one atomic read when nobody subscribed. Streams per-rx (multi-RX): the
// frame's rx index = the receiver's TCI index and only clients subscribed to
// that index receive it.
void tci_iq_feed(RECEIVER *rx, const double *iq, int nsamples, int sample_rate) {
  if (!g_atomic_int_get(&server_running)) return;
  if (g_atomic_int_get(&iq_sub_count) <= 0) return;
  if (rx == NULL || iq == NULL || nsamples <= 0) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;
  // The receiver's buffer is (Q, I) — that is how WDSP reads it (analyzer.c
  // Spectrum0) and therefore what the panadapter shows. TCI carries (I, Q), so
  // send it in that order: passing the buffer through verbatim hands the client
  // the conjugate, i.e. a spectrum mirrored against this radio's own display,
  // which puts every signal a skimmer finds on the wrong side of the centre.
  guint32 nf = (guint32)nsamples * 2u;
  float *swapped = g_new(float, nf);
  for (int i = 0; i < nsamples; i++) {
    swapped[2*i]     = (float)iq[2*i + 1];   // I
    swapped[2*i + 1] = (float)iq[2*i];       // Q
  }
  tci_stream_broadcast(TCI_STREAM_IQ, idx, sample_rate, 2, NULL, swapped, nf, FALSE);
  g_free(swapped);
}

// Phase C: demodulated RX audio (interleaved stereo doubles, `nstereo` frames at
// `sample_rate`, natively 48 kHz). Per-rx (multi-RX). If a client asked for a
// non-48k stream rate the block is resampled per channel first.
void tci_audio_feed(RECEIVER *rx, const double *audio, int nstereo, int sample_rate) {
  if (!g_atomic_int_get(&server_running)) return;
  if (g_atomic_int_get(&audio_sub_count) <= 0) return;
  if (rx == NULL || audio == NULL || nstereo <= 0) return;
  int idx = tci_rx_index(rx);
  if (idx < 0 || idx >= MAX_RECEIVERS) return;

  int target = g_atomic_int_get(&audio_stream_rate);
  if (target <= 0 || target == sample_rate) {
    tci_stream_broadcast(TCI_STREAM_RX_AUDIO, idx, sample_rate, 2, audio, NULL,
                         (guint32)nstereo * 2u, TRUE);
    return;
  }

  // Resample this rx's two channels sample_rate -> target (state per rx).
  TCI_RESAMP *L = &rs_rx[idx][0], *R = &rs_rx[idx][1];
  if (L->rin != sample_rate || L->rout != target) tci_resamp_reset(L, sample_rate, target);
  if (R->rin != sample_rate || R->rout != target) tci_resamp_reset(R, sample_rate, target);

  int cap = (int)((gint64)nstereo * target / sample_rate) + 4;
  float *inL = g_new(float, nstereo), *inR = g_new(float, nstereo);
  for (int i = 0; i < nstereo; i++) { inL[i] = (float)audio[2*i]; inR[i] = (float)audio[2*i+1]; }
  float *outL = g_new(float, cap), *outR = g_new(float, cap);
  int nL = tci_resamp_run(L, inL, nstereo, outL, cap);
  int nR = tci_resamp_run(R, inR, nstereo, outR, cap);
  int no = (nL < nR) ? nL : nR;             // stay interleave-aligned
  if (no > 0) {
    float *inter = g_new(float, (size_t)no * 2);
    for (int i = 0; i < no; i++) { inter[2*i] = outL[i]; inter[2*i+1] = outR[i]; }
    tci_stream_broadcast(TCI_STREAM_RX_AUDIO, idx, target, 2, NULL, inter, (guint32)no * 2u, TRUE);
    g_free(inter);
  }
  g_free(inL); g_free(inR); g_free(outL); g_free(outR);
}

// --- TX audio ingest -> mic substitution (transmitter.c:add_mic_sample) ------

// TRUE while a client is actively streaming TX audio and we are transmitting.
// Self-clears TCI_TX_ACTIVE_US after the last frame (or when MOX drops), so the
// real mic path is untouched unless a TCI client is driving TX right now.
gboolean tci_tx_active(void) {
  if (!g_atomic_int_get(&server_running)) return FALSE;
  if (g_radio == NULL || !g_radio->mox) return FALSE;
  return (g_get_monotonic_time() - tx_audio_last_us) < TCI_TX_ACTIVE_US;
}

// Pop one mono TX sample (0.0 on underrun). Called per-sample on the TX thread.
float tci_tx_next_sample(void) {
  // Lock-free single-consumer read off the audio (hot) path: producers publish
  // head atomically after the slot write, so reading head atomically then the
  // slot is safe without taking tx_ring_mutex per sample.
  float s = 0.0f;
  int tail = tx_ring_tail;                             // only this thread writes tail
  if (tail != g_atomic_int_get(&tx_ring_head)) {
    s = tx_ring[tail];
    g_atomic_int_set(&tx_ring_tail, (tail + 1) % TCI_TX_RING);
  }
  return s;
}
