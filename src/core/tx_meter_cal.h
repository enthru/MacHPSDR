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

#ifndef _TX_METER_CAL_H
#define _TX_METER_CAL_H

// Power-meter calibration by frequency. A tandem match's coupling is not flat
// across HF, so one factor that is right on 40 m reads wrong on 10 m. HF is
// split into three segments, each with its own forward/reflected factor; a
// frequency past either end of HF takes the nearest segment's.
//
// Pure (property store only), so props_offline can pin it.

enum {
  TX_METER_CAL_HF_LOW,    // below 8 MHz: 160/80/60/40 m
  TX_METER_CAL_HF_MID,    // 8-20 MHz: 30/20/17 m
  TX_METER_CAL_HF_HIGH,   // 20 MHz and up: 15/12/10 m
  TX_METER_CAL_SEGMENTS
};

#define TX_METER_CAL_HF_MID_HZ   8000000LL
#define TX_METER_CAL_HF_HIGH_HZ 20000000LL

#define TX_METER_CAL_MIN 0.05
#define TX_METER_CAL_MAX 20.0

// The segment a transmit frequency (RF at the radio, Hz) is calibrated in.
extern int tx_meter_cal_segment(long long hz);
// Operator-facing name, e.g. "Low: 160-40 m".
extern const char *tx_meter_cal_segment_name(int seg);

// A persisted or typed factor reaches the meter only through here: an
// old/hand-edited props file is a supported input, and NaN slips past a
// plain `x < lo` test.
extern double tx_meter_cal_clamp(double v);

extern void tx_meter_cal_save(int channel, const double fwd[TX_METER_CAL_SEGMENTS],
                              const double rev[TX_METER_CAL_SEGMENTS]);
// Leaves a factor untouched when its key is absent. A props file from before
// the split carries only the single factor, which seeds all three HF segments
// so a calibrated meter stays calibrated.
extern void tx_meter_cal_restore(int channel, double fwd[TX_METER_CAL_SEGMENTS],
                                 double rev[TX_METER_CAL_SEGMENTS]);

#endif
