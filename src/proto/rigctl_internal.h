/* Shared internal state/prototypes between rigctl.c and rigctl_parse.c
 *
 * NOTE: include this AFTER receiver.h / filter.h / gtk/gtk.h (and the usual
 * sys/socket.h / netinet/in.h trio) — the types below rely on those already
 * being visible, and this header deliberately does not pull in any heavy
 * project/toolkit headers itself.
 */
#ifndef RIGCTL_INTERNAL_H
#define RIGCTL_INTERNAL_H

typedef struct _rigctl {
  GMutex mutex;

  gint listening_port;
  gboolean socket_listening;
  GThread *server_thread_id;
  gint server_socket;
  gint server_address_length;
  struct sockaddr_in server_address;

  socklen_t address_length;
  struct sockaddr_in address;
  int socket_fd;
  gboolean socket_running;

  char ser_port[64];
  int serial_baudrate;
  int serial_parity;
  int serial_fd;
  gboolean serial_running;

  gboolean debug;
} RIGCTL;

typedef struct _command {
  RECEIVER *rx;
  char *command;
  int fd;
} COMMAND;

// Defined in rigctl_parse.c; rigctl.c's rigctl_client()/serial_server() hand
// commands off to parse_cmd() via g_idle_add().
extern int parse_cmd(void *data);
extern gboolean parse_extended_cmd(COMMAND *cmd);

#endif // RIGCTL_INTERNAL_H
