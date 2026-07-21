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
 * stations.  Provides the reference-station table, a manual "tune to station"
 * helper, and an automatic carrier-offset measurement that discovers the ppm
 * error from a station's carrier and corrects it (closed loop).
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
// offset — the operator can eyeball it on the panadapter now, and the
// measurement below can find it precisely.  Also records the choice in
// radio->ppm_ref_station.
void ppm_cal_tune_to_station(RADIO *r, int index);

// -------- Automatic carrier-offset calibration --------

// Start an automatic calibration against the currently selected reference
// station.  Retunes the active RX so the carrier sits mid-passband and begins
// averaging the carrier's frequency offset over a few seconds of I/Q.  Returns
// FALSE if it could not start (no active receiver).  Poll ppm_cal_measure_poll()
// for progress/result.
gboolean ppm_cal_measure_start(RADIO *r);

// TRUE while a measurement is running.
gboolean ppm_cal_measuring(void);

// Abort any measurement in progress (also restores the RX to where it was).
void ppm_cal_measure_cancel(void);

// Restore the active RX to the VFO frequency / mode / filter it had before the
// measurement retuned it. GTK thread only. A no-op if nothing was saved (e.g.
// already restored). Called by the UI once a run finishes so the operator's
// radio does not stay parked on the reference station.
void ppm_cal_restore_rx(void);

// Copy the current measurement state for the UI (thread-safe).  *status gets a
// short human-readable line.  When *done is TRUE the run has finished and
// *offset_hz / *suggested_ppm / *ok carry the result (residual carrier offset
// in Hz, the suggested new ppm value, and whether a carrier was actually found).
void ppm_cal_measure_poll(gboolean *done, char *status, int status_sz,
                          double *offset_hz, double *suggested_ppm, gboolean *ok);

// I/Q measurement tap, called from the RX audio thread (receiver.c).  A cheap
// no-op unless a measurement is in progress for this receiver.  iq is
// interleaved off-air I/Q (iq[2k]=I, iq[2k+1]=Q) at rx->sample_rate;
// n_frames complex samples.
void ppm_cal_iq_feed(RECEIVER *rx, const double *iq, int n_frames);

#endif
