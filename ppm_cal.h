/* Copyright (C)
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
 * PPM (oscillator) calibration against time/frequency-standard broadcast
 * stations.  Phase 1 provides the reference-station table and a "tune to
 * station" helper; the actual carrier-offset measurement is a later phase.
 */

#ifndef _PPM_CAL_H
#define _PPM_CAL_H

#include <glib.h>
#include "radio.h"

// A time/frequency-standard broadcast station with a precise, continuously
// transmitted carrier, usable as a reference for oscillator (ppm) calibration.
typedef struct {
  const char *name;   // display name, e.g. "RWM 9.996 MHz (Russia)"
  long long   freq;   // carrier frequency, Hz
} PPM_STATION;

// Number of reference stations, and accessor (NULL for an out-of-range index).
int                ppm_station_count(void);
const PPM_STATION *ppm_station(int index);

// Tune the active receiver to the selected reference station's carrier in
// CW-USB, so the carrier appears as a clean narrow tone at the CW sidetone
// offset — the operator can eyeball it on the panadapter now, and the Phase-2
// measurement can find it precisely.  Also records the choice in
// radio->ppm_ref_station.
void ppm_cal_tune_to_station(RADIO *r, int index);

#endif
