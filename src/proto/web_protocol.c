/* Browser-remote protocol parser and fixed HTTP router. */
#include "web_protocol.h"

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
  gboolean have_type=FALSE, have_action=FALSE;
  gboolean have_id=FALSE, have_rx=FALSE, have_value=FALSE;
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
    else if(strcmp(key,"rx")==0 && v.type==JV_INTEGER && v.integer>=G_MININT && v.integer<=G_MAXINT) {
      if(have_rx) { set_error(error,error_len,"duplicate receiver index"); return FALSE; }
      out->rx=(gint)v.integer; have_rx=TRUE;
    } else if(strcmp(key,"value")==0 && v.type==JV_INTEGER) {
      if(have_value) { set_error(error,error_len,"duplicate command value"); return FALSE; }
      out->value=v.integer; have_value=TRUE;
    } else if(strcmp(key,"value")==0 && v.type==JV_BOOL) {
      if(have_value) { set_error(error,error_len,"duplicate command value"); return FALSE; }
      out->flag=v.boolean; out->value_is_bool=TRUE; have_value=TRUE;
    } else if(strcmp(key,"rx")==0 || strcmp(key,"value")==0) {
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
  if(strcmp(type,"command")!=0) { set_error(error,error_len,"unknown request type"); return FALSE; }
  if(strcmp(action,"set_frequency")==0) out->type=WEB_REQUEST_SET_FREQUENCY;
  else if(strcmp(action,"set_mode")==0) out->type=WEB_REQUEST_SET_MODE;
  else if(strcmp(action,"set_mox")==0) out->type=WEB_REQUEST_SET_MOX;
  else { set_error(error,error_len,"unknown action"); return FALSE; }

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
