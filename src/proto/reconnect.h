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

#ifndef _RECONNECT_H
#define _RECONNECT_H

#include <glib.h>

// Disconnect watchdog.  Each protocol receive path calls reconnect_note_data()
// whenever a valid data block arrives from the hardware.  A GTK-main-thread
// timer (armed by reconnect_init) watches for a prolonged data gap and, on a
// suspected disconnect, pops a modal dialog offering Reconnect / Exit instead
// of leaving the app frozen with a dead stream.

// Arm the watchdog.  Call once, on the GTK main thread, after the radio has
// started streaming.  Safe to call again (re-arm) after a protocol restart.
extern void reconnect_init(void);

// Called from protocol/receive threads on every valid data block received.
// Thread-safe and cheap.
extern void reconnect_note_data(void);

// The dialog's two buttons, and "ask the operator" (the default).
#define RECONNECT_ASK            (-1)
#define RECONNECT_BTN_RECONNECT    0
#define RECONNECT_BTN_EXIT         1

// Test hook: answer the lost-link dialog with `button` instead of raising it.
// Set from MACHPSDR_RECONNECT_TEST in radio.c -- see the comment there.  The
// two branches are the same code a click runs, so this cannot drift into
// exercising something the operator never gets.
extern void reconnect_set_auto(int button);

#endif
