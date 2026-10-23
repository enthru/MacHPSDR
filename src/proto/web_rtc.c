/* WebRTC transport for the browser remote's receive audio (libdatachannel). */
#include "web_rtc.h"

#include <rtc/rtc.h>
#include <opus.h>
#include <string.h>

#include "log.h"

/* Opus/RTP constants for the one send-only audio track.  The payload type is in
 * the dynamic range; the SSRC is arbitrary but fixed per session so the RTP
 * packetizer and the SDP agree. */
#define WEB_RTC_OPUS_PT    111
#define WEB_RTC_OPUS_SSRC  0x4d485344u /* 'MHSD' */
#define WEB_RTC_AUDIO_MID  "audio"

/* Opus runs at 48 kHz mono; a 20 ms frame is exactly 960 samples and the RTP
 * clock advances by that per frame.  The ring holds 200 ms of audio: a slow or
 * stalled connection loses the oldest samples rather than letting the ring grow
 * and add latency to the producer (decision #6). */
#define WEB_RTC_SAMPLE_RATE 48000
#define WEB_RTC_FRAME       960                  /* 20 ms @ 48 kHz */
#define WEB_RTC_RING        (WEB_RTC_SAMPLE_RATE / 5) /* 200 ms */
#define WEB_RTC_MAX_PACKET  1500                 /* one Opus frame, with margin */
#define WEB_RTC_BITRATE     32000

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
  int rx_track;
  web_rtc_signal_cb signal;
  web_rtc_state_cb state;
  web_rtc_tx_audio_cb tx_audio;
  void *user;
  char advertised_address[64];
  guint16 udp_port;

  /* Audio feed: a bounded single-producer ring (the RX audio thread) drained by
     one encoder worker thread.  af_w/af_r are free-running sample counters; the
     live span af_w-af_r never exceeds WEB_RTC_RING. */
  OpusEncoder *enc;
  OpusDecoder *dec;
  float *ring;
  guint64 af_w, af_r;
  guint64 dropped;
  GMutex af_mutex;
  GCond af_cond;
  GThread *worker;
  volatile gint worker_run;
  volatile gint connected;
  uint32_t rtp_ts;
};

/* Locate an Opus payload in one RTP packet.  The browser may include CSRCs,
   header extensions or padding; none are part of the Opus packet handed to the
   decoder.  RTCP is multiplexed on the same callback and is rejected here. */
static gboolean web_rtc_rtp_payload(const guint8 *p, int size,
                                    const guint8 **payload, int *payload_len) {
  if(p==NULL || size<12 || (p[0]>>6)!=2) return FALSE;
  if(p[1]>=192 && p[1]<=223) return FALSE;
  if((p[1]&0x7f)!=WEB_RTC_OPUS_PT) return FALSE;
  int off=12+4*(p[0]&0x0f);
  if(off>size) return FALSE;
  if(p[0]&0x10) {
    if(off+4>size) return FALSE;
    int words=((int)p[off+2]<<8)|p[off+3];
    off+=4+4*words;
    if(off>size) return FALSE;
  }
  int end=size;
  if(p[0]&0x20) {
    int padding=p[size-1];
    if(padding<=0 || padding>end-off) return FALSE;
    end-=padding;
  }
  if(end<=off) return FALSE;
  *payload=p+off; *payload_len=end-off;
  return TRUE;
}

static void web_rtc_on_track_message(int id, const char *message, int size, void *ptr) {
  (void)id;
  WEB_RTC_SESSION *s=(WEB_RTC_SESSION *)ptr;
  if(s==NULL || s->dec==NULL || s->tx_audio==NULL) return;
  const guint8 *payload=NULL; int payload_len=0;
  if(!web_rtc_rtp_payload((const guint8 *)message,size,&payload,&payload_len)) return;
  float pcm[5760]; /* Opus maximum: 120 ms at 48 kHz. */
  int frames=opus_decode_float(s->dec,payload,payload_len,pcm,G_N_ELEMENTS(pcm),0);
  if(frames<0) {
    log_debug("webrtc: opus_decode_float failed (%d)\n",frames);
    return;
  }
  if(frames>0) s->tx_audio(s->user,pcm,frames);
}

static void web_rtc_on_track(int pc, int tr, void *ptr) {
  (void)pc;
  WEB_RTC_SESSION *s=(WEB_RTC_SESSION *)ptr;
  if(s==NULL) return;
  s->rx_track=tr;
  rtcSetUserPointer(tr,s);
  rtcChainRtcpReceivingSession(tr);
  rtcSetMessageCallback(tr,web_rtc_on_track_message);
}

/* --- Opus encoder worker (one per session) -------------------------------- */

static gpointer web_rtc_worker(gpointer data) {
  WEB_RTC_SESSION *s = (WEB_RTC_SESSION *)data;
  float frame[WEB_RTC_FRAME];
  unsigned char packet[WEB_RTC_MAX_PACKET];

  for(;;) {
    g_mutex_lock(&s->af_mutex);
    while(g_atomic_int_get(&s->worker_run) && (s->af_w - s->af_r) < WEB_RTC_FRAME)
      g_cond_wait(&s->af_cond, &s->af_mutex);
    if(!g_atomic_int_get(&s->worker_run)) { g_mutex_unlock(&s->af_mutex); break; }
    for(int i=0;i<WEB_RTC_FRAME;i++) {
      frame[i] = s->ring[s->af_r % WEB_RTC_RING];
      s->af_r++;
    }
    g_mutex_unlock(&s->af_mutex);

    /* Before the DTLS-SRTP connection is up there is nowhere to send: the frame
       was consumed above (so the ring cannot back up) and is simply dropped. */
    if(!g_atomic_int_get(&s->connected)) continue;

    int bytes = opus_encode_float(s->enc, frame, WEB_RTC_FRAME, packet, sizeof(packet));
    if(bytes < 0) { log_error("webrtc: opus_encode_float failed (%d)\n", bytes); continue; }
    if(bytes <= 2) continue; /* DTX / no payload: emit nothing */

    /* The packetizer reads rtpConfig->timestamp at send time; a plain
       rtcSendMessage carries no per-frame timing, so set it ourselves and
       advance one 20 ms frame.  set and send are serialized on this one
       thread, so consecutive frames get distinct, monotonic timestamps. */
    rtcSetTrackRtpTimestamp(s->track, s->rtp_ts);
    int rc = rtcSendMessage(s->track, (const char *)packet, bytes);
    if(rc < 0) log_debug("webrtc: rtcSendMessage failed (%d)\n", rc);
    s->rtp_ts += WEB_RTC_FRAME;
  }
  return NULL;
}

/* --- libdatachannel callbacks (run on its worker threads) ----------------- */

static void on_local_description(int pc, const char *sdp, const char *type, void *ptr) {
  (void)pc;
  WEB_RTC_SESSION *s = (WEB_RTC_SESSION *)ptr;
  if(s != NULL && s->signal != NULL) s->signal(s->user, type, sdp, NULL);
}

static char *web_rtc_advertised_candidate(const char *candidate,
                                          const char *address, guint16 port) {
  if(candidate==NULL || address==NULL || *address=='\0' || port==0) return NULL;
  gchar **fields=g_strsplit_set(candidate," \t",-1);
  int index[8], count=0;
  for(int i=0;fields[i]!=NULL && count<8;i++)
    if(*fields[i]!='\0') index[count++]=i;
  char *alias=NULL;
  if(count==8 && g_ascii_strcasecmp(fields[index[2]],"udp")==0 &&
     g_strcmp0(fields[index[6]],"typ")==0 &&
     g_ascii_strcasecmp(fields[index[7]],"host")==0) {
    g_free(fields[index[4]]);
    fields[index[4]]=g_strdup(address);
    g_free(fields[index[5]]);
    fields[index[5]]=g_strdup_printf("%u",port);
    alias=g_strjoinv(" ",fields);
  }
  g_strfreev(fields);
  return alias;
}

static void on_local_candidate(int pc, const char *cand, const char *mid, void *ptr) {
  (void)pc;
  WEB_RTC_SESSION *s = (WEB_RTC_SESSION *)ptr;
  if(s == NULL || s->signal == NULL) return;
  s->signal(s->user, "candidate", cand, mid);
  char *alias=web_rtc_advertised_candidate(cand,s->advertised_address,s->udp_port);
  if(alias!=NULL && g_strcmp0(alias,cand)!=0)
    s->signal(s->user,"candidate",alias,mid);
  g_free(alias);
}

static void on_state_change(int pc, rtcState state, void *ptr) {
  (void)pc;
  WEB_RTC_SESSION *s = (WEB_RTC_SESSION *)ptr;
  if(s == NULL) return;
  /* The worker gates sends on this flag; a plain store is enough (read once
     per 20 ms frame, no ordering dependency on other session state). */
  g_atomic_int_set(&s->connected, state == RTC_CONNECTED ? 1 : 0);
  if(s->state == NULL) return;
  if(state == RTC_CONNECTED) s->state(s->user, TRUE, FALSE);
  else if(state == RTC_FAILED) s->state(s->user, FALSE, TRUE);
  else if(state == RTC_DISCONNECTED || state == RTC_CLOSED) s->state(s->user, FALSE, FALSE);
}

/* --- session ------------------------------------------------------------- */

WEB_RTC_SESSION *web_rtc_session_new(web_rtc_signal_cb signal,
                                     web_rtc_state_cb state,
                                     web_rtc_tx_audio_cb tx_audio, void *user,
                                     const char *advertised_address,
                                     guint16 udp_port) {
  web_rtc_init();
  WEB_RTC_SESSION *s = g_new0(WEB_RTC_SESSION, 1);
  s->track = -1;
  s->rx_track = -1;
  s->signal = signal;
  s->state = state;
  s->tx_audio = tx_audio;
  s->user = user;
  if(advertised_address!=NULL)
    g_strlcpy(s->advertised_address,advertised_address,sizeof(s->advertised_address));
  s->udp_port=udp_port;
  g_mutex_init(&s->af_mutex);
  g_cond_init(&s->af_cond);

  rtcConfiguration cfg;
  memset(&cfg, 0, sizeof(cfg));
  if(udp_port>0) {
    cfg.enableIceUdpMux=true;
    cfg.portRangeBegin=udp_port;
    cfg.portRangeEnd=udp_port;
  }
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
  rtcSetTrackCallback(s->pc, web_rtc_on_track);
  return s;
}

gboolean web_rtc_session_offer(WEB_RTC_SESSION *s) {
  if(s == NULL || s->pc < 0) return FALSE;

  rtcTrackInit init;
  memset(&init, 0, sizeof(init));
  init.direction = RTC_DIRECTION_SENDRECV; /* RX audio out, browser mic in */
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
  /* A sendrecv answer normally reuses this negotiated m-line rather than
     creating a second remote track, so receive callbacks belong here too.
     web_rtc_on_track remains registered for peers that expose one separately. */
  rtcSetUserPointer(s->track,s);
  rtcChainRtcpReceivingSession(s->track);
  rtcSetMessageCallback(s->track,web_rtc_on_track_message);

  /* Attach the RTP packetizer so rtcSendMessage() on the track produces proper
     Opus/RTP (PT 111, 48 kHz clock, our fixed SSRC).  The SR reporter keeps the
     browser's RTCP sender-report clock fed. */
  rtcPacketizerInit pinit;
  memset(&pinit, 0, sizeof(pinit));
  pinit.ssrc = WEB_RTC_OPUS_SSRC;
  pinit.cname = "machpsdr";
  pinit.payloadType = WEB_RTC_OPUS_PT;
  pinit.clockRate = WEB_RTC_SAMPLE_RATE;
  int prc = rtcSetOpusPacketizer(s->track, &pinit);
  if(prc < 0) {
    log_error("webrtc: rtcSetOpusPacketizer failed (%d)\n", prc);
    return FALSE;
  }
  rtcChainRtcpSrReporter(s->track);

  /* The encoder and its drain worker exist for the life of the session; the
     worker blocks on the ring until the connection is up and audio is fed. */
  int oerr = 0;
  s->enc = opus_encoder_create(WEB_RTC_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &oerr);
  if(s->enc == NULL || oerr != OPUS_OK) {
    log_error("webrtc: opus_encoder_create failed (%d)\n", oerr);
    return FALSE;
  }
  opus_encoder_ctl(s->enc, OPUS_SET_BITRATE(WEB_RTC_BITRATE));
  /* Receive audio crosses the public Internet; in-band FEC lets the browser
     reconstruct a lost packet from the next one. */
  opus_encoder_ctl(s->enc, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(s->enc, OPUS_SET_PACKET_LOSS_PERC(10));
  s->dec = opus_decoder_create(WEB_RTC_SAMPLE_RATE,1,&oerr);
  if(s->dec==NULL || oerr!=OPUS_OK) {
    log_error("webrtc: opus_decoder_create failed (%d)\n",oerr);
    return FALSE;
  }
  s->ring = g_new0(float, WEB_RTC_RING);
  g_atomic_int_set(&s->worker_run, 1);
  s->worker = g_thread_new("web-opus", web_rtc_worker, s);

  int rc = rtcSetLocalDescription(s->pc, "offer");
  if(rc < 0) {
    log_error("webrtc: rtcSetLocalDescription(offer) failed (%d)\n", rc);
    return FALSE;
  }
  return TRUE;
}

void web_rtc_session_feed(WEB_RTC_SESSION *s, const double *stereo, int frames) {
  if(s == NULL || stereo == NULL || frames <= 0 || s->ring == NULL) return;
  g_mutex_lock(&s->af_mutex);
  for(int i=0;i<frames;i++) {
    s->ring[s->af_w % WEB_RTC_RING] = (float)stereo[2*i]; /* left = mono source */
    s->af_w++;
  }
  /* Keep only the newest ring's worth; an overrun means the connection is not
     draining, and the freshest audio is what the listener wants. */
  if(s->af_w - s->af_r > WEB_RTC_RING) {
    s->dropped += (s->af_w - s->af_r) - WEB_RTC_RING;
    s->af_r = s->af_w - WEB_RTC_RING;
  }
  g_cond_signal(&s->af_cond);
  g_mutex_unlock(&s->af_mutex);
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
  /* Stop the worker before anything it touches (track, encoder) goes away. */
  if(s->worker != NULL) {
    g_mutex_lock(&s->af_mutex);
    g_atomic_int_set(&s->worker_run, 0);
    g_cond_signal(&s->af_cond);
    g_mutex_unlock(&s->af_mutex);
    g_thread_join(s->worker);
    s->worker = NULL;
  }
  if(s->dropped > 0)
    log_info("webrtc: dropped %llu audio samples to a slow connection\n",
             (unsigned long long)s->dropped);
  if(s->pc >= 0) {
    if(s->track>=0) {
      rtcSetMessageCallback(s->track,NULL);
      rtcSetUserPointer(s->track,NULL);
    }
    if(s->rx_track>=0) {
      rtcSetMessageCallback(s->rx_track,NULL);
      rtcSetUserPointer(s->rx_track,NULL);
    }
    rtcSetUserPointer(s->pc, NULL);
    rtcDeletePeerConnection(s->pc); /* closes and releases the track */
  }
  if(s->enc != NULL) opus_encoder_destroy(s->enc);
  if(s->dec != NULL) opus_decoder_destroy(s->dec);
  g_free(s->ring);
  g_cond_clear(&s->af_cond);
  g_mutex_clear(&s->af_mutex);
  g_free(s);
}
