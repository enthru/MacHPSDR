#include <glib.h>
#include <openssl/ssl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "web_tls.h"

typedef struct { WEB_TLS_CONTEXT *context; int fd; gboolean ok; } SERVER;

static gpointer server_thread(gpointer opaque) {
  SERVER *server=opaque;
  TCI_WS_IO io={0}; char *error=NULL;
  WEB_TLS_SESSION *session=web_tls_accept(server->context,server->fd,&io,&error);
  char input[5]={0};
  server->ok=session!=NULL && io.read(io.context,input,4)==4 &&
             memcmp(input,"ping",4)==0 && tci_ws_io_send_all(&io,"pong",4);
  g_free(error); web_tls_session_free(session); close(server->fd); return NULL;
}

int main(void) {
  char *error=NULL;
  WEB_TLS_CONTEXT *context=web_tls_context_new(NULL,&error);
  if(context==NULL) { g_printerr("context: %s\n",error); g_free(error); return 1; }
  if(*web_tls_context_fingerprint(context)=='\0') return 2;
  int pair[2];
  if(socketpair(AF_UNIX,SOCK_STREAM,0,pair)!=0) return 3;
  SERVER server={context,pair[0],FALSE};
  GThread *thread=g_thread_new("tls-test",server_thread,&server);
  SSL_CTX *client_context=SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_verify(client_context,SSL_VERIFY_NONE,NULL);
  SSL *client=SSL_new(client_context);
  SSL_set_fd(client,pair[1]);
  char reply[5]={0};
  gboolean client_ok=SSL_connect(client)==1 && SSL_write(client,"ping",4)==4 &&
                     SSL_read(client,reply,4)==4 && memcmp(reply,"pong",4)==0;
  SSL_shutdown(client); SSL_free(client); SSL_CTX_free(client_context); close(pair[1]);
  g_thread_join(thread);
  web_tls_context_free(context);
  g_print("web_tls_offline: %s\n",client_ok && server.ok?"PASS":"FAIL");
  return client_ok && server.ok?0:4;
}
