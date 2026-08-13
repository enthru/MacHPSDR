/* TCI cw_msg field split and concatenation.
 *
 * Split out of tci.c because it is the one piece of the TCI server whose
 * behaviour is an INTERPRETATION rather than a reading of the spec, and so the
 * one piece that needs a test of its own. The protocol document does not say
 * whether cw_msg's <before>/<after> fields are transmitted or only used for
 * on-screen highlighting of the worked callsign; this fork sends
 * "<before> <text> <after>", joined with single spaces (CW word gaps), which is
 * the reading that agrees with the %C macro the app already has. See the
 * cw_msg handler in tci.c for the full argument.
 *
 * Everything here is PURE -- glib only, no RADIO, no GTK, no sockets -- which is
 * what lets tools/tci_offline.c link it and assert the concatenation without
 * standing a server up. Keep it that way: the moment this reaches for the radio
 * the assumption goes back to being untested.
 *
 * The token these parse is "cw_msg:<rx>,<before>,<after>,<text>".
 */
#ifndef TCI_CW_H
#define TCI_CW_H

#include <glib.h>

/* Split "cw_msg:<rx>,<before>,<after>,<text>" positionally. Fields can be empty
 * and <text> may contain commas of its own, which is why this cannot go through
 * a strtok_r'd argument vector (that both collapses empty fields and truncates
 * at the fourth comma). Returns FALSE unless all three separators are present.
 * The two out-params before and after are g_malloc'd (caller frees); text
 * points into `token`. */
gboolean tci_cw_msg_fields(const char *token, char **before, char **after,
                           const char **text);

/* Assemble what cw_msg puts on the air. Returns a g_malloc'd string, or NULL if
 * the token was malformed or every field was empty. */
char *tci_cw_msg_text(const char *token);

#endif /* TCI_CW_H */
