/* Copyright (C)
* 2018 - John Melton, G0ORX/N6LYT
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

#include <stdint.h>
#include "log.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <gtk/gtk.h>

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "discovered.h"
#include "bpsk.h"
#include "band.h"
#include "adc.h"
#include "dac.h"
#include "agc.h"
#include "web_protocol.h"
#include "receiver.h"
#include "filter.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"
#include "vfo.h"
#include "ext.h"
#include "subrx.h"
#include "bookmark_dialog.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif

int ext_num_pad(void *data) {
  gint val=GPOINTER_TO_INT(data);
  RECEIVER *rx=radio->active_receiver;
  log_info("%s: %d\n",__FUNCTION__,val);
  if(!rx->entering_frequency) {
    rx->entered_frequency=0;
    rx->entering_frequency=true;
  }
  switch(val) {
    case -1: // clear
      rx->entered_frequency=0;
      rx->entering_frequency=false;
      break;
    case -2: // enter
      // Direct entry assigns frequency_a, which is past every receiver_move*()
      // guard -- so LOCK has to be tested here, as the VFO row's own type-in
      // popover already does.
      if(rx->entered_frequency!=0 && !rx->locked) {
        rx->frequency_a=rx->entered_frequency;
      }
      rx->entering_frequency=false;
      break;
    default:
      rx->entered_frequency=(rx->entered_frequency*10)+val;
      break;
  } 
  update_vfo(rx);
  return 0;
}

int ext_band_select(void *data) {
  int b=GPOINTER_TO_INT(data);
  log_info("%s: %d\n",__FUNCTION__,b);
  set_band(radio->active_receiver,b,-1);
  update_vfo(radio->active_receiver);
  return 0;
}

int ext_vox_changed(void *data) {
  vox_changed((RADIO *)data);
  return 0;
}

int ext_ptt_changed(void *data) {
log_info("ext_ptt_changed\n");
  ptt_changed((RADIO *)data);
  return 0;
}

int ext_set_mox(void *data) {
  MOX_STATE *m=(MOX_STATE *)data;
  set_mox(m->radio,m->state);
  g_free(m);
  return 0;
}

// Every ext_* entry point below is queued from a thread that is not the GTK one
// (rigctl, TCI, MIDI) and runs a main-loop iteration or more later. The receiver
// it names can be closed in that gap -- and delete_receiver frees it -- so each
// re-validates the pointer with receiver_is_live() before the first dereference.
// Dropping the request is the right answer: whatever it was going to change no
// longer exists.
// Absolute set of VFO A from the network (TCI). Routed through the VFO's own
// applier, exactly as CAT's FA is (cat_set_vfo_a): assigning frequency_a moves
// the span CENTRE, so under ctun or freetune a client's "put me on 10489.784"
// left the receiver demodulating wherever the cursor already was — and the
// matching read then reported the centre, so the client could not even see the
// disagreement. A setter has to move what its getter reports
// (receiver_tuned_frequency).
int ext_set_frequency_a(void *data) {
  RX_FREQUENCY *f=(RX_FREQUENCY *)data;

  if(!receiver_is_live(f->rx)) { g_free(f); return 0; }
  vfo_apply_frequency(f->rx,f->frequency,FALSE);

  g_free(f);
  return 0;
}

// Absolute set of VFO B (TCI "vfo:<rx>,1,<hz>"). Deliberately a direct
// assignment rather than a receiver_move_b() delta: the split modes make a
// delta ambiguous (RSAT moves B the other way, SAT drags A along), whereas
// "put VFO B on this frequency" has one meaning. The [0, fmax] guard is the
// same directional one receiver_move_b uses — reject only a move that pushes
// further out of range, so a B already above the device max can be brought back.
int ext_set_frequency_b(void *data) {
  RX_FREQUENCY *f=(RX_FREQUENCY *)data;

  if(!receiver_is_live(f->rx)) { g_free(f); return 0; }
  g_mutex_lock(&f->rx->mutex);
  if(f->rx!=NULL && !f->rx->locked) {
    long long fmax=receiver_max_frequency(f->rx);
    long long nf=f->frequency;
    if(!((nf <= 0    && nf < f->rx->frequency_b) ||
         (nf >  fmax && nf > f->rx->frequency_b))) {
      f->rx->frequency_b=nf;
      f->rx->band_b=get_band_from_frequency(nf);
      frequency_changed(f->rx);
      update_vfo(f->rx);
    }
  }
  g_mutex_unlock(&f->rx->mutex);

  g_free(f);
  return 0;
}

int ext_set_mode(void *data) {
  MODE *m=(MODE *)data;
  if(!receiver_is_live(m->rx)) { g_free(m); return 0; }
  if (m->rx != NULL) {
    m->rx->mode_a = m->mode_a;
  }
  receiver_mode_changed(m->rx, m->rx->mode_a);    
  update_vfo(m->rx);
  g_free(m);
  return 0;
}

int ext_set_filter_edges(void *data) {
  RX_FILTER_EDGES *f=(RX_FILTER_EDGES *)data;
  if(!receiver_is_live(f->rx)) { g_free(f); return 0; }
  set_filter(f->rx,f->low,f->high);
  update_vfo(f->rx);
  g_free(f);
  return 0;
}


// PureSignal on/off from a thread that is not the GTK one (CAT ZZLI, MIDI).
// transmitter_set_ps() adds and deletes the hidden feedback receivers, i.e. it
// builds and destroys widgets, so it must land on the GTK thread and can never
// be called from the rigctl or MIDI thread directly.
//
// The free-slot precondition and the puresignal_enabled bookkeeping live here
// rather than in each caller: they were previously only in the dialog's
// enable_cb, so every non-UI path (there was one, MIDI) switched PureSignal on
// without recording that it was on and without checking there was room for the
// feedback receivers. Returns with puresignal_enabled telling the caller what
// actually happened.
int ext_tx_set_ps(void *data) {
#ifdef PURESIGNAL
  TRANSMITTER *tx = (radio!=NULL) ? radio->transmitter : NULL;
  if(tx==NULL || radio->discovered==NULL) return 0;
  if((uintptr_t)data) {
    // PureSignal needs free receiver slots for the TX/RX feedback channels.
    // ps_tx_fdbk_chan is -1 on a radio that has none, which fails this too.
    if(radio->receivers <= (radio->discovered->ps_tx_fdbk_chan - 1)) {
      transmitter_set_ps(tx,1);
      tx->puresignal_enabled=TRUE;
    }
  } else {
    transmitter_set_ps(tx,0);
    tx->puresignal_enabled=FALSE;
  }
#endif
  return 0;
}

/*
int ext_ps_twotone(void *data) {
  ps_twotone((uintptr_t)data);
  return 0;
}
*/

int ext_vfo_update(void *data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(!receiver_is_live(rx)) return 0;
  update_vfo(rx);
  return 0;
}

int ext_vfo_step(void *data) {
  RX_STEP *s=(RX_STEP *)data;
  RECEIVER *rx=s->rx;
  if(!receiver_is_live(rx)) { g_free(s); return 0; }
  if(rx!=NULL && !rx->locked) {
    rx->frequency_a=rx->frequency_a+(rx->step*s->step);
    rx->band_a=get_band_from_frequency(rx->frequency_a);
  }
  update_vfo(s->rx);
  g_free(s);
  return 0;
}

int ext_set_afgain(void *data) {
  RX_GAIN *s=(RX_GAIN *)data;
  RECEIVER *rx=s->rx;
  if(!receiver_is_live(rx)) { g_free(s); return 0; }
  if(rx!=NULL) {
    rx->volume=s->gain;
  }
  update_vfo(s->rx);
  g_free(s);
  return 0;
}

// One scalar RX control from the browser remote (dispatched by WEB_CONTROL).
// Each case applies the value through the SAME choke point the GTK UI uses --
// set_agc()/set_squelch()/receiver_set_agc_gain() -- and clamps to the UI's own
// range, so a remote change behaves exactly like turning the knob (CLAUDE.md:
// "a setter that only writes the field is a lie"). Queued from the web socket
// thread, so it re-validates the receiver with receiver_is_live() first.
int ext_set_rx_control(void *data) {
  RX_CONTROL *c=(RX_CONTROL *)data;
  RECEIVER *rx=c->rx;
  if(!receiver_is_live(rx)) { g_free(c); return 0; }
  switch(c->control) {
    case WEB_CTRL_AF_GAIN: {
      double g=(double)c->value/100.0;
      rx->volume=CLAMP(g,0.0,1.0);
      receiver_set_volume(rx);
      break;
    }
    case WEB_CTRL_AGC:
      rx->agc=(int)CLAMP(c->value,(gint64)AGC_OFF,(gint64)AGC_LAST);
      if(rx->channel>=0) set_agc(rx);
      break;
    case WEB_CTRL_AGC_GAIN:
      rx->agc_gain=CLAMP((double)c->value,-20.0,120.0);
      receiver_set_agc_gain(rx);
      break;
    case WEB_CTRL_SQUELCH:
      rx->squelch=CLAMP((double)c->value/100.0,0.0,1.0);
      set_squelch(rx);
      break;
    case WEB_CTRL_STEP:
      if(c->value>0 && c->value<=1000000000LL) rx->step=c->value;
      break;
    // The noise controls go through update_noise()/receiver_set_nr_mode() exactly
    // like the VFO menu, so the bypass rule (NB/NR/ANF/SNB forced off in DIGU/DIGL
    // or under a decoder) applies to a remote change too. The operator's selection
    // is still recorded and echoed in state; only the WDSP run flag is suppressed.
    case WEB_CTRL_NB:
      if(rx->channel>=0) {
        int v=(int)CLAMP(c->value,(gint64)0,(gint64)2);
        rx->nb=(v==1); rx->nb2=(v==2);
        update_noise(rx);
      }
      break;
    case WEB_CTRL_NR:
      if(rx->channel>=0)
        receiver_set_nr_mode(rx,(int)CLAMP(c->value,(gint64)0,(gint64)4));
      break;
    case WEB_CTRL_ANF:
      if(rx->channel>=0) { rx->anf=(c->value!=0); update_noise(rx); }
      break;
    case WEB_CTRL_SNB:
      if(rx->channel>=0) { rx->snb=(c->value!=0); update_noise(rx); }
      break;
    // Filter preset: the VFO filter menu's own choke point, which sets filter_a,
    // remembers it per mode and applies the mode's filters[] edges (handling the
    // FMN deviation special-case) — so a remote pick is identical to the menu.
    case WEB_CTRL_FILTER_PRESET:
      if(rx->channel>=0 && c->value>=0 && c->value<FILTERS)
        receiver_filter_changed(rx,(int)c->value);
      break;
    // RIT/XIT: value is the offset in Hz. Both reach the hardware the same way the
    // VFO row does — frequency_changed() recomputes the commanded tune, so the
    // offset actually leaves the app; update_frequency() repaints the dial. RIT is
    // per-receiver; XIT lives on the transmitter and only moves on a TX-capable radio.
    case WEB_CTRL_RIT:
      rx->rit=CLAMP(c->value,(gint64)-10000,(gint64)10000);
      frequency_changed(rx); update_frequency(rx);
      break;
    case WEB_CTRL_RIT_ENABLE:
      rx->rit_enabled=(c->value!=0);
      frequency_changed(rx); update_frequency(rx);
      break;
    case WEB_CTRL_XIT:
      if(radio->transmitter!=NULL) {
        radio->transmitter->xit=CLAMP(c->value,(gint64)-10000,(gint64)10000);
        if(radio->transmitter->rx!=NULL) frequency_changed(radio->transmitter->rx);
      }
      break;
    case WEB_CTRL_XIT_ENABLE:
      if(radio->transmitter!=NULL) {
        radio->transmitter->xit_enabled=(c->value!=0);
        if(radio->transmitter->rx!=NULL) frequency_changed(radio->transmitter->rx);
      }
      break;
    // Split toggle: SPLIT_ON <-> SPLIT_OFF, keeping split_last like the VFO button.
    case WEB_CTRL_SPLIT:
      if(c->value!=0) { rx->split=SPLIT_ON; rx->split_last=SPLIT_ON; }
      else rx->split=SPLIT_OFF;
      break;
    // Band up (+1) / down (-1). set_band is the one choke point that refuses a
    // locked VFO (CLAUDE.md), so the lock rule needs no repeat here.
    case WEB_CTRL_BAND:
      if(c->value>0) set_band(rx,next_band(rx->band_a),-1);
      else if(c->value<0) set_band(rx,previous_band(rx->band_a),-1);
      break;
    // Band by index, as the band menu does (bandstack entry -1 keeps the band's
    // current one). An index the menu would not offer is dropped, not folded.
    case WEB_CTRL_SET_BAND:
      if(c->value>=0 && c->value<BANDS+XVTRS && band_is_offered((int)c->value))
        set_band(rx,(int)c->value,-1);
      break;
    // One device-shaped front-end value.  Soapy calls it gain and exposes its
    // range during discovery; HPSDR-family radios expose the existing ADC
    // attenuation/LNA value.  Both paths use the same fields and hardware
    // appliers as Configure -> Radio.
    case WEB_CTRL_RF_GAIN:
#ifdef SOAPYSDR
      if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
        double lo=0.0,hi=100.0;
        if(radio->discovered->info.soapy.rx_gains>0) {
          lo=hi=0.0;
          for(size_t i=0;i<radio->discovered->info.soapy.rx_gains;i++) {
            lo+=radio->discovered->info.soapy.rx_range[i].minimum;
            hi+=radio->discovered->info.soapy.rx_range[i].maximum;
          }
        }
        radio->adc[rx->adc].gain=CLAMP((double)c->value,lo,hi);
        soapy_protocol_set_gain(&radio->adc[rx->adc]);
        break;
      }
#endif
      if(radio->discovered->device==DEVICE_HERMES_LITE ||
         radio->discovered->device==DEVICE_HERMES_LITE2)
        radio->adc[rx->adc].attenuation=(int)CLAMP(c->value,(gint64)-12,(gint64)48);
      else
        radio->adc[rx->adc].attenuation=(int)CLAMP(c->value,(gint64)0,(gint64)31);
      if(radio->discovered->protocol==PROTOCOL_2) protocol2_high_priority();
      break;
    case WEB_CTRL_SUBRX:
      if(c->value!=0 && !rx->subrx_enable) {
        create_subrx(rx);
        rx->subrx_enable=TRUE;
      } else if(c->value==0 && rx->subrx_enable) {
        rx->subrx_enable=FALSE;
        destroy_subrx(rx);
      }
      break;
    case WEB_CTRL_SUBRX_MIX:
      rx->subrx_mix=(int)CLAMP(c->value,(gint64)0,(gint64)100);
      break;
    case WEB_CTRL_BOOKMARK:
      bookmarks_recall(rx,(int)c->value);
      break;
    default: break;
  }
  update_vfo(rx);
  g_free(c);
  return 0;
}
