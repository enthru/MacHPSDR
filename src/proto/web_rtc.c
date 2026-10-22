/* WebRTC transport for the browser remote's receive audio (libdatachannel). */
#include "web_rtc.h"

#include <rtc/rtc.h>
#include <string.h>

#include "log.h"

/* Opus/RTP constants for the one send-only audio track.  The payload type is in
 * the dynamic range; the SSRC is arbitrary but fixed per session so the RTP
 * packetizer (slice 3) and the SDP agree. */
#define WEB_RTC_OPUS_PT   111
#define WEB_RTC_OPUS_SSRC 0x4d485344u /* 'MHSD' */
#define WEB_RTC_AUDIO_MID "audio"

static gboolean web_rtc_inited;

static void web_rtc_log_cb(rtcLogLevel level, const char *message) {
  (void)level;
  log_debug("webrtc: %s\n", message != NULL ? message : "");
}

void web_rtc_init(void) {
  if(web_rtc_inited) return;
  /* Route libdatachannel's own diagnostics through our log at WARNING and above;
     the guard in log_debug() keeps the callback cheap when debug is off. */
  rtcInitLogger(RTC_LOG_WARNING, web_rtc_log_cb);
  web_rtc_inited = TRUE;
}

void web_rtc_cleanup(void) {
  if(!web_rtc_inited) return;
  rtcCleanup();
  web_rtc_inited = FALSE;
}

gboolean web_rtc_selftest(void) {
  web_rtc_init();
  rtcConfiguration cfg;
  memset(&cfg, 0, sizeof(cfg));
  int pc = rtcCreatePeerConnection(&cfg);
  if(pc < 0) {
    log_error("webrtc: rtcCreatePeerConnection failed (%d)\n", pc);
    return FALSE;
  }
  rtcDeletePeerConnection(pc);
  return TRUE;
}

struct WEB_RTC_SESSION {
  int pc;
  int track;
  web_rtc_signal_cb signal;
  web_rtc_state_cb state;
  void *user;
};

/* --- libdatachannel callbacks (run on its worker threads) ----------------- */

static void on_local_description(int pc, const char *sdp, const char *type, void *ptr) {
  (void)pc;
  WEB_RTC_SESSION *s = (WEB_RTC_SESSION *)ptr;
  if(s != NULL && s->signal != NULL) s->signal(s->user, type, sdp, NULL);
}

static void on_local_candidate(int pc, const char *cand, const char *mid, void *ptr) {
  (void)pc;
  WEB_RTC_SESSION *s = (WEB_RTC_SESSION *)ptr;
  if(s != NULL && s->signal != NULL) s->signal(s->user, "candidate", cand, mid);
}

static void on_state_change(int pc, rtcState state, void *ptr) {
  (void)pc;
  WEB_RTC_SESSION *s = (WEB_RTC_SESSION *)ptr;
  if(s == NULL || s->state == NULL) return;
  if(state == RTC_CONNECTED) s->state(s->user, TRUE, FALSE);
  else if(state == RTC_FAILED) s->state(s->user, FALSE, TRUE);
  else if(state == RTC_DISCONNECTED || state == RTC_CLOSED) s->state(s->user, FALSE, FALSE);
}

/* --- session ------------------------------------------------------------- */

WEB_RTC_SESSION *web_rtc_session_new(web_rtc_signal_cb signal,
                                     web_rtc_state_cb state, void *user) {
  web_rtc_init();
  WEB_RTC_SESSION *s = g_new0(WEB_RTC_SESSION, 1);
  s->track = -1;
  s->signal = signal;
  s->state = state;
  s->user = user;

  rtcConfiguration cfg;
  memset(&cfg, 0, sizeof(cfg));
  /* Drive negotiation explicitly (offer on demand) rather than letting the
     stack renegotiate behind us. */
  cfg.disableAutoNegotiation = true;
  s->pc = rtcCreatePeerConnection(&cfg);
  if(s->pc < 0) {
    log_error("webrtc: rtcCreatePeerConnection failed (%d)\n", s->pc);
    g_free(s);
    return NULL;
  }
  rtcSetUserPointer(s->pc, s);
  rtcSetLocalDescriptionCallback(s->pc, on_local_description);
  rtcSetLocalCandidateCallback(s->pc, on_local_candidate);
  rtcSetStateChangeCallback(s->pc, on_state_change);
  return s;
}

gboolean web_rtc_session_offer(WEB_RTC_SESSION *s) {
  if(s == NULL || s->pc < 0) return FALSE;

  rtcTrackInit init;
  memset(&init, 0, sizeof(init));
  init.direction = RTC_DIRECTION_SENDONLY; /* server sends, browser receives */
  init.codec = RTC_CODEC_OPUS;
  init.payloadType = WEB_RTC_OPUS_PT;
  init.ssrc = WEB_RTC_OPUS_SSRC;
  init.mid = WEB_RTC_AUDIO_MID;
  init.name = "machpsdr";
  int tr = rtcAddTrackEx(s->pc, &init);
  if(tr < 0) {
    log_error("webrtc: rtcAddTrackEx failed (%d)\n", tr);
    return FALSE;
  }
  s->track = tr;

  int rc = rtcSetLocalDescription(s->pc, "offer");
  if(rc < 0) {
    log_error("webrtc: rtcSetLocalDescription(offer) failed (%d)\n", rc);
    return FALSE;
  }
  return TRUE;
}

void web_rtc_session_set_answer(WEB_RTC_SESSION *s, const char *sdp) {
  if(s == NULL || s->pc < 0 || sdp == NULL) return;
  int rc = rtcSetRemoteDescription(s->pc, sdp, "answer");
  if(rc < 0) log_error("webrtc: rtcSetRemoteDescription(answer) failed (%d)\n", rc);
}

void web_rtc_session_add_candidate(WEB_RTC_SESSION *s, const char *cand, const char *mid) {
  if(s == NULL || s->pc < 0 || cand == NULL) return;
  rtcAddRemoteCandidate(s->pc, cand, mid); /* mid may be NULL */
}

int web_rtc_session_track(const WEB_RTC_SESSION *s) {
  return s != NULL ? s->track : -1;
}

void web_rtc_session_free(WEB_RTC_SESSION *s) {
  if(s == NULL) return;
  if(s->pc >= 0) {
    rtcSetUserPointer(s->pc, NULL);
    rtcDeletePeerConnection(s->pc); /* closes and releases the track */
  }
  g_free(s);
}
