#ifndef WEB_TLS_H
#define WEB_TLS_H

#include <glib.h>
#include "tci_ws.h"

typedef struct web_tls_context WEB_TLS_CONTEXT;
typedef struct web_tls_session WEB_TLS_SESSION;

/* bundle_path may be empty: a persistent self-signed PEM bundle is then
   generated below the application's data directory. */
WEB_TLS_CONTEXT *web_tls_context_new(const char *bundle_path, char **error);
void web_tls_context_free(WEB_TLS_CONTEXT *context);
const char *web_tls_context_fingerprint(WEB_TLS_CONTEXT *context);
const char *web_tls_context_bundle(WEB_TLS_CONTEXT *context);

WEB_TLS_SESSION *web_tls_accept(WEB_TLS_CONTEXT *context, int fd,
                                TCI_WS_IO *io, char **error);
void web_tls_session_free(WEB_TLS_SESSION *session);

#endif
