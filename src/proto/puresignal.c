/* Copyright (C)
* 2022 - m5evt
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
#include "log.h"

#include "puresignal.h"

#include "wdsp.h"

#include "agc.h"
#include "mode.h"
#include "filter.h"
#include "bandstack.h"
#include "band.h"
#include "discovered.h"
#include "bpsk.h"
#include "peak_detect.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "vfo.h"
#include "meter.h"
#include "radio_info.h"
#include "rx_panadapter.h"
#include "tx_panadapter.h"
#include "waterfall.h"
#include "protocol1.h"
#include "protocol2.h"
#include "audio.h"
#include "receiver_dialog.h"
#include "configure_dialog.h"
#include "property.h"
#include "rigctl.h"
#include "subrx.h"
#include "audio.h"
#include "math.h"

#define INFO_SIZE 16

// Runtime PureSignal debug flag (declared in puresignal.h). Off by default so
// the diagnostic logging below stays silent unless explicitly enabled.
int ps_debug = 0;

static gboolean info_timer_cb(void *data) {
  PSIGNAL *ps=(PSIGNAL *)data;

  if (!ps_debug) return TRUE;

  log_info("**********************************\n");

  static int info[INFO_SIZE];
  
  GetPSInfo(radio->transmitter->channel, &info[0]);
  for(int i = 0; i < INFO_SIZE; i++) {
    switch(i) {
      case 4:
        // Feedback
        log_info("Feedback: %i\n", info[i]);
        break;

      case 5:
        // Corrections count
        log_info("cor.count: %i\n", info[i]);
        break;

      case 6:
        // Computed correction solution
        log_info("sln.chk: %i\n", info[i]);
        break;

      case 13:
        // Dog count
        log_info("dg.count: %i\n", info[i]);
        break;

      case 14:
        // 
        log_info("State: %i\n", info[i]);
        break;

      case 15:
        log_info("Control state: \n");
        // State of the control state-machine
        switch(info[i]) {
          case 0:
            log_info("RESET\n");
            break;
          case 1:
            log_info("WAIT\n");
            break;
          case 2:
            log_info("MOXDELAY\n");
            break;
          case 3:
            log_info("SETUP\n");
            break;
          case 4:
            log_info("COLLECT\n");
            break;
          case 5:
            log_info("MOXCHECK\n");
            break;
          case 6:
            log_info("CALC\n");
            break;
          case 7:
            log_info("DELAY\n");
            break;
          case 8:
            log_info("STAYON\n");
            break;
          case 9:
            log_info("TURNON\n");
            break;
          default:
            log_info("?\n");
            break;
        }
        break;
    }
  }

  double pk;
  GetPSMaxTX(radio->transmitter->channel, &pk);
  log_info("Get Peak: %f\n", pk);

  GetPSHWPeak(radio->transmitter->channel, &pk);
  log_info("Set Peak: %f\n", pk);

  return TRUE;
}

void ps_change_tx_attenuation(PSIGNAL *ps, int att_diff) {
  // Work out new attuenation
  int new_att = 0;
  if (radio->hl2 != NULL) {
    new_att = radio->hl2->lna_gain_tx - att_diff; 
  }
  else {
    new_att = radio->transmitter->attenuation + att_diff;
  }
  
  PS_DEBUG("att_diff %i New %i old %i \n", att_diff, new_att, ps->attenuation);
  // Limit range of new attenuation value
  if (new_att < 0) {
    new_att = 0;
  } 
  else if (new_att > 31) {
    new_att = 31;
  }
 
  if (new_att != ps->attenuation) {
    PS_DEBUG("New att\n");
    SetPSControl(radio->transmitter->channel, 1, 0, 0, 0);

    if (radio->hl2 != NULL) {
      hl2_set_tx_attenuation(radio->hl2, new_att); 
    }
    else {
      radio->transmitter->attenuation = new_att;
      // Protocol 2: the TX-time ADC0 attenuation is sent as
      // high_priority_buffer_to_radio[1443] = transmitter->attenuation
      // (protocol2.c), so the new value is picked up on the next high-priority
      // frame.  Push one out immediately so the feedback-ADC gain tracks the
      // correction loop without waiting for the periodic update.
#ifdef PURESIGNAL_P2
      if(radio->discovered!=NULL && radio->discovered->protocol==PROTOCOL_2) {
        protocol2_high_priority();
      }
#endif
    }

    ps->state=1;
  }
}

int ps_get_tx_attenuation(PSIGNAL *ps) {
  if (radio->hl2 != NULL) {
    ps->attenuation = hl2_get_tx_attenuation(radio->hl2);
  } else {
    ps->attenuation = radio->transmitter->attenuation;
  }
  
  return ps->attenuation; 
}

PSIGNAL *create_puresignal(void) {
  PSIGNAL *ps = g_new0(PSIGNAL, 1);

  // Enable PureSignal diagnostics if the MACHPSDR_PS_DEBUG env var is set
  // (any value). Off unless the user opts in, so no rebuild is needed to
  // turn the logging on/off.
  if (g_getenv("MACHPSDR_PS_DEBUG") != NULL) {
    ps_debug = 1;
  }

  PS_DEBUG("----------------------Create new puresignal\n");
  ps->ints = 16;
  ps->spi = 256;
  ps->stbl = 0;
  ps->map = 1;
  ps->pin = 1;
  ps->ptol = 0.8;
  ps->mox_delay = 0.2;
  ps->loop_delay = 0.0;
  ps->amp_delay = 150E-9;

  ps->peak_value = 0;

  ps->state = 0;
  ps->auto_on = 1;
  ps->old_cor_cnt = 0;
  ps->attenuation = 0;

  SetPSIntsAndSpi(radio->transmitter->channel, ps->ints, ps->spi);
  SetPSStabilize(radio->transmitter->channel, ps->stbl);
  SetPSMapMode(radio->transmitter->channel, ps->map);
  SetPSPinMode(radio->transmitter->channel, ps->pin);
  SetPSPtol(radio->transmitter->channel, ps->ptol);
  SetPSMoxDelay(radio->transmitter->channel, ps->mox_delay);
  SetPSTXDelay(radio->transmitter->channel, ps->amp_delay);
  SetPSLoopDelay(radio->transmitter->channel, ps->loop_delay);

  // Periodic status dump; only started when PS debug logging is enabled.
  if (ps_debug) {
    ps->info_timer_id = g_timeout_add(500, info_timer_cb,(gpointer)ps);
  }

  // TODO this value is only valid for the HL2
  double set_peak = 0.4067;

  if (radio->discovered->device == DEVICE_HERMES_LITE2) {
    set_peak = 0.2300;
  } 
  else if (radio->discovered->protocol == PROTOCOL_2) {
    set_peak = 0.2899;
  }

  SetPSFeedbackRate(radio->transmitter->channel, radio->transmitter->rx->sample_rate);

  SetPSHWPeak(radio->transmitter->channel, set_peak);

  return ps;
}

