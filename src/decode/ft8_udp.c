/* Copyright (C)
* 2026 - MacHPSDR fork
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <stdio.h>
#include "log.h"
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "net_compat.h"   // must precede gtk.h: winsock2 before windows.h
#include <gtk/gtk.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"

#include "ft8_udp.h"

// The WSJT-X UDP protocol serialises with QDataStream: multi-byte integers are
// big-endian, and each string is a big-endian uint32 byte-count followed by the
// UTF-8 bytes (0xffffffff means a null string).  We only need a couple of
// primitives to build the "ADIF logged" message (schema 2, type 12).
#define WSJTX_MAGIC   0xadbccbdaU
#define WSJTX_SCHEMA  2U
#define WSJTX_ADIF    12U

static int put_u32(uint8_t *buf, int off, uint32_t v) {
  buf[off+0]=(v>>24)&0xff;
  buf[off+1]=(v>>16)&0xff;
  buf[off+2]=(v>>8)&0xff;
  buf[off+3]=v&0xff;
  return off+4;
}

static int put_str(uint8_t *buf, int off, const char *s) {
  int len=(int)strlen(s);
  off=put_u32(buf,off,(uint32_t)len);
  memcpy(buf+off,s,len);
  return off+len;
}

void ft8_udp_log(const char *adif_record) {
  if(radio==NULL || !radio->ft8_log_udp) return;
  if(adif_record==NULL || adif_record[0]=='\0') return;
  if(radio->ft8_log_udp_host[0]=='\0' || radio->ft8_log_udp_port<=0) return;

  // WSJT-X wraps the record in a minimal ADIF document (header + record). Most
  // loggers accept either; sending the header matches WSJT-X/JTDX exactly.
  char adif[1024];
  snprintf(adif,sizeof(adif),
           "\n<adif_ver:5>3.1.0\n<programid:8>MacHPSDR\n<EOH>\n%s",adif_record);

  uint8_t buf[1400];
  // The datagram is three u32 headers, the "MacHPSDR" id as a counted string,
  // and the ADIF text as another -- so the worst case is a compile-time
  // quantity, and this asserts it rather than testing `off` AFTER the writes
  // that would already have overrun (which is what stood here: a bound checked
  // after the copy is not a bound, it is a comment with an if in front of it).
  _Static_assert(sizeof(buf) >= 3*4 + 4+8 + 4+sizeof(adif),
                 "WSJT-X UDP datagram buffer must hold the largest ADIF record");
  int off=0;
  off=put_u32(buf,off,WSJTX_MAGIC);
  off=put_u32(buf,off,WSJTX_SCHEMA);
  off=put_u32(buf,off,WSJTX_ADIF);
  off=put_str(buf,off,"MacHPSDR");   // id (unique key)
  off=put_str(buf,off,adif);         // ADIF text

  char portstr[16];
  snprintf(portstr,sizeof(portstr),"%d",radio->ft8_log_udp_port);
  struct addrinfo hints, *res=NULL;
  memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_DGRAM;
  if(getaddrinfo(radio->ft8_log_udp_host,portstr,&hints,&res)!=0 || res==NULL) {
    log_error("ft8-udp: cannot resolve %s:%s\n",radio->ft8_log_udp_host,portstr);
    return;
  }
  int fd=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
  if(fd>=0) {
    // Allow broadcast/multicast destinations (e.g. 255.255.255.255) to work too.
    int on=1;
    setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&on,sizeof(on));
    if(sendto(fd,buf,off,0,res->ai_addr,res->ai_addrlen)<0)
      log_error("ft8-udp: sendto %s:%s failed\n",radio->ft8_log_udp_host,portstr);
    else
      log_info("ft8-udp: logged QSO to %s:%s\n",radio->ft8_log_udp_host,portstr);
    closesocket(fd);
  }
  freeaddrinfo(res);
}
