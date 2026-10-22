/* Browser-remote protocol primitives that do not depend on GTK or a radio. */
#ifndef WEB_PROTOCOL_H
#define WEB_PROTOCOL_H

#include <glib.h>
#include <stddef.h>

#define WEB_PROTOCOL_VERSION 1
#define WEB_MAX_TEXT_MESSAGE 4096
#define WEB_BINARY_HEADER_SIZE 32
#define WEB_SPECTRUM_MAX_BINS 1024

typedef enum {
  WEB_SPECTRUM_LOW = 0,
  WEB_SPECTRUM_BALANCED,
  WEB_SPECTRUM_HIGH
} WEB_SPECTRUM_PROFILE;

typedef struct {
  guint8 version;
  guint8 stream;
  guint8 receiver;
  guint8 flags;
  guint32 sequence;
  guint64 timestamp_us;
  guint32 count;
  guint32 rate_or_width;
  guint32 payload_bytes;
} WEB_BINARY_HEADER;

typedef struct {
  guint bins;
  guint fps;
} WEB_SPECTRUM_LIMITS;

/* A one-element ref-counted queue used by each client/receiver pair. */
typedef struct {
  GBytes *frame;
  guint64 dropped;
} WEB_LATEST_FRAME;

typedef enum {
  WEB_REQUEST_INVALID = 0,
  WEB_REQUEST_GET_STATE,
  WEB_REQUEST_SET_FREQUENCY,
  WEB_REQUEST_SET_MODE,
  WEB_REQUEST_SET_MOX,
  WEB_REQUEST_SET_FILTER,
  WEB_REQUEST_SPECTRUM_SUBSCRIBE,
  WEB_REQUEST_SPECTRUM_UNSUBSCRIBE
} WEB_REQUEST_TYPE;

typedef struct {
  WEB_REQUEST_TYPE type;
  gint64 id;
  gint rx;
  gint64 value;
  gboolean flag;
  gboolean value_is_bool;
  gint low;
  gint high;
  WEB_SPECTRUM_PROFILE profile;
} WEB_REQUEST;

/* Parse one flat protocol-v1 JSON request.  Returns TRUE on success. */
gboolean web_protocol_parse(const char *text, size_t len, WEB_REQUEST *out,
                            char *error, size_t error_len);

/* Fixed HTTP route allow-list.  asset_name is NULL for /ws. */
gboolean web_protocol_route(const char *target, const char **asset_name,
                            const char **content_type, gboolean *websocket);

gboolean web_binary_header_encode(guint8 out[WEB_BINARY_HEADER_SIZE],
                                  const WEB_BINARY_HEADER *header);
gboolean web_binary_header_decode(const guint8 *data, size_t len,
                                  WEB_BINARY_HEADER *header);
gboolean web_spectrum_profile_parse(const char *name,
                                    WEB_SPECTRUM_PROFILE *profile);
const char *web_spectrum_profile_name(WEB_SPECTRUM_PROFILE profile);
WEB_SPECTRUM_LIMITS web_spectrum_profile_limits(int profile);

/* Peak-preserving bucket reduction followed by floor/span quantization. */
gboolean web_spectrum_quantize(const float *input, guint input_count,
                               guint8 *output, guint output_count,
                               float floor_db, float span_db);

void web_latest_frame_replace(WEB_LATEST_FRAME *slot, GBytes *frame);
GBytes *web_latest_frame_take(WEB_LATEST_FRAME *slot);
void web_latest_frame_clear(WEB_LATEST_FRAME *slot);

#endif
