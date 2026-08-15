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

#include <stdlib.h>
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <gtk/gtk.h>

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "error_handler.h"
#include "discovered.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"

// This logs and returns; it does not put up a dialog and does not terminate.
// It used to build a second "... will terminate in 5 seconds" message (naming
// piHPSDR, which this is not) into the same buffer and then drop it on the
// floor, next to a timeout_cb that destroyed a `dialog` nothing ever assigned --
// gtk_window_destroy(NULL) on a path that was already unreachable. Both are
// gone; what remains is the one thing the single caller (protocol1.c, on a
// failed recvfrom) actually wanted.
//
// It also formats straight into the log rather than through a fixed 1024-byte
// stack buffer filled with two caller-supplied %s -- the shape ASan caught in
// radio_restore_state, and a trap for the next caller even though today's one
// passes a literal and strerror().
void error_handler(char *text,char *err) {
  log_error("ERROR: %s: %s\n", text ? text : "(null)", err ? err : "(null)");
}
