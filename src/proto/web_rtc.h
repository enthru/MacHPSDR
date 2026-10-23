/* WebRTC transport for the browser remote's receive audio.
 *
 * The browser remote carries control/signaling over its WebSocket; receive
 * audio is a DTLS-SRTP Opus track the browser decodes with its own jitter
 * buffer, so the server never ships I/Q or display-rate data over the Internet.
 * This unit wraps libdatachannel's C API (rtc/rtc.h) as the offering (server)
 * side of one peer connection per browser audio subscription.
 *
 * Compiled only under -D WEBRTC (make WEBRTC_INCLUDE=WEBRTC).  The dependency is
 * built from source by tools/build-libdatachannel.sh; see doc/web-remote.md and
 * doc/web-remote-status.md.
 */
#ifndef WEB_RTC_H
#define WEB_RTC_H

#include <glib.h>

/* One-time libdatachannel logger/setup; idempotent. */
void web_rtc_init(void);

/* Process teardown; idempotent. */
void web_rtc_cleanup(void);

/* Create and immediately destroy a PeerConnection: proves the stack links and
 * initialises.  TRUE on success. */
gboolean web_rtc_selftest(void);

/* One offering peer connection toward a single browser. */
typedef struct WEB_RTC_SESSION WEB_RTC_SESSION;

/* Outgoing signaling, delivered from a libdatachannel worker thread.  For a
 * local description `kind` is "offer"/"answer" and `data` is the SDP, `mid` is
 * NULL; for a local ICE candidate `kind` is "candidate", `data` is the candidate
 * line and `mid` its media id.  The callback must copy what it needs and must
 * not block on the network (the server forwards it on its serialized WebSocket
 * send path). */
typedef void (*web_rtc_signal_cb)(void *user, const char *kind,
                                  const char *data, const char *mid);

/* Connection-state change, from a libdatachannel worker thread. */
typedef void (*web_rtc_state_cb)(void *user, gboolean connected, gboolean failed);

/* Create a session (does not negotiate yet).  Returns NULL on failure. */
WEB_RTC_SESSION *web_rtc_session_new(web_rtc_signal_cb signal,
                                     web_rtc_state_cb state, void *user);

/* Add the Opus send-only audio track and emit the SDP offer via the signal
 * callback.  Call once, after creation. */
gboolean web_rtc_session_offer(WEB_RTC_SESSION *s);

/* Feed the browser's SDP answer. */
void web_rtc_session_set_answer(WEB_RTC_SESSION *s, const char *sdp);

/* Feed a remote ICE candidate from the browser (`mid` may be NULL). */
void web_rtc_session_add_candidate(WEB_RTC_SESSION *s, const char *cand, const char *mid);

/* The Opus track id to push RTP into (slice 3); -1 until offered. */
int web_rtc_session_track(const WEB_RTC_SESSION *s);

/* Hand one block of demodulated receive audio to the session's Opus encoder.
 * `stereo` is interleaved L/R at 48 kHz (the RX audio_output_buffer layout) and
 * `frames` the number of sample pairs; the left channel is taken as the mono
 * source.  This is the audio producer's only touch point: it converts and
 * enqueues into a bounded ring under a short lock and never encodes, allocates
 * or writes a socket (decision #6/#14).  A per-session worker thread drains the
 * ring, encodes 20 ms Opus frames and packetizes them into the track.  Safe to
 * call before the connection is up (the frames are consumed and dropped) and
 * when `s` is NULL (no-op). */
void web_rtc_session_feed(WEB_RTC_SESSION *s, const double *stereo, int frames);

/* Close and free the session. */
void web_rtc_session_free(WEB_RTC_SESSION *s);

#endif /* WEB_RTC_H */
