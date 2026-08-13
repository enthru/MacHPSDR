// Offline TCI harness.  Today it covers one thing: how cw_msg's four fields are
// split and put back together.
//
//   tci_offline --selftest
//
// Why this one command has a test when the rest of the TCI server does not:
// every other command in tci.c either mirrors a value the radio already holds
// (readable by inspection, and wrong in an obvious way if it breaks) or needs a
// live client to mean anything.  cw_msg is different on both counts.
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
// The parser is pure (glib only), so this links tci_cw.o alone -- no server, no
// sockets, no RADIO, no GTK.

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "tci_cw.h"

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

int main(int argc, char **argv) {
  if (argc != 2 || strcmp(argv[1], "--selftest")) {
    fprintf(stderr, "usage: %s --selftest\n", argv[0]);
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

  printf("\n%s\n", failures == 0 ? "all cases passed" : "FAILURES ABOVE");
  return failures == 0 ? 0 : 1;
}
