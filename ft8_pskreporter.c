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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <gtk/gtk.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"

#include "ft8_pskreporter.h"

// PSK Reporter's public collector.  UDP; the same endpoint WSJT-X/JTDX feed.
#define PSKR_HOST   "report.pskreporter.info"
#define PSKR_PORT   "4739"

// PSK Reporter's IPFIX private enterprise number (RFC 5101 style templates).
#define PSKR_PEN    30351U

// Template IDs we advertise (arbitrary but stable, matching WSJT-X's choice so
// nothing on the server side is surprised).
#define TID_RX      0x50E2   // receiver-information (options) template
#define TID_TX      0x50E3   // sender/reception-record template

#define IPFIX_VERSION 0x000AU

// ---- little serialisation helpers (all big-endian, per IPFIX) --------------
static int put_u8(uint8_t *b, int off, uint8_t v)  { b[off]=v; return off+1; }
static int put_u16(uint8_t *b, int off, uint16_t v){ b[off]=(v>>8)&0xff; b[off+1]=v&0xff; return off+2; }
static int put_u32(uint8_t *b, int off, uint32_t v){
  b[off]=(v>>24)&0xff; b[off+1]=(v>>16)&0xff; b[off+2]=(v>>8)&0xff; b[off+3]=v&0xff; return off+4;
}
// 5-byte big-endian (PSK Reporter's frequency field width).
static int put_u40(uint8_t *b, int off, uint64_t v){
  b[off]=(v>>32)&0xff; b[off+1]=(v>>24)&0xff; b[off+2]=(v>>16)&0xff;
  b[off+3]=(v>>8)&0xff; b[off+4]=v&0xff; return off+5;
}
// Variable-length string: single-byte length prefix + UTF-8 bytes (<=254).
static int put_vstr(uint8_t *b, int off, const char *s) {
  int len=(int)strlen(s);
  if(len>254) len=254;
  off=put_u8(b,off,(uint8_t)len);
  memcpy(b+off,s,len);
  return off+len;
}
// One enterprise-specific field spec inside a template: field-id (high bit set),
// length, PEN.
static int put_efield(uint8_t *b, int off, uint16_t id, uint16_t len) {
  off=put_u16(b,off,(uint16_t)(0x8000|id));
  off=put_u16(b,off,len);
  off=put_u32(b,off,PSKR_PEN);
  return off;
}
// Pad the buffer up to the next 4-byte boundary (relative to message start).
static int pad4(uint8_t *b, int off) {
  while(off & 3) off=put_u8(b,off,0);
  return off;
}

// A 4/6-char Maidenhead locator (AA00 / AA00aa) — only such an "extra" field is
// a grid we can report as the sender's locator; reports/RRR/73 are not.
static gboolean is_grid(const char *s) {
  int n=(int)strlen(s);
  if(n!=4 && n!=6) return FALSE;
  if(!isalpha((unsigned char)s[0]) || !isalpha((unsigned char)s[1])) return FALSE;
  if(!isdigit((unsigned char)s[2]) || !isdigit((unsigned char)s[3])) return FALSE;
  if(n==6 && (!isalpha((unsigned char)s[4]) || !isalpha((unsigned char)s[5]))) return FALSE;
  return TRUE;
}

void ft8_pskreporter_report(const FT8_DECODE *decodes, int n, time_t slot_time) {
  static uint32_t seq = 0;
  static uint32_t domain = 0;   // random session id, chosen once

  if(radio==NULL || !radio->ft8_pskr) return;
  if(n<=0 || decodes==NULL) return;
  // PSK Reporter needs the receiver's own call + grid to attribute the report.
  if(radio->station_call[0]=='\0' || radio->station_grid[0]=='\0') return;
  RECEIVER *rx=radio->active_receiver;
  if(rx==NULL) return;
  long long dial=(long long)rx->frequency_a;   // DIGU/USB dial; RF = dial + audio Hz

  if(domain==0) {
    srand((unsigned)(time(NULL) ^ (uintptr_t)&seq));
    domain = ((uint32_t)rand()<<16) ^ (uint32_t)rand();
    if(domain==0) domain=1;
  }

  uint8_t buf[1400];
  int off=0;

  // ---- IPFIX message header (length filled in at the end) ----
  off=put_u16(buf,off,IPFIX_VERSION);
  off=put_u16(buf,off,0);                       // length placeholder
  off=put_u32(buf,off,(uint32_t)time(NULL));    // export time
  off=put_u32(buf,off,++seq);                   // sequence number
  off=put_u32(buf,off,domain);                  // observation domain id

  // ---- Receiver-information options template (Set ID 3) ----
  {
    int set=off;
    off=put_u16(buf,off,3);                      // set id: options template
    off=put_u16(buf,off,0);                      // set length placeholder
    off=put_u16(buf,off,TID_RX);                 // template id
    off=put_u16(buf,off,4);                      // field count
    off=put_u16(buf,off,0);                      // scope field count (WSJT-X: 0)
    off=put_efield(buf,off,0x0002,0xFFFF);       // receiverCallsign  (var)
    off=put_efield(buf,off,0x0004,0xFFFF);       // receiverLocator   (var)
    off=put_efield(buf,off,0x0008,0xFFFF);       // decodingSoftware  (var)
    off=put_efield(buf,off,0x0009,0xFFFF);       // antennaInformation(var)
    off=pad4(buf,off);
    put_u16(buf,set+2,(uint16_t)(off-set));      // set length (incl. padding)
  }

  // ---- Sender/reception-record template (Set ID 2) ----
  {
    int set=off;
    off=put_u16(buf,off,2);                      // set id: template
    off=put_u16(buf,off,0);                      // set length placeholder
    off=put_u16(buf,off,TID_TX);                 // template id
    off=put_u16(buf,off,7);                      // field count
    off=put_efield(buf,off,0x0001,0xFFFF);       // senderCallsign   (var)
    off=put_efield(buf,off,0x0005,5);            // frequency        (5 bytes)
    off=put_efield(buf,off,0x0006,1);            // sNR              (1 byte)
    off=put_efield(buf,off,0x000A,0xFFFF);       // mode             (var)
    off=put_efield(buf,off,0x0003,0xFFFF);       // senderLocator    (var)
    off=put_efield(buf,off,0x000B,1);            // informationSource(1 byte)
    off=put_u16(buf,off,0x0096);                 // dateTimeSeconds (IE 150),
    off=put_u16(buf,off,4);                      //   standard element: no PEN
    off=pad4(buf,off);
    put_u16(buf,set+2,(uint16_t)(off-set));
  }

  // ---- Receiver data record (Set ID = TID_RX) ----
  {
    int set=off;
    off=put_u16(buf,off,TID_RX);                 // set id = receiver template id
    off=put_u16(buf,off,0);                      // set length placeholder
    off=put_vstr(buf,off,radio->station_call);
    off=put_vstr(buf,off,radio->station_grid);
    off=put_vstr(buf,off,"MacHPSDR");            // decodingSoftware
    off=put_vstr(buf,off,"");                    // antennaInformation (unused)
    off=pad4(buf,off);
    put_u16(buf,set+2,(uint16_t)(off-set));
  }

  // ---- Sender/reception data records (Set ID = TID_TX) ----
  int set=off;
  off=put_u16(buf,off,TID_TX);                   // set id = sender template id
  off=put_u16(buf,off,0);                        // set length placeholder
  int reported=0;
  for(int i=0;i<n;i++) {
    const FT8_DECODE *d=&decodes[i];
    // Only spot decodes that yielded a real sender callsign; skip free
    // text/telemetry (empty call_de) and never spot our own transmissions.
    if(d->call_de[0]=='\0') continue;
    if(g_ascii_strcasecmp(d->call_de,radio->station_call)==0) continue;

    long long rf=dial + (long long)lroundf(d->freq);
    if(rf<0) continue;
    const char *grid = is_grid(d->extra) ? d->extra : "";
    int snr=(int)lroundf(d->snr);
    if(snr>127) snr=127; else if(snr<-128) snr=-128;

    // Bound each record so we never overrun the datagram (leave a little slack).
    int need = 1+(int)strlen(d->call_de) + 5 + 1 + 1+3 + 1+(int)strlen(grid) + 1 + 4;
    if(off+need+8 > (int)sizeof(buf)) break;

    off=put_vstr(buf,off,d->call_de);            // senderCallsign
    off=put_u40(buf,off,(uint64_t)rf);           // frequency (Hz)
    off=put_u8(buf,off,(uint8_t)(int8_t)snr);    // sNR
    off=put_vstr(buf,off,radio->ft8_proto?"FT4":"FT8"); // mode
    off=put_vstr(buf,off,grid);                  // senderLocator
    off=put_u8(buf,off,1);                        // informationSource (1 = auto)
    off=put_u32(buf,off,(uint32_t)slot_time);    // dateTimeSeconds
    reported++;
  }
  if(reported==0) return;                         // nothing worth sending
  off=pad4(buf,off);
  put_u16(buf,set+2,(uint16_t)(off-set));

  // ---- finalise header length and send ----
  put_u16(buf,2,(uint16_t)off);

  struct addrinfo hints, *res=NULL;
  memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_DGRAM;
  if(getaddrinfo(PSKR_HOST,PSKR_PORT,&hints,&res)!=0 || res==NULL) {
    fprintf(stderr,"pskreporter: cannot resolve %s:%s\n",PSKR_HOST,PSKR_PORT);
    return;
  }
  int fd=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
  if(fd>=0) {
    if(sendto(fd,buf,off,0,res->ai_addr,res->ai_addrlen)<0)
      fprintf(stderr,"pskreporter: sendto %s:%s failed\n",PSKR_HOST,PSKR_PORT);
    else
      fprintf(stderr,"pskreporter: reported %d spot(s) to %s:%s\n",reported,PSKR_HOST,PSKR_PORT);
    close(fd);
  }
  freeaddrinfo(res);
}
