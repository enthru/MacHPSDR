/* Copyright (C)
* 2026 - MacHPSDR contributors
*
*   This program is free software; you can redistribute it and/or
*   modify it under the terms of the GNU General Public License
*   as published by the Free Software Foundation; either version 2
*   of the License, or (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 51 Franklin Street, Boston, MA  02110-1301, USA.
*
*/

#ifndef _SMETER_CAL_H
#define _SMETER_CAL_H

#include "tx_meter_cal.h"

// S-meter calibration by frequency. A receiver's gain is not flat across HF,
// and a transverter or LNB in front of it adds gain of its own, so one dB
// offset that is right on 40 m reads wrong on 10 m and wildly wrong through a
// converter. HF uses the power meter's segments (tx_meter_cal_segment: three on
// HF and one for everything outside 1-30 MHz);
// each transverter slot has its own offset, which wins whenever the receiver
// is on that transverter's band.
//
// Pure (property store only), so props_offline can pin it.

// Must equal XVTRS (band.h, which pulls in GTK); asserted where both are seen.
#define SMETER_CAL_XVTRS 8

#define SMETER_CAL_MIN -100.0
#define SMETER_CAL_MAX  100.0

typedef struct {
  double hf[TX_METER_CAL_SEGMENTS];
  double xvtr[SMETER_CAL_XVTRS];
} SMETER_CAL;

// NaN and out-of-range land inside [MIN, MAX]; NaN reads as 0 dB.
extern double smeter_cal_clamp(double v);
// Every segment and every transverter slot to one value (the device default).
extern void smeter_cal_fill(SMETER_CAL *c, double v);
// The offset for a receiver: its transverter slot's when it is on one
// (xvtr_slot >= 0), otherwise the HF segment of the RF at the radio.
extern double smeter_cal_pick(const SMETER_CAL *c, int xvtr_slot, long long rf_hz);

extern void smeter_cal_save(const SMETER_CAL *c);
// Leaves an offset untouched when its key is absent. "radio.meter_calibration"
// (the single offset of older builds, now the outside-HF segment) seeds every
// segment and every transverter, so a calibrated meter stays calibrated.
extern void smeter_cal_restore(SMETER_CAL *c);

#endif
