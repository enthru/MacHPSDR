/* WebRTC transport for the browser remote's receive audio.
 *
 * The browser remote carries control/signaling over its WebSocket; receive
 * audio is a DTLS-SRTP Opus track the browser decodes with its own jitter
 * buffer, so the server never ships I/Q or display-rate data over the Internet.
 * This unit wraps libdatachannel's C API (rtc/rtc.h).
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
 * initialises.  TRUE on success.  Used by the offline harness and as a startup
 * sanity check. */
gboolean web_rtc_selftest(void);

#endif /* WEB_RTC_H */
