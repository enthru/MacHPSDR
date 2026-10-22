/* Browser-remote protocol primitives that do not depend on GTK or a radio. */
#ifndef WEB_PROTOCOL_H
#define WEB_PROTOCOL_H

#include <glib.h>
#include <stddef.h>

#define WEB_PROTOCOL_VERSION 1
#define WEB_MAX_TEXT_MESSAGE 4096

typedef enum {
  WEB_REQUEST_INVALID = 0,
  WEB_REQUEST_GET_STATE,
  WEB_REQUEST_SET_FREQUENCY,
  WEB_REQUEST_SET_MODE,
  WEB_REQUEST_SET_MOX
} WEB_REQUEST_TYPE;

typedef struct {
  WEB_REQUEST_TYPE type;
  gint64 id;
  gint rx;
  gint64 value;
  gboolean flag;
  gboolean value_is_bool;
} WEB_REQUEST;

/* Parse one flat protocol-v1 JSON request.  Returns TRUE on success. */
gboolean web_protocol_parse(const char *text, size_t len, WEB_REQUEST *out,
                            char *error, size_t error_len);

/* Fixed HTTP route allow-list.  asset_name is NULL for /ws. */
gboolean web_protocol_route(const char *target, const char **asset_name,
                            const char **content_type, gboolean *websocket);

#endif
