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
#include "smeter_cal.h"

#define LEGACY_KEY "radio.meter_calibration"

double smeter_cal_clamp(double v) {
  if(v!=v) return 0.0;
  if(v < SMETER_CAL_MIN) return SMETER_CAL_MIN;
  if(v > SMETER_CAL_MAX) return SMETER_CAL_MAX;
  return v;
}

void smeter_cal_fill(SMETER_CAL *c, double v) {
  for(int s=0;s<TX_METER_CAL_SEGMENTS;s++) c->hf[s]=v;
  for(int i=0;i<SMETER_CAL_XVTRS;i++) c->xvtr[i]=v;
}

double smeter_cal_pick(const SMETER_CAL *c, int xvtr_slot, long long rf_hz) {
  if(xvtr_slot>=0 && xvtr_slot<SMETER_CAL_XVTRS) return c->xvtr[xvtr_slot];
  return c->hf[tx_meter_cal_segment(rf_hz)];
}

// The OTHER segment (below 1 / above 30 MHz) keeps the single offset's key,
// as the power meter's does: a build from before the split reads that key and
// nothing else, and a file it wrote seeds every segment here.
static void hf_key(char *name, size_t len, int seg) {
  if(seg==TX_METER_CAL_OTHER) snprintf(name,len,"%s",LEGACY_KEY);
  else snprintf(name,len,"radio.meter_calibration_hf[%d]",seg);
}

void smeter_cal_save(const SMETER_CAL *c) {
  char name[64];
  for(int s=0;s<TX_METER_CAL_SEGMENTS;s++) {
    hf_key(name,sizeof(name),s);
    setPropertyDouble(name,c->hf[s]);
  }
  for(int i=0;i<SMETER_CAL_XVTRS;i++) {
    snprintf(name,sizeof(name),"radio.meter_calibration_xvtr[%d]",i);
    setPropertyDouble(name,c->xvtr[i]);
  }
}

void smeter_cal_restore(SMETER_CAL *c) {
  char name[64];
  char *value=getProperty(LEGACY_KEY);
  if(value) smeter_cal_fill(c,smeter_cal_clamp(propToDouble(value)));
  for(int s=0;s<TX_METER_CAL_OTHER;s++) {
    hf_key(name,sizeof(name),s);
    value=getProperty(name);
    if(value) c->hf[s]=smeter_cal_clamp(propToDouble(value));
  }
  for(int i=0;i<SMETER_CAL_XVTRS;i++) {
    snprintf(name,sizeof(name),"radio.meter_calibration_xvtr[%d]",i);
    value=getProperty(name);
    if(value) c->xvtr[i]=smeter_cal_clamp(propToDouble(value));
  }
}
