/* Browser-remote protocol parser and fixed HTTP router. */
#include "web_protocol.h"

#include <math.h>
#include <string.h>

#define WEB_AUTH_DIGEST_SIZE 32

typedef enum { JV_NONE, JV_STRING, JV_INTEGER, JV_BOOL } JSON_VALUE_TYPE;

typedef struct {
  JSON_VALUE_TYPE type;
  char string[64];
  gint64 integer;
  gboolean boolean;
} JSON_VALUE;

static void set_error(char *error, size_t len, const char *message) {
  if(error!=NULL && len>0) g_strlcpy(error,message,len);
}

static void skip_ws(const char **p, const char *end) {
  while(*p<end && g_ascii_isspace(**p)) (*p)++;
}

static gboolean parse_string(const char **p, const char *end, char *out, size_t out_len) {
  size_t n=0;
  if(*p>=end || **p!='\"') return FALSE;
  (*p)++;
  while(*p<end && **p!='\"') {
    unsigned char c=(unsigned char)**p;
    // Protocol keys/actions are ASCII. Reject escapes and controls rather than
    // accidentally accepting two spellings of a command name.
    if(c=='\\' || c<0x20 || n+1>=out_len) return FALSE;
    out[n++]=(char)c;
    (*p)++;
  }
  if(*p>=end || **p!='\"') return FALSE;
  (*p)++;
  out[n]='\0';
  return TRUE;
}

static gboolean parse_value(const char **p, const char *end, JSON_VALUE *v) {
  memset(v,0,sizeof(*v));
  if(*p>=end) return FALSE;
  if(**p=='\"') {
    v->type=JV_STRING;
    return parse_string(p,end,v->string,sizeof(v->string));
  }
  if(end-*p>=4 && strncmp(*p,"true",4)==0) {
    *p+=4; v->type=JV_BOOL; v->boolean=TRUE; return TRUE;
  }
  if(end-*p>=5 && strncmp(*p,"false",5)==0) {
    *p+=5; v->type=JV_BOOL; v->boolean=FALSE; return TRUE;
  }
  if(**p=='-' || g_ascii_isdigit(**p)) {
    const char *start=*p;
    if(**p=='-') (*p)++;
    if(*p>=end || !g_ascii_isdigit(**p)) return FALSE;
    while(*p<end && g_ascii_isdigit(**p)) (*p)++;
    char buf[32];
    size_t n=(size_t)(*p-start);
    if(n==0 || n>=sizeof(buf)) return FALSE;
    memcpy(buf,start,n); buf[n]='\0';
    char *tail=NULL;
    v->integer=g_ascii_strtoll(buf,&tail,10);
    if(tail==NULL || *tail!='\0') return FALSE;
    v->type=JV_INTEGER;
    return TRUE;
  }
  return FALSE;
}

/* Validate a JSON string and record its raw (still-escaped) span, advancing p
   past the closing quote.  Nothing is allocated, so an in-loop parse failure can
   never leak a large SDP: the owning request only allocates in its terminal
   branch, from this span, while the source text is still alive. */
static gboolean scan_string(const char **p, const char *end,
                            const char **tok, size_t *tok_len) {
  if(*p>=end || **p!='\"') return FALSE;
  (*p)++;
  const char *start=*p;
  while(*p<end && **p!='\"') {
    unsigned char c=(unsigned char)**p;
    if(c=='\\') {
      (*p)++;
      if(*p>=end) return FALSE;   /* dangling escape */
      (*p)++;                      /* skip the escaped char; decoded later */
      continue;
    }
    if(c<0x20) return FALSE;       /* raw control char */
    (*p)++;
  }
  if(*p>=end || **p!='\"') return FALSE;
  *tok=start; *tok_len=(size_t)(*p-start);
  (*p)++;                          /* past closing quote */
  return TRUE;
}

static int hex_value(char c) {
  if(c>='0' && c<='9') return c-'0';
  if(c>='a' && c<='f') return c-'a'+10;
  if(c>='A' && c<='F') return c-'A'+10;
  return -1;
}

/* Decode a JSON string span into a newly allocated UTF-8 string; NULL on a bad
   escape.  SDP uses CRLF, so \r/\n/\" are the escapes that actually occur. */
static gchar *json_unescape(const char *s, size_t len) {
  GString *out=g_string_sized_new(len+1);
  for(size_t i=0;i<len;i++) {
    char c=s[i];
    if(c!='\\') { g_string_append_c(out,c); continue; }
    if(++i>=len) { g_string_free(out,TRUE); return NULL; }
    switch(s[i]) {
      case '\"': g_string_append_c(out,'\"'); break;
      case '\\': g_string_append_c(out,'\\'); break;
      case '/':  g_string_append_c(out,'/'); break;
      case 'b':  g_string_append_c(out,'\b'); break;
      case 'f':  g_string_append_c(out,'\f'); break;
      case 'n':  g_string_append_c(out,'\n'); break;
      case 'r':  g_string_append_c(out,'\r'); break;
      case 't':  g_string_append_c(out,'\t'); break;
      case 'u': {
        if(i+4>=len) { g_string_free(out,TRUE); return NULL; }
        int h0=hex_value(s[i+1]),h1=hex_value(s[i+2]);
        int h2=hex_value(s[i+3]),h3=hex_value(s[i+4]);
        if(h0<0||h1<0||h2<0||h3<0) { g_string_free(out,TRUE); return NULL; }
        gunichar cp=(gunichar)((h0<<12)|(h1<<8)|(h2<<4)|h3);
        /* Surrogates are not expected in SDP/candidates; reject rather than
           mis-decode a split pair. */
        if(cp>=0xD800 && cp<=0xDFFF) { g_string_free(out,TRUE); return NULL; }
        g_string_append_unichar(out,cp);
        i+=4;
        break;
      }
      default: g_string_free(out,TRUE); return NULL;
    }
  }
  return g_string_free(out,FALSE);
}

gchar *web_json_escape(const char *s) {
  if(s==NULL) return g_strdup("");
  GString *out=g_string_new(NULL);
  for(const unsigned char *p=(const unsigned char *)s; *p!='\0'; p++) {
    switch(*p) {
      case '\"': g_string_append(out,"\\\""); break;
      case '\\': g_string_append(out,"\\\\"); break;
      case '\b': g_string_append(out,"\\b"); break;
      case '\f': g_string_append(out,"\\f"); break;
      case '\n': g_string_append(out,"\\n"); break;
      case '\r': g_string_append(out,"\\r"); break;
      case '\t': g_string_append(out,"\\t"); break;
      default:
        if(*p<0x20) g_string_append_printf(out,"\\u%04x",*p);
        else g_string_append_c(out,(char)*p);
    }
  }
  return g_string_free(out,FALSE);
}

void web_request_clear(WEB_REQUEST *req) {
  if(req==NULL) return;
  g_clear_pointer(&req->sdp,g_free);
  g_clear_pointer(&req->candidate,g_free);
  g_clear_pointer(&req->mid,g_free);
}

gboolean web_protocol_parse(const char *text, size_t len, WEB_REQUEST *out,
                            char *error, size_t error_len) {
  if(out==NULL) return FALSE;
  memset(out,0,sizeof(*out));
  out->type=WEB_REQUEST_INVALID;
  out->rx=-1;
  if(text==NULL || len==0 || len>WEB_MAX_TEXT_MESSAGE) {
    set_error(error,error_len,"message is empty or too large");
    return FALSE;
  }

  const char *p=text, *end=text+len;
  char type[32]="", action[64]="";
  char profile[32]="";
  gboolean have_type=FALSE, have_action=FALSE;
  gboolean have_id=FALSE, have_rx=FALSE, have_value=FALSE;
  gboolean have_low=FALSE, have_high=FALSE, have_profile=FALSE;
  gboolean have_sdp=FALSE, have_candidate=FALSE, have_mid=FALSE;
  const char *sdp_tok=NULL, *cand_tok=NULL, *mid_tok=NULL;
  size_t sdp_len=0, cand_len=0, mid_len=0;
  skip_ws(&p,end);
  if(p>=end || *p!='{') { set_error(error,error_len,"expected JSON object"); return FALSE; }
  p++;
  for(;;) {
    char key[64];
    JSON_VALUE v;
    skip_ws(&p,end);
    if(p<end && *p=='}') { p++; break; }
    if(!parse_string(&p,end,key,sizeof(key))) { set_error(error,error_len,"invalid object key"); return FALSE; }
    skip_ws(&p,end);
    if(p>=end || *p!=':') { set_error(error,error_len,"expected colon"); return FALSE; }
    p++; skip_ws(&p,end);

    /* sdp/candidate/mid are large and escaped (SDP carries CRLF); record the
       span now and decode it only in the terminal rtc_* branch, so an in-loop
       failure never allocates. */
    if(strcmp(key,"sdp")==0 || strcmp(key,"candidate")==0 || strcmp(key,"mid")==0) {
      const char *tok=NULL; size_t tl=0;
      if(!scan_string(&p,end,&tok,&tl)) { set_error(error,error_len,"invalid value"); return FALSE; }
      if(strcmp(key,"sdp")==0) {
        if(have_sdp) { set_error(error,error_len,"duplicate sdp"); return FALSE; }
        sdp_tok=tok; sdp_len=tl; have_sdp=TRUE;
      } else if(strcmp(key,"candidate")==0) {
        if(have_candidate) { set_error(error,error_len,"duplicate candidate"); return FALSE; }
        cand_tok=tok; cand_len=tl; have_candidate=TRUE;
      } else {
        if(have_mid) { set_error(error,error_len,"duplicate mid"); return FALSE; }
        mid_tok=tok; mid_len=tl; have_mid=TRUE;
      }
    } else {
    if(!parse_value(&p,end,&v)) { set_error(error,error_len,"invalid value"); return FALSE; }

    if(strcmp(key,"type")==0) {
      if(have_type || v.type!=JV_STRING) { set_error(error,error_len,"invalid or duplicate type"); return FALSE; }
      g_strlcpy(type,v.string,sizeof(type)); have_type=TRUE;
    } else if(strcmp(key,"action")==0) {
      if(have_action || v.type!=JV_STRING) { set_error(error,error_len,"invalid or duplicate action"); return FALSE; }
      g_strlcpy(action,v.string,sizeof(action)); have_action=TRUE;
    } else if(strcmp(key,"id")==0) {
      if(have_id || v.type!=JV_INTEGER) { set_error(error,error_len,"invalid or duplicate id"); return FALSE; }
      out->id=v.integer; have_id=TRUE;
    }
    else if(strcmp(key,"profile")==0 && v.type==JV_STRING) {
      if(have_profile) { set_error(error,error_len,"duplicate spectrum profile"); return FALSE; }
      g_strlcpy(profile,v.string,sizeof(profile)); have_profile=TRUE;
    }
    else if(strcmp(key,"rx")==0 && v.type==JV_INTEGER && v.integer>=G_MININT && v.integer<=G_MAXINT) {
      if(have_rx) { set_error(error,error_len,"duplicate receiver index"); return FALSE; }
      out->rx=(gint)v.integer; have_rx=TRUE;
    } else if(strcmp(key,"value")==0 && v.type==JV_INTEGER) {
      if(have_value) { set_error(error,error_len,"duplicate command value"); return FALSE; }
      out->value=v.integer; have_value=TRUE;
    } else if(strcmp(key,"value")==0 && v.type==JV_BOOL) {
      if(have_value) { set_error(error,error_len,"duplicate command value"); return FALSE; }
      out->flag=v.boolean; out->value_is_bool=TRUE; have_value=TRUE;
    } else if(strcmp(key,"low")==0 && v.type==JV_INTEGER && v.integer>=G_MININT && v.integer<=G_MAXINT) {
      if(have_low) { set_error(error,error_len,"duplicate low filter edge"); return FALSE; }
      out->low=(gint)v.integer; have_low=TRUE;
    } else if(strcmp(key,"high")==0 && v.type==JV_INTEGER && v.integer>=G_MININT && v.integer<=G_MAXINT) {
      if(have_high) { set_error(error,error_len,"duplicate high filter edge"); return FALSE; }
      out->high=(gint)v.integer; have_high=TRUE;
    } else if(strcmp(key,"rx")==0 || strcmp(key,"value")==0 ||
              strcmp(key,"profile")==0 || strcmp(key,"low")==0 || strcmp(key,"high")==0) {
      set_error(error,error_len,"invalid field type"); return FALSE;
    }
    } /* end non-signaling value branch */

    skip_ws(&p,end);
    if(p<end && *p==',') { p++; continue; }
    if(p<end && *p=='}') { p++; break; }
    set_error(error,error_len,"expected comma or object end");
    return FALSE;
  }
  skip_ws(&p,end);
  if(p!=end) { set_error(error,error_len,"trailing data"); return FALSE; }
  if(!have_type) { set_error(error,error_len,"missing request type"); return FALSE; }
  if(!have_id || out->id<0) { set_error(error,error_len,"missing request id"); return FALSE; }

  if(strcmp(type,"get_state")==0) {
    out->type=WEB_REQUEST_GET_STATE;
    return TRUE;
  }
  if(strcmp(type,"spectrum_subscribe")==0) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    if(!have_profile || !web_spectrum_profile_parse(profile,&out->profile)) {
      set_error(error,error_len,"profile must be Low, Balanced, High or Ultra"); return FALSE;
    }
    out->type=WEB_REQUEST_SPECTRUM_SUBSCRIBE;
    return TRUE;
  }
  if(strcmp(type,"spectrum_unsubscribe")==0) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    out->type=WEB_REQUEST_SPECTRUM_UNSUBSCRIBE;
    return TRUE;
  }
  if(strcmp(type,"audio_subscribe")==0) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    out->type=WEB_REQUEST_AUDIO_SUBSCRIBE;
    return TRUE;
  }
  if(strcmp(type,"audio_unsubscribe")==0) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    out->type=WEB_REQUEST_AUDIO_UNSUBSCRIBE;
    return TRUE;
  }
  /* The two signaling replies allocate their payload here, only on success. */
  if(strcmp(type,"rtc_answer")==0) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    if(!have_sdp) { set_error(error,error_len,"missing sdp"); return FALSE; }
    out->sdp=json_unescape(sdp_tok,sdp_len);
    if(out->sdp==NULL) { set_error(error,error_len,"invalid sdp escape"); return FALSE; }
    out->type=WEB_REQUEST_RTC_ANSWER;
    return TRUE;
  }
  if(strcmp(type,"rtc_candidate")==0) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    if(!have_candidate) { set_error(error,error_len,"missing candidate"); return FALSE; }
    out->candidate=json_unescape(cand_tok,cand_len);
    if(out->candidate==NULL) { set_error(error,error_len,"invalid candidate escape"); return FALSE; }
    if(have_mid) {
      out->mid=json_unescape(mid_tok,mid_len);
      if(out->mid==NULL) { web_request_clear(out); set_error(error,error_len,"invalid mid escape"); return FALSE; }
    }
    out->type=WEB_REQUEST_RTC_CANDIDATE;
    return TRUE;
  }
  if(strcmp(type,"command")!=0) { set_error(error,error_len,"unknown request type"); return FALSE; }
  if(strcmp(action,"set_frequency")==0) out->type=WEB_REQUEST_SET_FREQUENCY;
  else if(strcmp(action,"set_mode")==0) out->type=WEB_REQUEST_SET_MODE;
  else if(strcmp(action,"set_mox")==0) out->type=WEB_REQUEST_SET_MOX;
  else if(strcmp(action,"set_filter")==0) out->type=WEB_REQUEST_SET_FILTER;
  else {
    /* Scalar receiver controls: allow-listed here (unknown names are rejected,
       exactly like a bad frequency action), dispatched by WEB_CONTROL. */
    static const struct { const char *action; WEB_CONTROL control; } controls[]={
      {"set_af_gain",  WEB_CTRL_AF_GAIN},
      {"set_agc",      WEB_CTRL_AGC},
      {"set_agc_gain", WEB_CTRL_AGC_GAIN},
      {"set_squelch",  WEB_CTRL_SQUELCH},
      {"set_step",     WEB_CTRL_STEP},
      {"set_nb",       WEB_CTRL_NB},
      {"set_nr",       WEB_CTRL_NR},
      {"set_anf",      WEB_CTRL_ANF},
      {"set_snb",      WEB_CTRL_SNB},
    };
    for(size_t i=0;i<G_N_ELEMENTS(controls);i++)
      if(strcmp(action,controls[i].action)==0) { out->control=controls[i].control; break; }
    if(out->control==WEB_CTRL_NONE) { set_error(error,error_len,"unknown action"); return FALSE; }
    out->type=WEB_REQUEST_SET_CONTROL;
  }

  if(out->type==WEB_REQUEST_SET_FILTER) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    if(!have_low || !have_high || out->low>=out->high) {
      set_error(error,error_len,"invalid filter edges"); return FALSE;
    }
    return TRUE;
  }
  if(!have_value) { set_error(error,error_len,"missing command value"); return FALSE; }
  if(out->type==WEB_REQUEST_SET_MOX && !out->value_is_bool) {
    set_error(error,error_len,"set_mox requires a boolean value"); return FALSE;
  }
  if(out->type!=WEB_REQUEST_SET_MOX && out->value_is_bool) {
    set_error(error,error_len,"numeric command requires an integer value"); return FALSE;
  }
  if(out->type!=WEB_REQUEST_SET_MOX && !have_rx) {
    set_error(error,error_len,"missing receiver index"); return FALSE;
  }
  return TRUE;
}

static void put_u32(guint8 *p, guint32 v) {
  p[0]=(guint8)v; p[1]=(guint8)(v>>8); p[2]=(guint8)(v>>16); p[3]=(guint8)(v>>24);
}

static void put_u64(guint8 *p, guint64 v) {
  put_u32(p,(guint32)v); put_u32(p+4,(guint32)(v>>32));
}

static guint32 get_u32(const guint8 *p) {
  return (guint32)p[0] | ((guint32)p[1]<<8) | ((guint32)p[2]<<16) | ((guint32)p[3]<<24);
}

static guint64 get_u64(const guint8 *p) {
  return (guint64)get_u32(p) | ((guint64)get_u32(p+4)<<32);
}

gboolean web_binary_header_encode(guint8 out[WEB_BINARY_HEADER_SIZE],
                                  const WEB_BINARY_HEADER *h) {
  if(out==NULL || h==NULL || h->version!=WEB_PROTOCOL_VERSION || h->stream==0) return FALSE;
  memcpy(out,"MPSD",4);
  out[4]=h->version; out[5]=h->stream; out[6]=h->receiver; out[7]=h->flags;
  put_u32(out+8,h->sequence); put_u64(out+12,h->timestamp_us);
  put_u32(out+20,h->count); put_u32(out+24,h->rate_or_width);
  put_u32(out+28,h->payload_bytes);
  return TRUE;
}

gboolean web_binary_header_decode(const guint8 *data, size_t len,
                                  WEB_BINARY_HEADER *h) {
  if(data==NULL || h==NULL || len<WEB_BINARY_HEADER_SIZE || memcmp(data,"MPSD",4)!=0) return FALSE;
  h->version=data[4]; h->stream=data[5]; h->receiver=data[6]; h->flags=data[7];
  h->sequence=get_u32(data+8); h->timestamp_us=get_u64(data+12);
  h->count=get_u32(data+20); h->rate_or_width=get_u32(data+24);
  h->payload_bytes=get_u32(data+28);
  return h->version==WEB_PROTOCOL_VERSION && h->stream!=0;
}

gboolean web_spectrum_profile_parse(const char *name, WEB_SPECTRUM_PROFILE *profile) {
  if(name==NULL || profile==NULL) return FALSE;
  if(g_ascii_strcasecmp(name,"Low")==0) *profile=WEB_SPECTRUM_LOW;
  else if(g_ascii_strcasecmp(name,"Balanced")==0) *profile=WEB_SPECTRUM_BALANCED;
  else if(g_ascii_strcasecmp(name,"High")==0) *profile=WEB_SPECTRUM_HIGH;
  else if(g_ascii_strcasecmp(name,"Ultra")==0) *profile=WEB_SPECTRUM_ULTRA;
  else return FALSE;
  return TRUE;
}

const char *web_spectrum_profile_name(WEB_SPECTRUM_PROFILE profile) {
  static const char *names[]={"Low","Balanced","High","Ultra"};
  return profile>=WEB_SPECTRUM_LOW && profile<=WEB_SPECTRUM_ULTRA ? names[profile] : "Low";
}

WEB_SPECTRUM_LIMITS web_spectrum_profile_limits(int profile) {
  static const WEB_SPECTRUM_LIMITS limits[]={{256,5},{512,10},{1024,15},{2048,30}};
  if(profile<WEB_SPECTRUM_LOW) profile=WEB_SPECTRUM_LOW;
  if(profile>WEB_SPECTRUM_ULTRA) profile=WEB_SPECTRUM_ULTRA;
  return limits[(unsigned)profile];
}

gboolean web_spectrum_quantize(const float *input, guint input_count,
                               guint8 *output, guint output_count,
                               float floor_db, float span_db) {
  if(input==NULL || output==NULL || input_count==0 || output_count==0 ||
     output_count>input_count || !isfinite(floor_db) || !isfinite(span_db) || span_db<=0.0f) return FALSE;
  for(guint i=0;i<output_count;i++) {
    guint first=(guint)(((guint64)i*input_count)/output_count);
    guint last=(guint)(((guint64)(i+1)*input_count)/output_count);
    if(last<=first) last=first+1;
    float peak=-INFINITY;
    for(guint j=first;j<last;j++) if(isfinite(input[j]) && input[j]>peak) peak=input[j];
    if(!isfinite(peak)) peak=floor_db;
    float scaled=(peak-floor_db)*255.0f/span_db;
    if(scaled<0.0f) scaled=0.0f;
    if(scaled>255.0f) scaled=255.0f;
    output[i]=(guint8)lrintf(scaled);
  }
  return TRUE;
}

void web_latest_frame_replace(WEB_LATEST_FRAME *slot, GBytes *frame) {
  if(slot==NULL) return;
  if(slot->frame!=NULL) { g_bytes_unref(slot->frame); slot->dropped++; }
  slot->frame=frame==NULL?NULL:g_bytes_ref(frame);
}

GBytes *web_latest_frame_take(WEB_LATEST_FRAME *slot) {
  if(slot==NULL) return NULL;
  GBytes *frame=slot->frame; slot->frame=NULL; return frame;
}

void web_latest_frame_clear(WEB_LATEST_FRAME *slot) {
  if(slot!=NULL && slot->frame!=NULL) { g_bytes_unref(slot->frame); slot->frame=NULL; }
}

gboolean web_protocol_route(const char *target, const char **asset_name,
                            const char **content_type, gboolean *websocket) {
  if(asset_name) *asset_name=NULL;
  if(content_type) *content_type=NULL;
  if(websocket) *websocket=FALSE;
  if(target==NULL || strchr(target,'?')!=NULL || strchr(target,'#')!=NULL ||
     strstr(target,"..")!=NULL || strchr(target,'%')!=NULL) return FALSE;
  if(strcmp(target,"/")==0 || strcmp(target,"/index.html")==0) {
    if(asset_name) *asset_name="index.html";
    if(content_type) *content_type="text/html; charset=utf-8";
    return TRUE;
  }
  if(strcmp(target,"/app.css")==0) {
    if(asset_name) *asset_name="app.css";
    if(content_type) *content_type="text/css; charset=utf-8";
    return TRUE;
  }
  if(strcmp(target,"/app.js")==0) {
    if(asset_name) *asset_name="app.js";
    if(content_type) *content_type="text/javascript; charset=utf-8";
    return TRUE;
  }
  if(strcmp(target,"/ws")==0) {
    if(websocket) *websocket=TRUE;
    return TRUE;
  }
  return FALSE;
}

gboolean web_origin_matches_host(const char *origin, const char *host) {
  if(origin==NULL || host==NULL || *host=='\0' || strchr(host,'/')!=NULL ||
     strchr(host,'\\')!=NULL || strchr(host,' ')!=NULL || strchr(host,'\t')!=NULL)
    return FALSE;
  char http_origin[320], https_origin[320];
  if(strlen(host)+9>=sizeof(https_origin)) return FALSE;
  g_snprintf(http_origin,sizeof(http_origin),"http://%s",host);
  g_snprintf(https_origin,sizeof(https_origin),"https://%s",host);
  return strcmp(origin,http_origin)==0 || strcmp(origin,https_origin)==0;
}

/* PBKDF2-HMAC-SHA256, one 32-byte block.  GLib supplies the HMAC primitive on
   every platform already supported by the application, so authentication does
   not add a crypto or packaging dependency. */
gboolean web_auth_password_hash(const char *password, const char *salt,
                                guint iterations,
                                char out[WEB_AUTH_HASH_HEX_SIZE]) {
  if(password==NULL || *password=='\0' || salt==NULL || *salt=='\0' ||
     iterations==0 || out==NULL) return FALSE;
  const guchar *key=(const guchar *)password;
  gsize key_len=strlen(password), salt_len=strlen(salt);
  guint8 u[WEB_AUTH_DIGEST_SIZE], result[WEB_AUTH_DIGEST_SIZE];
  guint8 block[]={0,0,0,1};
  GHmac *hmac=g_hmac_new(G_CHECKSUM_SHA256,key,key_len);
  if(hmac==NULL) return FALSE;
  g_hmac_update(hmac,(const guchar *)salt,salt_len);
  g_hmac_update(hmac,block,sizeof(block));
  gsize digest_len=sizeof(u);
  g_hmac_get_digest(hmac,u,&digest_len);
  g_hmac_unref(hmac);
  if(digest_len!=sizeof(u)) return FALSE;
  memcpy(result,u,sizeof(result));
  for(guint round=1;round<iterations;round++) {
    hmac=g_hmac_new(G_CHECKSUM_SHA256,key,key_len);
    if(hmac==NULL) return FALSE;
    g_hmac_update(hmac,u,sizeof(u));
    digest_len=sizeof(u);
    g_hmac_get_digest(hmac,u,&digest_len);
    g_hmac_unref(hmac);
    if(digest_len!=sizeof(u)) return FALSE;
    for(guint i=0;i<sizeof(result);i++) result[i]^=u[i];
  }
  for(guint i=0;i<sizeof(result);i++)
    g_snprintf(out+i*2,3,"%02x",result[i]);
  out[WEB_AUTH_HASH_HEX_SIZE-1]='\0';
  return TRUE;
}

static gboolean constant_time_equal(const char *a, const char *b) {
  if(a==NULL || b==NULL) return FALSE;
  size_t alen=strlen(a), blen=strlen(b), count=MAX(alen,blen);
  size_t diff=alen^blen;
  for(size_t i=0;i<count;i++) {
    unsigned char ac=i<alen?(unsigned char)a[i]:0;
    unsigned char bc=i<blen?(unsigned char)b[i]:0;
    diff|=(size_t)(ac^bc);
  }
  return diff==0;
}

gboolean web_auth_check_basic(const char *authorization, const char *username,
                              const char *salt, const char *expected_hash,
                              guint iterations) {
  if(authorization==NULL || username==NULL || salt==NULL || expected_hash==NULL ||
     g_ascii_strncasecmp(authorization,"Basic ",6)!=0) return FALSE;
  const char *encoded=authorization+6;
  while(*encoded==' ' || *encoded=='\t') encoded++;
  if(*encoded=='\0' || strlen(encoded)>512) return FALSE;
  gsize decoded_len=0;
  guchar *decoded=g_base64_decode(encoded,&decoded_len);
  if(decoded==NULL || decoded_len==0 || memchr(decoded,'\0',decoded_len)!=NULL) {
    g_free(decoded); return FALSE;
  }
  gchar *credentials=g_strndup((const gchar *)decoded,decoded_len);
  g_free(decoded);
  gchar *colon=strchr(credentials,':');
  if(colon==NULL) { g_free(credentials); return FALSE; }
  *colon='\0';
  char actual_hash[WEB_AUTH_HASH_HEX_SIZE];
  gboolean ok=constant_time_equal(credentials,username) &&
              web_auth_password_hash(colon+1,salt,iterations,actual_hash) &&
              constant_time_equal(actual_hash,expected_hash);
  memset(actual_hash,0,sizeof(actual_hash));
  memset(credentials,0,decoded_len);
  g_free(credentials);
  return ok;
}
