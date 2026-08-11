/*  net_compat.h

    The single BSD-sockets / Winsock seam.

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

/*
 * On Linux/macOS this header is nothing but the union of the POSIX network
 * headers the networking sources already included, so including it in place of
 * that block is a no-op on the platforms that work today.  Everything below is
 * for the Windows build.
 *
 * Rules for the networking sources:
 *   - include "net_compat.h" INSTEAD of <sys/socket.h> & friends, and include
 *     it FIRST (see the winsock2/windows.h ordering trap below),
 *   - close a socket with closesocket(), never close(),
 *   - make a socket non-blocking with net_set_nonblocking(), never fcntl(),
 *   - guard SO_REUSEPORT with #ifdef (Windows has no such option).
 */

#ifndef NET_COMPAT_H
#define NET_COMPAT_H

#if defined(_WIN32)

/*
 * ORDERING TRAP: <windows.h> pulls in the Winsock *1.1* <winsock.h> unless
 * WIN32_LEAN_AND_MEAN is defined, and winsock.h and winsock2.h cannot coexist
 * (redefinition of every sockaddr).  GTK's headers include <windows.h>, so any
 * translation unit that mixes GTK and sockets is one include-order slip away
 * from a wall of redefinition errors.  Defining WIN32_LEAN_AND_MEAN here is a
 * belt; the Makefile passes -DWIN32_LEAN_AND_MEAN globally as the braces, so
 * the guarantee does not depend on which header a given .c happens to reach
 * first.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

/* Windows 7: WSAPoll and the GetAdaptersAddresses prefix-length field. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * Socket descriptors stay `int`, deliberately.
 *
 * Win64's SOCKET is a 64-bit UINT_PTR, so the textbook port retypes every
 * descriptor — including the ones living in RADIO/RECEIVER — to SOCKET.  That
 * would be a shared-header struct-layout change (see CLAUDE.md on stale .o
 * files) for no benefit: Windows documents socket handles as values that fit in
 * 32 bits, and the failure tests still read correctly through an int, because
 * (int)INVALID_SOCKET == -1 and SOCKET_ERROR == -1.  So the existing `fd < 0`
 * checks keep their meaning and no struct changes size.
 */

/* POSIX spells this close(); Winsock insists on its own call. */
/* (closesocket() is already declared by winsock2.h.) */

/* mingw-w64 declares WSAPoll but not poll(); struct pollfd is already there. */
#define poll(fds, nfds, timeout) WSAPoll((fds), (ULONG)(nfds), (INT)(timeout))
typedef unsigned long nfds_t;

/*
 * Winsock's sockopt/payload arguments are char pointers, ours are pointers to
 * int or to unsigned char.  In C that is an incompatible-pointer warning at
 * every one of the call sites.  Wrapping
 * the five calls in void*-taking inlines keeps the ~50 call sites byte-identical
 * instead of sprinkling casts through the protocol hot paths.
 */
static inline int net_setsockopt_(int s, int level, int opt,
                                  const void *val, socklen_t len) {
  return setsockopt((SOCKET)s, level, opt, (const char *)val, (int)len);
}
static inline int net_getsockopt_(int s, int level, int opt,
                                  void *val, socklen_t *len) {
  int n = len ? (int)*len : 0;
  int rc = getsockopt((SOCKET)s, level, opt, (char *)val, len ? &n : NULL);
  if (len) *len = (socklen_t)n;
  return rc;
}
static inline int net_send_(int s, const void *buf, size_t len, int flags) {
  return send((SOCKET)s, (const char *)buf, (int)len, flags);
}
static inline int net_recv_(int s, void *buf, size_t len, int flags) {
  return recv((SOCKET)s, (char *)buf, (int)len, flags);
}
static inline int net_sendto_(int s, const void *buf, size_t len, int flags,
                              const struct sockaddr *to, socklen_t tolen) {
  return sendto((SOCKET)s, (const char *)buf, (int)len, flags, to, (int)tolen);
}
static inline int net_recvfrom_(int s, void *buf, size_t len, int flags,
                                struct sockaddr *from, socklen_t *fromlen) {
  int n = fromlen ? (int)*fromlen : 0;
  int rc = recvfrom((SOCKET)s, (char *)buf, (int)len, flags,
                    from, fromlen ? &n : NULL);
  if (fromlen) *fromlen = (socklen_t)n;
  return rc;
}
#define setsockopt net_setsockopt_
#define getsockopt net_getsockopt_
#define send       net_send_
#define recv       net_recv_
#define sendto     net_sendto_
#define recvfrom   net_recvfrom_

/* Interface flags: POSIX gets these from <net/if.h>.  Only our own
 * getifaddrs() shim ever sets them, so the values just have to be distinct. */
#ifndef IFF_UP
#define IFF_UP        0x0001
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK  0x0008
#endif
#ifndef IFF_RUNNING
#define IFF_RUNNING   0x0040
#endif

/*
 * getifaddrs() over GetAdaptersAddresses().  This is not a general-purpose
 * implementation: it reports IPv4 unicast addresses only, which is all the
 * HPSDR discovery code looks at (it walks the list for AF_INET interfaces that
 * are up and running, and broadcasts on each one's netmask).
 */
struct ifaddrs {
  struct ifaddrs  *ifa_next;
  char            *ifa_name;
  unsigned int     ifa_flags;
  struct sockaddr *ifa_addr;
  struct sockaddr *ifa_netmask;
  void            *ifa_data;
};

int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

/* Winsock keeps its error code out of errno. */
#define net_errno() WSAGetLastError()

#else /* !_WIN32 — the union of what the networking sources included before */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* net/if_arp.h does not exist on macOS and nothing here uses it. */
#if defined(__linux__)
#include <net/if_arp.h>
#endif

#define closesocket(fd) close(fd)
#define net_errno()     errno

#endif /* _WIN32 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Winsock needs an explicit per-process startup before any socket call, and
 * never returns a usable socket without it.  Called once from main(); a no-op
 * everywhere else.  Returns TRUE on success.
 */
int  net_startup(void);
void net_cleanup(void);

/*
 * O_NONBLOCK via fcntl() has no Winsock equivalent (ioctlsocket/FIONBIO does
 * the job, and cannot report the previous state — which is why this takes the
 * flag rather than returning it).  Returns 0 on success, -1 on failure.
 */
int  net_set_nonblocking(int fd, int on);

#ifdef __cplusplus
}
#endif

#endif /* NET_COMPAT_H */
