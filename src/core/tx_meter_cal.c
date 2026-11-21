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

#include <stdio.h>

#include "property.h"
#include "tx_meter_cal.h"

int tx_meter_cal_segment(long long hz) {
  if(hz < TX_METER_CAL_HF_MID_HZ) return TX_METER_CAL_HF_LOW;
  if(hz < TX_METER_CAL_HF_HIGH_HZ) return TX_METER_CAL_HF_MID;
  return TX_METER_CAL_HF_HIGH;
}

const char *tx_meter_cal_segment_name(int seg) {
  switch(seg) {
    case TX_METER_CAL_HF_LOW:  return "Low: 160-40 m";
    case TX_METER_CAL_HF_MID:  return "Mid: 30-17 m";
    default:                   return "High: 15-10 m";
  }
}

double tx_meter_cal_clamp(double v) {
  if(!(v >= TX_METER_CAL_MIN)) return TX_METER_CAL_MIN;
  if(v > TX_METER_CAL_MAX) return TX_METER_CAL_MAX;
  return v;
}

static void save_dir(int channel, const char *dir, const double cal[]) {
  char name[80];
  for(int s=0;s<TX_METER_CAL_SEGMENTS;s++) {
    snprintf(name,sizeof(name),"transmitter[%d].%s_cal_hf[%d]",channel,dir,s);
    setPropertyDouble(name,cal[s]);
  }
}

static void restore_dir(int channel, const char *dir, double cal[]) {
  char name[80];
  char *value;
  // The single factor from before the split ("fwd_cal") seeds every segment;
  // it is not written back, so it is read once.
  snprintf(name,sizeof(name),"transmitter[%d].%s_cal",channel,dir);
  value=getProperty(name);
  if(value) {
    double v=tx_meter_cal_clamp(propToDouble(value));
    for(int s=0;s<TX_METER_CAL_SEGMENTS;s++) cal[s]=v;
  }
  for(int s=0;s<TX_METER_CAL_SEGMENTS;s++) {
    snprintf(name,sizeof(name),"transmitter[%d].%s_cal_hf[%d]",channel,dir,s);
    value=getProperty(name);
    if(value) cal[s]=tx_meter_cal_clamp(propToDouble(value));
  }
}

void tx_meter_cal_save(int channel, const double fwd[TX_METER_CAL_SEGMENTS],
                       const double rev[TX_METER_CAL_SEGMENTS]) {
  save_dir(channel,"fwd",fwd);
  save_dir(channel,"rev",rev);
}

void tx_meter_cal_restore(int channel, double fwd[TX_METER_CAL_SEGMENTS],
                          double rev[TX_METER_CAL_SEGMENTS]) {
  restore_dir(channel,"fwd",fwd);
  restore_dir(channel,"rev",rev);
}
