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

#define WEB_MAX_CLIENTS 8
#define WEB_HTTP_MAX 8192

typedef struct {
  int fd;
} WEB_CLIENT;

static RADIO *web_radio;
static volatile gint web_running;
static GThread *web_thread;
static int web_listen=-1;
static int web_port=WEB_DEFAULT_PORT;
static GMutex web_clients_mutex;
static WEB_CLIENT web_clients[WEB_MAX_CLIENTS];
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

static gboolean ws_send_text(int fd, const char *text) {
  return tci_ws_send_blocking(fd,1,text,strlen(text));
}

static gboolean ws_send_error(int fd, gint64 id, const char *code, const char *message) {
  char line[512];
  // code/message are constants controlled by this file, never client text.
  g_snprintf(line,sizeof(line),
      "{\"type\":\"error\",\"id\":%lld,\"code\":\"%s\",\"message\":\"%s\"}",
      (long long)id,code,message);
  return ws_send_text(fd,line);
}

static gboolean ws_send_state(int fd, gint64 id) {
  if(web_radio==NULL || web_radio->discovered==NULL)
    return ws_send_error(fd,id,"radio_unavailable","radio is not ready");
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
    if(!first) g_string_append_c(s,',');
    first=FALSE;
    int mode=(rx->mode_a>=0 && rx->mode_a<MODES)?rx->mode_a:USB;
    g_string_append_printf(s,
        "{\"id\":%d,\"frequency\":%lld,\"frequency_b\":%lld,"
        "\"mode\":%d,\"mode_name\":\"%s\",\"meter_db\":%.1f,\"locked\":%s}",
        i,(long long)receiver_tuned_frequency(rx),(long long)rx->frequency_b,
        mode,mode_string[mode],meter,rx->locked?"true":"false");
  }
  g_string_append(s,"]}");
  gboolean ok=tci_ws_send_blocking(fd,1,s->str,s->len);
  g_string_free(s,TRUE);
  return ok;
}

static gboolean dispatch_request(int fd, const WEB_REQUEST *request) {
  if(request->type==WEB_REQUEST_GET_STATE) return ws_send_state(fd,request->id);

  RECEIVER *rx=NULL;
  if(request->type!=WEB_REQUEST_SET_MOX) {
    rx=visible_rx(request->rx);
    if(rx==NULL) return ws_send_error(fd,request->id,"invalid_receiver","receiver is not available");
  }
  switch(request->type) {
    case WEB_REQUEST_SET_FREQUENCY: {
      long long maxf=receiver_max_frequency(rx);
      if(request->value<=0 || request->value>maxf)
        return ws_send_error(fd,request->id,"invalid_value","frequency is out of range");
      RX_FREQUENCY *f=g_new(RX_FREQUENCY,1);
      f->rx=rx; f->frequency=request->value;
      g_idle_add(ext_set_frequency_a,f);
      break;
    }
    case WEB_REQUEST_SET_MODE: {
      if(request->value<0 || request->value>=MODES)
        return ws_send_error(fd,request->id,"invalid_value","mode is out of range");
      MODE *m=g_new0(MODE,1);
      m->rx=rx; m->mode_a=(int)request->value;
      g_idle_add(ext_set_mode,m);
      break;
    }
    case WEB_REQUEST_SET_MOX: {
      if(web_radio==NULL || !web_radio->can_transmit || !web_radio->web_tx_enable)
        return ws_send_error(fd,request->id,"not_armed","browser PTT is not armed in Configure");
      MOX_STATE *m=g_new(MOX_STATE,1);
      m->radio=web_radio; m->state=request->flag;
      g_idle_add(ext_set_mox,m);
      break;
    }
    default:
      return ws_send_error(fd,request->id,"invalid_request","unsupported request");
  }
  char ack[96];
  g_snprintf(ack,sizeof(ack),"{\"type\":\"ack\",\"id\":%lld}",(long long)request->id);
  return ws_send_text(fd,ack);
}

static gboolean websocket_session(int fd) {
  const char *hello=
      "{\"type\":\"hello\",\"protocol\":1,\"server\":\"MacHPSDR\","
      "\"capabilities\":[\"control\"]}";
  if(!ws_send_text(fd,hello)) return FALSE;
  for(;;) {
    char *payload=NULL;
    size_t len=0;
    int op=tci_ws_recv(fd,&payload,&len);
    if(op<0) { g_free(payload); return TRUE; }
    if(op==8) { g_free(payload); return TRUE; }
    if(op==9) {
      gboolean ok=tci_ws_send_blocking(fd,10,payload,len);
      g_free(payload);
      if(!ok) return FALSE;
      continue;
    }
    if(op!=1) { g_free(payload); continue; }
    WEB_REQUEST request;
    char error[128];
    if(!web_protocol_parse(payload,len,&request,error,sizeof(error))) {
      g_free(payload);
      if(!ws_send_error(fd,0,"invalid_request",error)) return FALSE;
      continue;
    }
    g_free(payload);
    if(!dispatch_request(fd,&request)) return FALSE;
  }
}

static void web_client_release(WEB_CLIENT *client) {
  g_mutex_lock(&web_clients_mutex);
  int fd=client->fd;
  client->fd=-1;
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
      websocket_session(fd);
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
#ifdef SO_NOSIGPIPE
    setsockopt(client_fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    WEB_CLIENT *slot=NULL;
    g_mutex_lock(&web_clients_mutex);
    for(int i=0;i<WEB_MAX_CLIENTS;i++) if(web_clients[i].fd<0) {
      web_clients[i].fd=client_fd; slot=&web_clients[i]; break;
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
  for(int i=0;i<WEB_MAX_CLIENTS;i++) web_clients[i].fd=-1;
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
  for(int i=0;i<WEB_MAX_CLIENTS;i++) if(web_clients[i].fd>=0)
    shutdown(web_clients[i].fd,SHUT_RDWR);
  g_mutex_unlock(&web_clients_mutex);
  if(web_thread!=NULL) { g_thread_join(web_thread); web_thread=NULL; }
  g_strlcpy(web_status_line,"stopped",sizeof(web_status_line));
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
