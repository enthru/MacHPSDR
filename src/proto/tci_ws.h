/* Minimal WebSocket (RFC 6455) framing for the TCI server.
 *
 * Split out of tci.c for the reason tci_cw.c was: it is the part of the server
 * that parses bytes chosen by whoever connected, and it is testable without a
 * radio.  Everything here knows about sockets and glib and NOTHING about
 * RECEIVER, TRANSMITTER or GTK, which is what lets tools/tci_offline.c drive it
 * over a socketpair and assert the framing.  Keep it that way.
 *
 * Scope: text/binary/close/ping, 7/16/64-bit lengths, client masking,
 * fragmentation reassembly.  No extensions, no compression, no TLS.
 */
#ifndef TCI_WS_H
#define TCI_WS_H

#include "net_compat.h"   /* must precede glib on Windows: winsock2 before windows.h */
#include <glib.h>

/* Suppress SIGPIPE on a broken client socket: macOS uses the SO_NOSIGPIPE
 * sockopt (set by the caller on accept), Linux and Windows use the MSG_NOSIGNAL
 * send flag.  tci.c needs this for its own non-blocking sends too. */
#ifdef __APPLE__
  #define TCI_SEND_FLAGS 0
#else
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif
  #define TCI_SEND_FLAGS MSG_NOSIGNAL
#endif

/* Largest message accepted, counting every fragment of it.  Control lines are
 * tiny and TX-audio blocks are small; anything larger is not a TCI client. */
#define TCI_WS_MAX_MSG 262144

/* Send a full buffer (blocking).  TRUE on success. */
gboolean tci_ws_send_all(int fd, const void *buf, size_t n);

/* Write a server->client (unmasked) frame header for `len` payload bytes into
 * `hdr` (which must be >= 10 bytes).  Returns the header length: 2, 4 or 10.
 * Exposed because tci.c builds broadcast frames once and reuses the bytes. */
size_t tci_ws_write_header(guint8 *hdr, int opcode, size_t len);

/* Blocking send of one framed message on `fd` (the caller holds the per-client
 * send lock).  TRUE on success. */
gboolean tci_ws_send_blocking(int fd, int opcode, const void *payload, size_t len);

/* Receive one COMPLETE message.  Returns its opcode (>= 0) and a g_malloc'd,
 * NUL-terminated payload in *out (caller frees), or -1 on error/close.
 *
 * Fragments are reassembled rather than delivered piecemeal: the TCI dispatcher
 * executes any command token it is handed whether or not a ';' terminated it,
 * so half a message is not an incomplete request, it is a DIFFERENT one that
 * gets carried out.  A control frame interleaved into a fragmented message is
 * returned at once and the partial message dropped. */
int tci_ws_recv(int fd, char **out, size_t *out_len);

/* HTTP Upgrade handshake: read the request, answer 101 with the computed
 * Sec-WebSocket-Accept.  TRUE on success. */
gboolean tci_ws_handshake(int fd);

#endif /* TCI_WS_H */
