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

#include <gtk/gtk.h>
#include "log.h"
#include <gdk/gdk.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol1_discovery.h"
#include "protocol2_discovery.h"
#ifdef SOAPYSDR
#include "soapy_discovery.h"
#endif
#include "fake_protocol.h"

// Shared across protocol1_discovery.c / protocol2_discovery.c. A statically
// allocated GMutex needs no g_mutex_init() (GLib >= 2.32).
GMutex discovery_mutex;
gboolean skip_network_discovery=FALSE;

static gpointer protocol1_discovery_thread(gpointer data) {
  protocol1_discovery();
  return NULL;
}

static gpointer protocol2_discovery_thread(gpointer data) {
  protocol2_discovery();
  return NULL;
}

void discovery() {
log_info("discovery\n");
  devices=0;
  // Run the two blocking network discoveries in parallel (each waits on a
  // socket receive timeout) so the total wait is one timeout window, not two.
  // The receive threads append to the shared discovered[] array under
  // discovery_mutex. Skipped entirely with --usb-only.
  if(!skip_network_discovery) {
    GThread *p1=g_thread_new("p1 discovery", protocol1_discovery_thread, NULL);
    GThread *p2=g_thread_new("p2 discovery", protocol2_discovery_thread, NULL);
    g_thread_join(p1);
    g_thread_join(p2);
  } else {
    log_info("discovery: network (Protocol 1/2) discovery skipped (--usb-only)\n");
  }
#ifdef SOAPYSDR
  soapy_discovery();
#endif
  fake_discovery();
}
