/* Copyright (C)
* 2019 - John Melton, G0ORX/N6LYT
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

#ifndef _SOAPY_DISCOVERY_H
#define _SOAPY_DISCOVERY_H

#include <glib.h>

void soapy_discovery(void);

// Network SoapySDR devices the operator added by hand.  A device on another
// subnet answers no scan - neither the USB one nor the mDNS one - so there is
// nothing to discover and the only way to reach it is to be told where it is.
// The list is remembered in ~/.local/share/machpsdr/devices.props and probed at
// every discovery; adding one probes it immediately, so it shows up in the
// device list without restarting.
#define SOAPY_NETDEV_MAX 8

typedef struct {
  char driver[32];       // SoapySDR driver key, e.g. "plutosdr"
  char address[96];      // as typed by the operator, e.g. "192.168.36.190"
} SOAPY_NETDEV;

// The kinds of network device the UI offers.  Only PlutoSDR for now; adding
// another is one line here plus its uri syntax.
typedef struct {
  const char *label;     // shown in the drop-down
  const char *driver;
  const char *uri_fmt;   // printf format turning the address into a Soapy uri
  const char *hint;      // placeholder text for the address entry
} SOAPY_NETDEV_TYPE;

extern const SOAPY_NETDEV_TYPE soapy_netdev_types[];
int soapy_netdev_types_count(void);

int soapy_netdev_count(void);
const SOAPY_NETDEV *soapy_netdev_at(int i);

// Probe a device and, if it answers, append it to discovered[] and remember it.
// Returns FALSE (and leaves the list untouched) if it could not be opened;
// error, if any, is left in SoapySDRDevice_lastError().
gboolean soapy_netdev_add(const char *driver, const char *address);

// Forget the saved device backing discovered[index], if any.  Returns FALSE if
// that device was not one of ours.  The device list must be rebuilt afterwards.
gboolean soapy_netdev_forget_discovered(int index);

// TRUE if discovered[index] came from the saved list (so it can be forgotten).
gboolean soapy_netdev_is_saved(int index);

#endif
