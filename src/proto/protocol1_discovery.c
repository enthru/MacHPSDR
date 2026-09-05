/* Copyright (C)
* 2018 - John Melton, G0ORX/N6LYT
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

#include "net_compat.h"   // must precede gtk.h: winsock2 before windows.h
#include <gtk/gtk.h>
#include "log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol1_discovery.h"

#define DISCOVERY_PORT 1024

// One interface's discovery pass, and everything it used to keep in file-static
// storage. It is per-pass because the passes now RUN AT THE SAME TIME: each
// costs a full SO_RCVTIMEO second whether or not anything answers, and walking
// the adapters one after another made start-up a second per adapter -- three
// here (en0, en7, lo0), more on a machine carrying VPN or VM adapters, all of
// it spent waiting on sockets that are not talking to each other. A shared
// `discovery_socket` is what forced the sequence; giving each pass its own is
// what removes it.
typedef struct {
    char   interface_name[64];
    struct sockaddr_in interface_addr;
    struct sockaddr_in interface_netmask;
    int    socket;
    GThread *thread;
} DISCOVER_PASS;

static gpointer discover_receive_thread(gpointer data);

// inet_ntoa() returns a pointer to ONE static buffer, so two threads formatting
// two addresses hand each other the wrong string -- and with a pass per adapter
// there are now several. Every log line here takes its own buffer.
static const char *addr_str(struct in_addr a, char *buf, size_t len) {
    return inet_ntop(AF_INET, &a, buf, (socklen_t)len) ? buf : "?";
}

// Start one interface's discovery pass: bind, broadcast, and leave the receive
// thread running. The caller joins it in discover_finish(); TRUE means there is
// something to join.
//
// Every failure here is that interface's alone: this runs once per adapter and
// a machine has several that cannot carry a broadcast (loopback, disconnected
// NICs, the virtual adapters VPN and VM software leave behind), so a failure
// SKIPS the interface -- it does not exit(-1) and take the application down
// during start-up, which is what each of these used to do. That argument was
// already made in this file for the sendto below; it is the same argument for
// the socket, the bind and the sockopt.
static gboolean discover_start(struct ifaddrs* iface, DISCOVER_PASS *pass) {
    int rc;
    struct sockaddr_in *sa;
    struct sockaddr_in *mask;
    int discovery_socket;
    char *interface_name=pass->interface_name;
    struct sockaddr_in *interface_addr=&pass->interface_addr;
    struct sockaddr_in *interface_netmask=&pass->interface_netmask;

    // NOT strcpy: on Windows ifa_name is the adapter's FriendlyName out of
    // net_compat.c's getifaddrs() shim, a 256-byte buffer holding a name the
    // operator can rename at will and which several virtual adapters exceed 63
    // characters of by default. POSIX caps it at IFNAMSIZ (16) and cannot
    // overflow; Windows is the platform that has never run on real hardware.
    g_strlcpy(interface_name,iface->ifa_name,sizeof(pass->interface_name));
    log_info("discover: looking for HPSDR devices on %s\n", interface_name);

    // send a broadcast to locate hpsdr boards on the network
    discovery_socket=socket(PF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(discovery_socket<0) {
        net_perror("discover: create socket failed for discovery_socket");
        return FALSE;
    }
    pass->socket=discovery_socket;

    int optval = 1;
    setsockopt(discovery_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    sa = (struct sockaddr_in *) iface->ifa_addr;
    mask = (struct sockaddr_in *) iface->ifa_netmask;
    interface_netmask->sin_addr.s_addr = mask->sin_addr.s_addr;

    // bind to this interface and the discovery port
    //interface_addr->sin_family = AF_INET;
    interface_addr->sin_family = iface->ifa_addr->sa_family;
    interface_addr->sin_addr.s_addr = sa->sin_addr.s_addr;
    //interface_addr->sin_port = htons(DISCOVERY_PORT*2);
    interface_addr->sin_port = htons(0); // system assigned port
    if(bind(discovery_socket,(struct sockaddr*)interface_addr,sizeof(*interface_addr))<0) {
        net_perror("discover: bind socket failed for discovery_socket");
        closesocket(discovery_socket);
        return FALSE;
    }

    log_info("discover: bound to %s\n",interface_name);

    // allow broadcast on the socket
    int on=1;
    rc=setsockopt(discovery_socket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    if(rc != 0) {
        log_info("discover: cannot set SO_BROADCAST: rc=%d\n", rc);
        closesocket(discovery_socket);
        return FALSE;
    }

    // setup to address
    struct sockaddr_in to_addr={0};
    //to_addr.sin_family=AF_INET;
    to_addr.sin_family = iface->ifa_addr->sa_family;
    to_addr.sin_port=htons(DISCOVERY_PORT);
    to_addr.sin_addr.s_addr=htonl(INADDR_BROADCAST);

    // start a receive thread to collect discovery response packets
    pass->thread = g_thread_new( "protocol1 discover receive", discover_receive_thread, pass);
    if( ! pass->thread )
    {
        log_info("g_thread_new failed on discover_receive_thread\n");
        closesocket(discovery_socket);
        return FALSE;
    }



    // send discovery packet
    unsigned char buffer[63];
    buffer[0]=0xEF;
    buffer[1]=0xFE;
    buffer[2]=0x02;
    int i;
    for(i=3;i<63;i++) {
        buffer[i]=0x00;
    }

    if(sendto(discovery_socket,buffer,63,0,(struct sockaddr*)&to_addr,sizeof(to_addr))<0) {
        net_perror("discover: sendto socket failed for discovery_socket");
        // A per-adapter broadcast failure is NOT fatal, on any platform and
        // whatever the code.  This runs once per adapter and a typical machine
        // has several that cannot carry a broadcast: loopback (observed on
        // Windows: WSAEINVAL 10022, with the socket bound to 127.0.0.1),
        // disconnected NICs, and the virtual adapters VPN and VM software leave
        // behind -- and on POSIX a firewall answers EPERM.  Enumerating the
        // tolerable codes is guesswork, which is what the errno test here used
        // to be: it tolerated two codes and called exit(-1) on the rest, so one
        // adapter out of several saying no killed the application during
        // start-up.  Windows already tolerated everything; both do now.
        //
        // Deliberately NOT an early return: the receive thread was started
        // above, so falling through to the join below is what reaps it — and it
        // returns on its own within one SO_RCVTIMEO period.
    }

    return TRUE;
}

// The other half: one SO_RCVTIMEO second, waited out concurrently with every
// other adapter's rather than after it.
static void discover_finish(DISCOVER_PASS *pass) {
    g_thread_join(pass->thread);
    closesocket(pass->socket);
    log_info("discover: exiting discover for %s\n",pass->interface_name);
}

//static void *discover_receive_thread(void* arg) {
static gpointer discover_receive_thread(gpointer data) {
    DISCOVER_PASS *pass=(DISCOVER_PASS *)data;
    int discovery_socket=pass->socket;
    struct sockaddr_in addr;
    socklen_t len;
    unsigned char buffer[2048];
    char abuf[INET_ADDRSTRLEN];
    int bytes_read;
    int i;
    int version;

log_info("discover_receive_thread: %s\n",pass->interface_name);

    version=0;

    net_set_rcvtimeo(discovery_socket, 1000);

    len=sizeof(addr);
    while(1) {
        bytes_read=recvfrom(discovery_socket,buffer,sizeof(buffer),0,(struct sockaddr*)&addr,&len);
        if(bytes_read<0) {
            log_info("discovery: bytes read %d\n", bytes_read);
            net_perror("discovery: recvfrom socket failed for discover_receive_thread");
            break;
        }
        log_info("discovered: received %d bytes\n",bytes_read);
        // A count off the wire is not a bound, and neither is the absence of
        // one: this parse reaches buffer[0x15] (the Hermes-Lite receiver count),
        // so anything shorter would have it reading whatever the last, longer
        // datagram left on the stack -- and inventing a board type, a firmware
        // version and a MAC out of it. The port is open to the whole LAN and a
        // three-byte "EF FE 02" is enough to reach here. A real reply is 60
        // bytes; the bound is tied to the highest index this code touches
        // rather than to a spec number no board here can confirm.
        if(bytes_read < 0x16) continue;
        if ((buffer[0] & 0xFF) == 0xEF && (buffer[1] & 0xFF) == 0xFE) {
            int status = buffer[2] & 0xFF;
            if (status == 2 || status == 3) {
                g_mutex_lock(&discovery_mutex);
                if(devices<MAX_DEVICES) {
                    discovered[devices].protocol=PROTOCOL_1;
                    version=buffer[9]&0xFF;                    
                    sprintf(discovered[devices].software_version,"%d",version);
 
                    switch(buffer[10]&0xFF) {
                        case OLD_DEVICE_METIS:
                            discovered[devices].device=DEVICE_METIS;
                            strcpy(discovered[devices].name,"Metis");
                            discovered[devices].supported_receivers=5;
                            discovered[devices].supported_transmitters=1;
                            discovered[devices].adcs=1;
                            discovered[devices].ps_tx_fdbk_chan = 1;
                            discovered[devices].frequency_min=0.0;
                            discovered[devices].frequency_max=61440000.0;
                            break;
                        case OLD_DEVICE_HERMES:
                            discovered[devices].device=DEVICE_HERMES;
                            strcpy(discovered[devices].name,"Hermes");
                            discovered[devices].supported_receivers=5;
                            discovered[devices].supported_transmitters=1;
                            discovered[devices].adcs=1;
                            discovered[devices].ps_tx_fdbk_chan = 1;
                            discovered[devices].frequency_min=0.0;
                            discovered[devices].frequency_max=61440000.0;
                            break;
                        case OLD_DEVICE_ANGELIA:
                            discovered[devices].device=DEVICE_ANGELIA;
                            strcpy(discovered[devices].name,"Angelia");
                            discovered[devices].supported_receivers=7;
                            discovered[devices].supported_transmitters=1;
                            discovered[devices].adcs=2;
                            discovered[devices].ps_tx_fdbk_chan = 4;
                            discovered[devices].frequency_min=0.0;
                            discovered[devices].frequency_max=61440000.0;
                            break;
                        case OLD_DEVICE_ORION:
                            discovered[devices].device=DEVICE_ORION;
                            strcpy(discovered[devices].name,"Orion");
                            discovered[devices].supported_receivers=7;
                            discovered[devices].supported_transmitters=1;
                            discovered[devices].adcs=2;
                            discovered[devices].ps_tx_fdbk_chan = 4;
                            discovered[devices].frequency_min=0.0;
                            discovered[devices].frequency_max=61440000.0;
                            break;
                        case OLD_DEVICE_HERMES_LITE:
                            discovered[devices].device=DEVICE_HERMES_LITE;
			                      if (version < 42) {
                              strcpy(discovered[devices].name,"Hermes Lite V1");
                              discovered[devices].supported_receivers = 2;                                
			                      } else {
                              strcpy(discovered[devices].name,"Hermes Lite V2");
			                        discovered[devices].device = DEVICE_HERMES_LITE2;
                              // HL2 send max supported receveirs in discovery response.
                              discovered[devices].supported_receivers=buffer[0x13];   
                              int patch = buffer[0x15]&0xFF; 
                              log_info("Patch num = %d\n", patch); 
                              char gateware_patch[8];
                              snprintf(gateware_patch, sizeof(gateware_patch),"%d", patch);

                              int char_len = strlen(discovered[devices].software_version);
                              discovered[devices].software_version[char_len] = 'p';                              
                              discovered[devices].software_version[char_len+1] = gateware_patch[0];                                
                              discovered[devices].software_version[char_len+2] = '\0';
			                      }                            
                            discovered[devices].supported_transmitters=1;
                            discovered[devices].adcs=1;
                            discovered[devices].ps_tx_fdbk_chan = 3;
                            discovered[devices].frequency_min=0.0;
                            discovered[devices].frequency_max=30720000.0;
                            break;
                        case OLD_DEVICE_ORION2:
                            discovered[devices].device=DEVICE_ORION2;
                            strcpy(discovered[devices].name,"Orion 2");
                            discovered[devices].supported_receivers=7;
                            discovered[devices].supported_transmitters=1;
                            discovered[devices].adcs=2;
                            discovered[devices].ps_tx_fdbk_chan = 4;
                            discovered[devices].frequency_min=0.0;
                            discovered[devices].frequency_max=61440000.0;
                            break;
                        default:
                            discovered[devices].device=DEVICE_UNKNOWN;
                            strcpy(discovered[devices].name,"Unknown");
                            discovered[devices].supported_receivers=7;
                            discovered[devices].supported_transmitters=1;
                            discovered[devices].adcs=1;
                            discovered[devices].ps_tx_fdbk_chan = -1;
                            discovered[devices].frequency_min=0.0;
                            discovered[devices].frequency_max=61440000.0;
                            break;
                    }

                    for(i=0;i<6;i++) {
                        discovered[devices].info.network.mac_address[i]=buffer[i+3];
                    }
                    discovered[devices].status=status;
                    memcpy((void*)&discovered[devices].info.network.address,(void*)&addr,sizeof(addr));
                    discovered[devices].info.network.address_length=sizeof(addr);
                    memcpy((void*)&discovered[devices].info.network.interface_address,(void*)&pass->interface_addr,sizeof(pass->interface_addr));
                    memcpy((void*)&discovered[devices].info.network.interface_netmask,(void*)&pass->interface_netmask,sizeof(pass->interface_netmask));
                    discovered[devices].info.network.interface_length=sizeof(pass->interface_addr);
                    g_strlcpy(discovered[devices].info.network.interface_name,pass->interface_name,
                              sizeof(discovered[devices].info.network.interface_name));
                    log_info("discovery: found device=%d software_version=%s status=%d address=%s (%02X:%02X:%02X:%02X:%02X:%02X) on %s\n",
                            discovered[devices].device,
                            discovered[devices].software_version,
                            discovered[devices].status,
                            addr_str(discovered[devices].info.network.address.sin_addr,abuf,sizeof(abuf)),
                            discovered[devices].info.network.mac_address[0],
                            discovered[devices].info.network.mac_address[1],
                            discovered[devices].info.network.mac_address[2],
                            discovered[devices].info.network.mac_address[3],
                            discovered[devices].info.network.mac_address[4],
                            discovered[devices].info.network.mac_address[5],
                            discovered[devices].info.network.interface_name);
                    devices++;
                }
                g_mutex_unlock(&discovery_mutex);
            }
        }

    }
    log_info("discovery: exiting discover_receive_thread for %s\n",pass->interface_name);
    //g_thread_exit(NULL);
    return NULL;
}

void protocol1_discovery(void) {
    struct ifaddrs *addrs,*ifa;
    GPtrArray *passes;
    char abuf[INET_ADDRSTRLEN];
    guint p;

log_info("protocol1_discovery\n");
    // addrs is left UNSET when this fails, so the walk below would run off an
    // uninitialised pointer rather than simply finding nothing.
    if(getifaddrs(&addrs)!=0 || addrs==NULL) {
        log_error("protocol1_discovery: getifaddrs failed, no interfaces to scan\n");
        return;
    }
    // Two phases, and the split is the whole point: every adapter is broadcast
    // to first, and only then is the second waited out. Sequentially this cost
    // one SO_RCVTIMEO PER ADAPTER of start-up, for adapters that have nothing
    // to say to each other.
    passes = g_ptr_array_new_with_free_func(g_free);
    ifa = addrs;
    while (ifa) {
        // NB: runs in a worker thread (see discovery.c) — do not pump the GTK
        // main context here.
        if (ifa->ifa_addr && (ifa->ifa_addr->sa_family == AF_INET || ifa->ifa_addr->sa_family==AF_LOCAL)) {
            if((ifa->ifa_flags&IFF_UP)==IFF_UP
                && (ifa->ifa_flags&IFF_RUNNING)==IFF_RUNNING
                /*&& (ifa->ifa_flags&IFF_LOOPBACK)!=IFF_LOOPBACK*/) {
                DISCOVER_PASS *pass=g_new0(DISCOVER_PASS,1);
                if(discover_start(ifa,pass)) {
                    g_ptr_array_add(passes,pass);
                } else {
                    g_free(pass);
                }
            }
        }
        ifa = ifa->ifa_next;
    }
    for(p=0;p<passes->len;p++) {
        discover_finish(g_ptr_array_index(passes,p));
    }
    // Only now: every pass copied what it needed out of its ifaddrs entry, but
    // nothing is freed underneath a running thread.
    g_ptr_array_free(passes,TRUE);
    freeifaddrs(addrs);

    log_info( "discovery found %d devices\n",devices);

    int i;
    for(i=0;i<devices;i++) {
                    log_info("discovery: found device=%d software_version=%s status=%d address=%s (%02X:%02X:%02X:%02X:%02X:%02X) on %s\n",
                            discovered[i].device,
                            discovered[i].software_version,
                            discovered[i].status,
                            addr_str(discovered[i].info.network.address.sin_addr,abuf,sizeof(abuf)),
                            discovered[i].info.network.mac_address[0],
                            discovered[i].info.network.mac_address[1],
                            discovered[i].info.network.mac_address[2],
                            discovered[i].info.network.mac_address[3],
                            discovered[i].info.network.mac_address[4],
                            discovered[i].info.network.mac_address[5],
                            discovered[i].info.network.interface_name);
    }

}

