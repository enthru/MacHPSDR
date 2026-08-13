/* tci_cw.c -- see tci_cw.h. Pure: glib and nothing else. */

#include <string.h>
#include "tci_cw.h"

// Append one cw_msg field to the message being assembled, separating it from
// what is already there with a single space (a CW word gap) unless the client
// put whitespace at that boundary itself. Empty fields -- which before/after
// usually are -- contribute nothing, so the common case is byte-identical to
// sending <text> alone.
static void tci_cw_append(GString *s, const char *part) {
  if (part == NULL || *part == '\0') return;
  if (s->len > 0 && !g_ascii_isspace(s->str[s->len - 1]) && !g_ascii_isspace(part[0]))
    g_string_append_c(s, ' ');
  g_string_append(s, part);
}

gboolean tci_cw_msg_fields(const char *token, char **before, char **after,
                           const char **text) {
  *before = NULL; *after = NULL; *text = NULL;
  if (token == NULL) return FALSE;
  const char *p = strchr(token, ':');
  if (p == NULL) return FALSE;
  const char *c1 = NULL, *c2 = NULL, *c3 = NULL;
  for (const char *q = p + 1; *q; q++) {
    if (*q != ',') continue;
    if      (c1 == NULL) c1 = q;
    else if (c2 == NULL) c2 = q;
    else                 { c3 = q; break; }
  }
  if (c3 == NULL) return FALSE;
  *before = g_strndup(c1 + 1, (size_t)(c2 - c1) - 1);
  *after  = g_strndup(c2 + 1, (size_t)(c3 - c2) - 1);
  *text   = c3 + 1;
  return TRUE;
}

char *tci_cw_msg_text(const char *token) {
  char *before = NULL, *after = NULL;
  const char *text = NULL;
  if (!tci_cw_msg_fields(token, &before, &after, &text)) return NULL;
  GString *msg = g_string_new(NULL);
  tci_cw_append(msg, before);
  tci_cw_append(msg, text);
  tci_cw_append(msg, after);
  g_free(before);
  g_free(after);
  return g_string_free(msg, msg->len == 0);   // NULL when nothing was assembled
}
