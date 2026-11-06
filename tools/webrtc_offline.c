/* webrtc_offline — prove the native WebRTC signaling/ICE/DTLS path AND the Opus
 * audio track end to end without a browser.
 *
 *   make WEBRTC_INCLUDE=WEBRTC webrtc-offline && ./webrtc_offline --selftest
 *
 * It drives the server side through the real web_rtc.c session API (offer + Opus
 * send/receive track + the audio feeds) and stands in for the browser with a second
 * libdatachannel peer connection (the answerer) built from the raw C API.  The
 * two peers' signaling is wired together in-process.  Two checks:
 *   1. BOTH peers reach the connected state over loopback (offer/answer, ICE
 *      trickle, DTLS) — everything the browser path needs except the browser.
 *   2. Synthetic 48 kHz audio fed in each direction at real time arrives
 *      on the answerer's track as RTP packets at ~50/s (one 20 ms Opus frame
 *      each) carrying the negotiated payload type 111.
 *   3. The spectrum DataChannel opens unordered and partially reliable, carries
 *      rows, refuses to queue behind a backlog and reports closed once its
 *      session is gone.
 *   4. Setting the audio rate changes what is on the wire.
 */
#include <glib.h>
#include <rtc/rtc.h>
#include <opus/opus.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "web_rtc.h"

#define RTC_OPUS_PT 111

/* The server session and the fake-browser peer are file-static so the
 * libdatachannel callbacks (which run on its worker threads) can reach each
 * other.  Connection flags are atomic: written from those threads, read here. */
static WEB_RTC_SESSION *g_offerer;
static int g_answerer = -1;
static int g_answerer_send_track = -1;
static volatile gint g_offerer_connected;
static volatile gint g_offerer_failed;
static volatile gint g_answerer_connected;
static volatile gint g_answerer_failed;
static volatile gint g_fixed_port_candidate;
static volatile gint g_advertised_candidate;

/* RTP counters, written from the answerer track's message callback. */
static volatile gint g_rtp_pt111;    /* RTP packets with the expected PT */
static volatile gint g_rtp_other;    /* RTP packets with any other PT    */
/* RTP header progression: a well-formed Opus stream advances the sequence
 * number by 1 and the timestamp by 960 (20 ms @ 48 kHz) each packet.  The track
 * message callback is serialized, so last_* need no lock; the mismatch count is
 * read on the main thread. */
static uint16_t g_last_seq;
static uint32_t g_last_ts;
static gboolean g_have_prev;
static volatile gint g_rtp_step_bad; /* seq or timestamp did not advance as expected */
static GMutex g_tx_audio_mutex;
static gint g_tx_audio_frames;
static double g_tx_audio_energy;
static volatile gint g_rtp_bytes;    /* RTP bytes on the track, for the rate check */

/* The spectrum channel: its id at the server, and what the fake browser saw. */
static volatile gint g_channel = -1;
static volatile gint g_dc_seen;
static volatile gint g_dc_unordered;
static volatile gint g_dc_lifetime;
static volatile gint g_dc_rows;

/* Server -> browser: the session hands us its offer and ICE candidates. */
static void offerer_signal(void *user, const char *kind, const char *data, const char *mid) {
  (void)user;
  if(g_strcmp0(kind, "offer") == 0) {
    rtcSetRemoteDescription(g_answerer, data, "offer");
    rtcSetLocalDescription(g_answerer, "answer");
  } else if(g_strcmp0(kind, "candidate") == 0) {
    if(data != NULL && strstr(data, " 49001 typ host") != NULL)
      g_atomic_int_set(&g_fixed_port_candidate, 1);
    if(data != NULL && strstr(data, " 127.0.0.1 49001 typ host") != NULL)
      g_atomic_int_set(&g_advertised_candidate, 1);
    rtcAddRemoteCandidate(g_answerer, data, mid);
  }
}

static void offerer_state(void *user, gboolean connected, gboolean failed) {
  (void)user;
  if(connected) g_atomic_int_set(&g_offerer_connected, 1);
  if(failed) g_atomic_int_set(&g_offerer_failed, 1);
}

/* Decoded fake-browser microphone audio delivered by the real server session. */
static void offerer_tx_audio(void *user, const float *mono, int frames) {
  (void)user;
  double energy=0.0;
  for(int i=0;i<frames;i++) energy+=(double)mono[i]*mono[i];
  g_mutex_lock(&g_tx_audio_mutex);
  g_tx_audio_frames+=frames;
  g_tx_audio_energy+=energy;
  g_mutex_unlock(&g_tx_audio_mutex);
}

static void offerer_channel(void *user, int channel) {
  (void)user;
  g_atomic_int_set(&g_channel, channel);
}

static void answerer_dc_message(int id, const char *message, int size, void *ptr) {
  (void)id; (void)message; (void)size; (void)ptr;
  g_atomic_int_inc(&g_dc_rows);
}

static void answerer_datachannel(int pc, int dc, void *ptr) {
  (void)pc; (void)ptr;
  char label[32] = "";
  rtcGetDataChannelLabel(dc, label, sizeof(label));
  rtcReliability reliability;
  memset(&reliability, 0, sizeof(reliability));
  rtcGetDataChannelReliability(dc, &reliability);
  if(g_strcmp0(label, "spectrum") == 0) g_atomic_int_set(&g_dc_seen, 1);
  g_atomic_int_set(&g_dc_unordered, reliability.unordered && reliability.unreliable);
  g_atomic_int_set(&g_dc_lifetime, (gint)reliability.maxPacketLifeTime);
  rtcSetMessageCallback(dc, answerer_dc_message);
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

/* Media arriving on the answerer's track.  RTP and RTCP are multiplexed on the
 * one path, so demux first (RFC 5761): byte 1 in 192..223 is an RTCP packet
 * type (the SR reporter's sender reports land here) and is not media.  What is
 * left is RTP; classify it by payload type (byte 1, low 7 bits). */
static void answerer_track_message(int id, const char *message, int size, void *ptr) {
  (void)id; (void)ptr;
  if(size < 12 || message == NULL) return;                 /* shorter than an RTP header */
  const uint8_t *p = (const uint8_t *)message;
  if((p[0] >> 6) != 2) return;                             /* not version 2 */
  if(p[1] >= 192 && p[1] <= 223) return;                   /* RTCP, not media */
  if((p[1] & 0x7f) != RTC_OPUS_PT) { g_atomic_int_inc(&g_rtp_other); return; }
  g_atomic_int_inc(&g_rtp_pt111);
  g_atomic_int_add(&g_rtp_bytes, size);

  uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
  uint32_t ts  = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                 ((uint32_t)p[6] << 8) | p[7];
  if(g_have_prev && (seq != (uint16_t)(g_last_seq + 1) || ts != g_last_ts + 960))
    g_atomic_int_inc(&g_rtp_step_bad);
  g_last_seq = seq;
  g_last_ts = ts;
  g_have_prev = TRUE;
}

/* Capture a separately exposed remote track when the implementation creates
 * one; sendrecv peers commonly reuse the local negotiated m-line instead. */
static void answerer_track(int pc, int tr, void *ptr) {
  (void)pc; (void)ptr;
  rtcSetMessageCallback(tr, answerer_track_message);
}

/* Feed `ms` of a tone at real time and report the RTP bytes that came out. */
static int feed_and_measure(double *phase, int ms) {
  double pcm[2 * 960];
  const double dphi = 2.0 * M_PI * 440.0 / 48000.0;
  g_usleep(100000);                       /* drain what the last setting sent */
  g_atomic_int_set(&g_rtp_bytes, 0);
  gint64 t0 = g_get_monotonic_time();
  for(int b = 0; b < ms / 20; b++) {
    for(int i = 0; i < 960; i++) {
      pcm[2*i] = pcm[2*i+1] = 0.25 * sin(*phase);
      *phase += dphi;
    }
    web_rtc_session_feed(g_offerer, pcm, 960);
    gint64 target = t0 + (gint64)(b + 1) * 20000;
    gint64 now = g_get_monotonic_time();
    if(target > now) g_usleep((gulong)(target - now));
  }
  return g_atomic_int_get(&g_rtp_bytes);
}

static int run_selftest(void) {
  web_rtc_init();
  g_mutex_init(&g_tx_audio_mutex);

  rtcConfiguration cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.disableAutoNegotiation = true;
  g_answerer = rtcCreatePeerConnection(&cfg);
  if(g_answerer < 0) { fprintf(stderr, "answerer create failed (%d)\n", g_answerer); return 1; }
  rtcSetLocalDescriptionCallback(g_answerer, answerer_description);
  rtcSetLocalCandidateCallback(g_answerer, answerer_candidate);
  rtcSetStateChangeCallback(g_answerer, answerer_state);
  rtcSetTrackCallback(g_answerer, answerer_track);
  rtcSetDataChannelCallback(g_answerer, answerer_datachannel);

  /* Match the browser's addTrack(microphone) before it answers the server's
     sendrecv offer, and packetize outgoing Opus on the same audio m-line. */
  rtcTrackInit track_init;
  memset(&track_init,0,sizeof(track_init));
  track_init.direction=RTC_DIRECTION_SENDRECV;
  track_init.codec=RTC_CODEC_OPUS;
  track_init.payloadType=RTC_OPUS_PT;
  track_init.ssrc=43;
  track_init.mid="audio";
  track_init.name="fake-browser";
  g_answerer_send_track=rtcAddTrackEx(g_answerer,&track_init);
  if(g_answerer_send_track<0) { fprintf(stderr,"answerer track create failed (%d)\n",g_answerer_send_track); return 1; }
  rtcPacketizerInit packetizer;
  memset(&packetizer,0,sizeof(packetizer));
  packetizer.ssrc=43; packetizer.cname="fake-browser";
  packetizer.payloadType=RTC_OPUS_PT; packetizer.clockRate=48000;
  if(rtcSetOpusPacketizer(g_answerer_send_track,&packetizer)<0) {
    fprintf(stderr,"answerer packetizer create failed\n"); return 1;
  }
  rtcChainRtcpSrReporter(g_answerer_send_track);
  rtcChainRtcpReceivingSession(g_answerer_send_track);
  rtcSetMessageCallback(g_answerer_send_track,answerer_track_message);

  g_offerer = web_rtc_session_new(offerer_signal, offerer_state, offerer_tx_audio,
                                  offerer_channel, NULL, "127.0.0.1", 49001);
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
  int connect_ok = offerer_ok && answerer_ok;
  int fixed_port_ok = g_atomic_int_get(&g_fixed_port_candidate);

  /* With the pipe up, feed real-time audio and watch the RTP come out the far
     end.  One 20 ms block (960 samples @ 48 kHz) every 20 ms -> one Opus frame
     -> one RTP packet, i.e. ~50/s.  A 440 Hz tone keeps Opus out of DTX so
     every frame carries payload. */
  const int blocks = 75;                /* ~1.5 s */
  double pcm[2 * 960];
  double phase = 0.0;
  double tx_phase = 0.0;
  const double dphi = 2.0 * M_PI * 440.0 / 48000.0;
  const double tx_dphi = 2.0 * M_PI * 660.0 / 48000.0;
  int opus_error=0;
  OpusEncoder *browser_encoder=opus_encoder_create(48000,1,OPUS_APPLICATION_AUDIO,&opus_error);
  gint64 t0 = 0, t1 = 0;
  if(connect_ok) {
    t0 = g_get_monotonic_time();
    for(int b = 0; b < blocks; b++) {
      for(int i = 0; i < 960; i++) {
        double v = 0.25 * sin(phase);
        phase += dphi;
        pcm[2*i] = v;        /* left = mono source */
        pcm[2*i+1] = v;
      }
      web_rtc_session_feed(g_offerer, pcm, 960);
      float tx_pcm[960]; unsigned char tx_opus[4000];
      for(int i=0;i<960;i++) {
        tx_pcm[i]=(float)(0.2*sin(tx_phase));
        tx_phase+=tx_dphi;
      }
      int encoded=browser_encoder==NULL ? -1 :
                  opus_encode_float(browser_encoder,tx_pcm,960,tx_opus,sizeof(tx_opus));
      if(encoded>0) {
        rtcSetTrackRtpTimestamp(g_answerer_send_track,(uint32_t)b*960);
        rtcSendMessage(g_answerer_send_track,(const char *)tx_opus,encoded);
      }
      /* Pace against an absolute 20 ms grid: g_usleep() oversleeps enough on a
         loaded machine to skew a per-iteration delay, which would understate
         the stream's true rate. */
      gint64 target = t0 + (gint64)(b + 1) * 20000;
      gint64 now = g_get_monotonic_time();
      if(target > now) g_usleep((gulong)(target - now));
    }
    t1 = g_get_monotonic_time();  /* feed window closes here; measure rate over it */
    g_usleep(300000);             /* let the last in-flight packets arrive */
  }
  int pt111 = g_atomic_int_get(&g_rtp_pt111);
  int ptother = g_atomic_int_get(&g_rtp_other);
  int stepbad = g_atomic_int_get(&g_rtp_step_bad);
  double secs = (t1 > t0) ? (double)(t1 - t0) / 1e6 : 0.0;
  double rate = (secs > 0.0) ? pt111 / secs : 0.0;
  if(browser_encoder!=NULL) opus_encoder_destroy(browser_encoder);
  g_mutex_lock(&g_tx_audio_mutex);
  int tx_frames=g_tx_audio_frames;
  double tx_rms=tx_frames>0 ? sqrt(g_tx_audio_energy/tx_frames) : 0.0;
  g_mutex_unlock(&g_tx_audio_mutex);

  /* The spectrum channel: open, the right reliability, rows through, no backlog. */
  int rows_sent = 0, burst_congested = 0, burst_closed = 0;
  int channel = -1;
  if(connect_ok) {
    for(int i = 0; i < 100 && (channel = g_atomic_int_get(&g_channel)) < 0; i++) g_usleep(50000);
    if(channel >= 0) {
      static char row[2084];
      for(int i = 0; i < 50; i++) {
        if(web_rtc_channel_send(channel, row, sizeof(row)) == WEB_RTC_SENT) rows_sent++;
        g_usleep(5000);
      }
      g_usleep(300000);
      /* Far faster than anything can leave: it must refuse, not queue. */
      static char big[60000];
      for(int i = 0; i < 400; i++) {
        WEB_RTC_SEND_RESULT result = web_rtc_channel_send(channel, big, sizeof(big));
        if(result == WEB_RTC_CONGESTED) burst_congested++;
        else if(result == WEB_RTC_CLOSED) burst_closed++;
      }
    }
  }
  int dc_rows = g_atomic_int_get(&g_dc_rows);

  /* The rate setting reaches the wire. */
  int bytes_high = 0, bytes_low = 0;
  if(connect_ok) {
    web_rtc_session_set_audio_rate(g_offerer, 64000, 2);
    bytes_high = feed_and_measure(&phase, 1000);
    web_rtc_session_set_audio_rate(g_offerer, 16000, 2);
    bytes_low = feed_and_measure(&phase, 1000);
  }

  web_rtc_session_free(g_offerer);
  g_offerer = NULL;
  gboolean closed_after_free = channel < 0 ||
      web_rtc_channel_send(channel, "x", 1) == WEB_RTC_CLOSED;
  if(g_answerer >= 0) { rtcDeletePeerConnection(g_answerer); g_answerer = -1; }
  web_rtc_cleanup();
  g_mutex_clear(&g_tx_audio_mutex);

  int failures = 0;

  printf("offer/answer/ICE/DTLS loopback connect             %s\n",
         connect_ok ? "PASS" : "FAIL");
  if(!connect_ok) {
    fprintf(stderr, "server connected=%d browser connected=%d (waited %d ms)\n",
            offerer_ok, answerer_ok, waited);
    failures++;
  }
  printf("offerer host candidate uses fixed UDP port 49001   %s\n",
         fixed_port_ok ? "PASS" : "FAIL");
  if(!fixed_port_ok) failures++;
  int advertised_ok=g_atomic_int_get(&g_advertised_candidate);
  printf("page address is advertised on the audio UDP port   %s\n",
         advertised_ok ? "PASS" : "FAIL");
  if(!advertised_ok) failures++;

  /* Expect ~75 packets over ~1.5 s; allow DTLS warm-up loss and timer jitter but
     insist the stream is clearly ~50/s, every packet is Opus (PT 111) and the
     RTP sequence/timestamp advance by exactly 1 / 960 per packet. */
  gboolean audio_ok = connect_ok && ptother == 0 && stepbad == 0 &&
                      pt111 >= blocks - 10 && rate >= 40.0 && rate <= 55.0;
  printf("Opus RTP on the track at ~50/s (PT %d)             %s\n",
         RTC_OPUS_PT, audio_ok ? "PASS" : (connect_ok ? "FAIL" : "SKIP"));
  if(connect_ok) {
    printf("    %d packets PT %d, %d other, %d bad seq/ts steps, %.1f/s over %.2f s\n",
           pt111, RTC_OPUS_PT, ptother, stepbad, rate, secs);
    if(!audio_ok) failures++;
  }

  gboolean tx_audio_ok=connect_ok && opus_error==OPUS_OK &&
                       tx_frames>=960*(blocks-10) && tx_rms>0.05 && tx_rms<0.3;
  printf("browser Opus decodes to 48 kHz TX audio             %s\n",
         tx_audio_ok ? "PASS" : (connect_ok ? "FAIL" : "SKIP"));
  if(connect_ok) {
    printf("    %d decoded samples, RMS %.3f\n",tx_frames,tx_rms);
    if(!tx_audio_ok) failures++;
  }

  gboolean dc_ok = connect_ok && channel >= 0 && g_atomic_int_get(&g_dc_seen) &&
                   g_atomic_int_get(&g_dc_unordered) && g_atomic_int_get(&g_dc_lifetime) == 250;
  printf("spectrum channel opens unordered, 250 ms lifetime   %s\n",
         dc_ok ? "PASS" : (connect_ok ? "FAIL" : "SKIP"));
  if(connect_ok && !dc_ok) failures++;
  gboolean rows_ok = dc_ok && rows_sent >= 45 && dc_rows >= rows_sent;
  printf("spectrum rows cross the channel                     %s\n",
         rows_ok ? "PASS" : (connect_ok ? "FAIL" : "SKIP"));
  if(connect_ok) {
    printf("    %d rows accepted, %d arrived\n", rows_sent, dc_rows);
    if(!rows_ok) failures++;
  }
  gboolean refuse_ok = dc_ok && burst_congested > 0 && burst_closed == 0;
  printf("a burst is refused rather than queued               %s\n",
         refuse_ok ? "PASS" : (connect_ok ? "FAIL" : "SKIP"));
  if(connect_ok) {
    printf("    %d of 400 refused, %d closed\n", burst_congested, burst_closed);
    if(!refuse_ok) failures++;
  }
  printf("a freed session's channel reports closed            %s\n",
         closed_after_free ? "PASS" : "FAIL");
  if(!closed_after_free) failures++;

  /* Payload scales with the bitrate; 64 k against 16 k must clearly differ. */
  gboolean rate_ok = connect_ok && bytes_high > 0 && bytes_low > 0 && bytes_low * 2 < bytes_high;
  printf("the audio rate setting changes the wire             %s\n",
         rate_ok ? "PASS" : (connect_ok ? "FAIL" : "SKIP"));
  if(connect_ok) {
    printf("    %d bytes/s at 64 kbit/s, %d bytes/s at 16 kbit/s\n", bytes_high, bytes_low);
    if(!rate_ok) failures++;
  }

  printf("\n%d failures\n", failures);
  return failures ? 1 : 0;
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
