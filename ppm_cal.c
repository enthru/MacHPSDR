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

#include <gtk/gtk.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "mode.h"
#include "ppm_cal.h"

// Reference stations, ordered most-useful-first for a shortwave SDR.  The HF
// carrier standards (RWM/WWV/CHU/BPM) all radiate a strong continuous carrier
// that is disciplined to a national frequency standard, so a measured offset
// is pure receiver-oscillator error.  The LF/MF entries are only reachable by
// front-ends that tune that low.
static const PPM_STATION stations[] = {
  { "RWM 4.996 MHz (Russia)",   4996000LL },
  { "RWM 9.996 MHz (Russia)",   9996000LL },
  { "RWM 14.996 MHz (Russia)", 14996000LL },
  { "WWV 2.5 MHz (USA)",        2500000LL },
  { "WWV 5 MHz (USA)",          5000000LL },
  { "WWV 10 MHz (USA)",        10000000LL },
  { "WWV 15 MHz (USA)",        15000000LL },
  { "WWV 20 MHz (USA)",        20000000LL },
  { "CHU 3.330 MHz (Canada)",   3330000LL },
  { "CHU 7.850 MHz (Canada)",   7850000LL },
  { "CHU 14.670 MHz (Canada)", 14670000LL },
  { "BPM 5 MHz (China)",        5000000LL },
  { "BPM 10 MHz (China)",      10000000LL },
  { "BPM 15 MHz (China)",      15000000LL },
  { "MSF 60 kHz (UK)",            60000LL },
  { "DCF77 77.5 kHz (Germany)",   77500LL },
  { "Droitwich 198 kHz (UK)",    198000LL },
};

int ppm_station_count(void) {
  return (int)(sizeof(stations)/sizeof(stations[0]));
}

const PPM_STATION *ppm_station(int index) {
  if(index<0 || index>=ppm_station_count()) return NULL;
  return &stations[index];
}

void ppm_cal_tune_to_station(RADIO *r, int index) {
  const PPM_STATION *st=ppm_station(index);
  if(st==NULL) return;
  RECEIVER *rx=r->active_receiver;
  if(rx==NULL) return;

  r->ppm_ref_station=index;

  // Park the exact carrier at the VFO centre and go CW-USB so it beats down to
  // a clean tone at the sidetone offset (mirrors the bookmark-recall retune
  // path in bookmark_dialog.c).
  rx->frequency_a=st->freq;
  rx->ctun=0;
  rx->ctun_frequency=st->freq;
  receiver_mode_changed(rx,CWU);
  frequency_changed(rx);
}
