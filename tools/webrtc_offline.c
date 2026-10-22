/* webrtc_offline — prove the native WebRTC signaling/ICE/DTLS path end to end
 * without a browser.
 *
 *   make WEBRTC_INCLUDE=WEBRTC webrtc-offline && ./webrtc_offline --selftest
 *
 * It drives the server side through the real web_rtc.c session API (offer + Opus
 * send-only track) and stands in for the browser with a second libdatachannel
 * peer connection (the answerer) built from the raw C API.  The two peers'
 * signaling is wired together in-process; the test passes when BOTH reach the
 * connected state over loopback, which exercises offer/answer exchange, ICE
 * candidate trickle, and the DTLS handshake — everything the browser path needs
 * except the browser itself.
 */
#include <glib.h>
#include <rtc/rtc.h>
#include <stdio.h>
#include <string.h>

#include "web_rtc.h"

/* The server session and the fake-browser peer are file-static so the
 * libdatachannel callbacks (which run on its worker threads) can reach each
 * other.  Connection flags are atomic: written from those threads, read here. */
static WEB_RTC_SESSION *g_offerer;
static int g_answerer = -1;
static volatile gint g_offerer_connected;
static volatile gint g_offerer_failed;
static volatile gint g_answerer_connected;
static volatile gint g_answerer_failed;

/* Server -> browser: the session hands us its offer and ICE candidates. */
static void offerer_signal(void *user, const char *kind, const char *data, const char *mid) {
  (void)user;
  if(g_strcmp0(kind, "offer") == 0) {
    rtcSetRemoteDescription(g_answerer, data, "offer");
    rtcSetLocalDescription(g_answerer, "answer");
  } else if(g_strcmp0(kind, "candidate") == 0) {
    rtcAddRemoteCandidate(g_answerer, data, mid);
  }
}

static void offerer_state(void *user, gboolean connected, gboolean failed) {
  (void)user;
  if(connected) g_atomic_int_set(&g_offerer_connected, 1);
  if(failed) g_atomic_int_set(&g_offerer_failed, 1);
}

/* Browser -> server: the raw peer hands us its answer and ICE candidates. */
static void answerer_description(int pc, const char *sdp, const char *type, void *ptr) {
  (void)pc; (void)ptr;
  if(g_strcmp0(type, "answer") == 0) web_rtc_session_set_answer(g_offerer, sdp);
}

static void answerer_candidate(int pc, const char *cand, const char *mid, void *ptr) {
  (void)pc; (void)ptr;
  web_rtc_session_add_candidate(g_offerer, cand, mid);
}

static void answerer_state(int pc, rtcState state, void *ptr) {
  (void)pc; (void)ptr;
  if(state == RTC_CONNECTED) g_atomic_int_set(&g_answerer_connected, 1);
  else if(state == RTC_FAILED) g_atomic_int_set(&g_answerer_failed, 1);
}

static int run_selftest(void) {
  web_rtc_init();

  rtcConfiguration cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.disableAutoNegotiation = true;
  g_answerer = rtcCreatePeerConnection(&cfg);
  if(g_answerer < 0) { fprintf(stderr, "answerer create failed (%d)\n", g_answerer); return 1; }
  rtcSetLocalDescriptionCallback(g_answerer, answerer_description);
  rtcSetLocalCandidateCallback(g_answerer, answerer_candidate);
  rtcSetStateChangeCallback(g_answerer, answerer_state);

  g_offerer = web_rtc_session_new(offerer_signal, offerer_state, NULL);
  if(g_offerer == NULL) { fprintf(stderr, "offerer session create failed\n"); return 1; }

  if(!web_rtc_session_offer(g_offerer)) { fprintf(stderr, "offer failed\n"); return 1; }
  if(web_rtc_session_track(g_offerer) < 0) { fprintf(stderr, "no audio track id\n"); return 1; }

  /* Wait for both peers to connect, or fail/timeout.  libdatachannel does the
     work on its own threads; this just polls the atomic flags. */
  const int timeout_ms = 15000, step_ms = 50;
  int waited = 0;
  while(waited < timeout_ms) {
    if(g_atomic_int_get(&g_offerer_connected) && g_atomic_int_get(&g_answerer_connected)) break;
    if(g_atomic_int_get(&g_offerer_failed) || g_atomic_int_get(&g_answerer_failed)) break;
    g_usleep(step_ms * 1000);
    waited += step_ms;
  }

  int offerer_ok = g_atomic_int_get(&g_offerer_connected);
  int answerer_ok = g_atomic_int_get(&g_answerer_connected);

  web_rtc_session_free(g_offerer);
  g_offerer = NULL;
  if(g_answerer >= 0) { rtcDeletePeerConnection(g_answerer); g_answerer = -1; }
  web_rtc_cleanup();

  printf("offer/answer/ICE/DTLS loopback connect             %s\n",
         (offerer_ok && answerer_ok) ? "PASS" : "FAIL");
  if(!(offerer_ok && answerer_ok)) {
    fprintf(stderr, "server connected=%d browser connected=%d (waited %d ms)\n",
            offerer_ok, answerer_ok, waited);
    return 1;
  }
  printf("\n0 failures\n");
  return 0;
}

int main(int argc, char **argv) {
  gboolean selftest = FALSE;
  for(int i = 1; i < argc; i++)
    if(g_strcmp0(argv[i], "--selftest") == 0) selftest = TRUE;
  if(!selftest) {
    fprintf(stderr, "usage: %s --selftest\n", argv[0]);
    return 2;
  }
  return run_selftest();
}
