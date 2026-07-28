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

/*
 * DX cluster telnet client + mutex-guarded spot store, feeding the RX
 * panadapter spot overlay (rx_panadapter.c) and click-to-tune
 * (receiver.c).  Always compiled (no feature flag).
 */

#ifndef _DXCLUSTER_H
#define _DXCLUSTER_H

#include <glib.h>
#include <gtk/gtk.h>
#include "radio.h"

#define DXCLUSTER_MAX_SPOTS 256

typedef struct {
  char      call[16];      // DX callsign
  long long freq;          // absolute RF Hz
  char      spotter[16];   // who spotted
  char      comment[40];
  long long utc;           // unix seconds when received
  int       entity;        // ft8_dxcc_entity(call), -1 if unknown
} DX_SPOT;

// Store the RADIO pointer and set up defaults / test-spot injection. Call once
// at start-up, after radio_restore_state() has run and receiver 0 exists.
extern void dxcluster_init(RADIO *radio);

// Spawn the client thread if radio->cluster_enable and not already running.
extern void dxcluster_start(void);

// Signal the client thread to stop, close the socket to unblock recv(), and
// join. Safe to call when not running.
extern void dxcluster_stop(void);

extern gboolean dxcluster_running(void);

// "disconnected" / "connecting" / "connected" / "error: ..."
extern const char *dxcluster_status(void);

// Snapshot API for the overlay / hit-test: caller must hold the lock while
// reading via dxcluster_count()/dxcluster_spot().
extern void dxcluster_lock(void);
extern void dxcluster_unlock(void);
extern int  dxcluster_count(void);
extern const DX_SPOT *dxcluster_spot(int i);

// Click-to-tune hit test: nearest spot within tol_hz of freq. Returns 1 and
// fills *out_freq with the spot's exact frequency, else returns 0. Takes the
// lock internally.
extern int dxcluster_nearest(long long freq, long long tol_hz, long long *out_freq);

#endif
