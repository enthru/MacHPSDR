#include "web_tls.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string.h>

struct web_tls_context {
  SSL_CTX *ssl;
  char *bundle;
  char fingerprint[96];
};

struct web_tls_session { SSL *ssl; GMutex mutex; int fd; };

static void wait_io(WEB_TLS_SESSION *session, gboolean write_ready) {
  fd_set read_set,write_set;
  FD_ZERO(&read_set); FD_ZERO(&write_set);
  FD_SET(session->fd,write_ready?&write_set:&read_set);
  struct timeval timeout={0,100000};
  select(session->fd+1,write_ready?NULL:&read_set,write_ready?&write_set:NULL,NULL,&timeout);
}

static char *ssl_error(const char *prefix) {
  unsigned long code=ERR_get_error();
  char detail[256]="unknown TLS error";
  if(code!=0) ERR_error_string_n(code,detail,sizeof(detail));
  return g_strdup_printf("%s: %s",prefix,detail);
}

static gboolean generate_bundle(const char *path, char **error) {
  gboolean ok=FALSE;
  char *temporary=g_strdup_printf("%s.tmp",path);
  EVP_PKEY_CTX *key_ctx=NULL;
  EVP_PKEY *key=NULL;
  X509 *cert=NULL;
  FILE *file=NULL;
  key_ctx=EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,NULL);
  if(key_ctx==NULL || EVP_PKEY_keygen_init(key_ctx)<=0 ||
     EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx,2048)<=0 ||
     EVP_PKEY_keygen(key_ctx,&key)<=0) goto done;
  cert=X509_new();
  if(cert==NULL || X509_set_version(cert,2)!=1) goto done;
  guint64 serial=0;
  if(RAND_bytes((unsigned char *)&serial,sizeof(serial))!=1) goto done;
  serial&=G_MAXINT64;
  if(serial==0) serial=1;
  ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert),serial);
  X509_gmtime_adj(X509_getm_notBefore(cert),-300);
  X509_gmtime_adj(X509_getm_notAfter(cert),60L*60L*24L*3650L);
  if(X509_set_pubkey(cert,key)!=1) goto done;
  X509_NAME *name=X509_NAME_new();
  if(name==NULL || X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,
       (const unsigned char *)"MacHPSDR browser remote",-1,-1,0)!=1 ||
     X509_set_subject_name(cert,name)!=1 || X509_set_issuer_name(cert,name)!=1) {
    X509_NAME_free(name); goto done;
  }
  X509_NAME_free(name);
  X509V3_CTX ext_ctx;
  X509V3_set_ctx(&ext_ctx,cert,cert,NULL,NULL,0);
  X509_EXTENSION *ext=X509V3_EXT_conf_nid(NULL,&ext_ctx,NID_subject_alt_name,
                                          "DNS:localhost,IP:127.0.0.1");
  if(ext==NULL || X509_add_ext(cert,ext,-1)!=1) { X509_EXTENSION_free(ext); goto done; }
  X509_EXTENSION_free(ext);
  if(X509_sign(cert,key,EVP_sha256())<=0) goto done;
  file=g_fopen(temporary,"wb");
  if(file==NULL || PEM_write_X509(file,cert)!=1 ||
     PEM_write_PrivateKey(file,key,NULL,NULL,0,NULL,NULL)!=1) goto done;
  if(fclose(file)!=0) { file=NULL; goto done; }
  file=NULL;
  g_chmod(temporary,0600);
  if(g_rename(temporary,path)!=0) goto done;
  ok=TRUE;
done:
  if(file!=NULL) fclose(file);
  if(!ok) {
    g_unlink(temporary);
    if(error!=NULL) *error=ssl_error("could not create self-signed certificate");
  }
  X509_free(cert); EVP_PKEY_free(key); EVP_PKEY_CTX_free(key_ctx);
  g_free(temporary);
  return ok;
}

WEB_TLS_CONTEXT *web_tls_context_new(const char *bundle_path, char **error) {
  if(error!=NULL) *error=NULL;
  char *generated=NULL;
  const char *path=bundle_path;
  if(path==NULL || *path=='\0') {
    char *dir=g_build_filename(g_get_user_data_dir(),"machpsdr",NULL);
    if(g_mkdir_with_parents(dir,0700)!=0) {
      if(error!=NULL) *error=g_strdup("could not create the MacHPSDR data directory");
      g_free(dir); return NULL;
    }
    generated=g_build_filename(dir,"web-server.pem",NULL);
    g_free(dir); path=generated;
    if(!g_file_test(path,G_FILE_TEST_IS_REGULAR) && !generate_bundle(path,error)) {
      g_free(generated); return NULL;
    }
  }
  WEB_TLS_CONTEXT *context=g_new0(WEB_TLS_CONTEXT,1);
  context->bundle=g_strdup(path);
  context->ssl=SSL_CTX_new(TLS_server_method());
  if(context->ssl==NULL || SSL_CTX_set_min_proto_version(context->ssl,TLS1_2_VERSION)!=1 ||
     SSL_CTX_use_certificate_chain_file(context->ssl,path)!=1 ||
     SSL_CTX_use_PrivateKey_file(context->ssl,path,SSL_FILETYPE_PEM)!=1 ||
     SSL_CTX_check_private_key(context->ssl)!=1) {
    if(error!=NULL) *error=ssl_error("could not load TLS PEM bundle");
    web_tls_context_free(context); g_free(generated); return NULL;
  }
  X509 *cert=SSL_CTX_get0_certificate(context->ssl);
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int len=0;
  if(cert!=NULL && X509_digest(cert,EVP_sha256(),digest,&len)==1) {
    char *p=context->fingerprint;
    for(unsigned int i=0;i<len;i++) p+=g_snprintf(p,
      sizeof(context->fingerprint)-(size_t)(p-context->fingerprint),"%s%02X",i?":":"",digest[i]);
  }
  g_free(generated);
  return context;
}

void web_tls_context_free(WEB_TLS_CONTEXT *context) {
  if(context==NULL) return;
  SSL_CTX_free(context->ssl); g_free(context->bundle); g_free(context);
}
const char *web_tls_context_fingerprint(WEB_TLS_CONTEXT *context) { return context?context->fingerprint:""; }
const char *web_tls_context_bundle(WEB_TLS_CONTEXT *context) { return context?context->bundle:""; }

static gssize tls_read(void *opaque, void *buf, size_t len) {
  WEB_TLS_SESSION *session=opaque;
  for(;;) {
    g_mutex_lock(&session->mutex);
    int n=SSL_read(session->ssl,buf,(int)MIN(len,(size_t)G_MAXINT));
    int e=SSL_get_error(session->ssl,n);
    g_mutex_unlock(&session->mutex);
    if(n>0) return n;
    if(e!=SSL_ERROR_WANT_READ && e!=SSL_ERROR_WANT_WRITE) { errno=EIO; return -1; }
    wait_io(session,e==SSL_ERROR_WANT_WRITE);
  }
}
static gssize tls_write(void *opaque, const void *buf, size_t len) {
  WEB_TLS_SESSION *session=opaque;
  for(;;) {
    g_mutex_lock(&session->mutex);
    int n=SSL_write(session->ssl,buf,(int)MIN(len,(size_t)G_MAXINT));
    int e=SSL_get_error(session->ssl,n);
    g_mutex_unlock(&session->mutex);
    if(n>0) return n;
    if(e!=SSL_ERROR_WANT_READ && e!=SSL_ERROR_WANT_WRITE) { errno=EIO; return -1; }
    wait_io(session,e==SSL_ERROR_WANT_WRITE);
  }
}

WEB_TLS_SESSION *web_tls_accept(WEB_TLS_CONTEXT *context, int fd,
                                TCI_WS_IO *io, char **error) {
  if(error!=NULL) *error=NULL;
  if(context==NULL || io==NULL) return NULL;
  WEB_TLS_SESSION *session=g_new0(WEB_TLS_SESSION,1);
  g_mutex_init(&session->mutex);
  session->fd=fd;
  session->ssl=SSL_new(context->ssl);
  if(session->ssl==NULL || SSL_set_fd(session->ssl,fd)!=1) {
    if(error!=NULL) *error=ssl_error("TLS handshake failed");
    web_tls_session_free(session); return NULL;
  }
  gint64 deadline=g_get_monotonic_time()+5*G_USEC_PER_SEC;
  for(;;) {
    int rc=SSL_accept(session->ssl);
    if(rc==1) break;
    int e=SSL_get_error(session->ssl,rc);
    if((e!=SSL_ERROR_WANT_READ && e!=SSL_ERROR_WANT_WRITE) ||
       g_get_monotonic_time()>=deadline) {
      if(error!=NULL) *error=ssl_error("TLS handshake failed");
      web_tls_session_free(session); return NULL;
    }
    wait_io(session,e==SSL_ERROR_WANT_WRITE);
  }
  io->context=session; io->read=tls_read; io->write=tls_write;
  return session;
}

void web_tls_session_free(WEB_TLS_SESSION *session) {
  if(session==NULL) return;
  if(session->ssl!=NULL) { SSL_shutdown(session->ssl); SSL_free(session->ssl); }
  g_mutex_clear(&session->mutex);
  g_free(session);
}
