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

  printf("\n%d failure%s\n",failures,failures==1?"":"s");
  return failures?1:0;
}
