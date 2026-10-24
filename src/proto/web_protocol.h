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
  WEB_REQUEST_SET_TUNE,
  WEB_REQUEST_SET_FILTER,
  WEB_REQUEST_SET_CONTROL,
  WEB_REQUEST_SET_TX_CONTROL,
  WEB_REQUEST_SPECTRUM_SUBSCRIBE,
  WEB_REQUEST_SPECTRUM_UNSUBSCRIBE,
  WEB_REQUEST_AUDIO_SUBSCRIBE,
  WEB_REQUEST_AUDIO_UNSUBSCRIBE,
  WEB_REQUEST_RTC_ANSWER,
  WEB_REQUEST_RTC_CANDIDATE,
  WEB_REQUEST_LEASE_ACQUIRE,
  WEB_REQUEST_LEASE_RENEW,
  WEB_REQUEST_LEASE_RELEASE,
  WEB_REQUEST_CONTROL_ACQUIRE,
  WEB_REQUEST_CONTROL_RENEW,
  WEB_REQUEST_CONTROL_RELEASE
} WEB_REQUEST_TYPE;

/* Who is allowed to send a request.  A connection's ROLE comes from the login
   it used (the viewer login can never control); the CONTROL LEASE picks the one
   controller among the control-role connections. */
typedef enum {
  WEB_ROLE_VIEW = 0,
  WEB_ROLE_CONTROL
} WEB_ROLE;

typedef enum {
  WEB_ACCESS_VIEW = 0,      /* any connection: snapshots, spectrum, receive audio */
  WEB_ACCESS_CONTROL_ROLE,  /* control role, no lease needed: the control-lease
                               messages and the MOX/Tune-off emergency unkey */
  WEB_ACCESS_CONTROL_LEASE  /* control role AND the live control lease */
} WEB_ACCESS;

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
  WEB_CTRL_SNB,        /* 0/1 spectral noise blanker */
  WEB_CTRL_FILTER_PRESET, /* filter index F0..FVar2 into filters[mode] */
  WEB_CTRL_RIT,        /* RIT offset, Hz (clamped to +/-10000) */
  WEB_CTRL_RIT_ENABLE, /* 0/1 RIT on */
  WEB_CTRL_XIT,        /* XIT offset, Hz (clamped to +/-10000) */
  WEB_CTRL_XIT_ENABLE, /* 0/1 XIT on */
  WEB_CTRL_SPLIT,      /* 0/1 split (A/B) */
  WEB_CTRL_BAND,       /* +1 up / -1 down through the band table */
  WEB_CTRL_RF_GAIN,    /* device-shaped RX gain or attenuation, dB */
  WEB_CTRL_SUBRX,      /* 0/1 second demodulator */
  WEB_CTRL_SUBRX_MIX,  /* 0 hard L/R split .. 100 mono blend */
  WEB_CTRL_BOOKMARK    /* recall bookmark by zero-based index */
} WEB_CONTROL;

/* Scalar transmitter controls.  They are global (no receiver index) and may
   only be applied by the current browser TX-lease holder. */
typedef enum {
  WEB_TX_CTRL_NONE = 0,
  WEB_TX_CTRL_DRIVE,        /* 0..100 percent */
  WEB_TX_CTRL_TUNE_DRIVE,   /* 0..100 percent */
  WEB_TX_CTRL_MIC_GAIN,     /* -10..+50 dB */
  WEB_TX_CTRL_COMPRESSOR,   /* 0/1 */
  WEB_TX_CTRL_COMP_LEVEL,   /* 0..20 dB */
  WEB_TX_CTRL_LEVELER,      /* 0/1 */
  WEB_TX_CTRL_CESSB         /* 0/1 */
} WEB_TX_CONTROL;

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
  WEB_TX_CONTROL tx_control;
  WEB_SPECTRUM_PROFILE profile;
  gboolean force;          /* control_acquire: take the lease over from its holder */
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

/* What a parsed request needs before the server may act on it.  A request type
   the policy does not name needs the lease, so a type added later is
   controller-only until someone decides otherwise. */
WEB_ACCESS web_request_access(const WEB_REQUEST *req);
const char *web_role_name(WEB_ROLE role);

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

/* Failed-login throttling per IPv4 source address, on an explicit clock so the
   harness can drive it.  A request with no Authorization header is the
   browser's first try and is not a failure; a wrong one is.  After
   WEB_AUTH_FREE_FAILURES wrong tries the address is refused without the hash
   being computed for WEB_AUTH_FIRST_LOCK_US, doubling with every further
   lockout up to WEB_AUTH_MAX_LOCK_US.  WEB_AUTH_FORGET_US after its last
   failure an address starts clean; a correct login clears it at once.  The
   table is bounded: when full, the stalest unlocked entry is reused, and with
   every slot locked the lockout that ends soonest -- a flood of sources can
   shorten a lockout but never shut a fresh address (the operator) out. */
#define WEB_AUTH_THROTTLE_SLOTS 64
#define WEB_AUTH_FREE_FAILURES 5
#define WEB_AUTH_FIRST_LOCK_US (30 * G_USEC_PER_SEC)
#define WEB_AUTH_MAX_LOCK_US (15 * 60 * G_USEC_PER_SEC)
#define WEB_AUTH_FORGET_US (15 * 60 * G_USEC_PER_SEC)

typedef struct {
  gboolean used;
  guint32 address;          /* network byte order */
  guint failures;
  gint64 last_failure_us;
  gint64 locked_until_us;
  gint64 lock_us;           /* length of the next lockout */
} WEB_AUTH_THROTTLE_ENTRY;

typedef struct {
  WEB_AUTH_THROTTLE_ENTRY entry[WEB_AUTH_THROTTLE_SLOTS];
} WEB_AUTH_THROTTLE;

void web_auth_throttle_init(WEB_AUTH_THROTTLE *t);
/* Microseconds this address must still wait (0 = it may try). */
gint64 web_auth_throttle_wait(WEB_AUTH_THROTTLE *t, guint32 address, gint64 now_us);
/* Record a wrong login.  Returns the lockout it started, or 0 if none. */
gint64 web_auth_throttle_failure(WEB_AUTH_THROTTLE *t, guint32 address, gint64 now_us);
void web_auth_throttle_success(WEB_AUTH_THROTTLE *t, guint32 address);

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
