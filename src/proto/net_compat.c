/*  net_compat.c

    The single BSD-sockets / Winsock seam — see net_compat.h.

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

#include "net_compat.h"

#if defined(_WIN32)

#include <iphlpapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int net_startup(void) {
  WSADATA wsa;
  int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
  return rc == 0;
}

void net_cleanup(void) {
  WSACleanup();
}

int net_set_nonblocking(int fd, int on) {
  u_long mode = on ? 1 : 0;
  return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
}

int net_set_rcvtimeo(int fd, int ms) {
  DWORD timeout = (DWORD)ms;         /* Winsock: milliseconds, not a timeval */
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                    &timeout, sizeof(timeout)) == 0 ? 0 : -1;
}

void net_perror(const char *msg) {
  fprintf(stderr, "%s: winsock error %d\n", msg ? msg : "socket", WSAGetLastError());
}

int net_send_nowait(int fd, const void *buf, size_t len, int flags) {
  struct timeval zero;
  fd_set wr;
  zero.tv_sec = 0;
  zero.tv_usec = 0;
  FD_ZERO(&wr);
  FD_SET((SOCKET)fd, &wr);
  /* select()'s first argument is ignored by Winsock. */
  if (select(0, NULL, &wr, NULL, &zero) <= 0) return -1;   /* would block */
  return send(fd, buf, len, flags);
}

/* ------------------------------------------------------------------ *
 *  getifaddrs() over GetAdaptersAddresses()
 * ------------------------------------------------------------------ */

/*
 * The public struct is the FIRST member, so freeifaddrs() can hand the node
 * pointer straight to free() without a second lookup table.
 */
typedef struct {
  struct ifaddrs     pub;
  char               name[256];
  struct sockaddr_in addr;
  struct sockaddr_in mask;
} ifaddr_node;

/*
 * Windows reports a prefix length, POSIX code wants a mask.  A prefix of 0 is
 * left as 0.0.0.0 rather than shifted: a 32-bit shift by 32 is undefined
 * behaviour, and this is the one input (an unconfigured adapter) that produces
 * it.
 */
static uint32_t mask_from_prefix(ULONG prefix) {
  if (prefix == 0)  return 0;
  if (prefix >= 32) return 0xFFFFFFFFu;
  return 0xFFFFFFFFu << (32 - prefix);
}

static void adapter_name(const IP_ADAPTER_ADDRESSES *aa, char *out, size_t len) {
  out[0] = '\0';
  /* FriendlyName is what the operator sees in Windows ("Ethernet", "Wi-Fi");
   * AdapterName is a GUID string and is only a fallback, since this name is
   * logged and shown in the discovery list. */
  if (aa->FriendlyName &&
      WideCharToMultiByte(CP_UTF8, 0, aa->FriendlyName, -1,
                          out, (int)len, NULL, NULL) > 0) {
    return;
  }
  if (aa->AdapterName) {
    strncpy(out, aa->AdapterName, len - 1);
    out[len - 1] = '\0';
  }
}

int getifaddrs(struct ifaddrs **ifap) {
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                      GAA_FLAG_SKIP_DNS_SERVER;
  IP_ADAPTER_ADDRESSES *buf = NULL;
  ULONG size = 16 * 1024;
  ULONG rc;
  int tries;
  struct ifaddrs *head = NULL, *tail = NULL;

  if (!ifap) return -1;
  *ifap = NULL;

  /* The adapter list can grow between the sizing call and the real one, so
   * ERROR_BUFFER_OVERFLOW is retried rather than treated as fatal. */
  for (tries = 0; tries < 4; tries++) {
    void *grown = realloc(buf, size);
    if (!grown) { free(buf); return -1; }
    buf = (IP_ADAPTER_ADDRESSES *)grown;
    rc = GetAdaptersAddresses(AF_INET, flags, NULL, buf, &size);
    if (rc != ERROR_BUFFER_OVERFLOW) break;
  }
  if (rc != NO_ERROR) { free(buf); return -1; }

  for (IP_ADAPTER_ADDRESSES *aa = buf; aa != NULL; aa = aa->Next) {
    unsigned int fl = 0;
    if (aa->OperStatus == IfOperStatusUp)          fl |= IFF_UP | IFF_RUNNING;
    if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK)   fl |= IFF_LOOPBACK;

    for (IP_ADAPTER_UNICAST_ADDRESS *ua = aa->FirstUnicastAddress;
         ua != NULL; ua = ua->Next) {
      const struct sockaddr_in *sa;
      ifaddr_node *n;

      if (!ua->Address.lpSockaddr ||
          ua->Address.lpSockaddr->sa_family != AF_INET) continue;
      sa = (const struct sockaddr_in *)ua->Address.lpSockaddr;

      n = (ifaddr_node *)calloc(1, sizeof(*n));
      if (!n) { freeifaddrs(head); free(buf); return -1; }

      adapter_name(aa, n->name, sizeof(n->name));

      n->addr = *sa;
      n->mask.sin_family      = AF_INET;
      n->mask.sin_addr.s_addr = htonl(mask_from_prefix(ua->OnLinkPrefixLength));

      n->pub.ifa_name    = n->name;
      n->pub.ifa_flags   = fl;
      n->pub.ifa_addr    = (struct sockaddr *)&n->addr;
      n->pub.ifa_netmask = (struct sockaddr *)&n->mask;
      n->pub.ifa_next    = NULL;

      if (tail) tail->ifa_next = &n->pub; else head = &n->pub;
      tail = &n->pub;
    }
  }

  free(buf);
  *ifap = head;
  return 0;
}

void freeifaddrs(struct ifaddrs *ifa) {
  while (ifa) {
    struct ifaddrs *next = ifa->ifa_next;
    free(ifa);            /* the public struct is the node's first member */
    ifa = next;
  }
}

#else /* !_WIN32 */

#include <stdio.h>

int net_startup(void) { return 1; }
void net_cleanup(void) { }

int net_set_nonblocking(int fd, int on) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) return -1;
  if (on) flags |=  O_NONBLOCK;
  else    flags &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags) == -1 ? -1 : 0;
}

int net_set_rcvtimeo(int fd, int ms) {
  struct timeval tv;
  tv.tv_sec  = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int net_send_nowait(int fd, const void *buf, size_t len, int flags) {
  return (int)send(fd, buf, len, flags | MSG_DONTWAIT);
}

void net_perror(const char *msg) {
  perror(msg);
}

#endif /* _WIN32 */
