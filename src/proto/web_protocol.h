/* Browser-remote protocol primitives that do not depend on GTK or a radio. */
#ifndef WEB_PROTOCOL_H
#define WEB_PROTOCOL_H

#include <glib.h>
#include <stddef.h>

#define WEB_PROTOCOL_VERSION 1
#define WEB_MAX_TEXT_MESSAGE 4096
#define WEB_BINARY_HEADER_SIZE 32
#define WEB_SPECTRUM_MAX_BINS 2048
#define WEB_AUTH_HASH_HEX_SIZE 65
#define WEB_AUTH_PBKDF2_ITERATIONS 100000

typedef enum {
  WEB_SPECTRUM_LOW = 0,
  WEB_SPECTRUM_BALANCED,
  WEB_SPECTRUM_HIGH,
  WEB_SPECTRUM_ULTRA
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
  WEB_REQUEST_SET_CONTROL,
  WEB_REQUEST_SPECTRUM_SUBSCRIBE,
  WEB_REQUEST_SPECTRUM_UNSUBSCRIBE,
  WEB_REQUEST_AUDIO_SUBSCRIBE,
  WEB_REQUEST_AUDIO_UNSUBSCRIBE,
  WEB_REQUEST_RTC_ANSWER,
  WEB_REQUEST_RTC_CANDIDATE
} WEB_REQUEST_TYPE;

/* Scalar per-receiver controls carried by a SET_CONTROL command.  The allow-list
   lives in the parser; range-clamping is done on the GTK thread by the setter. */
typedef enum {
  WEB_CTRL_NONE = 0,
  WEB_CTRL_AF_GAIN,    /* 0..100 -> rx->volume 0..1 */
  WEB_CTRL_AGC,        /* AGC_OFF..AGC_FAST (0..4) */
  WEB_CTRL_AGC_GAIN,   /* dB, clamped to [-20, 120] */
  WEB_CTRL_SQUELCH,    /* 0..100 -> rx->squelch 0..1 */
  WEB_CTRL_STEP,       /* tuning step, Hz */
  WEB_CTRL_NB,         /* 0=off, 1=NB, 2=NB2 */
  WEB_CTRL_NR,         /* 0=off, 1=NR, 2=NR2, 3=NR3, 4=NR4 */
  WEB_CTRL_ANF,        /* 0/1 automatic notch filter */
  WEB_CTRL_SNB         /* 0/1 spectral noise blanker */
} WEB_CONTROL;

typedef struct {
  WEB_REQUEST_TYPE type;
  gint64 id;
  gint rx;
  gint64 value;
  gboolean flag;
  gboolean value_is_bool;
  gint low;
  gint high;
  WEB_CONTROL control;
  WEB_SPECTRUM_PROFILE profile;
  /* WebRTC signaling payloads, heap-allocated and unescaped only on a
     successful parse; released by web_request_clear(). */
  gchar *sdp;
  gchar *candidate;
  gchar *mid;
} WEB_REQUEST;

/* Parse one flat protocol-v1 JSON request.  Returns TRUE on success.  On
   success the caller must call web_request_clear() to release any heap fields
   (sdp/candidate/mid); on failure nothing is allocated. */
gboolean web_protocol_parse(const char *text, size_t len, WEB_REQUEST *out,
                            char *error, size_t error_len);

/* Free the heap fields of a parsed request (safe on a zeroed struct). */
void web_request_clear(WEB_REQUEST *req);

/* JSON-escape a NUL-terminated string into a newly allocated string (control
   chars, quote and backslash), for embedding SDP/candidates in a text frame.
   Caller frees with g_free(). */
gchar *web_json_escape(const char *s);

/* Fixed HTTP route allow-list.  asset_name is NULL for /ws. */
gboolean web_protocol_route(const char *target, const char **asset_name,
                            const char **content_type, gboolean *websocket);

/* True only when a browser Origin is the HTTP(S) origin for this request Host. */
gboolean web_origin_matches_host(const char *origin, const char *host);

/* Browser-remote HTTP Basic authentication.  Only the salted PBKDF2 result is
   persisted; the clear-text password exists only while the operator enters it
   and while one Authorization header is being verified. */
gboolean web_auth_password_hash(const char *password, const char *salt,
                                guint iterations,
                                char out[WEB_AUTH_HASH_HEX_SIZE]);
gboolean web_auth_check_basic(const char *authorization, const char *username,
                              const char *salt, const char *expected_hash,
                              guint iterations);

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
