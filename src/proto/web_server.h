/* Embedded browser remote server. */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

typedef struct _radio RADIO;

#define WEB_DEFAULT_PORT 8080

void web_server_init(RADIO *radio);
void web_server_start(void);
void web_server_stop(void);
const char *web_server_status(void);

#endif
