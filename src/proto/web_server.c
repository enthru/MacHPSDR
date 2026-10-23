/* Embedded HTTP/WebSocket server for the browser remote. */
#include "net_compat.h"             /* first: winsock2 before GTK/windows.h */

#include <gtk/gtk.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "mode.h"
#include "resource_path.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "ext.h"
#include "tci_ws.h"
#include "web_protocol.h"
#include "web_server.h"
#ifdef WEBRTC
#include "web_rtc.h"
#endif

#define WEB_MAX_CLIENTS 8
#define WEB_HTTP_MAX 8192
#define WEB_SPECTRUM_MAX_AGE_US 500000
#define WEB_SLOW_SEND_US 100000
#define WEB_RECOVERY_US 10000000

typedef struct {
  guint8 receiver;
  guint8 reserved[3];
  guint32 sequence;
  guint64 timestamp_us;
  guint32 count;
  float floor_db;
  float span_db;
  float bins[];
} WEB_SNAPSHOT;

typedef struct {
  int fd;
  gint upgraded;
  gint closing;
  GMutex queue_mutex;
  GMutex send_mutex;
  GCond queue_cond;
  GThread *sender;
  WEB_LATEST_FRAME pending[MAX_RECEIVERS];
  gboolean subscribed[MAX_RECEIVERS];
  WEB_SPECTRUM_PROFILE desired[MAX_RECEIVERS];
  WEB_SPECTRUM_PROFILE effective[MAX_RECEIVERS];
  guint effective_bins[MAX_RECEIVERS];
  guint effective_fps[MAX_RECEIVERS];
  gint64 next_offer_us[MAX_RECEIVERS];
  gint64 healthy_since_us[MAX_RECEIVERS];
  guint pressure[MAX_RECEIVERS];
  guint64 expired[MAX_RECEIVERS];
  gint64 last_status_us[MAX_RECEIVERS];
#ifdef WEBRTC
  /* One receive-audio peer connection per client (slice 2b/3).  Created from
     the client's recv thread; its callbacks run on libdatachannel threads and
     send through the serialized client_send() path, never touching GTK. */
  WEB_RTC_SESSION *rtc;
  gint rtc_rx;
#endif
} WEB_CLIENT;

static RADIO *web_radio;
static volatile gint web_running;
static GThread *web_thread;
static int web_listen=-1;
static int web_port=WEB_DEFAULT_PORT;
static GMutex web_clients_mutex;
static GCond web_clients_cond;
static WEB_CLIENT web_clients[WEB_MAX_CLIENTS];
static guint web_client_workers;
static GMutex web_spectrum_mutex;
static GBytes *web_latest_spectrum[MAX_RECEIVERS];
static guint32 web_spectrum_sequence[MAX_RECEIVERS];
static gboolean web_sync_initialized;
static char web_status_line[128]="stopped";
static gint web_revision;

static const char *http_reason(int code) {
  switch(code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 426: return "Upgrade Required";
    case 500: return "Internal Server Error";
    default: return "Error";
  }
}

static gboolean http_send(int fd, int code, const char *type,
                          const void *body, size_t len, gboolean head) {
  char header[1024];
  int n=g_snprintf(header,sizeof(header),
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %zu\r\n"
      "Cache-Control: no-store\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Referrer-Policy: no-referrer\r\n"
      "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self' ws://127.0.0.1:* ws://localhost:*\r\n"
      "Connection: close\r\n\r\n",
      code,http_reason(code),type?type:"text/plain; charset=utf-8",len);
  if(n<=0 || (size_t)n>=sizeof(header) || !tci_ws_send_all(fd,header,(size_t)n)) return FALSE;
  return head || len==0 || tci_ws_send_all(fd,body,len);
}

static void http_error(int fd, int code, const char *message) {
  http_send(fd,code,"text/plain; charset=utf-8",message,strlen(message),FALSE);
}

static gchar *read_asset(const char *name, gsize *len) {
  char resource_name[128], resource_path[1200];
  g_snprintf(resource_name,sizeof(resource_name),"web/%s",name);
  if(machpsdr_resource_path(resource_name,resource_path,sizeof(resource_path))) {
    gchar *contents=NULL;
    if(g_file_get_contents(resource_path,&contents,len,NULL)) return contents;
  }
  static const char *roots[]={"assets/web","share/machpsdr/web","web",NULL};
  for(int i=0;roots[i]!=NULL;i++) {
    gchar *path=g_build_filename(roots[i],name,NULL);
    gchar *contents=NULL;
    if(g_file_get_contents(path,&contents,len,NULL)) {
      g_free(path);
      return contents;
    }
    g_free(path);
  }
  return NULL;
}

static const char *header_value(const char *request, const char *name,
                                char *out, size_t out_len) {
  size_t want=strlen(name);
  const char *p=strstr(request,"\r\n");
  if(p==NULL) return NULL;
  p+=2;
  while(*p!='\0' && !(p[0]=='\r' && p[1]=='\n')) {
    const char *end=strstr(p,"\r\n");
    if(end==NULL) break;
    const char *colon=memchr(p,':',(size_t)(end-p));
    if(colon!=NULL && (size_t)(colon-p)==want && g_ascii_strncasecmp(p,name,want)==0) {
      const char *v=colon+1;
      while(v<end && (*v==' ' || *v=='\t')) v++;
      size_t n=(size_t)(end-v);
      while(n>0 && (v[n-1]==' ' || v[n-1]=='\t')) n--;
      if(n>=out_len) return NULL;
      memcpy(out,v,n); out[n]='\0';
      return out;
    }
    p=end+2;
  }
  return NULL;
}

static gboolean origin_allowed(const char *request) {
  char origin[256];
  if(header_value(request,"Origin",origin,sizeof(origin))==NULL) return TRUE;
  char a[128], b[128];
  g_snprintf(a,sizeof(a),"http://127.0.0.1:%d",web_port);
  g_snprintf(b,sizeof(b),"http://localhost:%d",web_port);
  return strcmp(origin,a)==0 || strcmp(origin,b)==0;
}

static gboolean header_has_token(const char *value, const char *wanted) {
  size_t wanted_len=strlen(wanted);
  while(value!=NULL && *value!='\0') {
    while(*value==' ' || *value=='\t' || *value==',') value++;
    const char *end=strchr(value,',');
    if(end==NULL) end=value+strlen(value);
    const char *trim=end;
    while(trim>value && (trim[-1]==' ' || trim[-1]=='\t')) trim--;
    if((size_t)(trim-value)==wanted_len &&
       g_ascii_strncasecmp(value,wanted,wanted_len)==0) return TRUE;
    value=*end=='\0'?end:end+1;
  }
  return FALSE;
}

static RECEIVER *visible_rx(gint index) {
  if(web_radio==NULL || web_radio->discovered==NULL || index<0 ||
     index>=web_radio->discovered->supported_receivers) return NULL;
  RECEIVER *rx=web_radio->receiver[index];
  return rx!=NULL && rx->show_rx ? rx : NULL;
}

static gboolean client_send(WEB_CLIENT *client, int opcode, const void *payload, size_t len) {
  gboolean ok=FALSE;
  g_mutex_lock(&client->send_mutex);
  if(!g_atomic_int_get(&client->closing) && client->fd>=0)
    ok=tci_ws_send_blocking(client->fd,opcode,payload,len);
  g_mutex_unlock(&client->send_mutex);
  return ok;
}

static gboolean ws_send_text(WEB_CLIENT *client, const char *text) {
  return client_send(client,1,text,strlen(text));
}

static gboolean ws_send_error(WEB_CLIENT *client, gint64 id, const char *code, const char *message) {
  char line[512];
  // code/message are constants controlled by this file, never client text.
  g_snprintf(line,sizeof(line),
      "{\"type\":\"error\",\"id\":%lld,\"code\":\"%s\",\"message\":\"%s\"}",
      (long long)id,code,message);
  return ws_send_text(client,line);
}

static gboolean ws_send_state(WEB_CLIENT *client, gint64 id) {
  if(web_radio==NULL || web_radio->discovered==NULL)
    return ws_send_error(client,id,"radio_unavailable","radio is not ready");
  GString *s=g_string_sized_new(1024);
  int active=web_radio->active_receiver ? web_radio->active_receiver->channel : -1;
  int revision=g_atomic_int_add(&web_revision,1)+1;
  g_string_append_printf(s,
      "{\"type\":\"state\",\"id\":%lld,\"revision\":%d,"
      "\"radio\":{\"mox\":%s,\"can_transmit\":%s,\"active_rx\":%d},\"receivers\":[",
      (long long)id,revision,web_radio->mox?"true":"false",
      (web_radio->can_transmit && web_radio->web_tx_enable)?"true":"false",active);
  gboolean first=TRUE;
  for(int i=0;i<web_radio->discovered->supported_receivers;i++) {
    RECEIVER *rx=visible_rx(i);
    if(rx==NULL) continue;
    double meter=isfinite(rx->meter_db)?rx->meter_db:-200.0;
    // JSON is an ASCII interchange format: GTK runs under the operator's locale,
    // so "%.1f" on a comma-decimal machine (ru_RU) emits "-200,0" and the whole
    // state frame becomes invalid JSON the browser silently drops. Format the one
    // float locale-independently, like every other ASCII format this tree writes.
    char meter_text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(meter_text,sizeof(meter_text),"%.1f",meter);
    if(!first) g_string_append_c(s,',');
    first=FALSE;
    int mode=(rx->mode_a>=0 && rx->mode_a<MODES)?rx->mode_a:USB;
    g_string_append_printf(s,
        "{\"id\":%d,\"frequency\":%lld,\"frequency_b\":%lld,"
        "\"mode\":%d,\"mode_name\":\"%s\",\"meter_db\":%s,\"locked\":%s,"
        "\"step\":%lld,\"filter_low\":%d,\"filter_high\":%d,"
        "\"display_low\":%lld,\"display_high\":%lld}",
        i,(long long)receiver_tuned_frequency(rx),(long long)rx->frequency_b,
        mode,mode_string[mode],meter_text,rx->locked?"true":"false",(long long)rx->step,
        rx->filter_low_a,rx->filter_high_a,
        (long long)(rx->frequency_a-(rx->sample_rate/2)+(rx->pan*rx->hz_per_pixel)),
        (long long)(rx->frequency_a-(rx->sample_rate/2)+
                    ((rx->pan+rx->panadapter_width)*rx->hz_per_pixel)));
  }
  g_string_append(s,"]}");
  gboolean ok=client_send(client,1,s->str,s->len);
  g_string_free(s,TRUE);
  return ok;
}

#ifdef WEBRTC
/* WebRTC signaling out (runs on a libdatachannel worker thread): wrap the offer
   or ICE candidate as a text frame and push it through the serialized send
   path.  client_send() early-returns once the client is closing, so a callback
   that races teardown cannot touch a half-freed client. */
static void web_rtc_on_signal(void *user, const char *kind, const char *data,
                              const char *mid) {
  WEB_CLIENT *client=(WEB_CLIENT *)user;
  if(client==NULL || data==NULL) return;
  gchar *edata=web_json_escape(data);
  gchar *msg;
  if(g_strcmp0(kind,"candidate")==0) {
    gchar *emid=web_json_escape(mid!=NULL?mid:"");
    msg=g_strdup_printf("{\"type\":\"rtc_candidate\",\"rx\":%d,"
                        "\"candidate\":\"%s\",\"mid\":\"%s\"}",
                        client->rtc_rx,edata,emid);
    g_free(emid);
  } else {
    /* The server is always the offerer. */
    msg=g_strdup_printf("{\"type\":\"rtc_offer\",\"rx\":%d,\"sdp\":\"%s\"}",
                        client->rtc_rx,edata);
  }
  client_send(client,1,msg,strlen(msg));
  g_free(edata);
  g_free(msg);
}

static void web_rtc_on_state(void *user, gboolean connected, gboolean failed) {
  WEB_CLIENT *client=(WEB_CLIENT *)user;
  if(client==NULL) return;
  log_info("web: rtc rx %d %s\n",client->rtc_rx,
           connected?"connected":(failed?"failed":"closed"));
  char msg[96];
  g_snprintf(msg,sizeof(msg),
             "{\"type\":\"rtc_state\",\"rx\":%d,\"connected\":%s}",
             client->rtc_rx,connected?"true":"false");
  client_send(client,1,msg,strlen(msg));
}

/* Free the audio session.  The pointer is cleared under queue_mutex (the lock
   the RX-audio tap holds while feeding the session), so the tap never touches a
   session being freed; the actual free -- which joins libdatachannel and the
   encoder worker -- happens with NO lock held, both because those threads' own
   callbacks take send_mutex and so the join never stalls the audio thread. */
static void web_rtc_teardown(WEB_CLIENT *client) {
  g_mutex_lock(&client->queue_mutex);
  WEB_RTC_SESSION *s=client->rtc;
  client->rtc=NULL;
  g_mutex_unlock(&client->queue_mutex);
  if(s!=NULL) web_rtc_session_free(s);
}
#endif /* WEBRTC */

static gboolean dispatch_request(WEB_CLIENT *client, const WEB_REQUEST *request) {
  if(request->type==WEB_REQUEST_GET_STATE) return ws_send_state(client,request->id);

  if(request->type==WEB_REQUEST_SPECTRUM_SUBSCRIBE ||
     request->type==WEB_REQUEST_SPECTRUM_UNSUBSCRIBE) {
    RECEIVER *sub_rx=visible_rx(request->rx);
    if(sub_rx==NULL) return ws_send_error(client,request->id,"invalid_receiver","receiver is not available");
    g_mutex_lock(&client->queue_mutex);
    if(request->type==WEB_REQUEST_SPECTRUM_SUBSCRIBE) {
      client->subscribed[request->rx]=TRUE;
      client->desired[request->rx]=request->profile;
      client->effective[request->rx]=request->profile;
      WEB_SPECTRUM_LIMITS selected=web_spectrum_profile_limits(request->profile);
      client->effective_bins[request->rx]=selected.bins;
      client->effective_fps[request->rx]=selected.fps;
      client->pressure[request->rx]=0;
      client->healthy_since_us[request->rx]=g_get_monotonic_time();
    } else {
      client->subscribed[request->rx]=FALSE;
      web_latest_frame_clear(&client->pending[request->rx]);
    }
    guint64 dropped=client->pending[request->rx].dropped+client->expired[request->rx];
    WEB_SPECTRUM_PROFILE effective=client->effective[request->rx];
    guint bins=client->effective_bins[request->rx];
    guint fps=client->effective_fps[request->rx];
    g_mutex_unlock(&client->queue_mutex);
    char reply[256];
    g_snprintf(reply,sizeof(reply),
        "{\"type\":\"spectrum_subscription\",\"id\":%lld,\"rx\":%d,"
        "\"enabled\":%s,\"profile\":\"%s\",\"bins\":%u,\"fps\":%u,\"dropped\":%llu}",
        (long long)request->id,request->rx,
        request->type==WEB_REQUEST_SPECTRUM_SUBSCRIBE?"true":"false",
        web_spectrum_profile_name(effective),bins,fps,
        (unsigned long long)dropped);
    return ws_send_text(client,reply);
  }

  if(request->type==WEB_REQUEST_AUDIO_SUBSCRIBE ||
     request->type==WEB_REQUEST_AUDIO_UNSUBSCRIBE ||
     request->type==WEB_REQUEST_RTC_ANSWER ||
     request->type==WEB_REQUEST_RTC_CANDIDATE) {
#ifdef WEBRTC
    if(visible_rx(request->rx)==NULL)
      return ws_send_error(client,request->id,"invalid_receiver","receiver is not available");
    switch(request->type) {
      case WEB_REQUEST_AUDIO_SUBSCRIBE: {
        web_rtc_teardown(client);            /* replace any existing session */
        /* rtc_rx must be set before the offer: web_rtc_session_offer() can fire
           the signal callback synchronously, and that callback tags the SDP
           with rtc_rx.  Build and offer the session with no client lock held
           (gathering starts here), then publish the pointer under queue_mutex
           so the audio tap can begin feeding it. */
        g_mutex_lock(&client->queue_mutex);
        client->rtc_rx=request->rx;
        g_mutex_unlock(&client->queue_mutex);
        WEB_RTC_SESSION *s=web_rtc_session_new(web_rtc_on_signal,web_rtc_on_state,client);
        if(s==NULL || !web_rtc_session_offer(s)) {
          if(s!=NULL) web_rtc_session_free(s);
          return ws_send_error(client,request->id,"rtc_failed","could not start audio");
        }
        g_mutex_lock(&client->queue_mutex);
        client->rtc=s;
        g_mutex_unlock(&client->queue_mutex);
        break;
      }
      case WEB_REQUEST_AUDIO_UNSUBSCRIBE:
        web_rtc_teardown(client);
        break;
      case WEB_REQUEST_RTC_ANSWER:
        if(client->rtc==NULL)
          return ws_send_error(client,request->id,"no_session","no audio session");
        web_rtc_session_set_answer(client->rtc,request->sdp);
        break;
      case WEB_REQUEST_RTC_CANDIDATE:
        if(client->rtc!=NULL)
          web_rtc_session_add_candidate(client->rtc,request->candidate,request->mid);
        break;
      default: break;
    }
    char ack[96];
    g_snprintf(ack,sizeof(ack),"{\"type\":\"ack\",\"id\":%lld}",(long long)request->id);
    return ws_send_text(client,ack);
#else
    return ws_send_error(client,request->id,"webrtc_unavailable",
                         "this build has no WebRTC audio");
#endif
  }

  RECEIVER *rx=NULL;
  if(request->type!=WEB_REQUEST_SET_MOX) {
    rx=visible_rx(request->rx);
    if(rx==NULL) return ws_send_error(client,request->id,"invalid_receiver","receiver is not available");
  }
  switch(request->type) {
    case WEB_REQUEST_SET_FREQUENCY: {
      long long maxf=receiver_max_frequency(rx);
      if(request->value<=0 || request->value>maxf)
        return ws_send_error(client,request->id,"invalid_value","frequency is out of range");
      RX_FREQUENCY *f=g_new(RX_FREQUENCY,1);
      f->rx=rx; f->frequency=request->value;
      g_idle_add(ext_set_frequency_a,f);
      break;
    }
    case WEB_REQUEST_SET_MODE: {
      if(request->value<0 || request->value>=MODES)
        return ws_send_error(client,request->id,"invalid_value","mode is out of range");
      MODE *m=g_new0(MODE,1);
      m->rx=rx; m->mode_a=(int)request->value;
      g_idle_add(ext_set_mode,m);
      break;
    }
    case WEB_REQUEST_SET_MOX: {
      if(web_radio==NULL || !web_radio->can_transmit || !web_radio->web_tx_enable)
        return ws_send_error(client,request->id,"not_armed","browser PTT is not armed in Configure");
      MOX_STATE *m=g_new(MOX_STATE,1);
      m->radio=web_radio; m->state=request->flag;
      g_idle_add(ext_set_mox,m);
      break;
    }
    case WEB_REQUEST_SET_FILTER: {
      if(request->low < -1000000 || request->high > 1000000)
        return ws_send_error(client,request->id,"invalid_value","filter edge is out of range");
      RX_FILTER_EDGES *f=g_new(RX_FILTER_EDGES,1);
      f->rx=rx; f->low=request->low; f->high=request->high;
      g_idle_add(ext_set_filter_edges,f);
      break;
    }
    default:
      return ws_send_error(client,request->id,"invalid_request","unsupported request");
  }
  char ack[96];
  g_snprintf(ack,sizeof(ack),"{\"type\":\"ack\",\"id\":%lld}",(long long)request->id);
  return ws_send_text(client,ack);
}

static gboolean client_has_pending(WEB_CLIENT *client) {
  for(int i=0;i<MAX_RECEIVERS;i++) if(client->pending[i].frame!=NULL) return TRUE;
  return FALSE;
}

static void spectrum_status(WEB_CLIENT *client, int rx_index) {
  char line[256];
  g_mutex_lock(&client->queue_mutex);
  WEB_SPECTRUM_PROFILE profile=client->effective[rx_index];
  guint bins=client->effective_bins[rx_index];
  guint fps=client->effective_fps[rx_index];
  guint64 dropped=client->pending[rx_index].dropped+client->expired[rx_index];
  g_mutex_unlock(&client->queue_mutex);
  g_snprintf(line,sizeof(line),
      "{\"type\":\"spectrum_status\",\"rx\":%d,\"profile\":\"%s\","
      "\"bins\":%u,\"fps\":%u,\"dropped\":%llu}",
      rx_index,web_spectrum_profile_name(profile),bins,fps,
      (unsigned long long)dropped);
  ws_send_text(client,line);
}

static GBytes *spectrum_binary_frame(const WEB_SNAPSHOT *snapshot,
                                     WEB_SPECTRUM_PROFILE profile, guint requested_bins) {
  guint count=MIN(snapshot->count,requested_bins);
  if(count==0) return NULL;
  gsize total=WEB_BINARY_HEADER_SIZE+4+count;
  guint8 *frame=g_malloc(total);
  WEB_BINARY_HEADER header={WEB_PROTOCOL_VERSION,1,snapshot->receiver,(guint8)profile,
                            snapshot->sequence,snapshot->timestamp_us,count,count,4+count};
  if(!web_binary_header_encode(frame,&header) ||
     !web_spectrum_quantize(snapshot->bins,snapshot->count,frame+WEB_BINARY_HEADER_SIZE+4,
                            count,snapshot->floor_db,snapshot->span_db)) {
    g_free(frame); return NULL;
  }
  gint floor_x10=(gint)lrintf(snapshot->floor_db*10.0f);
  guint span_x10=(guint)lrintf(snapshot->span_db*10.0f);
  if(floor_x10<G_MININT16) floor_x10=G_MININT16;
  if(floor_x10>G_MAXINT16) floor_x10=G_MAXINT16;
  if(span_x10>G_MAXUINT16) span_x10=G_MAXUINT16;
  guint16 floor_wire=(guint16)(gint16)floor_x10;
  frame[WEB_BINARY_HEADER_SIZE]=(guint8)floor_wire;
  frame[WEB_BINARY_HEADER_SIZE+1]=(guint8)(floor_wire>>8);
  frame[WEB_BINARY_HEADER_SIZE+2]=(guint8)span_x10;
  frame[WEB_BINARY_HEADER_SIZE+3]=(guint8)(span_x10>>8);
  return g_bytes_new_take(frame,total);
}

static gpointer web_sender_thread(gpointer data) {
  WEB_CLIENT *client=(WEB_CLIENT *)data;
  for(;;) {
    GBytes *raw=NULL;
    int rx_index=-1;
    WEB_SPECTRUM_PROFILE profile=WEB_SPECTRUM_LOW;
    guint requested_bins=256;
    gboolean status_due=FALSE;
    gint64 now=g_get_monotonic_time();
    g_mutex_lock(&client->queue_mutex);
    while(!g_atomic_int_get(&client->closing) && !client_has_pending(client))
      g_cond_wait(&client->queue_cond,&client->queue_mutex);
    if(g_atomic_int_get(&client->closing)) { g_mutex_unlock(&client->queue_mutex); break; }
    for(int i=0;i<MAX_RECEIVERS;i++) if(client->pending[i].frame!=NULL) {
      rx_index=i;
      raw=web_latest_frame_take(&client->pending[i]);
      if(client->pressure[i]>=3 && client->effective[i]>WEB_SPECTRUM_LOW) {
        WEB_SPECTRUM_LIMITS lower=web_spectrum_profile_limits(client->effective[i]-1);
        if(client->effective_fps[i]>lower.fps) client->effective_fps[i]=lower.fps;
        else {
          client->effective_bins[i]=lower.bins;
          client->effective[i]--;
        }
        client->pressure[i]=0;
        client->healthy_since_us[i]=now;
        status_due=TRUE;
      } else if(client->pressure[i]==0 &&
                now-client->healthy_since_us[i]>=WEB_RECOVERY_US) {
        WEB_SPECTRUM_LIMITS wanted=web_spectrum_profile_limits(client->desired[i]);
        if(client->effective_bins[i]<wanted.bins) {
          client->effective[i]++;
          client->effective_bins[i]=web_spectrum_profile_limits(client->effective[i]).bins;
          client->healthy_since_us[i]=now; status_due=TRUE;
        } else if(client->effective_fps[i]<wanted.fps) {
          client->effective_fps[i]=web_spectrum_profile_limits(client->effective[i]).fps;
          client->healthy_since_us[i]=now; status_due=TRUE;
        }
      }
      profile=client->effective[i];
      requested_bins=client->effective_bins[i];
      break;
    }
    g_mutex_unlock(&client->queue_mutex);
    if(raw==NULL) continue;
    gsize raw_len=0;
    const WEB_SNAPSHOT *snapshot=g_bytes_get_data(raw,&raw_len);
    now=g_get_monotonic_time();
    if(raw_len<sizeof(*snapshot) || snapshot->receiver!=rx_index ||
       snapshot->count>(raw_len-sizeof(*snapshot))/sizeof(float) ||
       now-(gint64)snapshot->timestamp_us>WEB_SPECTRUM_MAX_AGE_US) {
      g_mutex_lock(&client->queue_mutex);
      client->expired[rx_index]++;
      client->pressure[rx_index]++;
      g_mutex_unlock(&client->queue_mutex);
      g_bytes_unref(raw);
      continue;
    }
    GBytes *wire=spectrum_binary_frame(snapshot,profile,requested_bins);
    g_bytes_unref(raw);
    if(wire==NULL) continue;
    gsize wire_len=0; const guint8 *wire_data=g_bytes_get_data(wire,&wire_len);
    gint64 started=g_get_monotonic_time();
    gboolean ok=client_send(client,2,wire_data,wire_len);
    gint64 elapsed=g_get_monotonic_time()-started;
    g_bytes_unref(wire);
    g_mutex_lock(&client->queue_mutex);
    if(elapsed>WEB_SLOW_SEND_US) client->pressure[rx_index]++;
    else if(client->pressure[rx_index]>0) client->pressure[rx_index]--;
    if(now-client->last_status_us[rx_index]>=G_USEC_PER_SEC) {
      client->last_status_us[rx_index]=now;
      status_due=TRUE;
    }
    if(!ok) g_atomic_int_set(&client->closing,TRUE);
    g_mutex_unlock(&client->queue_mutex);
    if(!ok) { shutdown(client->fd,SHUT_RDWR); break; }
    if(status_due) spectrum_status(client,rx_index);
  }
  return NULL;
}

static gboolean websocket_session(WEB_CLIENT *client) {
  int fd=client->fd;
  const char *hello=
      "{\"type\":\"hello\",\"protocol\":1,\"server\":\"MacHPSDR\","
#ifdef WEBRTC
      "\"capabilities\":[\"control\",\"spectrum\",\"audio\"]}";
#else
      "\"capabilities\":[\"control\",\"spectrum\"]}";
#endif
  g_atomic_int_set(&client->upgraded,TRUE);
  client->sender=g_thread_new("web-spectrum",web_sender_thread,client);
  if(!ws_send_text(client,hello)) return FALSE;
  for(;;) {
    char *payload=NULL;
    size_t len=0;
    int op=tci_ws_recv(fd,&payload,&len);
    if(op<0) { g_free(payload); return TRUE; }
    if(op==8) { g_free(payload); return TRUE; }
    if(op==9) {
      gboolean ok=client_send(client,10,payload,len);
      g_free(payload);
      if(!ok) return FALSE;
      continue;
    }
    if(op!=1) { g_free(payload); continue; }
    WEB_REQUEST request;
    char error[128];
    if(!web_protocol_parse(payload,len,&request,error,sizeof(error))) {
      g_free(payload);
      if(!ws_send_error(client,0,"invalid_request",error)) return FALSE;
      continue;
    }
    g_free(payload);
    gboolean ok=dispatch_request(client,&request);
    web_request_clear(&request); /* release any sdp/candidate/mid the parse took */
    if(!ok) return FALSE;
  }
}

static void web_client_release(WEB_CLIENT *client) {
  g_mutex_lock(&client->queue_mutex);
  g_atomic_int_set(&client->closing,TRUE);
  g_cond_broadcast(&client->queue_cond);
  g_mutex_unlock(&client->queue_mutex);
  if(client->fd>=0) shutdown(client->fd,SHUT_RDWR);
  if(client->sender!=NULL) {
    g_thread_join(client->sender);
    client->sender=NULL;
  }
#ifdef WEBRTC
  /* closing is set above, so any in-flight signal callback early-returns from
     client_send(); this joins the libdatachannel threads with no lock held. */
  web_rtc_teardown(client);
#endif
  g_mutex_lock(&client->queue_mutex);
  for(int i=0;i<MAX_RECEIVERS;i++) {
    web_latest_frame_clear(&client->pending[i]);
    client->subscribed[i]=FALSE;
  }
  g_atomic_int_set(&client->upgraded,FALSE);
  g_mutex_unlock(&client->queue_mutex);
  g_mutex_lock(&web_clients_mutex);
  int fd=client->fd;
  client->fd=-1;
  if(web_client_workers>0) web_client_workers--;
  g_cond_broadcast(&web_clients_cond);
  g_mutex_unlock(&web_clients_mutex);
  if(fd>=0) closesocket(fd);
}

static gpointer web_client_thread(gpointer data) {
  WEB_CLIENT *client=(WEB_CLIENT *)data;
  int fd=client->fd;
  char request[WEB_HTTP_MAX+1];
  size_t total=0;
  net_set_rcvtimeo(fd,5000);
  while(total<WEB_HTTP_MAX) {
    ssize_t n=recv(fd,request+total,WEB_HTTP_MAX-total,0);
    if(n<=0) { web_client_release(client); return NULL; }
    total+=(size_t)n; request[total]='\0';
    if(strstr(request,"\r\n\r\n")!=NULL) break;
  }
  if(total==WEB_HTTP_MAX || strstr(request,"\r\n\r\n")==NULL) {
    http_error(fd,400,"request header is too large\n");
    web_client_release(client); return NULL;
  }

  char method[8], target[256], version[16];
  if(sscanf(request,"%7s %255s %15s",method,target,version)!=3 ||
     strcmp(version,"HTTP/1.1")!=0) {
    http_error(fd,400,"invalid request line\n");
    web_client_release(client); return NULL;
  }
  gboolean head=strcmp(method,"HEAD")==0;
  if(strcmp(method,"GET")!=0 && !head) {
    http_error(fd,405,"only GET and HEAD are supported\n");
    web_client_release(client); return NULL;
  }
  const char *asset=NULL, *type=NULL;
  gboolean websocket=FALSE;
  if(!web_protocol_route(target,&asset,&type,&websocket)) {
    http_error(fd,404,"not found\n");
    web_client_release(client); return NULL;
  }
  if(websocket) {
    char key[128], upgrade[64], protocol[128];
    if(head || header_value(request,"Sec-WebSocket-Key",key,sizeof(key))==NULL ||
       header_value(request,"Upgrade",upgrade,sizeof(upgrade))==NULL ||
       g_ascii_strcasecmp(upgrade,"websocket")!=0 ||
       header_value(request,"Sec-WebSocket-Protocol",protocol,sizeof(protocol))==NULL ||
       !header_has_token(protocol,"machpsdr.v1")) {
      http_error(fd,426,"WebSocket protocol machpsdr.v1 is required\n");
    } else if(!origin_allowed(request)) {
      http_error(fd,403,"WebSocket origin is not allowed\n");
    } else if(tci_ws_accept(fd,key,"machpsdr.v1")) {
      net_set_rcvtimeo(fd,0);
      net_set_sndtimeo(fd,250);
      websocket_session(client);
    }
    web_client_release(client); return NULL;
  }

  gsize len=0;
  gchar *contents=read_asset(asset,&len);
  if(contents==NULL) http_error(fd,500,"web asset is missing\n");
  else {
    http_send(fd,200,type,contents,len,head);
    g_free(contents);
  }
  web_client_release(client);
  return NULL;
}

static gpointer web_server_thread(gpointer data) {
  (void)data;
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0) goto failed;
  int one=1;
  setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
#ifdef SO_NOSIGPIPE
  setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
  struct sockaddr_in addr;
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  addr.sin_port=htons((guint16)web_port);
  if(bind(fd,(struct sockaddr *)&addr,sizeof(addr))<0 || listen(fd,8)<0) {
    closesocket(fd); goto failed;
  }
  web_listen=fd;
  g_snprintf(web_status_line,sizeof(web_status_line),"http://127.0.0.1:%d",web_port);
  log_info("web: listening on %s\n",web_status_line);

  while(g_atomic_int_get(&web_running)) {
    int client_fd=accept(fd,NULL,NULL);
    if(client_fd<0) {
      if(!g_atomic_int_get(&web_running)) break;
      if(errno==EINTR) continue;
      break;
    }
    if(!g_atomic_int_get(&web_running)) { closesocket(client_fd); break; }
#ifdef SO_NOSIGPIPE
    setsockopt(client_fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    WEB_CLIENT *slot=NULL;
    g_mutex_lock(&web_clients_mutex);
    for(int i=0;i<WEB_MAX_CLIENTS;i++) if(web_clients[i].fd<0) {
      slot=&web_clients[i];
      g_mutex_lock(&slot->queue_mutex);
      slot->fd=client_fd;
      g_atomic_int_set(&slot->closing,FALSE);
      g_atomic_int_set(&slot->upgraded,FALSE);
      slot->sender=NULL;
      for(int j=0;j<MAX_RECEIVERS;j++) {
        web_latest_frame_clear(&slot->pending[j]);
        slot->pending[j].dropped=0;
        slot->subscribed[j]=FALSE;
        slot->desired[j]=WEB_SPECTRUM_BALANCED;
        slot->effective[j]=WEB_SPECTRUM_BALANCED;
        slot->effective_bins[j]=512;
        slot->effective_fps[j]=10;
        slot->next_offer_us[j]=0;
        slot->healthy_since_us[j]=g_get_monotonic_time();
        slot->pressure[j]=0;
        slot->expired[j]=0;
        slot->last_status_us[j]=0;
      }
#ifdef WEBRTC
      slot->rtc=NULL;
      slot->rtc_rx=-1;
#endif
      g_mutex_unlock(&slot->queue_mutex);
      web_client_workers++;
      break;
    }
    g_mutex_unlock(&web_clients_mutex);
    if(slot==NULL) { closesocket(client_fd); continue; }
    GThread *thread=g_thread_new("web-client",web_client_thread,slot);
    if(thread) g_thread_unref(thread); else web_client_release(slot);
  }
  if(web_listen>=0) { closesocket(web_listen); web_listen=-1; }
  return NULL;

failed:
  g_snprintf(web_status_line,sizeof(web_status_line),"failed to listen on localhost:%d",web_port);
  log_error("web: %s\n",web_status_line);
  g_atomic_int_set(&web_running,0);
  return NULL;
}

void web_server_init(RADIO *radio) {
  web_server_stop();
  web_radio=radio;
  if(!web_sync_initialized) {
    for(int i=0;i<WEB_MAX_CLIENTS;i++) {
      web_clients[i].fd=-1;
      g_mutex_init(&web_clients[i].queue_mutex);
      g_mutex_init(&web_clients[i].send_mutex);
      g_cond_init(&web_clients[i].queue_cond);
    }
    web_sync_initialized=TRUE;
  }
  const char *env=getenv("MACHPSDR_WEB");
  if(env!=NULL && radio!=NULL) {
    radio->web_enable=TRUE;
    int port=atoi(env);
    if(port>0 && port<=65535) radio->web_port=port;
  }
  if(radio!=NULL && radio->web_enable) web_server_start();
}

void web_server_start(void) {
  if(g_atomic_int_get(&web_running) || web_radio==NULL) return;
  web_port=(web_radio->web_port>0 && web_radio->web_port<=65535)
             ? web_radio->web_port : WEB_DEFAULT_PORT;
  g_atomic_int_set(&web_running,1);
  g_snprintf(web_status_line,sizeof(web_status_line),"starting on localhost:%d",web_port);
  web_thread=g_thread_new("web-server",web_server_thread,NULL);
}

void web_server_stop(void) {
  if(!g_atomic_int_get(&web_running) && web_thread==NULL) return;
  g_atomic_int_set(&web_running,0);
  if(web_listen>=0) {
    shutdown(web_listen,SHUT_RDWR);
    closesocket(web_listen); web_listen=-1;
  }
  g_mutex_lock(&web_clients_mutex);
  for(int i=0;i<WEB_MAX_CLIENTS;i++) if(web_clients[i].fd>=0) {
    g_mutex_lock(&web_clients[i].queue_mutex);
    g_atomic_int_set(&web_clients[i].closing,TRUE);
    g_cond_broadcast(&web_clients[i].queue_cond);
    g_mutex_unlock(&web_clients[i].queue_mutex);
    shutdown(web_clients[i].fd,SHUT_RDWR);
  }
  g_mutex_unlock(&web_clients_mutex);
  if(web_thread!=NULL) { g_thread_join(web_thread); web_thread=NULL; }
  g_mutex_lock(&web_clients_mutex);
  while(web_client_workers>0) g_cond_wait(&web_clients_cond,&web_clients_mutex);
  g_mutex_unlock(&web_clients_mutex);
  g_mutex_lock(&web_spectrum_mutex);
  for(int i=0;i<MAX_RECEIVERS;i++) {
    if(web_latest_spectrum[i]!=NULL) g_bytes_unref(web_latest_spectrum[i]);
    web_latest_spectrum[i]=NULL;
  }
  g_mutex_unlock(&web_spectrum_mutex);
  g_strlcpy(web_status_line,"stopped",sizeof(web_status_line));
}

void web_server_publish_spectrum(int receiver, const float *bins, unsigned count,
                                 float calibration_db, float floor_db, float span_db) {
  if(!g_atomic_int_get(&web_running) || bins==NULL || receiver<0 ||
     receiver>=MAX_RECEIVERS || count==0 || !isfinite(floor_db) ||
     !isfinite(calibration_db) || !isfinite(span_db) || span_db<=0.0f) return;
  if(count>65536) count=65536;
  gsize bytes=sizeof(WEB_SNAPSHOT)+(gsize)count*sizeof(float);
  WEB_SNAPSHOT *snapshot=g_malloc(bytes);
  snapshot->receiver=(guint8)receiver;
  memset(snapshot->reserved,0,sizeof(snapshot->reserved));
  snapshot->timestamp_us=(guint64)g_get_monotonic_time();
  snapshot->count=count;
  snapshot->floor_db=floor_db;
  snapshot->span_db=span_db;
  for(unsigned i=0;i<count;i++) snapshot->bins[i]=bins[i]+calibration_db;
  g_mutex_lock(&web_spectrum_mutex);
  snapshot->sequence=++web_spectrum_sequence[receiver];
  GBytes *raw=g_bytes_new_take(snapshot,bytes);
  if(web_latest_spectrum[receiver]!=NULL) g_bytes_unref(web_latest_spectrum[receiver]);
  web_latest_spectrum[receiver]=g_bytes_ref(raw);
  g_mutex_unlock(&web_spectrum_mutex);

  gint64 now=(gint64)snapshot->timestamp_us;
  g_mutex_lock(&web_clients_mutex);
  for(int i=0;i<WEB_MAX_CLIENTS;i++) {
    WEB_CLIENT *client=&web_clients[i];
    if(client->fd<0) continue;
    g_mutex_lock(&client->queue_mutex);
    if(g_atomic_int_get(&client->upgraded) && !g_atomic_int_get(&client->closing) &&
       client->subscribed[receiver] &&
       now>=client->next_offer_us[receiver]) {
      client->next_offer_us[receiver]=now+G_USEC_PER_SEC/(gint64)client->effective_fps[receiver];
      gboolean replacing=client->pending[receiver].frame!=NULL;
      web_latest_frame_replace(&client->pending[receiver],raw);
      if(replacing) client->pressure[receiver]++;
      g_cond_signal(&client->queue_cond);
    }
    g_mutex_unlock(&client->queue_mutex);
  }
  g_mutex_unlock(&web_clients_mutex);
  g_bytes_unref(raw);
}

/* Fan a receiver's demodulated audio block out to every client subscribed to
   that receiver's audio.  Called from the RX audio thread via receiver.c; the
   per-session feed only converts and enqueues under a short lock, and the whole
   call is a no-op with no WebRTC build or no audio subscribers.  queue_mutex
   guards client->rtc against a concurrent teardown (see web_rtc_teardown). */
void web_server_audio_tap(int receiver, const double *stereo, int frames) {
#ifdef WEBRTC
  if(!g_atomic_int_get(&web_running) || stereo==NULL || frames<=0 ||
     receiver<0 || receiver>=MAX_RECEIVERS) return;
  g_mutex_lock(&web_clients_mutex);
  for(int i=0;i<WEB_MAX_CLIENTS;i++) {
    WEB_CLIENT *client=&web_clients[i];
    if(client->fd<0) continue;
    g_mutex_lock(&client->queue_mutex);
    if(client->rtc!=NULL && client->rtc_rx==receiver &&
       g_atomic_int_get(&client->upgraded) && !g_atomic_int_get(&client->closing))
      web_rtc_session_feed(client->rtc,stereo,frames);
    g_mutex_unlock(&client->queue_mutex);
  }
  g_mutex_unlock(&web_clients_mutex);
#else
  (void)receiver; (void)stereo; (void)frames;
#endif
}

const char *web_server_status(void) {
  static char status[192];
  if(!g_atomic_int_get(&web_running)) return web_status_line;
  int clients=0;
  g_mutex_lock(&web_clients_mutex);
  for(int i=0;i<WEB_MAX_CLIENTS;i++) if(web_clients[i].fd>=0) clients++;
  g_mutex_unlock(&web_clients_mutex);
  g_snprintf(status,sizeof(status),"%s (clients: %d)",web_status_line,clients);
  return status;
}
