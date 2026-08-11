/* TS-2000 emulation via TCP
 * Copyright (C) 2016 Steve Wilson <wevets@gmail.com>
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * PiHPSDR RigCtl by Steve KA6S Oct 16 2016
 * With a kindly assist from Jae, K5JAE who has helped
 * greatly with hamlib integration!
 */
#include "net_compat.h"   // must precede gtk.h: winsock2 before windows.h
#include <gtk/gtk.h>
#include "log.h"
#include <gdk/gdk.h>
#include <errno.h>
#include <fcntl.h> 
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include <wdsp.h>

#include "receiver.h"
//#include "band_menu.h"
#include "rigctl.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "transmitter.h"
#include "receiver.h"
#include "wideband.h"
#include "radio.h"
#include "channel.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "bandstack.h"
//#include "filter_menu.h"
#include "vfo.h"
//#include "sliders.h"
#include "transmitter.h"
#include "agc.h"
//#include "store.h"
#include "ext.h"
//#include "rigctl_menu.h"
//#include "noise_menu.h"
#include "protocol1.h"
#ifdef LOCALCW
#include "iambic.h"              // declare keyer_update()
#endif
#include "main.h"
#include "subrx.h"
#include "rigctl_internal.h"

#define NEW_PARSER


int rigctl_port_base=19090;
int rigctl_enable=0;

#define RIGCTL_THROTTLE_NSEC    15000000L
#define NSEC_PER_SEC          1000000000L

// max number of bytes we can get at once
#define MAXDATASIZE 2000

int connect_cnt = 0;

int rigctlGetFilterLow(void);
int rigctlGetFilterHigh(void);
int new_level;
int active_transmitter = 0;
int rigctl_busy = 0;  // Used to tell rigctl_menu that launch has already occured

int cat_control;

extern int enable_tx_equalizer;

FILE * out;
int  output;
FILTER * band_filter;

static GThread *serial_server_thread_id = NULL;

static int server_socket=-1;
static int server_address_length;
static struct sockaddr_in server_address;

static int rigctl_timer = 0;

/*
static CLIENT client[MAX_CLIENTS];
*/

int squelch=-160; //local sim of squelch level
int fine = 0;     // FINE status for TS-2000 decides whether rit_increment is 1Hz/10Hz.


int read_size;

int freq_flag;  // Determines if we are in the middle of receiving frequency info

int digl_offset = 0;
int digl_pol = 0;
int digu_offset = 0;
int digu_pol = 0;
double new_vol = 0;
int  lcl_cmd=0;
long long new_freqA = 0;
long long new_freqB = 0;
long long orig_freqA = 0;
long long orig_freqB = 0;
int  lcl_split = 0;
int  mox_state = 0;
// Radio functions - 
// Memory channel stuff and things that aren't 
// implemented - but here for a more complete emulation
int ctcss_tone;  // Numbers 01-38 are legal values - set by CN command, read by CT command
int ctcss_mode;  // Numbers 0/1 - on off.

static void rigctl_client(RECEIVER *rx);

void disable_rigctl(RECEIVER *rx) {
  int i;
  struct linger linger = { 0 };
  linger.l_onoff = 1;
  linger.l_linger = 0;
  RIGCTL *rigctl=(RIGCTL *)rx->rigctl;

  log_info("%s: server_socket=%d\n",__FUNCTION__,rigctl->server_socket);
  rigctl->socket_running=FALSE;
  if(setsockopt(rigctl->socket_fd,SOL_SOCKET,SO_LINGER,(const char *)&linger,sizeof(linger))==-1) {
    perror("setsockopt(...,SO_LINGER,...) failed for client");
  }
  log_info("closing client socket: %d\n",rigctl->socket_fd);
  closesocket(rigctl->socket_fd);
  rigctl->socket_fd=-1;

  if(rigctl->server_socket>=0) {
    if(setsockopt(rigctl->server_socket,SOL_SOCKET,SO_LINGER,(const char *)&linger,sizeof(linger))==-1) {
      perror("setsockopt(...,SO_LINGER,...) failed for server");
    }
    closesocket(rigctl->server_socket);
    rigctl->server_socket=-1;
  }
}

int vfo_sm=0;   // VFO State Machine - this keeps track of

//#define RIGCTL_CW

#ifdef RIGCTL_CW
// 
//  CW sending stuff
//

static char cw_buf[30];
static int  cw_busy=0;
static int  cat_cw_seen=0;

static int dotlen;
static int dashlen;
static int dotsamples;
static int dashsamples;

//
// send_dash()         send a "key-down" of a dashlen, followed by a "key-up" of a dotlen
// send_dot()          send a "key-down" of a dotlen,  followed by a "key-up" of a dotlen
// send_space(int len) send a "key_down" of zero,      followed by a "key-up" of len*dotlen
//
// The "trick" to get proper timing is, that we really specify  the number of samples
// for the next element (dash/dot/nothing) and the following pause. 30 wpm is no
// problem, and without too much "busy waiting". We just take a nap until 10 msec
// before we have to act, and then wait several times for 1 msec until we can shoot.
//
void send_dash(void) {
  int TimeToGo;
  for(;;) {
    TimeToGo=cw_key_up+cw_key_down;
    // TimeToGo is invalid if local CW keying has set in
    if (cw_key_hit || cw_not_ready) return;
    if (TimeToGo == 0) break;
    // sleep until 10 msec before ignition
    if (TimeToGo > 500) usleep((long)(TimeToGo-500)*20L);
    // sleep 1 msec
    usleep(1000L);
  }
  // If local CW keying has set in, do not interfere
  if (cw_key_hit || cw_not_ready) return;
  cw_key_down = dashsamples;
  cw_key_up   = dotsamples;
}

void send_dot(void) {
  int TimeToGo;
  for(;;) {
    TimeToGo=cw_key_up+cw_key_down;
    // TimeToGo is invalid if local CW keying has set in
    if (cw_key_hit || cw_not_ready) return;
    if (TimeToGo == 0) break;
    // sleep until 10 msec before ignition
    if (TimeToGo > 500) usleep((long)(TimeToGo-500)*20L);
    // sleep 1 msec
    usleep(1000L);
  }
  // If local CW keying has set in, do not interfere
  if (cw_key_hit || cw_not_ready) return;
  cw_key_down = dotsamples;
  cw_key_up   = dotsamples;
}

void send_space(int len) {
  int TimeToGo;
    for(;;) {
    TimeToGo=cw_key_up+cw_key_down;
    // TimeToGo is invalid if local CW keying has set in
    if (cw_key_hit || cw_not_ready) return;
    if (TimeToGo == 0) break;
    // sleep until 10 msec before ignition
    if (TimeToGo > 500) usleep((long)(TimeToGo-500)*20L);
    // sleep 1 msec
    usleep(1000L);
  }
  // If local CW keying has set in, do not interfere
  if (cw_key_hit || cw_not_ready) return;
  cw_key_up = len*dotsamples;
}

void rigctl_send_cw_char(char cw_char) {
    char pattern[9],*ptr;
    strcpy(pattern,"");
    ptr = &pattern[0];
    switch (cw_char) {
       case 'a':
       case 'A': strcpy(pattern,".-"); break;
       case 'b': 
       case 'B': strcpy(pattern,"-..."); break;
       case 'c': 
       case 'C': strcpy(pattern,"-.-."); break;
       case 'd': 
       case 'D': strcpy(pattern,"-.."); break;
       case 'e': 
       case 'E': strcpy(pattern,"."); break;
       case 'f': 
       case 'F': strcpy(pattern,"..-."); break;
       case 'g': 
       case 'G': strcpy(pattern,"--."); break;
       case 'h': 
       case 'H': strcpy(pattern,"...."); break;
       case 'i': 
       case 'I': strcpy(pattern,".."); break;
       case 'j': 
       case 'J': strcpy(pattern,".---"); break;
       case 'k': 
       case 'K': strcpy(pattern,"-.-"); break;
       case 'l': 
       case 'L': strcpy(pattern,".-.."); break;
       case 'm': 
       case 'M': strcpy(pattern,"--"); break;
       case 'n': 
       case 'N': strcpy(pattern,"-."); break;
       case 'o': 
       case 'O': strcpy(pattern,"---"); break;
       case 'p': 
       case 'P': strcpy(pattern,".--."); break;
       case 'q': 
       case 'Q': strcpy(pattern,"--.-"); break;
       case 'r': 
       case 'R': strcpy(pattern,".-."); break;
       case 's': 
       case 'S': strcpy(pattern,"..."); break;
       case 't': 
       case 'T': strcpy(pattern,"-"); break;
       case 'u': 
       case 'U': strcpy(pattern,"..-"); break;
       case 'v': 
       case 'V': strcpy(pattern,"...-"); break;
       case 'w': 
       case 'W': strcpy(pattern,".--"); break;
       case 'x': 
       case 'X': strcpy(pattern,"-..-"); break;
       case 'y':
       case 'Y': strcpy(pattern,"-.--"); break;
       case 'z': 
       case 'Z': strcpy(pattern,"--.."); break;
       case '0': strcpy(pattern,"-----"); break;
       case '1': strcpy(pattern,".----"); break;
       case '2': strcpy(pattern,"..---"); break;
       case '3': strcpy(pattern,"...--"); break;
       case '4': strcpy(pattern,"....-"); break;
       case '5': strcpy(pattern,".....");break;
       case '6': strcpy(pattern,"-....");break;
       case '7': strcpy(pattern,"--...");break;
       case '8': strcpy(pattern,"---..");break;
       case '9': strcpy(pattern,"----.");break;
//
//     DL1YCF:
//     There were some signs I considered wrong, other
//     signs missing. Therefore I put the signs here
//     from ITU Recommendation M.1677-1 (2009)
//     in the order given there.
//
       case '.':  strcpy(pattern,".-.-.-"); break;
       case ',':  strcpy(pattern,"--..--"); break;
       case ':':  strcpy(pattern,"---..");  break;
       case '?':  strcpy(pattern,"..--.."); break;
       case '\'': strcpy(pattern,".----."); break;
       case '-':  strcpy(pattern,"-....-"); break;
       case '/':  strcpy(pattern,"-..-.");  break;
       case '(':  strcpy(pattern,"-.--.");  break;
       case ')':  strcpy(pattern,"-.--.-"); break;
       case '"':  strcpy(pattern,".-..-."); break;
       case '=':  strcpy(pattern,"-...-");  break;
       case '+':  strcpy(pattern,".-.-.");  break;
       case '@':  strcpy(pattern,".--.-."); break;
//
//     Often used, but not ITU: Ampersand for "wait"
//
       case '&': strcpy(pattern,".-...");break;
       default:  strcpy(pattern,"");
    }
     
    while(*ptr != '\0') {
       if(*ptr == '-') {
          send_dash();
       }
       if(*ptr == '.') {
          send_dot();
       }
       ptr++;
    }

    // The last element (dash or dot) sent already has one dotlen space appended.
    // If the current character is another "printable" sign, we need an additional
    // pause of 2 dotlens to form the inter-character spacing of 3 dotlens.
    // However if the current character is a "space" we must produce an inter-word
    // spacing (7 dotlens) and therefore need 6 additional dotlens
    // We need no longer take care of a sequence of spaces since adjacent spaces
    // are now filtered out while filling the CW character (ring-) buffer.

    if (cw_char == ' ') {
      send_space(6);  // produce inter-word space of 7 dotlens
    } else {
      send_space(2);  // produce inter-character space of 3 dotlens
    }
}

//
// This thread constantly looks whether CW data
// is available, and produces CW in this case.
//
// A ring buffer is maintained such that the contents
// of several KY commands can be buffered. This allows
// sending a large CW text word-by-word (each word in a
// separate KY command).
//
// If the contents of the last KY command do not fit into
// the ring buffer, cw_busy is NOT reset. Eventually, there
// is enough space in the ring buffer, then cw_busy is reset.
//
static gpointer rigctl_cw_thread(gpointer data)
{ 
  int i;
  char c;
  char last_char=0;
  char ring_buf[130];
  char *write_buf=ring_buf;
  char *read_buf =ring_buf;
  char *p;
  int  num_buf=0;
  
  while (server_running) {
    // wait for CW data (periodically look every 100 msec)
    if (!cw_busy && num_buf ==0) {
      cw_key_hit=0;
      usleep(100000L);
      continue;
    }

    // if new data is available and fits into the buffer, copy-in.
    // If there are several adjacent spaces, take only the first one.
    // This also swallows the "tails" of the KY commands which
    // (according to Kenwood) have to be padded with spaces up
    // to the maximum length (24)

    if (cw_busy && num_buf < 100) {
      p=cw_buf;
      while ((c=*p++)) {
        if (last_char == ' ' && c == ' ') continue;
        *write_buf++ = c;
        last_char=c;
        num_buf++;
        if (write_buf - ring_buf == 128) write_buf=ring_buf;  // wrap around
      }
      cw_busy=0; // mark one-line buffer free again
    }
    // This may happen if cw_buf was empty or contained only blanks
    if (num_buf == 0) continue;

    // these values may have changed, so recompute them here
    // This means that we can change the speed (KS command) while
    // the buffer is being sent

    dotlen = 1200000L/(long)cw_keyer_speed;
    dashlen = (dotlen * 3 * cw_keyer_weight) / 50L;
    dotsamples = 57600 / cw_keyer_speed;
    dashsamples = (3456 * cw_keyer_weight) / cw_keyer_speed;
    CAT_cw_is_active=1;
    if (!mox) {
	// activate PTT
        g_idle_add(ext_mox_update ,(gpointer)1);
	// have to wait until it is really there
	// Note that if out-of-band, we would wait
	// forever here, so allow at most 200 msec
	// We also have to wait for cw_not_ready becoming zero
	i=200;
        while ((!mox || cw_not_ready) && i-- > 0) usleep(1000L);
	// still no MOX? --> silently discard CW character and give up
	if (!mox) {
	    CAT_cw_is_active=0;
	    continue;
	}
    }
    // At this point, mox==1 and CAT_cw_active == 1
    if (cw_key_hit || cw_not_ready) {
       //
       // CW transmission has been aborted, either due to manually
       // removing MOX, changing the mode to non-CW, or because a CW key has been hit.
       // Do not remove PTT in the latter case
       CAT_cw_is_active=0;
       // If a CW key has been hit, we continue in TX mode.
       // Otherwise, switch PTT off.
       if (!cw_key_hit && mox) {
         g_idle_add(ext_mox_update ,(gpointer)0);
       }
       // Let the CAT system swallow incoming CW commands by setting cw_busy to -1.
       // Do so until no CAT CW message has arrived for 1 second
       cw_busy=-1;
       for (;;) {
         cat_cw_seen=0;
         usleep(1000000L);
         if (cat_cw_seen) continue;
         cw_busy=0;
         break;
       }
       write_buf=read_buf=ring_buf;
       num_buf=0;
    } else {
      rigctl_send_cw_char(*read_buf++);
      if (read_buf - ring_buf == 128) read_buf=ring_buf; // wrap around
      num_buf--;
      //
      // Character has been sent.
      // If there are more to send, or the next message is pending, continue.
      // Otherwise remove PTT and wait for next CAT CW command.
      if (cw_busy || num_buf > 0) continue;
      CAT_cw_is_active=0;
      if (!cw_key_hit) {
        g_idle_add(ext_mox_update ,(gpointer)0);
        // wait up to 500 msec for MOX having gone
        // otherwise there might be a race condition when sending
        // the next character really soon
        i=10;
        while (mox && (i--) > 0) usleep(50000L);
      }
    }
    // end of while (server_running)
  }
  // We arrive here if the rigctl server shuts down.
  // This very rarely happens. But we should shut down the
  // local CW system gracefully, in case we were in the mid
  // of a transmission
  rigctl_cw_thread_id = NULL;
  cw_busy=0;
  if (CAT_cw_is_active) {
    CAT_cw_is_active=0;
    g_idle_add(ext_mox_update ,(gpointer)0);
  }
  return NULL;
}
#endif


//
// only one connection via TCP/IP
//

static gpointer rigctl_server(gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  RIGCTL *rigctl=(RIGCTL *)rx->rigctl;
  int on=1;
  int i;

  log_info("%s: listening on port %d\n",__FUNCTION__,rigctl->listening_port);

  rigctl->server_socket=socket(AF_INET,SOCK_STREAM,0);
  if(rigctl->server_socket<0) {
    perror("rigctl_server: listen socket failed");
    return NULL;
  }

  setsockopt(rigctl->server_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT   // no Winsock equivalent; SO_REUSEADDR above covers Windows
  setsockopt(rigctl->server_socket, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

  // bind to listening port
  memset(&rigctl->server_address,0,sizeof(rigctl->server_address));
  rigctl->server_address_length=sizeof(rigctl->server_address);
  rigctl->server_address.sin_family=AF_INET;
  rigctl->server_address.sin_addr.s_addr=INADDR_ANY;
  rigctl->server_address.sin_port=htons(rigctl->listening_port);
  if(bind(rigctl->server_socket,(struct sockaddr*)&rigctl->server_address,sizeof(rigctl->server_address))<0) {
    perror("rigctl_server: listen socket bind failed");
    closesocket(rigctl->server_socket);
    return NULL;
  }

  memset(&rigctl->address,0,sizeof(rigctl->address));
  rigctl->address_length=sizeof(rigctl->address);

  // must start the thread here in order NOT to inherit a lock
  //if (!rigctl_cw_thread_id) rigctl_cw_thread_id = g_thread_new("RIGCTL cw", rigctl_cw_thread, NULL);

  rigctl->socket_listening=TRUE;
  while(rigctl->socket_listening) {
    if(listen(rigctl->server_socket,1)<0) {
      perror("rigctl_server: listen failed");
      closesocket(server_socket);
      return NULL;
    }

log_info("%s: accept connection\n",__FUNCTION__);
    rigctl->socket_fd=accept(rigctl->server_socket,(struct sockaddr*)&rigctl->address,&rigctl->address_length);
    if(rigctl->socket_fd<0) {
      perror("rigctl_server: client accept failed");
      continue;
    }

#ifdef __APPLE__
    if(setsockopt(rigctl->socket_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on))<0) {
#else
    if(setsockopt(rigctl->socket_fd, SOL_TCP, TCP_NODELAY, (void *)&on, sizeof(on))<0) {
#endif
      perror("TCP_NODELAY");
    }

    // no longer a separate thread as only one client per receiver
    rigctl_client(rx);

    log_info("%s: setting SO_LINGER to 0 for client_socket: %d\n",__FUNCTION__,rigctl->socket_fd);
    struct linger linger = { 0 };
    linger.l_onoff = 1;
    linger.l_linger = 0;
    if(setsockopt(rigctl->socket_fd,SOL_SOCKET,SO_LINGER,(const char *)&linger,sizeof(linger))==-1) {
      perror("setsockopt(...,SO_LINGER,...) failed for client");
    }
    closesocket(rigctl->socket_fd);
  }

  closesocket(server_socket);
  return NULL;
}

static void rigctl_client(RECEIVER *rx) {
  RIGCTL *rigctl=(RIGCTL *)rx->rigctl;
  int i;
  int numbytes;
  char  cmd_input[MAXDATASIZE] ;
  char *command=g_new(char,MAXDATASIZE);
  int command_index=0;

  log_info("%s: starting rigctl_client: socket=%d\n",__FUNCTION__,rigctl->socket_fd);

  rx->cat_client_connected = TRUE;
  rigctl->socket_running=TRUE;
  while(rigctl->socket_running && (numbytes=recv(rigctl->socket_fd , cmd_input , MAXDATASIZE-2 , 0)) > 0 ) {
    for(i=0;i<numbytes;i++) {
      // Guard against a run of bytes with no ';' terminator: a real CAT command
      // is a few dozen chars, so anything approaching MAXDATASIZE is junk (or a
      // hostile client). Discard the accumulated garbage rather than write past
      // the buffer (a heap overflow before this guard was added).
      if(command_index >= MAXDATASIZE-1) command_index=0;
      command[command_index]=cmd_input[i];
      command_index++;
      if(cmd_input[i]==';') {
        command[command_index]='\0';
        COMMAND *cmd=g_new(COMMAND,1);
        cmd->rx=rx;
        cmd->command=command;
        cmd->fd=rigctl->socket_fd;
        g_mutex_lock(&rigctl->mutex);
        g_idle_add(parse_cmd,cmd);
        g_mutex_unlock(&rigctl->mutex);
        command=g_new(char,MAXDATASIZE);
        command_index=0;
      }
    }
  }
  // The current (still-accumulating) buffer was never handed to a COMMAND, so
  // free it here rather than leak MAXDATASIZE bytes on every disconnect.
  g_free(command);
  rx->cat_client_connected = FALSE;
  
perror("recv");
log_info("%s: running=%d numbytes=%d\n",__FUNCTION__,rigctl->socket_running,numbytes);
}


// Serial Port Launch
int set_interface_attribs (int fd, int speed, int parity)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                log_info("RIGCTL: Error %d from tcgetattr", errno);
                return -1;
        }

        cfsetospeed (&tty, speed);
        cfsetispeed (&tty, speed);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
        // disable IGNBRK for mismatched speed tests; otherwise receive break
        // as \000 chars
        tty.c_iflag &= ~IGNBRK;         // disable break processing
        tty.c_lflag = 0;                // no signaling chars, no echo,
                                        // no canonical processing
        tty.c_oflag = 0;                // no remapping, no delays
        tty.c_cc[VMIN]  = 0;            // read doesn't block
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        //tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl
        tty.c_iflag |= (IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

        tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                        // enable reading
        tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
        tty.c_cflag |= parity;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
        {
                log_info( "RIGCTL: Error %d from tcsetattr", errno);
                return -1;
        }
        return 0;
}

void set_blocking (int fd, int should_block)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                log_info("RIGCTL: Error %d from tggetattr\n", errno);
                return;
        }
        tty.c_cc[VMIN]  = should_block ? 1 : 0;
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
                log_info("RIGCTL: error %d setting term attributes\n", errno);
}

static gpointer serial_server(gpointer data) {
     // We're going to Read the Serial port and
     // when we get data we'll send it to parse_cmd
     RECEIVER *rx=(RECEIVER *)data;
     RIGCTL *rigctl=(RIGCTL *)rx->rigctl;
     char cmd_input[MAXDATASIZE];
     char *command=g_new(char,MAXDATASIZE);
     int command_index=0;
     int numbytes;
     int i;
     cat_control++;
     rigctl->serial_running=TRUE;
     log_info("%s: starting serial_server: fd=%d\n",__FUNCTION__,rigctl->serial_fd);
     while(rigctl->serial_running && (numbytes=read(rigctl->serial_fd , cmd_input , MAXDATASIZE-2)) > 0 ) {
      for(i=0;i<numbytes;i++) {
        // Same overflow guard as the socket path: drop an unterminated run that
        // would otherwise write past the MAXDATASIZE buffer.
        if(command_index >= MAXDATASIZE-1) command_index=0;
        command[command_index]=cmd_input[i];
        command_index++;
        if(cmd_input[i]==';') {
          command[command_index]='\0';
          if(rigctl->debug) log_info("RIGCTL: command=%s\n",command);
          COMMAND *cmd=g_new(COMMAND,1);
          cmd->rx=rx;
          cmd->command=command;
          cmd->fd=rigctl->serial_fd;
          g_mutex_lock(&rigctl->mutex);
          g_idle_add(parse_cmd,cmd);
          g_mutex_unlock(&rigctl->mutex);
          command=g_new(char,MAXDATASIZE);
          command_index=0;
        }
      }
    }
  g_free(command);   // free the still-accumulating buffer (never handed to a COMMAND)
  close(rigctl->serial_fd);
  return NULL;
}

int launch_serial (RECEIVER *rx) {
  log_info("%s: serial_port=%s\n",__FUNCTION__,rx->rigctl_serial_port);

  RIGCTL *rigctl=(RIGCTL *)rx->rigctl;
  if(rigctl==NULL) {
    rigctl=g_new(RIGCTL,1);
    g_mutex_init(&rigctl->mutex);
    rx->rigctl=(void *)rigctl;
  }

  strcpy(rigctl->ser_port,rx->rigctl_serial_port);
  rigctl->serial_baudrate=rx->rigctl_serial_baudrate;
  rigctl->serial_fd=open(rigctl->ser_port, O_RDWR | O_NOCTTY | O_SYNC);   
  if(rigctl->serial_fd < 0) {
    rx->rigctl_serial_enable=FALSE;
    log_info("%s: Error %d opening %s: %s\n", __FUNCTION__,errno, rigctl->ser_port, strerror (errno));
    return 0 ;
  }

  //set_interface_attribs(rigctl->serial_fd, serial_baudrate, serial_parity); 
  set_interface_attribs(rigctl->serial_fd, rigctl->serial_baudrate, 0); 
  set_blocking(rigctl->serial_fd, 1);                   // set no blocking

  serial_server_thread_id = g_thread_new( "Serial server", serial_server, rx);
  if(!serial_server_thread_id ) {
    g_free(rigctl);
    log_info("g_thread_new failed on serial_server\n");
    return 0;
  }
  return 1;
}

// Serial Port close
void disable_serial (RECEIVER *rx) {
  RIGCTL *rigctl=(RIGCTL *)rx->rigctl;
  log_info("%s: Disable Serial port %s\n",__FUNCTION__,rigctl->ser_port);
  rigctl->serial_running=FALSE;
}

//
// each receiver has it's own thread and allows one connection
//

void launch_rigctl (RECEIVER *rx) {
   
  log_info("%s: port=%d\n",__FUNCTION__,rx->rigctl_port);

  RIGCTL *rigctl=(RIGCTL *)rx->rigctl;
  if(rigctl==NULL) {
    rigctl=g_new(RIGCTL,1);
    g_mutex_init(&rigctl->mutex);
    rx->rigctl=rigctl;
    rigctl->debug=rx->rigctl_debug;
  }

  // This routine encapsulates the thread call
  rigctl->listening_port=rx->rigctl_port;
  rigctl->server_thread_id=g_thread_new("rigctl server",rigctl_server,rx);
  if(!rigctl->server_thread_id ) {
    log_info("%s: g_thread_new failed on rigctl_server\n",__FUNCTION__);
  }
}

void rigctl_set_debug(RECEIVER *rx) {
  RIGCTL *rigctl=(RIGCTL *)rx->rigctl;
  rigctl->debug=rx->rigctl_debug;

  if(rigctl->debug) {
    log_info("---------- CAT DEBUG ON ----------\n");
  } else {
    log_info("---------- CAT DEBUG OFF ----------\n");
  }

}
