// Offline TCI harness.  Two things: how cw_msg's four fields are split and put
// back together, and how the WebSocket codec frames and reassembles a message.
//
//   tci_offline --selftest
//
// Why these two when the rest of the TCI server has no test: every other command
// in tci.c either mirrors a value the radio already holds (readable by
// inspection, and wrong in an obvious way if it breaks) or needs a live client
// to mean anything.  These two need neither a radio nor a client -- the codec is
// driven over a local socket pair -- and both fail in ways nothing else catches.
// cw_msg is different on both counts.
//
//   * IT IS AN INTERPRETATION, NOT A READING.  The TCI document does not say
//     whether cw_msg's <before>/<after> fields go on the air or only highlight
//     the worked callsign on the client's screen.  This fork sends
//     "<before> <text> <after>".  That decision is written down in tci.c and in
//     CLAUDE.md, and until now it was proven by a test that lived outside the
//     repository -- which is to say, by nothing a `make check` could see.
//
//   * IT PUTS SOMETHING ON THE AIR.  A parse slip here does not produce a wrong
//     readout, it transmits the wrong Morse.  The specific hazard is the
//     positional split: the fields may be empty, and <text> may contain commas
//     of its own, so it cannot go through the comma-split argument vector the
//     rest of the dispatcher uses.  Both of those are asserted below, because
//     both are exactly what a well-meaning "simplify this to strtok_r" would
//     break while every other TCI command kept working.
//
// The parser is pure (glib only) and the codec reaches no further than a socket,
// so this links tci_cw.o + tci_ws.o -- no server, no RADIO, no GTK.

#include "net_compat.h"   // must precede glib: winsock2 before windows.h
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
  #include <unistd.h>
#endif

#include <glib.h>

#include "tci_cw.h"
#include "tci_ws.h"

#if !defined(SHUT_WR)
  #define SHUT_WR SD_SEND        // Winsock spells the half-close differently
#endif

static int failures = 0;

static void check(const char *what, const char *token, const char *expect) {
  char *got = tci_cw_msg_text(token);
  gboolean ok = (expect == NULL) ? (got == NULL)
                                 : (got != NULL && !strcmp(got, expect));
  printf("%-44s %-6s %-30s -> %s\n",
         what, ok ? "PASS" : "FAIL", token,
         got ? got : "(null)");
  if (!ok) {
    printf("%50s expected %s\n", "", expect ? expect : "(null)");
    failures++;
  }
  g_free(got);
}

static void check_fields(const char *what, const char *token, gboolean expect_ok,
                         const char *eb, const char *ea, const char *et) {
  char *before = NULL, *after = NULL;
  const char *text = NULL;
  gboolean ok = tci_cw_msg_fields(token, &before, &after, &text);
  gboolean good = (ok == expect_ok);
  if (good && ok) {
    good = !strcmp(before, eb) && !strcmp(after, ea) && !strcmp(text, et);
  }
  printf("%-44s %-6s %-30s -> ", what, good ? "PASS" : "FAIL", token);
  if (ok) printf("before=\"%s\" after=\"%s\" text=\"%s\"\n", before, after, text);
  else    printf("rejected\n");
  if (!good) failures++;
  g_free(before);
  g_free(after);
}

// --- WebSocket codec ------------------------------------------------------
//
// Driven over a socketpair: this side writes the bytes a client would put on
// the wire, tci_ws_recv() reads them off the other end.  What is being pinned
// down is the reassembly of fragments.  The dispatcher tci_ws_recv() feeds
// executes any command token it is handed WHETHER OR NOT a ';' terminated it,
// so a message delivered in pieces is not an incomplete request -- it is a
// different one that gets carried out.  Measured against the running server:
// the first fragment of "vfo:0,0,14074000;" tuned the receiver to 140 Hz.

// Write one client->server frame (always masked, as RFC 6455 requires of a
// client) with an explicit FIN bit and opcode, so a fragmented message can be
// built frame by frame.
static void ws_client_frame(int fd, int fin, int opcode, const void *payload, size_t len) {
  guint8 h[14];
  size_t n = 0;
  h[n++] = (guint8)((fin ? 0x80 : 0x00) | (opcode & 0x0f));
  if (len < 126)        h[n++] = (guint8)(0x80 | len);
  else if (len < 65536) { h[n++] = 0x80 | 126; h[n++] = (guint8)(len >> 8); h[n++] = (guint8)len; }
  else {
    h[n++] = 0x80 | 127;
    for (int i = 0; i < 8; i++) h[n++] = (guint8)(((guint64)len >> (56 - 8 * i)) & 0xff);
  }
  const guint8 mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
  memcpy(h + n, mask, 4); n += 4;
  if (send(fd, (const char *)h, n, 0) != (ssize_t)n) { failures++; return; }
  guint8 *m = g_malloc(len ? len : 1);
  for (size_t i = 0; i < len; i++) m[i] = ((const guint8 *)payload)[i] ^ mask[i & 3];
  if (len && send(fd, (const char *)m, len, 0) != (ssize_t)len) failures++;
  g_free(m);
}

// A connected local stream pair. Windows has no socketpair(), so it gets a
// loopback TCP pair instead -- these harnesses run natively there (the .exe is
// the one thing about the Windows port that is actually tested), so a POSIX-only
// test would simply stop existing on the platform that most needs one.
static gboolean local_pair(int sv[2]) {
#if !defined(_WIN32)
  return socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
#else
  int lsn = socket(AF_INET, SOCK_STREAM, 0);
  if (lsn < 0) return FALSE;
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;                                  // let the OS choose
  socklen_t alen = sizeof(a);
  if (bind(lsn, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lsn, 1) != 0 ||
      getsockname(lsn, (struct sockaddr *)&a, &alen) != 0) { closesocket(lsn); return FALSE; }
  sv[0] = socket(AF_INET, SOCK_STREAM, 0);
  if (sv[0] < 0) { closesocket(lsn); return FALSE; }
  if (connect(sv[0], (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(sv[0]); closesocket(lsn); return FALSE; }
  sv[1] = accept(lsn, NULL, NULL);
  closesocket(lsn);
  return sv[1] >= 0;
#endif
}

static void check_ws(const char *what, int expect_op, const char *expect_payload,
                     void (*write_frames)(int fd)) {
  int sv[2];
  if (!local_pair(sv)) { printf("%-44s FAIL no local socket pair\n", what); failures++; return; }
  write_frames(sv[0]);
  shutdown(sv[0], SHUT_WR);          // so a codec that waits for more sees EOF, not a hang

  char *out = NULL;
  size_t olen = 0;
  int op = tci_ws_recv(sv[1], &out, &olen);

  gboolean ok = (op == expect_op);
  if (ok && expect_payload != NULL)
    ok = (out != NULL && olen == strlen(expect_payload) && !memcmp(out, expect_payload, olen));
  printf("%-44s %-6s op=%-3d -> %s\n", what, ok ? "PASS" : "FAIL", op,
         out ? out : "(null)");
  if (!ok) {
    printf("%52s expected op=%d %s\n", "", expect_op,
           expect_payload ? expect_payload : "(any)");
    failures++;
  }
  g_free(out);
  closesocket(sv[0]); closesocket(sv[1]);
}

static void w_single_text(int fd) {
  ws_client_frame(fd, 1, 0x1, "vfo:0,0,14074000;", 17);
}
static void w_fragmented(int fd) {
  // The exact split that used to retune the radio: "vfo:0,0,140" + "74000;".
  ws_client_frame(fd, 0, 0x1, "vfo:0,0,140", 11);
  ws_client_frame(fd, 1, 0x0, "74000;", 6);
}
static void w_fragmented_three(int fd) {
  ws_client_frame(fd, 0, 0x1, "vfo:", 4);
  ws_client_frame(fd, 0, 0x0, "0,0,140", 7);
  ws_client_frame(fd, 1, 0x0, "74000;", 6);
}
static void w_ping_between_fragments(int fd) {
  // A control frame may be interleaved into a fragmented message. It must come
  // back at once (the server has to pong it) and must never be spliced into the
  // payload being assembled.
  ws_client_frame(fd, 0, 0x1, "vfo:0,0,140", 11);
  ws_client_frame(fd, 1, 0x9, "hi", 2);
  ws_client_frame(fd, 1, 0x0, "74000;", 6);
}
static void w_orphan_continuation(int fd) {
  // A continuation with no message started names nothing; it must be dropped,
  // not treated as a message of its own.
  ws_client_frame(fd, 1, 0x0, "74000;", 6);
  ws_client_frame(fd, 1, 0x1, "trx:0;", 6);
}
static void w_binary_16bit_len(int fd) {
  char big[300];
  memset(big, 'x', sizeof(big));
  ws_client_frame(fd, 1, 0x2, big, sizeof(big));   // forces the 126 length form
}
static void w_oversize(int fd) {
  // A length past the cap must be refused outright rather than allocated.
  guint8 h[14];
  size_t n = 0;
  h[n++] = 0x81; h[n++] = 0x80 | 127;
  guint64 len = (guint64)TCI_WS_MAX_MSG + 1;
  for (int i = 0; i < 8; i++) h[n++] = (guint8)((len >> (56 - 8 * i)) & 0xff);
  memset(h + n, 0, 4); n += 4;
  if (send(fd, (const char *)h, n, 0) != (ssize_t)n) failures++;
}
static void w_truncated(int fd) {
  // The header promises 20 payload bytes; only 5 arrive, then EOF. A codec that
  // handed the short read on as a message would deliver a truncated command.
  guint8 h[6] = { 0x81, 0x80 | 20, 0, 0, 0, 0 };   // FIN|text, masked, len 20
  if (send(fd, (const char *)h, 6, 0) != 6) failures++;
  if (send(fd, "trx:0", 5, 0) != 5) failures++;
}

int main(int argc, char **argv) {
  if (argc != 2 || strcmp(argv[1], "--selftest")) {
    fprintf(stderr, "usage: %s --selftest\n", argv[0]);
    return 2;
  }
  if (!net_startup()) {            // no-op off Windows; Winsock needs it first
    fprintf(stderr, "net_startup failed\n");
    return 2;
  }

  printf("\n-- cw_msg: the common case must be byte-identical to <text> --\n");
  // A client that does not use the callsign markers sends them empty. Anything
  // added at those boundaries would be transmitted as extra Morse.
  check("text only", "cw_msg:0,,,CQ CQ DE MM0ABC K", "CQ CQ DE MM0ABC K");
  check("text only, trx 1", "cw_msg:1,,,TEST", "TEST");

  printf("\n-- cw_msg: the documented interpretation, before <text> after --\n");
  check("before + text",         "cw_msg:0,G0ORX,,DE MM0ABC",   "G0ORX DE MM0ABC");
  check("text + after",          "cw_msg:0,,K,DE MM0ABC",       "DE MM0ABC K");
  check("before + text + after", "cw_msg:0,G0ORX,K,DE MM0ABC",  "G0ORX DE MM0ABC K");

  printf("\n-- cw_msg: one word gap, never two --\n");
  // The fields are words, not glue: if the client already put whitespace at a
  // boundary, adding another space would send an extra word gap.
  check("client's trailing space is not doubled",
        "cw_msg:0,G0ORX ,,DE MM0ABC", "G0ORX DE MM0ABC");
  check("client's leading space is not doubled",
        "cw_msg:0,G0ORX, K,DE MM0ABC", "G0ORX DE MM0ABC K");

  printf("\n-- cw_msg: <text> may contain commas (the strtok_r trap) --\n");
  // The whole reason this is a positional split: a comma-splitting parser
  // truncates the message here, and would transmit only the first clause.
  check("commas inside text are kept",
        "cw_msg:0,,,RST 599,599 QTH EDINBURGH", "RST 599,599 QTH EDINBURGH");
  check("commas inside text, with markers",
        "cw_msg:0,G0ORX,K,RST 599,599", "G0ORX RST 599,599 K");
  check("text that is nothing but commas",
        "cw_msg:0,,,,,", ",,");

  printf("\n-- cw_msg: empty and malformed tokens key NOTHING --\n");
  // NULL means "do not transmit". An empty string here would key the
  // transmitter for a zero-length message.
  check("all four fields empty",     "cw_msg:0,,,",        NULL);
  check("no colon",                  "cw_msg 0,,,TEST",    NULL);
  check("only two separators",       "cw_msg:0,,TEST",     NULL);
  check("no separators at all",      "cw_msg:0",           NULL);
  check("nothing after the colon",   "cw_msg:",            NULL);

  printf("\n-- cw_msg: the field split itself --\n");
  check_fields("empty fields are preserved, not collapsed",
               "cw_msg:0,,,TEXT", TRUE, "", "", "TEXT");
  check_fields("all four present",
               "cw_msg:2,BEF,AFT,THE TEXT", TRUE, "BEF", "AFT", "THE TEXT");
  check_fields("text keeps every trailing comma",
               "cw_msg:0,B,A,x,y,z", TRUE, "B", "A", "x,y,z");
  check_fields("a token with two commas is rejected",
               "cw_msg:0,B,A", FALSE, NULL, NULL, NULL);

  printf("\n-- websocket codec: a message arrives whole or not at all --\n");
  check_ws("one unfragmented text frame", 0x1, "vfo:0,0,14074000;", w_single_text);
  check_ws("two fragments reassemble", 0x1, "vfo:0,0,14074000;", w_fragmented);
  check_ws("three fragments reassemble", 0x1, "vfo:0,0,14074000;", w_fragmented_three);
  check_ws("a ping between fragments comes back first", 0x9, "hi", w_ping_between_fragments);
  check_ws("an orphan continuation is dropped", 0x1, "trx:0;", w_orphan_continuation);

  printf("\n-- websocket codec: lengths and refusals --\n");
  check_ws("300-byte binary uses the 16-bit length", 0x2, NULL, w_binary_16bit_len);
  check_ws("a length past the cap is refused", -1, NULL, w_oversize);
  check_ws("a truncated frame is refused, not guessed", -1, NULL, w_truncated);

  printf("\n%s\n", failures == 0 ? "all cases passed" : "FAILURES ABOVE");
  return failures == 0 ? 0 : 1;
}
