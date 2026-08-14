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

#include "tci_ws.h"

#include <string.h>
#include <errno.h>

#define TCI_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

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

gboolean tci_ws_send_all(int fd, const void *buf, size_t n) {
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

size_t tci_ws_write_header(guint8 *hdr, int opcode, size_t len) {
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

gboolean tci_ws_send_blocking(int fd, int opcode, const void *payload, size_t len) {
  guint8 hdr[10];
  size_t hlen = tci_ws_write_header(hdr, opcode, len);
  if (!tci_ws_send_all(fd, hdr, hlen)) return FALSE;
  if (len && !tci_ws_send_all(fd, payload, len)) return FALSE;
  return TRUE;
}

// Fragmentation has to be reassembled here rather than ignored. A data frame
// carries FIN=0 when more is coming, and tci_process_text() executes any token
// it is handed whether or not a ';' terminated it — so handing it the first
// fragment of "vfo:0,0,14074000;" runs the truncated command, i.e. a split
// message retunes the receiver to whatever prefix happened to arrive (measured:
// an unterminated "vfo:0,0,140" tunes to 140 Hz and is answered as if asked
// for). The message is capped at TCI_WS_MAX_MSG in TOTAL, not per frame, so
// fragmentation cannot be used to grow the buffer without limit.
int tci_ws_recv(int fd, char **out, size_t *out_len) {
  char   *acc = NULL;                       // reassembled payload so far
  size_t  acc_len = 0;
  int     acc_op = -1;                      // opcode of the message being assembled
  *out = NULL; *out_len = 0;

  for (;;) {
    guint8 h[2];
    if (!recv_all(fd, h, 2)) { g_free(acc); return -1; }
    int fin    = (h[0] & 0x80) != 0;
    int opcode = h[0] & 0x0f;
    int masked = (h[1] & 0x80) != 0;
    guint64 len = h[1] & 0x7f;
    if (len == 126) {
      guint8 e[2];
      if (!recv_all(fd, e, 2)) { g_free(acc); return -1; }
      len = ((guint64)e[0] << 8) | e[1];
    } else if (len == 127) {
      guint8 e[8];
      if (!recv_all(fd, e, 8)) { g_free(acc); return -1; }
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    if (len > TCI_WS_MAX_MSG || acc_len + len > TCI_WS_MAX_MSG) { g_free(acc); return -1; }
    guint8 mask[4] = {0,0,0,0};
    if (masked && !recv_all(fd, mask, 4)) { g_free(acc); return -1; }
    char *buf = g_malloc(len + 1);
    if (len && !recv_all(fd, buf, len)) { g_free(buf); g_free(acc); return -1; }
    if (masked) for (guint64 i = 0; i < len; i++) buf[i] ^= mask[i & 3];
    buf[len] = '\0';

    if (opcode >= 0x8) {
      // A control frame (close/ping/pong) is never fragmented and may be
      // interleaved into a fragmented message. Answer it now and drop the
      // partial: losing an unfinished message is what the pre-reassembly code
      // did anyway, and it is the one outcome that is never a truncated command.
      g_free(acc);
      *out = buf; *out_len = (size_t)len;
      return opcode;
    }

    if (opcode == 0x0) {                    // continuation
      if (acc_op < 0) { g_free(buf); continue; }   // no message started: ignore
    } else {                                // a new message begins
      g_free(acc); acc = NULL; acc_len = 0;
      acc_op = opcode;
    }

    acc = g_realloc(acc, acc_len + len + 1);
    memcpy(acc + acc_len, buf, (size_t)len);
    acc_len += (size_t)len;
    acc[acc_len] = '\0';
    g_free(buf);

    if (fin) {
      *out = acc; *out_len = acc_len;
      return acc_op;
    }
  }
}

// HTTP Upgrade handshake: read the request, compute Sec-WebSocket-Accept via
// GLib SHA1+base64, reply 101. TRUE on success.
gboolean tci_ws_handshake(int fd) {
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
  return tci_ws_send_all(fd, resp, (size_t)rl);
}
