/* Browser-remote protocol parser and fixed HTTP router. */
#include "web_protocol.h"

#include <math.h>
#include <string.h>

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
      set_error(error,error_len,"profile must be Low, Balanced or High"); return FALSE;
    }
    out->type=WEB_REQUEST_SPECTRUM_SUBSCRIBE;
    return TRUE;
  }
  if(strcmp(type,"spectrum_unsubscribe")==0) {
    if(!have_rx) { set_error(error,error_len,"missing receiver index"); return FALSE; }
    out->type=WEB_REQUEST_SPECTRUM_UNSUBSCRIBE;
    return TRUE;
  }
  if(strcmp(type,"command")!=0) { set_error(error,error_len,"unknown request type"); return FALSE; }
  if(strcmp(action,"set_frequency")==0) out->type=WEB_REQUEST_SET_FREQUENCY;
  else if(strcmp(action,"set_mode")==0) out->type=WEB_REQUEST_SET_MODE;
  else if(strcmp(action,"set_mox")==0) out->type=WEB_REQUEST_SET_MOX;
  else if(strcmp(action,"set_filter")==0) out->type=WEB_REQUEST_SET_FILTER;
  else { set_error(error,error_len,"unknown action"); return FALSE; }

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
  else return FALSE;
  return TRUE;
}

const char *web_spectrum_profile_name(WEB_SPECTRUM_PROFILE profile) {
  static const char *names[]={"Low","Balanced","High"};
  return profile>=WEB_SPECTRUM_LOW && profile<=WEB_SPECTRUM_HIGH ? names[profile] : "Low";
}

WEB_SPECTRUM_LIMITS web_spectrum_profile_limits(int profile) {
  static const WEB_SPECTRUM_LIMITS limits[]={{256,5},{512,10},{1024,15}};
  if(profile<WEB_SPECTRUM_LOW) profile=WEB_SPECTRUM_LOW;
  if(profile>WEB_SPECTRUM_HIGH) profile=WEB_SPECTRUM_HIGH;
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
