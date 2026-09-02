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
 * What a keyboard shortcut DOES -- the half of keybind.c that touches the radio.
 *
 * Split out so the store (the table, the accelerator round trip, the matching
 * and the dispatch routing) can be linked into tools/keybind_offline.c against a
 * recording stub of this one function. Every other subsystem here has a harness
 * because a setting that will not stick is silent until an operator notices;
 * shortcuts are the same shape of bug, so keep the two files apart.
 */

#include <gtk/gtk.h>

#include "log.h"
#include "agc.h"
#include "adc.h"
#include "dac.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "radio.h"
#include "main.h"
#include "vfo.h"
#include "tci.h"
#include "keybind.h"

/* ---- dispatch ------------------------------------------------------------ */

static void kb_zoom(RECEIVER *rx, int zoom) {
  if(zoom<1) zoom=1;
  if(zoom>32) zoom=32;
  if(zoom==rx->zoom) return;
  receiver_change_zoom(rx,zoom);
  update_vfo(rx);
}

static void kb_pan(RECEIVER *rx, int delta) {
  int p=rx->pan+delta*(rx->zoom>0?rx->zoom:1);
  int max=rx->pixels-rx->panadapter_width;
  if(max<0) max=0;
  if(p<0) p=0;
  if(p>max) p=max;
  rx->pan=p;
}

static void kb_volume(RECEIVER *rx, double delta) {
  rx->volume+=delta;
  if(rx->volume>1.0) rx->volume=1.0;
  if(rx->volume<0.0) rx->volume=0.0;
  receiver_set_volume(rx);
  update_vfo(rx);
}

/* AGC-G runs -20..120 dB, the range the VFO row's bar is drawn over. */
static void kb_agc_gain(RECEIVER *rx, double delta) {
  rx->agc_gain+=delta;
  if(rx->agc_gain>120.0) rx->agc_gain=120.0;
  if(rx->agc_gain<-20.0) rx->agc_gain=-20.0;
  receiver_set_agc_gain(rx);
  update_vfo(rx);
}

/* There is no separate squelch on/off anywhere in this application: set_squelch()
   derives `squelch_enable` from the threshold being above zero, so running the
   level down to nothing IS switching it off, and a shortcut that flipped a flag
   of its own would be the second implementation this table refuses.  It is also
   the mode-aware choke point that remembers the level per mode. */
static void kb_squelch(RECEIVER *rx, double delta) {
  rx->squelch+=delta;
  if(rx->squelch>1.0) rx->squelch=1.0;
  if(rx->squelch<0.0) rx->squelch=0.0;
  set_squelch(rx);
  update_vfo(rx);
}

/* The partner of each sideband pair; anything else has no other sideband and
   is left alone rather than being turned into an unrelated mode. */
static int kb_other_sideband(int mode) {
  switch(mode) {
    case LSB:  return USB;
    case USB:  return LSB;
    case CWL:  return CWU;
    case CWU:  return CWL;
    case DIGL: return DIGU;
    case DIGU: return DIGL;
    default:   return -1;
  }
}

void keybind_run(int action, gboolean pressed) {
  RECEIVER *rx;
  int i;

  if(radio==NULL) return;
  rx=radio->active_receiver;
  if(rx==NULL) return;

  switch(action) {
    case KB_ZOOM_IN:     kb_zoom(rx,rx->zoom+1); break;
    case KB_ZOOM_OUT:    kb_zoom(rx,rx->zoom-1); break;
    case KB_ZOOM_RESET:  kb_zoom(rx,1);          break;
    case KB_PAN_LEFT:    kb_pan(rx,-1);          break;
    case KB_PAN_RIGHT:   kb_pan(rx,1);           break;

    case KB_PTT:
      /* The hold action: press keys the transmitter, release drops it. Both
         halves go through set_mox() -- the same call the space bar makes -- so
         nothing can be left keyed with the flag cleared underneath it. */
      if(radio->can_transmit) set_mox(radio,pressed);
      break;
    case KB_MOX:
      if(radio->can_transmit) set_mox(radio,!radio->mox);
      break;
    case KB_TUNE:
      if(radio->can_transmit) set_tune(radio,!radio->tune);
      break;

    case KB_SIDEBAND:
      i=kb_other_sideband(rx->mode_a);
      if(i>=0) vfo_set_mode(rx,i);
      break;
    case KB_MODE_NEXT:
      vfo_set_mode(rx,(rx->mode_a+1)%MODES);
      break;
    case KB_MODE_PREV:
      vfo_set_mode(rx,(rx->mode_a+MODES-1)%MODES);
      break;

    case KB_BAND_UP:
      set_band(rx,next_band(rx->band_a),-1);
      update_vfo(rx);
      break;
    case KB_BAND_DOWN:
      set_band(rx,previous_band(rx->band_a),-1);
      update_vfo(rx);
      break;
    case KB_FILTER_UP:
      i=rx->filter_a+1;
      if(i>=FILTERS) i=0;
      receiver_filter_changed(rx,i);
      update_vfo(rx);
      break;
    case KB_FILTER_DOWN:
      i=rx->filter_a-1;
      if(i<0) i=FILTERS-1;
      receiver_filter_changed(rx,i);
      update_vfo(rx);
      break;
    case KB_FREQ_UP:
      if(!rx->locked) receiver_move(rx,(long long)rx->step,TRUE);
      break;
    case KB_FREQ_DOWN:
      if(!rx->locked) receiver_move(rx,-(long long)rx->step,TRUE);
      break;
    case KB_LOCK:
      rx->locked=!rx->locked;
      update_vfo(rx);
      break;

    case KB_A_TO_B:   vfo_a2b(rx);    break;
    case KB_B_TO_A:   vfo_b2a(rx);    break;
    case KB_A_SWAP_B: vfo_aswapb(rx); break;
    case KB_SPLIT:
      if(rx->split==SPLIT_OFF) {
        rx->split=SPLIT_ON;
        if(radio->transmitter!=NULL && radio->transmitter->rx==rx) transmitter_set_mode(radio->transmitter,rx->mode_b);
      } else {
        rx->split=SPLIT_OFF;
        if(radio->transmitter!=NULL && radio->transmitter->rx==rx) transmitter_set_mode(radio->transmitter,rx->mode_a);
      }
      update_vfo(rx);
      tci_notify_state(rx);
      break;
    case KB_CTUN:
      rx->ctun=!rx->ctun;
      receiver_set_ctun(rx);
      update_vfo(rx);
      break;
    case KB_RIT:
      /* frequency_changed() as well as the flag: update_vfo only redraws the
         button, and RIT that is shown on but not applied is the "setter that
         only writes the field" the CAT rules refuse. */
      rx->rit_enabled=!rx->rit_enabled;
      frequency_changed(rx);
      update_vfo(rx);
      tci_notify_state(rx);
      break;
    case KB_RIT_CLEAR:
      rx->rit=0;
      frequency_changed(rx);
      update_vfo(rx);
      tci_notify_state(rx);
      break;
    case KB_XIT:
      if(radio->can_transmit && radio->transmitter!=NULL) {
        radio->transmitter->xit_enabled=!radio->transmitter->xit_enabled;
        update_vfo(rx);
        tci_notify_state(rx);
      }
      break;
    case KB_XIT_CLEAR:
      if(radio->can_transmit && radio->transmitter!=NULL && !isTransmitting(radio)) {
        radio->transmitter->xit=0;
        radio->transmitter->xit_enabled=0;
        update_vfo(rx);
        tci_notify_state(rx);
      }
      break;

    case KB_MUTE:
      rx->mute=!rx->mute;
      receiver_set_volume(rx);
      update_vfo(rx);
      break;
    /* The three gain rows step by exactly one notch of the VFO-row control they
       mirror (vfo.c's scroll handlers) and push with that control's own setter;
       update_vfo() then redraws the level bar, so the shortcut and the pointer
       cannot disagree about where the gain is. */
    case KB_VOLUME_UP:      kb_volume(rx,+0.01);     break;
    case KB_VOLUME_DOWN:    kb_volume(rx,-0.01);     break;
    case KB_AGC_GAIN_UP:    kb_agc_gain(rx,+1.0);    break;
    case KB_AGC_GAIN_DOWN:  kb_agc_gain(rx,-1.0);    break;
    case KB_SQUELCH_UP:     kb_squelch(rx,+0.01);    break;
    case KB_SQUELCH_DOWN:   kb_squelch(rx,-0.01);    break;
    case KB_AGC:
      rx->agc=(rx->agc>=AGC_FAST)?AGC_OFF:rx->agc+1;
      if(rx->mode_a>=0 && rx->mode_a<MODES) rx->mode_agc[rx->mode_a]=rx->agc;
      set_agc(rx);
      update_vfo(rx);
      break;
    case KB_NB:
      if(rx->nb)       { rx->nb=FALSE; rx->nb2=TRUE;  }
      else if(rx->nb2) { rx->nb=FALSE; rx->nb2=FALSE; }
      else             { rx->nb=TRUE;  rx->nb2=FALSE; }
      update_noise(rx);
      update_vfo(rx);
      break;
    case KB_NR:
      receiver_set_nr_mode(rx,(receiver_nr_mode(rx)+1)%5);
      update_vfo(rx);
      break;
    case KB_ANF:
      rx->anf=!rx->anf;
      update_noise(rx);
      update_vfo(rx);
      break;
    case KB_SNB:
      rx->snb=!rx->snb;
      update_noise(rx);
      update_vfo(rx);
      break;

    default:
      if(action>=KB_MODE_BASE && action<KB_MODE_BASE+MODES) {
        vfo_set_mode(rx,action-KB_MODE_BASE);
      }
      break;
  }
}
