/* Embedded browser remote server. */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

typedef struct _radio RADIO;

#define WEB_DEFAULT_PORT 8080

void web_server_init(RADIO *radio);
void web_server_start(void);
void web_server_stop(void);
const char *web_server_status(void);
const char *web_server_tls_fingerprint(void);
const char *web_server_tls_bundle(void);

/* GTK display-timer seam: bounded copy only, never a socket operation. */
void web_server_publish_spectrum(int receiver, const float *bins, unsigned count,
                                 float calibration_db, float floor_db, float span_db);

/* RX-audio-thread seam: hand one 48 kHz interleaved-stereo block to any client
   subscribed to this receiver's WebRTC audio.  Bounded enqueue only; no-op
   without WebRTC or subscribers. */
void web_server_audio_tap(int receiver, const double *stereo, int frames);

#endif
