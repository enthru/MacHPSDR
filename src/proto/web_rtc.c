/* WebRTC transport for the browser remote's receive audio (libdatachannel). */
#include "web_rtc.h"

#include <rtc/rtc.h>
#include <string.h>

#include "log.h"

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
