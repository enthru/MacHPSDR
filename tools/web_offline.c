/* Headless tests for the browser-remote protocol and HTTP allow-list. */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "web_protocol.h"

static int failures;

static void check(const char *name, gboolean ok) {
  printf("%-56s %s\n",name,ok?"PASS":"FAIL");
  if(!ok) failures++;
}

static gboolean parse(const char *text, WEB_REQUEST *request) {
  char error[128];
  return web_protocol_parse(text,strlen(text),request,error,sizeof(error));
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  WEB_REQUEST r;
  const char *asset, *type;
  gboolean ws;

  puts("web_offline: browser protocol self-test\n");
  check("state request parses",
        parse("{\"type\":\"get_state\",\"id\":1}",&r) &&
        r.type==WEB_REQUEST_GET_STATE && r.id==1);
  check("frequency command parses",
        parse("{\"type\":\"command\",\"id\":2,\"action\":\"set_frequency\",\"rx\":0,\"value\":14074000}",&r) &&
        r.type==WEB_REQUEST_SET_FREQUENCY && r.rx==0 && r.value==14074000);
  check("mode command parses independent of field order",
        parse(" { \"value\" : 7, \"rx\":1, \"action\":\"set_mode\",\"id\":3,\"type\":\"command\" } ",&r) &&
        r.type==WEB_REQUEST_SET_MODE && r.rx==1 && r.value==7);
  check("MOX requires and keeps a boolean",
        parse("{\"type\":\"command\",\"id\":4,\"action\":\"set_mox\",\"value\":true}",&r) &&
        r.type==WEB_REQUEST_SET_MOX && r.flag);
  check("Balanced spectrum subscription parses",
        parse("{\"type\":\"spectrum_subscribe\",\"id\":5,\"rx\":1,\"profile\":\"Balanced\"}",&r) &&
        r.type==WEB_REQUEST_SPECTRUM_SUBSCRIBE && r.rx==1 && r.profile==WEB_SPECTRUM_BALANCED);
  check("Ultra spectrum subscription parses (case-insensitive)",
        parse("{\"type\":\"spectrum_subscribe\",\"id\":5,\"rx\":0,\"profile\":\"ultra\"}",&r) &&
        r.type==WEB_REQUEST_SPECTRUM_SUBSCRIBE && r.profile==WEB_SPECTRUM_ULTRA);
  check("spectrum unsubscribe parses",
        parse("{\"type\":\"spectrum_unsubscribe\",\"id\":6,\"rx\":0}",&r) &&
        r.type==WEB_REQUEST_SPECTRUM_UNSUBSCRIBE);
  check("filter edges parse",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_filter\",\"rx\":0,\"low\":-3000,\"high\":2700}",&r) &&
        r.type==WEB_REQUEST_SET_FILTER && r.low==-3000 && r.high==2700);
  check("AF gain control parses to WEB_CTRL_AF_GAIN",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_af_gain\",\"rx\":0,\"value\":40}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_AF_GAIN && r.rx==0 && r.value==40);
  check("AGC mode control parses",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_agc\",\"rx\":1,\"value\":3}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_AGC && r.value==3);
  check("AGC gain accepts a negative value",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_agc_gain\",\"rx\":0,\"value\":-20}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_AGC_GAIN && r.value==-20);
  check("squelch control parses",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_squelch\",\"rx\":0,\"value\":55}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_SQUELCH && r.value==55);
  check("step control parses",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_step\",\"rx\":0,\"value\":1000}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_STEP && r.value==1000);
  check("NB control parses to WEB_CTRL_NB",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_nb\",\"rx\":0,\"value\":2}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_NB && r.value==2);
  check("NR control parses to WEB_CTRL_NR",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_nr\",\"rx\":0,\"value\":4}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_NR && r.value==4);
  check("ANF control parses to WEB_CTRL_ANF",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_anf\",\"rx\":0,\"value\":1}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_ANF && r.value==1);
  check("SNB control parses to WEB_CTRL_SNB",
        parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_snb\",\"rx\":0,\"value\":0}",&r) &&
        r.type==WEB_REQUEST_SET_CONTROL && r.control==WEB_CTRL_SNB && r.value==0);
  check("a noise control rejects a boolean value",
        !parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_nb\",\"rx\":0,\"value\":true}",&r));
  check("a noise control still requires a receiver index",
        !parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_snb\",\"value\":1}",&r));
  check("a control still requires a receiver index",
        !parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_af_gain\",\"value\":40}",&r));
  check("a control rejects a boolean value",
        !parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_agc\",\"rx\":0,\"value\":true}",&r));
  check("an unknown control action is rejected",
        !parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_everything\",\"rx\":0,\"value\":1}",&r));

  check("missing id is rejected",!parse("{\"type\":\"get_state\"}",&r));
  check("unknown command is rejected",
        !parse("{\"type\":\"command\",\"id\":5,\"action\":\"shell\",\"value\":1}",&r));
  check("MOX integer is rejected",
        !parse("{\"type\":\"command\",\"id\":6,\"action\":\"set_mox\",\"value\":1}",&r));
  check("numeric command boolean is rejected",
        !parse("{\"type\":\"command\",\"id\":7,\"action\":\"set_mode\",\"rx\":0,\"value\":true}",&r));
  check("duplicate command fields are rejected",
        !parse("{\"type\":\"get_state\",\"type\":\"command\",\"id\":8}",&r));
  check("recognized fields with wrong types are rejected",
        !parse("{\"type\":\"command\",\"id\":9,\"action\":\"set_mode\",\"rx\":\"0\",\"value\":7}",&r));
  check("trailing JSON is rejected",
        !parse("{\"type\":\"get_state\",\"id\":10}{}",&r));
  char huge[WEB_MAX_TEXT_MESSAGE+2]; memset(huge,'x',sizeof(huge)); huge[sizeof(huge)-1]='\0';
  check("oversized message is rejected",!web_protocol_parse(huge,sizeof(huge)-1,&r,NULL,0));

  check("root maps only to index.html",
        web_protocol_route("/",&asset,&type,&ws) && !ws && strcmp(asset,"index.html")==0);
  check("JavaScript has its explicit MIME type",
        web_protocol_route("/app.js",&asset,&type,&ws) && strstr(type,"javascript")!=NULL);
  check("WebSocket is a distinct route",
        web_protocol_route("/ws",&asset,&type,&ws) && ws && asset==NULL);
  check("unknown files are rejected",
        !web_protocol_route("/radio.props",&asset,&type,&ws));
  check("path traversal is rejected",
        !web_protocol_route("/../radio.props",&asset,&type,&ws));
  check("percent-encoded paths are rejected",
        !web_protocol_route("/%2e%2e/radio.props",&asset,&type,&ws));
  check("queries are not a second routing language",
        !web_protocol_route("/app.js?anything",&asset,&type,&ws));
  check("LAN WebSocket origin matches its request Host",
        web_origin_matches_host("http://192.168.1.25:8080","192.168.1.25:8080"));
  check("HTTPS reverse-proxy origin matches its request Host",
        web_origin_matches_host("https://radio.example","radio.example"));
  check("a cross-site WebSocket origin is rejected",
        !web_origin_matches_host("https://evil.example","192.168.1.25:8080"));
  check("an origin with a matching-host suffix is rejected",
        !web_origin_matches_host("http://192.168.1.25:8080.evil.example","192.168.1.25:8080"));

  char auth_hash[WEB_AUTH_HASH_HEX_SIZE];
  check("PBKDF2-HMAC-SHA256 matches the one-round test vector",
        web_auth_password_hash("password","salt",1,auth_hash) &&
        strcmp(auth_hash,"120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b")==0);
  check("HTTP Basic accepts the configured user and password",
        web_auth_check_basic("Basic bWFjaHBzZHI6cGFzc3dvcmQ=","machpsdr","salt",auth_hash,1));
  check("HTTP Basic rejects a wrong password",
        !web_auth_check_basic("Basic bWFjaHBzZHI6d3Jvbmc=","machpsdr","salt",auth_hash,1));
  check("HTTP Basic rejects a different username",
        !web_auth_check_basic("Basic b3RoZXI6cGFzc3dvcmQ=","machpsdr","salt",auth_hash,1));

  guint8 header[WEB_BINARY_HEADER_SIZE];
  WEB_BINARY_HEADER bh={WEB_PROTOCOL_VERSION,1,2,3,0x78563412u,
                        G_GUINT64_CONSTANT(0x1122334455667788),512,512,516}, decoded;
  check("MPSD header encodes exact little-endian fields",
        web_binary_header_encode(header,&bh) && memcmp(header,"MPSD",4)==0 &&
        header[8]==0x12 && header[11]==0x78 && header[12]==0x88 && header[19]==0x11 &&
        header[20]==0x00 && header[21]==0x02 && header[28]==0x04 && header[29]==0x02);
  check("MPSD header round-trips timestamp/count/size",
        web_binary_header_decode(header,sizeof(header),&decoded) &&
        decoded.sequence==bh.sequence && decoded.timestamp_us==bh.timestamp_us &&
        decoded.count==512 && decoded.rate_or_width==512 && decoded.payload_bytes==516);
  header[0]='X';
  check("bad MPSD magic is rejected",!web_binary_header_decode(header,sizeof(header),&decoded));

  WEB_SPECTRUM_LIMITS low=web_spectrum_profile_limits((WEB_SPECTRUM_PROFILE)-9);
  WEB_SPECTRUM_LIMITS high=web_spectrum_profile_limits((WEB_SPECTRUM_PROFILE)99);
  check("spectrum profiles clamp to 256/5 and 2048/30",
        low.bins==256 && low.fps==5 && high.bins==2048 && high.fps==30);
  check("Ultra profile is 2048 bins at 30 fps",
        web_spectrum_profile_limits(WEB_SPECTRUM_ULTRA).bins==2048 &&
        web_spectrum_profile_limits(WEB_SPECTRUM_ULTRA).fps==30);
  float bins[]={-120.0f,-80.0f,-110.0f,-70.0f,-130.0f,-90.0f,-100.0f,-60.0f};
  guint8 quantized[4];
  check("downsampling preserves each bucket peak",
        web_spectrum_quantize(bins,8,quantized,4,-140.0f,100.0f) &&
        quantized[0]==153 && quantized[1]==178 && quantized[2]==128 && quantized[3]==204);
  WEB_LATEST_FRAME latest={0};
  GBytes *one=g_bytes_new_static("one",3), *two=g_bytes_new_static("two",3);
  web_latest_frame_replace(&latest,one); web_latest_frame_replace(&latest,two);
  GBytes *taken=web_latest_frame_take(&latest);
  gsize taken_len=0; const char *taken_data=g_bytes_get_data(taken,&taken_len);
  check("latest-frame replacement drops the older frame",
        latest.dropped==1 && taken_len==3 && memcmp(taken_data,"two",3)==0 && latest.frame==NULL);
  g_bytes_unref(one); g_bytes_unref(two); g_bytes_unref(taken);

  /* WebRTC signaling messages. */
  check("audio subscribe parses with a receiver",
        parse("{\"type\":\"audio_subscribe\",\"id\":20,\"rx\":0}",&r) &&
        r.type==WEB_REQUEST_AUDIO_SUBSCRIBE && r.rx==0);
  web_request_clear(&r);
  check("audio unsubscribe parses",
        parse("{\"type\":\"audio_unsubscribe\",\"id\":21,\"rx\":1}",&r) &&
        r.type==WEB_REQUEST_AUDIO_UNSUBSCRIBE && r.rx==1);
  web_request_clear(&r);

  /* rtc_answer carries an escaped multi-line SDP that must decode to CRLF. */
  check("rtc_answer unescapes the SDP body",
        parse("{\"type\":\"rtc_answer\",\"id\":22,\"rx\":0,"
              "\"sdp\":\"v=0\\r\\no=- 1 2 IN IP4 0.0.0.0\\r\\n\"}",&r) &&
        r.type==WEB_REQUEST_RTC_ANSWER && r.sdp!=NULL &&
        strcmp(r.sdp,"v=0\r\no=- 1 2 IN IP4 0.0.0.0\r\n")==0);
  web_request_clear(&r);
  check("rtc_answer without sdp is rejected",
        !parse("{\"type\":\"rtc_answer\",\"id\":23,\"rx\":0}",&r));
  web_request_clear(&r);
  check("rtc_candidate carries candidate and mid",
        parse("{\"type\":\"rtc_candidate\",\"id\":24,\"rx\":0,"
              "\"candidate\":\"candidate:1 1 udp 2 127.0.0.1 5000 typ host\",\"mid\":\"audio\"}",&r) &&
        r.type==WEB_REQUEST_RTC_CANDIDATE && r.candidate!=NULL && r.mid!=NULL &&
        strcmp(r.mid,"audio")==0 &&
        strcmp(r.candidate,"candidate:1 1 udp 2 127.0.0.1 5000 typ host")==0);
  web_request_clear(&r);
  check("a bad escape in the SDP is rejected",
        !parse("{\"type\":\"rtc_answer\",\"id\":25,\"rx\":0,\"sdp\":\"v=0\\x\"}",&r));
  web_request_clear(&r);

  /* web_json_escape round-trips through the parser's unescaper. */
  gchar *escaped=web_json_escape("a\"b\\c\r\nd");
  check("web_json_escape escapes quote, backslash and CRLF",
        escaped!=NULL && strcmp(escaped,"a\\\"b\\\\c\\r\\nd")==0);
  gchar *frame=g_strdup_printf("{\"type\":\"rtc_answer\",\"id\":26,\"rx\":0,\"sdp\":\"%s\"}",escaped);
  check("escaped payload parses back to the original",
        parse(frame,&r) && r.sdp!=NULL && strcmp(r.sdp,"a\"b\\c\r\nd")==0);
  web_request_clear(&r);
  g_free(escaped); g_free(frame);

  printf("\n%d failure%s\n",failures,failures==1?"":"s");
  return failures?1:0;
}
