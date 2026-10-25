/* Embedded HTTP/WebSocket server for the browser remote. */
#include "net_compat.h"             /* first: winsock2 before GTK/windows.h */

#include <gtk/gtk.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wdsp.h>

#include "log.h"
#include "mode.h"
#include "resource_path.h"
#include "receiver.h"
#include "transmitter.h"
#include "filter.h"
#include "band.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "ext.h"
#include "bookmark_dialog.h"
#include "tci_ws.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "web_protocol.h"
#include "web_server.h"
#include "web_tls.h"
#ifdef WEBRTC
#include "web_rtc.h"
#endif

#define WEB_MAX_CLIENTS 8
#define WEB_HTTP_MAX 8192
#define WEB_SPECTRUM_MAX_AGE_US 500000
#define WEB_SLOW_SEND_US 100000
#define WEB_RECOVERY_US 10000000
/* Phase 4 pushed-state cadence: the detector tick that catches a discrete
   change, and the interval at which the S-meter is refreshed when nothing else
   moved. */
#define WEB_STATE_TICK_MS 50
#define WEB_METER_PUSH_US 200000
#define WEB_TX_LEASE_TIMEOUT_US (5 * G_USEC_PER_SEC)
#define WEB_TX_MEDIA_TIMEOUT_US 500000
#define WEB_TX_AUDIO_RING 12000 /* 250 ms at 48 kHz, bounded */

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
  TCI_WS_IO io;
  WEB_TLS_SESSION *tls;
  gint upgraded;
  gint closing;
  guint32 peer;                       /* IPv4 source, network order (throttling key) */
  char peer_name[INET_ADDRSTRLEN];
  WEB_ROLE role;                      /* from the login this connection used */
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
  char rtc_address[INET_ADDRSTRLEN];
#endif
} WEB_CLIENT;

static RADIO *web_radio;
static volatile gint web_running;
static GThread *web_thread;
static int web_listen=-1;
static int web_port=WEB_DEFAULT_PORT;
static WEB_TLS_CONTEXT *web_tls;
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
/* Phase 4 pushed state (GTK thread only): the last discrete-field signature
   broadcast, and when the last state frame went out. */
static guint web_state_source;
static GString *web_last_discrete;
static gint64 web_last_state_push_us;
/* Phase 6 remote-TX safety.  The lease is deliberately tied to one upgraded
   WebSocket, not to a reusable client-supplied token: once that connection is
   gone, no stale browser state can key the radio. */
static GMutex web_lease_mutex;
static WEB_CLIENT *web_tx_lease_owner;
static gint64 web_tx_lease_deadline_us;
/* The control lease (Phase 5 roles), same shape and same mutex: one
   control-role connection may change anything; every other connection only
   watches and listens.  The TX lease can only be taken by the control holder,
   and losing control drops it. */
static WEB_CLIENT *web_control_owner;
static gint64 web_control_deadline_us;
/* Failed-login throttling, shared by every client thread. */
static GMutex web_auth_mutex;
static WEB_AUTH_THROTTLE web_auth_throttle;
static volatile gint web_remote_ptt;
static volatile gint web_remote_mox;
static volatile gint web_remote_mox_seen;
static volatile gint web_remote_tune;
static volatile gint web_remote_tune_seen;
static GMutex web_tx_audio_mutex;
static float web_tx_audio_ring[WEB_TX_AUDIO_RING];
static volatile gint web_tx_audio_head;
static volatile gint web_tx_audio_tail;
static gint64 web_tx_audio_last_us; /* guarded by web_tx_audio_mutex */
static volatile gint web_tx_audio_active;
static volatile gint web_tx_audio_dropped;
static volatile gint web_tx_audio_underruns;
static gboolean web_state_tick(gpointer data);  /* defined below, armed in start */
static gboolean ws_send_lease_state(WEB_CLIENT *client, gint64 id, gboolean held);

typedef struct {
  RADIO *radio;
  WEB_TX_CONTROL control;
  gint64 value;
} WEB_TX_CONTROL_CMD;

typedef struct {
  RADIO *radio;
  gboolean state;
} WEB_TUNE_STATE;

static gboolean web_set_tune_idle(gpointer data) {
  WEB_TUNE_STATE *t=data;
  if(t->radio==web_radio && t->radio!=NULL && t->radio->can_transmit &&
     t->radio->transmitter!=NULL)
    set_tune(t->radio,t->state);
  g_free(t);
  return G_SOURCE_REMOVE;
}

/* GTK-thread counterpart of the desktop TX widgets.  Keep the actual WDSP and
   hardware calls identical to mic_gain.c, transmitter_dialog.c and
   drive_level.c; the browser is only another front end. */
static gboolean web_set_tx_control_idle(gpointer data) {
  WEB_TX_CONTROL_CMD *c=data;
  RADIO *r=c->radio;
  if(r!=web_radio || r==NULL || r->transmitter==NULL) { g_free(c); return G_SOURCE_REMOVE; }
  TRANSMITTER *tx=r->transmitter;
  switch(c->control) {
    case WEB_TX_CTRL_DRIVE:
      tx->drive=CLAMP((double)c->value,0.0,100.0);
      if(r->discovered!=NULL && r->discovered->protocol==PROTOCOL_2)
        protocol2_high_priority();
#ifdef SOAPYSDR
      if(r->discovered!=NULL && r->discovered->protocol==PROTOCOL_SOAPYSDR)
        soapy_protocol_set_tx_drive(tx->drive);
#endif
      if(r->drive_level!=NULL) gtk_widget_queue_draw(r->drive_level);
      break;
    case WEB_TX_CTRL_TUNE_DRIVE:
      tx->tune_percent=CLAMP((double)c->value,0.0,100.0);
      break;
    case WEB_TX_CTRL_MIC_GAIN:
      tx->mic_gain=CLAMP((double)c->value,-10.0,50.0);
      SetTXAPanelGain1(tx->channel,pow(10.0,tx->mic_gain/20.0));
      if(r->mic_gain!=NULL) gtk_widget_queue_draw(r->mic_gain);
      break;
    case WEB_TX_CTRL_COMPRESSOR:
      tx->compressor=c->value!=0;
      SetTXACompressorRun(tx->channel,tx->compressor);
      break;
    case WEB_TX_CTRL_COMP_LEVEL:
      tx->compressor_level=CLAMP((double)c->value,0.0,20.0);
      SetTXACompressorGain(tx->channel,tx->compressor_level);
      break;
    case WEB_TX_CTRL_LEVELER:
      tx->leveler=c->value!=0;
      SetTXALevelerSt(tx->channel,tx->leveler);
      break;
    case WEB_TX_CTRL_CESSB:
      tx->cessb=c->value!=0;
      SetTXAosctrlRun(tx->channel,tx->cessb);
      break;
    default: break;
  }
  g_free(c);
  return G_SOURCE_REMOVE;
}

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

static gboolean http_send(TCI_WS_IO *io, int code, const char *type,
                          const void *body, size_t len, gboolean head) {
  char header[1024];
  int n=g_snprintf(header,sizeof(header),
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %zu\r\n"
      "Cache-Control: no-store\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Referrer-Policy: no-referrer\r\n"
      "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self' ws: wss:\r\n"
      "Connection: close\r\n\r\n",
      code,http_reason(code),type?type:"text/plain; charset=utf-8",len);
  if(n<=0 || (size_t)n>=sizeof(header) || !tci_ws_io_send_all(io,header,(size_t)n)) return FALSE;
  return head || len==0 || tci_ws_io_send_all(io,body,len);
}

static void http_error(TCI_WS_IO *io, int code, const char *message) {
  http_send(io,code,"text/plain; charset=utf-8",message,strlen(message),FALSE);
}

static void http_unauthorized(TCI_WS_IO *io) {
  static const char body[]="Authentication required\n";
  char header[512];
  int n=g_snprintf(header,sizeof(header),
      "HTTP/1.1 401 Unauthorized\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Length: %zu\r\n"
      "Cache-Control: no-store\r\n"
      "WWW-Authenticate: Basic realm=\"MacHPSDR\", charset=\"UTF-8\"\r\n"
      "Connection: close\r\n\r\n",sizeof(body)-1);
  if(n>0 && (size_t)n<sizeof(header) &&
     tci_ws_io_send_all(io,header,(size_t)n))
    tci_ws_io_send_all(io,body,sizeof(body)-1);
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
  char host[256];
  return header_value(request,"Host",host,sizeof(host))!=NULL &&
         web_origin_matches_host(origin,host);
}

#ifdef WEBRTC
/* Resolve the authority the browser actually used for this HTTPS/WebSocket
   connection.  With an explicit UDP port-forward that same public/LAN address
   is the WebRTC destination, so no third-party STUN discovery is needed. */
static gboolean request_ipv4_address(const char *request, char *out, size_t out_len) {
  char authority[256], name[256];
  if(header_value(request,"Host",authority,sizeof(authority))==NULL || authority[0]=='[')
    return FALSE; /* the embedded listener is currently IPv4-only */
  const char *colon=strrchr(authority,':');
  size_t name_len=(colon!=NULL && strchr(authority,':')==colon)
                    ? (size_t)(colon-authority) : strlen(authority);
  if(name_len==0 || name_len>=sizeof(name)) return FALSE;
  memcpy(name,authority,name_len); name[name_len]='\0';
  struct addrinfo hints={0}, *result=NULL;
  hints.ai_family=AF_INET;
  hints.ai_socktype=SOCK_DGRAM;
  if(getaddrinfo(name,NULL,&hints,&result)!=0 || result==NULL) return FALSE;
  struct sockaddr_in *address=(struct sockaddr_in *)result->ai_addr;
  gboolean ok=inet_ntop(AF_INET,&address->sin_addr,out,(socklen_t)out_len)!=NULL;
  freeaddrinfo(result);
  return ok;
}
#endif

static void http_too_many(TCI_WS_IO *io, gint64 wait_us) {
  static const char body[]="Too many failed logins; try again later\n";
  char header[320];
  long long seconds=(wait_us+G_USEC_PER_SEC-1)/G_USEC_PER_SEC;
  int n=g_snprintf(header,sizeof(header),
      "HTTP/1.1 429 Too Many Requests\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Length: %zu\r\n"
      "Retry-After: %lld\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n",
      sizeof(body)-1,seconds);
  if(n>0 && (size_t)n<sizeof(header) && tci_ws_io_send_all(io,header,(size_t)n))
    tci_ws_io_send_all(io,body,sizeof(body)-1);
}

static gboolean view_login_set(void) {
  return web_radio->web_view_salt[0]!='\0' &&
         strlen(web_radio->web_view_hash)==WEB_AUTH_HASH_HEX_SIZE-1;
}

typedef enum { WEB_LOGIN_OK, WEB_LOGIN_CHALLENGE, WEB_LOGIN_THROTTLED } WEB_LOGIN;

/* Authenticate one request and give its role.  A locked-out address is refused
   before any hash is computed; only a WRONG Authorization header counts as a
   failure (a missing one is the browser's first, credential-less try). */
static WEB_LOGIN request_login(const char *request, WEB_CLIENT *client,
                               WEB_ROLE *role, gint64 *wait_us) {
  *role=WEB_ROLE_CONTROL;
  if(web_radio==NULL || !web_radio->web_auth_enable) return WEB_LOGIN_OK;
  gint64 now=g_get_monotonic_time();
  g_mutex_lock(&web_auth_mutex);
  *wait_us=web_auth_throttle_wait(&web_auth_throttle,client->peer,now);
  g_mutex_unlock(&web_auth_mutex);
  if(*wait_us>0) return WEB_LOGIN_THROTTLED;
  char authorization[768];
  if(header_value(request,"Authorization",authorization,sizeof(authorization))==NULL)
    return WEB_LOGIN_CHALLENGE;
  gboolean ok=web_auth_check_basic(authorization,web_radio->web_auth_user,
                                   web_radio->web_auth_salt,web_radio->web_auth_hash,
                                   WEB_AUTH_PBKDF2_ITERATIONS);
  if(!ok && view_login_set() &&
     web_auth_check_basic(authorization,web_radio->web_view_user,
                          web_radio->web_view_salt,web_radio->web_view_hash,
                          WEB_AUTH_PBKDF2_ITERATIONS)) {
    ok=TRUE;
    *role=WEB_ROLE_VIEW;
  }
  gint64 lock=0;
  g_mutex_lock(&web_auth_mutex);
  if(ok) web_auth_throttle_success(&web_auth_throttle,client->peer);
  else lock=web_auth_throttle_failure(&web_auth_throttle,client->peer,g_get_monotonic_time());
  g_mutex_unlock(&web_auth_mutex);
  if(lock>0)
    log_info("web: %s refused for %llds after %d failed logins\n",client->peer_name,
             (long long)(lock/G_USEC_PER_SEC),WEB_AUTH_FREE_FAILURES);
  return ok ? WEB_LOGIN_OK : WEB_LOGIN_CHALLENGE;
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
    ok=tci_ws_io_send_blocking(&client->io,opcode,payload,len);
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

static void web_tx_audio_reset(void) {
  g_mutex_lock(&web_tx_audio_mutex);
  g_atomic_int_set(&web_tx_audio_head,0);
  g_atomic_int_set(&web_tx_audio_tail,0);
  web_tx_audio_last_us=0;
  g_atomic_int_set(&web_tx_audio_active,FALSE);
  g_mutex_unlock(&web_tx_audio_mutex);
}

static gboolean web_tx_audio_fresh(void) {
  gboolean fresh;
  gint64 now=g_get_monotonic_time();
  g_mutex_lock(&web_tx_audio_mutex);
  fresh=web_tx_audio_last_us>0 && now-web_tx_audio_last_us<WEB_TX_MEDIA_TIMEOUT_US;
  g_mutex_unlock(&web_tx_audio_mutex);
  return fresh;
}

static void web_remote_unkey(void) {
  g_atomic_int_set(&web_remote_mox,FALSE);
  g_atomic_int_set(&web_remote_mox_seen,FALSE);
  g_atomic_int_set(&web_remote_tune,FALSE);
  g_atomic_int_set(&web_remote_tune_seen,FALSE);
  if(!g_atomic_int_compare_and_exchange(&web_remote_ptt,TRUE,FALSE)) return;
  if(web_radio==NULL) return;
  WEB_TUNE_STATE *t=g_new(WEB_TUNE_STATE,1);
  t->radio=web_radio; t->state=FALSE;
  g_idle_add(web_set_tune_idle,t);
  MOX_STATE *m=g_new(MOX_STATE,1);
  m->radio=web_radio; m->state=FALSE;
  g_idle_add(ext_set_mox,m);
}

/* Drop a client's lease and fail safe to RX.  This is safe from a socket
   worker or the GTK timer: ownership is protected separately and the actual
   radio mutation is always dispatched through the main loop. */
static gboolean web_lease_release_client(WEB_CLIENT *client, gboolean notify) {
  gboolean owned=FALSE;
  g_mutex_lock(&web_lease_mutex);
  if(web_tx_lease_owner==client) {
    web_tx_lease_owner=NULL;
    web_tx_lease_deadline_us=0;
    owned=TRUE;
  }
  g_mutex_unlock(&web_lease_mutex);
  if(owned) {
    web_tx_audio_reset();
    web_remote_unkey();
    /* Tell a still-connected holder immediately so it stops and releases its
       microphone when the desktop arm, heartbeat or media watchdog revokes it. */
    if(notify) ws_send_lease_state(client,0,FALSE);
  }
  return owned;
}

static gboolean web_lease_is_owner(WEB_CLIENT *client, gboolean renew) {
  gboolean owned=FALSE,expired=FALSE;
  gint64 now=g_get_monotonic_time();
  g_mutex_lock(&web_lease_mutex);
  if(web_tx_lease_owner!=NULL && now>=web_tx_lease_deadline_us) {
    web_tx_lease_owner=NULL;
    web_tx_lease_deadline_us=0;
    expired=TRUE;
  } else if(web_tx_lease_owner==client) {
    owned=TRUE;
    if(renew) web_tx_lease_deadline_us=now+WEB_TX_LEASE_TIMEOUT_US;
  }
  g_mutex_unlock(&web_lease_mutex);
  if(expired) {
    web_tx_audio_reset();
    web_remote_unkey();
  }
  return owned;
}

static gboolean web_lease_active(void) {
  gboolean active;
  gint64 now=g_get_monotonic_time();
  g_mutex_lock(&web_lease_mutex);
  active=web_tx_lease_owner!=NULL && now<web_tx_lease_deadline_us;
  g_mutex_unlock(&web_lease_mutex);
  return active;
}

static gboolean ws_send_lease_state(WEB_CLIENT *client, gint64 id, gboolean held) {
  char line[160];
  g_snprintf(line,sizeof(line),
      "{\"type\":\"lease_state\",\"id\":%lld,\"held\":%s,\"timeout_ms\":%d}",
      (long long)id,held?"true":"false",(int)(WEB_TX_LEASE_TIMEOUT_US/1000));
  return ws_send_text(client,line);
}

static gboolean ws_send_control_state(WEB_CLIENT *client, gint64 id, gboolean held) {
  char line[160];
  g_snprintf(line,sizeof(line),
      "{\"type\":\"control_state\",\"id\":%lld,\"held\":%s,\"timeout_ms\":%d}",
      (long long)id,held?"true":"false",(int)(WEB_TX_LEASE_TIMEOUT_US/1000));
  return ws_send_text(client,line);
}

/* Losing control takes the TX lease with it (and so unkeys) -- a viewer can
   never be left holding the transmitter.  notify tells a still-connected
   former holder at once (takeover, expiry). */
static void web_control_lost(WEB_CLIENT *client, gboolean notify) {
  web_lease_release_client(client,notify);
  if(notify) ws_send_control_state(client,0,FALSE);
}

static gboolean web_control_release_client(WEB_CLIENT *client, gboolean notify) {
  gboolean owned=FALSE;
  g_mutex_lock(&web_lease_mutex);
  if(web_control_owner==client) {
    web_control_owner=NULL;
    web_control_deadline_us=0;
    owned=TRUE;
  }
  g_mutex_unlock(&web_lease_mutex);
  if(owned) web_control_lost(client,notify);
  return owned;
}

/* Expire a silent holder first; then answer whether client holds control,
   optionally renewing it (every accepted command renews, like the TX lease). */
static gboolean web_control_is_owner(WEB_CLIENT *client, gboolean renew) {
  WEB_CLIENT *expired=NULL;
  gboolean owned=FALSE;
  gint64 now=g_get_monotonic_time();
  g_mutex_lock(&web_lease_mutex);
  if(web_control_owner!=NULL && now>=web_control_deadline_us) {
    expired=web_control_owner;
    web_control_owner=NULL;
    web_control_deadline_us=0;
  } else if(web_control_owner==client) {
    owned=TRUE;
    if(renew) web_control_deadline_us=now+WEB_TX_LEASE_TIMEOUT_US;
  }
  g_mutex_unlock(&web_lease_mutex);
  if(expired!=NULL) web_control_lost(expired,TRUE);
  return owned;
}

static gboolean web_control_active(void) {
  gboolean active;
  gint64 now=g_get_monotonic_time();
  g_mutex_lock(&web_lease_mutex);
  active=web_control_owner!=NULL && now<web_control_deadline_us;
  g_mutex_unlock(&web_lease_mutex);
  return active;
}

/* Append the radio + per-receiver body shared by the get_state reply, the pushed
   state frame and the change-detector signature.  With include_meter FALSE it is
   the discrete signature (the fields a control can move); the live S-meter is
   excluded so meter noise never reads as a state change. */
static void append_state_body(GString *s, gboolean include_meter) {
  int active=web_radio->active_receiver ? web_radio->active_receiver->channel : -1;
  gchar *decoder_title=web_json_escape(web_radio->decoder_status_title);
  gchar *decoder_text=web_json_escape(web_radio->decoder_status_text);
  g_string_append_printf(s,
      "\"radio\":{\"mox\":%s,\"tune\":%s,\"remote_ptt\":%s,\"control_active\":%s,"
      "\"tx_lease_active\":%s,\"tx_media_active\":%s,\"can_transmit\":%s,\"active_rx\":%d,"
      "\"drive\":%d,\"tune_drive\":%d,\"mic_gain\":%d,"
      "\"compressor\":%s,\"comp_level\":%d,\"leveler\":%s,\"cessb\":%s,"
      "\"decoder_mode\":%d,\"decoder_title\":\"%s\",\"decoder_text\":\"%s\","
      "\"vfo_scroll_reversed\":%s,\"bookmarks\":[",
      web_radio->mox?"true":"false",
      web_radio->tune?"true":"false",
      g_atomic_int_get(&web_remote_ptt)?"true":"false",
      web_control_active()?"true":"false",
      web_lease_active()?"true":"false",
      g_atomic_int_get(&web_tx_audio_active)?"true":"false",
      (web_radio->can_transmit && web_radio->web_tx_enable)?"true":"false",active,
      web_radio->transmitter?(int)lround(web_radio->transmitter->drive):0,
      web_radio->transmitter?(int)lround(web_radio->transmitter->tune_percent):0,
      web_radio->transmitter?(int)lround(web_radio->transmitter->mic_gain):0,
      (web_radio->transmitter && web_radio->transmitter->compressor)?"true":"false",
      web_radio->transmitter?(int)lround(web_radio->transmitter->compressor_level):0,
      (web_radio->transmitter && web_radio->transmitter->leveler)?"true":"false",
      (web_radio->transmitter && web_radio->transmitter->cessb)?"true":"false",
      web_radio->decode_mode,decoder_title,decoder_text,
      web_radio->vfo_scroll_reversed?"true":"false");
  g_free(decoder_title); g_free(decoder_text);
  gboolean first_bookmark=TRUE;
  int bookmark_count=bookmarks_count();
  for(int bi=0;bi<bookmark_count;bi++) {
    gchar *raw_name=NULL; gint64 frequency=0;
    if(!bookmarks_copy_summary(bi,&raw_name,&frequency)) continue;
    gchar *name=web_json_escape(raw_name);
    g_string_append_printf(s,"%s{\"id\":%d,\"name\":\"%s\",\"frequency\":%lld}",
                           first_bookmark?"":",",bi,name,(long long)frequency);
    first_bookmark=FALSE;
    g_free(raw_name); g_free(name);
  }
  // The band list the desktop band menu offers (transverter names are the
  // operator's own text, so escaped).
  g_string_append(s,"],\"bands\":[");
  gboolean first_band=TRUE;
  for(int b=0;b<BANDS+XVTRS;b++) {
    if(!band_is_offered(b)) continue;
    gchar *name=web_json_escape(band_display_name(b));
    g_string_append_printf(s,"%s{\"i\":%d,\"n\":\"%s\"}",first_band?"":",",b,name);
    first_band=FALSE;
    g_free(name);
  }
  g_string_append(s,"]},\"receivers\":[");
  gboolean first=TRUE;
  for(int i=0;i<web_radio->discovered->supported_receivers;i++) {
    RECEIVER *rx=visible_rx(i);
    if(rx==NULL) continue;
    if(!first) g_string_append_c(s,',');
    first=FALSE;
    int mode=(rx->mode_a>=0 && rx->mode_a<MODES)?rx->mode_a:USB;
    g_string_append_printf(s,
        "{\"id\":%d,\"frequency\":%lld,\"frequency_b\":%lld,"
        "\"mode\":%d,\"mode_name\":\"%s\",",
        i,(long long)receiver_tuned_frequency(rx),(long long)rx->frequency_b,
        mode,mode_string[mode]);
    if(include_meter) {
      double meter=isfinite(rx->meter_db)?rx->meter_db:-200.0;
      // JSON is an ASCII interchange format: GTK runs under the operator's
      // locale, so "%.1f" on a comma-decimal machine (ru_RU) emits "-200,0" and
      // the whole frame becomes invalid JSON the browser silently drops. Format
      // the one float locale-independently, like every ASCII format this tree
      // writes.
      char meter_text[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(meter_text,sizeof(meter_text),"%.1f",meter);
      g_string_append_printf(s,"\"meter_db\":%s,",meter_text);
    }
    // Noise controls: nb (0/1/2), nr (0..4 via receiver_nr_mode) and the two
    // toggles. noise_bypassed is TRUE in DIGU/DIGL or under a decoder, where the
    // desktop keeps the selection but suppresses the WDSP run flags; the browser
    // shows them suppressed rather than pretending they took effect.
    int nb = rx->nb2 ? 2 : (rx->nb ? 1 : 0);
    int rf_value=web_radio->adc[rx->adc].attenuation,rf_min=0,rf_max=31;
    const char *rf_kind="attenuation";
#ifdef SOAPYSDR
    if(web_radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
      double lo=0.0,hi=100.0;
      if(web_radio->discovered->info.soapy.rx_gains>0) {
        lo=hi=0.0;
        for(size_t gi=0;gi<web_radio->discovered->info.soapy.rx_gains;gi++) {
          lo+=web_radio->discovered->info.soapy.rx_range[gi].minimum;
          hi+=web_radio->discovered->info.soapy.rx_range[gi].maximum;
        }
      }
      rf_value=(int)lrint(web_radio->adc[rx->adc].gain);
      rf_min=(int)ceil(lo); rf_max=(int)floor(hi); rf_kind="gain";
    } else
#endif
    if(web_radio->discovered->device==DEVICE_HERMES_LITE ||
       web_radio->discovered->device==DEVICE_HERMES_LITE2) {
      rf_min=-12; rf_max=48; rf_kind="gain";
    }
    g_string_append_printf(s,
        "\"locked\":%s,\"step\":%lld,\"filter_low\":%d,\"filter_high\":%d,"
        "\"af_gain\":%d,\"agc\":%d,\"agc_gain\":%d,\"squelch\":%d,"
        "\"nb\":%d,\"nr\":%d,\"anf\":%s,\"snb\":%s,\"noise_bypassed\":%s,"
        "\"rf_gain\":%d,\"rf_min\":%d,\"rf_max\":%d,\"rf_kind\":\"%s\","
        "\"subrx\":%s,\"subrx_mix\":%d,"
        "\"display_low\":%lld,\"display_high\":%lld,",
        rx->locked?"true":"false",(long long)rx->step,
        rx->filter_low_a,rx->filter_high_a,
        (int)lrint(rx->volume*100.0),rx->agc,(int)lrint(rx->agc_gain),
        (int)lrint(rx->squelch*100.0),
        nb,receiver_nr_mode(rx),rx->anf?"true":"false",rx->snb?"true":"false",
        receiver_dsp_bypassed(rx)?"true":"false",
        rf_value,rf_min,rf_max,rf_kind,rx->subrx_enable?"true":"false",rx->subrx_mix,
        (long long)(rx->frequency_a-(rx->sample_rate/2)+(rx->pan*rx->hz_per_pixel)),
        (long long)(rx->frequency_a-(rx->sample_rate/2)+
                    ((rx->pan+rx->panadapter_width)*rx->hz_per_pixel)));
    // Band, split, RIT/XIT (XIT from the transmitter, 0/off without one) and the
    // current filter preset plus the mode's whole preset list (name + edges), so
    // the browser can offer a reliable width dropdown instead of only edge drags.
    gboolean xit_on = web_radio->transmitter ? web_radio->transmitter->xit_enabled : FALSE;
    long long xit = web_radio->transmitter ? (long long)web_radio->transmitter->xit : 0;
    // An XVTR band's name is the operator's own typed text: escape it.
    gchar *band_name=web_json_escape(band_display_name(rx->band_a));
    g_string_append_printf(s,
        "\"band\":\"%s\",\"band_id\":%d,\"split\":%s,\"rit\":%lld,\"rit_on\":%s,"
        "\"xit\":%lld,\"xit_on\":%s,\"filter\":%d,\"filters\":[",
        band_name,rx->band_a,rx->split!=SPLIT_OFF?"true":"false",
        (long long)rx->rit,rx->rit_enabled?"true":"false",
        xit,xit_on?"true":"false",rx->filter_a);
    g_free(band_name);
    FILTER *mode_filters=filters[mode];
    for(int fi=0;fi<FILTERS;fi++) {
      // Preset titles are compiled ASCII constants (no quote/backslash).
      g_string_append_printf(s,"%s{\"n\":\"%s\",\"l\":%d,\"h\":%d}",
          fi?",":"",mode_filters[fi].title,mode_filters[fi].low,mode_filters[fi].high);
    }
    g_string_append(s,"]}");
  }
  g_string_append_c(s,']');
}

static void build_state_frame(GString *s, gint64 id, int revision) {
  g_string_append_printf(s,"{\"type\":\"state\",\"id\":%lld,\"revision\":%d,",
                         (long long)id,revision);
  append_state_body(s,TRUE);
  g_string_append_c(s,'}');
}

static gboolean ws_send_state(WEB_CLIENT *client, gint64 id) {
  if(web_radio==NULL || web_radio->discovered==NULL)
    return ws_send_error(client,id,"radio_unavailable","radio is not ready");
  GString *s=g_string_sized_new(1024);
  /* A snapshot reply reports the current revision; only a real discrete change
     (detected by web_state_tick) advances it. */
  build_state_frame(s,id,g_atomic_int_get(&web_revision));
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

static void web_rtc_on_tx_audio(void *user, const float *mono, int frames) {
  WEB_CLIENT *client=(WEB_CLIENT *)user;
  if(client==NULL || mono==NULL || frames<=0 ||
     !web_lease_is_owner(client,FALSE) || web_radio==NULL ||
     !web_radio->web_tx_enable) return;
  g_mutex_lock(&web_tx_audio_mutex);
  if(!web_radio->mox) {
    /* In RX retain only this newest decoded packet, not old speech captured
       before the operator presses MOX. */
    g_atomic_int_set(&web_tx_audio_head,0);
    g_atomic_int_set(&web_tx_audio_tail,0);
  }
  int head=g_atomic_int_get(&web_tx_audio_head);
  for(int i=0;i<frames;i++) {
    int next=(head+1)%WEB_TX_AUDIO_RING;
    if(next==g_atomic_int_get(&web_tx_audio_tail)) {
      g_atomic_int_add(&web_tx_audio_dropped,frames-i);
      break;
    }
    web_tx_audio_ring[head]=mono[i];
    g_atomic_int_set(&web_tx_audio_head,next);
    head=next;
  }
  web_tx_audio_last_us=g_get_monotonic_time();
  g_atomic_int_set(&web_tx_audio_active,TRUE);
  g_mutex_unlock(&web_tx_audio_mutex);
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
  /* Role and control lease are checked once, here, from the one policy table
     (web_request_access) -- never per handler. */
  WEB_ACCESS access=web_request_access(request);
  if(access!=WEB_ACCESS_VIEW && client->role!=WEB_ROLE_CONTROL)
    return ws_send_error(client,request->id,"view_only","this login can only watch and listen");
  if(access==WEB_ACCESS_CONTROL_LEASE && !web_control_is_owner(client,TRUE))
    return ws_send_error(client,request->id,"control_required","take control before changing the radio");

  if(request->type==WEB_REQUEST_GET_STATE) return ws_send_state(client,request->id);

  if(request->type==WEB_REQUEST_CONTROL_ACQUIRE) {
    web_control_is_owner(client,FALSE);        /* expire a silent holder first */
    WEB_CLIENT *previous=NULL;
    char previous_name[INET_ADDRSTRLEN]="";
    gboolean acquired=FALSE;
    g_mutex_lock(&web_lease_mutex);
    if(web_control_owner==NULL || web_control_owner==client || request->force) {
      if(web_control_owner!=client && web_control_owner!=NULL) {
        previous=web_control_owner;
        g_strlcpy(previous_name,previous->peer_name,sizeof(previous_name));
      }
      web_control_owner=client;
      web_control_deadline_us=g_get_monotonic_time()+WEB_TX_LEASE_TIMEOUT_US;
      acquired=TRUE;
    }
    g_mutex_unlock(&web_lease_mutex);
    if(previous!=NULL) {
      /* A takeover unkeys whatever the previous holder was transmitting. */
      log_info("web: %s took control over from %s\n",client->peer_name,previous_name);
      web_control_lost(previous,TRUE);
    }
    if(!acquired)
      return ws_send_error(client,request->id,"control_busy","another browser has control");
    return ws_send_control_state(client,request->id,TRUE);
  }
  if(request->type==WEB_REQUEST_CONTROL_RENEW) {
    if(!web_control_is_owner(client,TRUE))
      return ws_send_error(client,request->id,"control_lost","control is no longer held");
    return ws_send_control_state(client,request->id,TRUE);
  }
  if(request->type==WEB_REQUEST_CONTROL_RELEASE) {
    web_control_release_client(client,FALSE);
    return ws_send_control_state(client,request->id,FALSE);
  }

  if(request->type==WEB_REQUEST_LEASE_ACQUIRE) {
    if(web_radio==NULL || !web_radio->can_transmit || !web_radio->web_tx_enable)
      return ws_send_error(client,request->id,"not_armed","browser PTT is not armed in Configure");
    /* Expire a silent holder before deciding whether this client may take it. */
    web_lease_is_owner(client,FALSE);
    gboolean acquired=FALSE;
    g_mutex_lock(&web_lease_mutex);
    if(web_tx_lease_owner==NULL || web_tx_lease_owner==client) {
      web_tx_lease_owner=client;
      web_tx_lease_deadline_us=g_get_monotonic_time()+WEB_TX_LEASE_TIMEOUT_US;
      acquired=TRUE;
    }
    g_mutex_unlock(&web_lease_mutex);
    if(!acquired)
      return ws_send_error(client,request->id,"lease_busy","another browser holds TX control");
    web_tx_audio_reset();
    g_atomic_int_set(&web_tx_audio_dropped,0);
    g_atomic_int_set(&web_tx_audio_underruns,0);
    return ws_send_lease_state(client,request->id,TRUE);
  }
  if(request->type==WEB_REQUEST_LEASE_RENEW) {
    if(!web_lease_is_owner(client,TRUE))
      return ws_send_error(client,request->id,"lease_lost","TX control lease is no longer held");
    return ws_send_lease_state(client,request->id,TRUE);
  }
  if(request->type==WEB_REQUEST_LEASE_RELEASE) {
    web_lease_release_client(client,FALSE);
    return ws_send_lease_state(client,request->id,FALSE);
  }

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
        WEB_RTC_SESSION *s=web_rtc_session_new(web_rtc_on_signal,web_rtc_on_state,
                                               web_rtc_on_tx_audio,client,
                                               client->rtc_address,
                                               (guint16)web_radio->web_rtc_port);
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
  if(request->type!=WEB_REQUEST_SET_MOX && request->type!=WEB_REQUEST_SET_TUNE &&
     request->type!=WEB_REQUEST_SET_TX_CONTROL) {
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
      /* Any control-role connection may request an emergency unkey (the
         access check above); only the live TX-lease holder can key.  Renewing on a valid TX command prevents a heartbeat racing
         the button from expiring an otherwise active controller. */
      if(request->flag && !web_lease_is_owner(client,TRUE))
        return ws_send_error(client,request->id,"lease_required","take TX control before keying");
      if(request->flag && !web_tx_audio_fresh())
        return ws_send_error(client,request->id,"audio_required","microphone audio is not available");
      g_atomic_int_set(&web_remote_mox,request->flag);
      g_atomic_int_set(&web_remote_tune,FALSE);
      g_atomic_int_set(&web_remote_tune_seen,FALSE);
      g_atomic_int_set(&web_remote_ptt,request->flag);
      g_atomic_int_set(&web_remote_mox_seen,FALSE);
      MOX_STATE *m=g_new(MOX_STATE,1);
      m->radio=web_radio; m->state=request->flag;
      g_idle_add(ext_set_mox,m);
      break;
    }
    case WEB_REQUEST_SET_TUNE: {
      if(web_radio==NULL || !web_radio->can_transmit || !web_radio->web_tx_enable ||
         web_radio->transmitter==NULL)
        return ws_send_error(client,request->id,"not_armed","browser PTT is not armed in Configure");
      /* Tune is an RF-emitting action but does not consume microphone media.
         On is lease-gated; off remains an emergency action for every
         control-role connection. */
      if(request->flag && !web_lease_is_owner(client,TRUE))
        return ws_send_error(client,request->id,"lease_required","take TX control before tuning");
      g_atomic_int_set(&web_remote_tune,request->flag);
      g_atomic_int_set(&web_remote_tune_seen,FALSE);
      g_atomic_int_set(&web_remote_mox,FALSE);
      g_atomic_int_set(&web_remote_mox_seen,FALSE);
      g_atomic_int_set(&web_remote_ptt,request->flag);
      WEB_TUNE_STATE *t=g_new(WEB_TUNE_STATE,1);
      t->radio=web_radio; t->state=request->flag;
      g_idle_add(web_set_tune_idle,t);
      break;
    }
    case WEB_REQUEST_SET_TX_CONTROL: {
      if(web_radio==NULL || !web_radio->can_transmit || !web_radio->web_tx_enable ||
         web_radio->transmitter==NULL)
        return ws_send_error(client,request->id,"not_armed","browser PTT is not armed in Configure");
      if(!web_lease_is_owner(client,TRUE))
        return ws_send_error(client,request->id,"lease_required","take TX control before changing transmit settings");
      gboolean valid=FALSE;
      switch(request->tx_control) {
        case WEB_TX_CTRL_DRIVE:
        case WEB_TX_CTRL_TUNE_DRIVE: valid=request->value>=0 && request->value<=100; break;
        case WEB_TX_CTRL_MIC_GAIN: valid=request->value>=-10 && request->value<=50; break;
        case WEB_TX_CTRL_COMP_LEVEL: valid=request->value>=0 && request->value<=20; break;
        case WEB_TX_CTRL_COMPRESSOR:
        case WEB_TX_CTRL_LEVELER:
        case WEB_TX_CTRL_CESSB: valid=request->value==0 || request->value==1; break;
        default: break;
      }
      if(!valid)
        return ws_send_error(client,request->id,"invalid_value","transmit control value is out of range");
      WEB_TX_CONTROL_CMD *c=g_new(WEB_TX_CONTROL_CMD,1);
      c->radio=web_radio; c->control=request->tx_control; c->value=request->value;
      g_idle_add(web_set_tx_control_idle,c);
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
    case WEB_REQUEST_SET_CONTROL: {
      /* The value range is checked on the GTK thread by ext_set_rx_control,
         which clamps to the matching UI control's own limits. */
      RX_CONTROL *c=g_new(RX_CONTROL,1);
      c->rx=rx; c->control=(int)request->control; c->value=request->value;
      g_idle_add(ext_set_rx_control,c);
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
  gchar *hello=g_strdup_printf(
      "{\"type\":\"hello\",\"protocol\":1,\"server\":\"MacHPSDR\",\"role\":\"%s\","
#ifdef WEBRTC
      "\"capabilities\":[\"control\",\"spectrum\",\"audio\",\"tx_lease\",\"control_lease\"],\"audio_udp_port\":%d}",
#else
      "\"capabilities\":[\"control\",\"spectrum\",\"tx_lease\",\"control_lease\"],\"audio_udp_port\":%d}",
#endif
      web_role_name(client->role),web_radio->web_rtc_port);
  g_atomic_int_set(&client->upgraded,TRUE);
  client->sender=g_thread_new("web-spectrum",web_sender_thread,client);
  gboolean hello_ok=ws_send_text(client,hello);
  g_free(hello);
  if(!hello_ok) return FALSE;
  for(;;) {
    char *payload=NULL;
    size_t len=0;
    int op=tci_ws_io_recv(&client->io,&payload,&len);
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
  /* A dead controlling socket must unkey before any lengthy WebRTC teardown;
     dropping control drops the TX lease too. */
  web_control_release_client(client,FALSE);
  web_lease_release_client(client,FALSE);
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
  web_tls_session_free(client->tls);
  client->tls=NULL;
  if(fd>=0) closesocket(fd);
}

static gpointer web_client_thread(gpointer data) {
  WEB_CLIENT *client=(WEB_CLIENT *)data;
  int fd=client->fd;
  net_set_nonblocking(fd,1);
  char *tls_error=NULL;
  client->tls=web_tls_accept(web_tls,fd,&client->io,&tls_error);
  if(client->tls==NULL) {
    log_debug("web: %s\n",tls_error!=NULL?tls_error:"TLS handshake failed");
    g_free(tls_error);
    web_client_release(client); return NULL;
  }
  char request[WEB_HTTP_MAX+1];
  size_t total=0;
  net_set_rcvtimeo(fd,5000);
  while(total<WEB_HTTP_MAX) {
    gssize n=client->io.read(client->io.context,request+total,WEB_HTTP_MAX-total);
    if(n<=0) { web_client_release(client); return NULL; }
    total+=(size_t)n; request[total]='\0';
    if(strstr(request,"\r\n\r\n")!=NULL) break;
  }
  if(total==WEB_HTTP_MAX || strstr(request,"\r\n\r\n")==NULL) {
    http_error(&client->io,400,"request header is too large\n");
    web_client_release(client); return NULL;
  }

  char method[8], target[256], version[16];
  if(sscanf(request,"%7s %255s %15s",method,target,version)!=3 ||
     strcmp(version,"HTTP/1.1")!=0) {
    http_error(&client->io,400,"invalid request line\n");
    web_client_release(client); return NULL;
  }
  gboolean head=strcmp(method,"HEAD")==0;
  if(strcmp(method,"GET")!=0 && !head) {
    http_error(&client->io,405,"only GET and HEAD are supported\n");
    web_client_release(client); return NULL;
  }
  const char *asset=NULL, *type=NULL;
  gboolean websocket=FALSE;
  if(!web_protocol_route(target,&asset,&type,&websocket)) {
    http_error(&client->io,404,"not found\n");
    web_client_release(client); return NULL;
  }
  gint64 wait_us=0;
  switch(request_login(request,client,&client->role,&wait_us)) {
    case WEB_LOGIN_OK: break;
    case WEB_LOGIN_THROTTLED:
      http_too_many(&client->io,wait_us);
      web_client_release(client); return NULL;
    default:
      http_unauthorized(&client->io);
      web_client_release(client); return NULL;
  }
  if(websocket) {
    char key[128], upgrade[64], protocol[128];
    if(head || header_value(request,"Sec-WebSocket-Key",key,sizeof(key))==NULL ||
       header_value(request,"Upgrade",upgrade,sizeof(upgrade))==NULL ||
       g_ascii_strcasecmp(upgrade,"websocket")!=0 ||
       header_value(request,"Sec-WebSocket-Protocol",protocol,sizeof(protocol))==NULL ||
       !header_has_token(protocol,"machpsdr.v1")) {
      http_error(&client->io,426,"WebSocket protocol machpsdr.v1 is required\n");
    } else if(!origin_allowed(request)) {
      http_error(&client->io,403,"WebSocket origin is not allowed\n");
    } else if(tci_ws_io_accept(&client->io,key,"machpsdr.v1")) {
#ifdef WEBRTC
      client->rtc_address[0]='\0';
      if(!request_ipv4_address(request,client->rtc_address,sizeof(client->rtc_address)))
        log_debug("web: could not resolve request Host for the WebRTC UDP candidate\n");
#endif
      net_set_rcvtimeo(fd,0);
      net_set_sndtimeo(fd,250);
      websocket_session(client);
    }
    web_client_release(client); return NULL;
  }

  gsize len=0;
  gchar *contents=read_asset(asset,&len);
  if(contents==NULL) http_error(&client->io,500,"web asset is missing\n");
  else {
    http_send(&client->io,200,type,contents,len,head);
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
  addr.sin_addr.s_addr=htonl(INADDR_ANY);
  addr.sin_port=htons((guint16)web_port);
  if(bind(fd,(struct sockaddr *)&addr,sizeof(addr))<0 || listen(fd,8)<0) {
    closesocket(fd); goto failed;
  }
  web_listen=fd;
  g_snprintf(web_status_line,sizeof(web_status_line),"HTTPS TCP %d, WebRTC UDP %d%s",
             web_port,web_radio!=NULL?web_radio->web_rtc_port:0,
             web_radio!=NULL && web_radio->web_auth_enable?" (password protected)":" (open)");
  log_info("web: listening on %s\n",web_status_line);

  while(g_atomic_int_get(&web_running)) {
    struct sockaddr_in peer;
    socklen_t peer_len=sizeof(peer);
    memset(&peer,0,sizeof(peer));
    int client_fd=accept(fd,(struct sockaddr *)&peer,&peer_len);
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
      slot->peer=peer.sin_addr.s_addr;
      slot->role=WEB_ROLE_VIEW;           /* until the request authenticates */
      if(inet_ntop(AF_INET,&peer.sin_addr,slot->peer_name,(socklen_t)sizeof(slot->peer_name))==NULL)
        g_strlcpy(slot->peer_name,"?",sizeof(slot->peer_name));
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
  g_snprintf(web_status_line,sizeof(web_status_line),"failed to listen on LAN port %d",web_port);
  log_error("web: %s\n",web_status_line);
  g_atomic_int_set(&web_running,0);
  return NULL;
}

void web_server_init(RADIO *radio) {
  web_server_stop();
  web_radio=radio;
  if(!web_sync_initialized) {
    g_mutex_init(&web_lease_mutex);
    g_mutex_init(&web_auth_mutex);
    web_auth_throttle_init(&web_auth_throttle);
    g_mutex_init(&web_tx_audio_mutex);
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
  char *tls_error=NULL;
  web_tls=web_tls_context_new(web_radio->web_tls_bundle,&tls_error);
  if(web_tls==NULL) {
    g_snprintf(web_status_line,sizeof(web_status_line),"TLS unavailable: %s",
               tls_error!=NULL?tls_error:"certificate error");
    log_error("web: %s\n",web_status_line);
    g_free(tls_error); return;
  }
  g_atomic_int_set(&web_running,1);
  g_mutex_lock(&web_lease_mutex);
  web_tx_lease_owner=NULL;
  web_tx_lease_deadline_us=0;
  web_control_owner=NULL;
  web_control_deadline_us=0;
  g_mutex_unlock(&web_lease_mutex);
  g_atomic_int_set(&web_remote_ptt,FALSE);
  g_atomic_int_set(&web_remote_mox,FALSE);
  g_atomic_int_set(&web_remote_mox_seen,FALSE);
  g_atomic_int_set(&web_remote_tune,FALSE);
  g_atomic_int_set(&web_remote_tune_seen,FALSE);
  web_tx_audio_reset();
  g_snprintf(web_status_line,sizeof(web_status_line),"starting HTTPS on port %d",web_port);
  web_thread=g_thread_new("web-server",web_server_thread,NULL);
  /* Pushed-state detector on the GTK main loop (start is always called on the
     GTK thread: web_server_init in create_radio, or the Configure dialog). */
  web_last_state_push_us=0;
  web_state_source=g_timeout_add(WEB_STATE_TICK_MS,web_state_tick,NULL);
}

void web_server_stop(void) {
  if(!g_atomic_int_get(&web_running) && web_thread==NULL) return;
  g_atomic_int_set(&web_running,0);
  /* Stop runs on the GTK thread, so the detector cannot be mid-tick here. */
  if(web_state_source!=0) { g_source_remove(web_state_source); web_state_source=0; }
  if(web_last_discrete!=NULL) { g_string_free(web_last_discrete,TRUE); web_last_discrete=NULL; }
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
  g_mutex_lock(&web_lease_mutex);
  web_tx_lease_owner=NULL;
  web_tx_lease_deadline_us=0;
  web_control_owner=NULL;
  web_control_deadline_us=0;
  g_mutex_unlock(&web_lease_mutex);
  web_remote_unkey();
  g_mutex_lock(&web_spectrum_mutex);
  for(int i=0;i<MAX_RECEIVERS;i++) {
    if(web_latest_spectrum[i]!=NULL) g_bytes_unref(web_latest_spectrum[i]);
    web_latest_spectrum[i]=NULL;
  }
  g_mutex_unlock(&web_spectrum_mutex);
  web_tls_context_free(web_tls);
  web_tls=NULL;
  g_strlcpy(web_status_line,"stopped",sizeof(web_status_line));
}

/* Broadcast one text frame to every upgraded client.  The client set is
   snapshotted under web_clients_mutex, then the mutex is dropped before any
   send: client_send() is serialized per client by send_mutex and re-checks
   closing/fd, so a client released right after the snapshot is a safe no-op, and
   a (up-to-250 ms) blocking send never stalls the accept thread or a sibling
   broadcast. */
static void web_broadcast_text(const char *text, gsize len) {
  WEB_CLIENT *targets[WEB_MAX_CLIENTS];
  int n=0;
  g_mutex_lock(&web_clients_mutex);
  for(int i=0;i<WEB_MAX_CLIENTS;i++) {
    WEB_CLIENT *c=&web_clients[i];
    if(c->fd>=0 && g_atomic_int_get(&c->upgraded) && !g_atomic_int_get(&c->closing))
      targets[n++]=c;
  }
  g_mutex_unlock(&web_clients_mutex);
  for(int i=0;i<n;i++) client_send(targets[i],1,text,len);
}

/* GTK-thread change detector (Phase 4): the browser no longer polls get_state.
   Each tick builds the discrete signature; a push goes out whenever it changed
   (frequency, mode, MOX, active RX, lock, step, filter, display window, receiver
   add/remove) and otherwise every WEB_METER_PUSH_US to keep the S-meter live.
   It is self-correcting -- a new mutation site cannot be "left off a list" of
   notify calls -- and touches no socket itself (web_broadcast_text hands each
   frame to the client's own serialized send path). Only a real discrete change
   advances web_revision, so the revision counts state changes, not meter ticks. */
static gboolean web_state_tick(gpointer data) {
  (void)data;
  if(!g_atomic_int_get(&web_running)) { web_state_source=0; return G_SOURCE_REMOVE; }
  if(web_radio==NULL || web_radio->discovered==NULL) return G_SOURCE_CONTINUE;

  /* A heartbeat timeout, or the operator withdrawing the session-only desktop
     arm, revokes TX control and schedules an emergency unkey. */
  WEB_CLIENT *lease_owner;
  /* A silent control holder expires here too, telling it and dropping its TX. */
  g_mutex_lock(&web_lease_mutex);
  lease_owner=web_control_owner;
  g_mutex_unlock(&web_lease_mutex);
  if(lease_owner!=NULL) web_control_is_owner(lease_owner,FALSE);
  g_mutex_lock(&web_lease_mutex);
  lease_owner=web_tx_lease_owner;
  g_mutex_unlock(&web_lease_mutex);
  if(lease_owner!=NULL) {
    web_lease_is_owner(lease_owner,FALSE);
    if(!web_radio->web_tx_enable || !web_radio->can_transmit) {
      g_mutex_lock(&web_lease_mutex);
      lease_owner=web_tx_lease_owner;
      g_mutex_unlock(&web_lease_mutex);
      if(lease_owner!=NULL) web_lease_release_client(lease_owner,TRUE);
    }
  }
  if(g_atomic_int_get(&web_tx_audio_active) && !web_tx_audio_fresh()) {
    g_atomic_int_set(&web_tx_audio_active,FALSE);
    if(g_atomic_int_get(&web_remote_mox)) {
      g_mutex_lock(&web_lease_mutex);
      lease_owner=web_tx_lease_owner;
      g_mutex_unlock(&web_lease_mutex);
      if(lease_owner!=NULL) web_lease_release_client(lease_owner,TRUE);
      else web_remote_unkey();
    }
  }
  if(g_atomic_int_get(&web_remote_mox)) {
    if(web_radio->mox) g_atomic_int_set(&web_remote_mox_seen,TRUE);
    else if(g_atomic_int_get(&web_remote_mox_seen)) web_remote_unkey();
  }
  if(g_atomic_int_get(&web_remote_tune)) {
    if(web_radio->tune) g_atomic_int_set(&web_remote_tune_seen,TRUE);
    else if(g_atomic_int_get(&web_remote_tune_seen)) web_remote_unkey();
  }

  gboolean have=FALSE;
  g_mutex_lock(&web_clients_mutex);
  for(int i=0;i<WEB_MAX_CLIENTS;i++)
    if(web_clients[i].fd>=0 && g_atomic_int_get(&web_clients[i].upgraded) &&
       !g_atomic_int_get(&web_clients[i].closing)) { have=TRUE; break; }
  g_mutex_unlock(&web_clients_mutex);
  if(!have) return G_SOURCE_CONTINUE;

  GString *sig=g_string_sized_new(256);
  append_state_body(sig,FALSE);
  gboolean changed=(web_last_discrete==NULL) ||
                   strcmp(sig->str,web_last_discrete->str)!=0;
  gint64 now=g_get_monotonic_time();
  gboolean meter_due=(now-web_last_state_push_us)>=WEB_METER_PUSH_US;
  if(changed || meter_due) {
    if(changed) {
      if(web_last_discrete!=NULL) g_string_free(web_last_discrete,TRUE);
      web_last_discrete=sig;
      sig=NULL;
      g_atomic_int_add(&web_revision,1);
    }
    GString *frame=g_string_sized_new(1024);
    build_state_frame(frame,0,g_atomic_int_get(&web_revision));
    web_broadcast_text(frame->str,frame->len);
    g_string_free(frame,TRUE);
    web_last_state_push_us=now;
  }
  if(sig!=NULL) g_string_free(sig,TRUE);
  return G_SOURCE_CONTINUE;
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

gboolean web_server_tx_audio_active(void) {
  return g_atomic_int_get(&web_tx_audio_active) &&
         g_atomic_int_get(&web_remote_mox) && web_radio!=NULL && web_radio->mox;
}

float web_server_tx_audio_next_sample(void) {
  float sample=0.0f;
  int tail=g_atomic_int_get(&web_tx_audio_tail);
  if(tail!=g_atomic_int_get(&web_tx_audio_head)) {
    sample=web_tx_audio_ring[tail];
    g_atomic_int_set(&web_tx_audio_tail,(tail+1)%WEB_TX_AUDIO_RING);
  } else if(g_atomic_int_get(&web_tx_audio_active)) {
    g_atomic_int_inc(&web_tx_audio_underruns);
  }
  return sample;
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

const char *web_server_tls_fingerprint(void) {
  return web_tls_context_fingerprint(web_tls);
}

const char *web_server_tls_bundle(void) {
  return web_tls_context_bundle(web_tls);
}
