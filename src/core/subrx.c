/* Copyright (C)
* 2020 - John Melton, G0ORX/N6LYT
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

#include <math.h>
#include "log.h"
#include <gtk/gtk.h>

#include <wdsp.h>

#include "agc.h"
#include "discovered.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "bpsk.h"
#include "subrx.h"
#include "mode.h"
#include "filter.h"
#include "radio.h"
#include "main.h"

void subrx_frequency_changed(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  gint64 offset=rx->frequency_b-rx->frequency_a;
  //if(rx->mode_b==CWU) {
  //  offset+=(gint64)radio->cw_keyer_sidetone_frequency;
  //} else if(rx->mode_b==CWL) {
  //  offset-=(gint64)radio->cw_keyer_sidetone_frequency;
  //}
  RXANBPSetShiftFrequency(subrx->channel, (double)offset);
  if(subrx->feed!=NULL) {
    // The sub-channel sees a stream already centred on VFO-B, so WDSP's shift
    // block stays off and the NCO carries the offset (see receiver_apply_shift).
    rx_feed_set_offset(subrx->feed,(double)offset);
    SetRXAShiftFreq(subrx->channel, 0.0);
    SetRXAShiftRun(subrx->channel, 0);
  } else {
    SetRXAShiftFreq(subrx->channel, (double)offset);
    SetRXAShiftRun(subrx->channel, 1);
  }
}

void subrx_set_mode(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  SetRXAMode(subrx->channel, rx->mode_b);
}

void subrx_set_filter(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  RXASetPassband(subrx->channel,(double)rx->filter_low_b,(double)rx->filter_high_b);
}

void subrx_set_deviation(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  SetRXAFMDeviation(subrx->channel, (double)rx->deviation);
}


void subrx_filter_changed(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  if(rx->mode_b==FMN) {
    subrx_set_deviation(rx);
  } else {
    subrx_set_filter(rx);
  }
}

// APF (CW audio peak filter) on the sub-channel. Same operator settings as the
// main receiver (there is one APF control set), but gated on the SUB's mode —
// the sub is usually parked on a different mode from VFO A, and an APF running
// on an SSB sub-channel would ring.
void subrx_set_apf(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  if(subrx==NULL) return;
  gboolean cw=(rx->mode_b==CWL || rx->mode_b==CWU);
  double freq=(double)radio->cw_keyer_sidetone_frequency;
  if(freq < 200.0) freq=200.0;   // SPCW design 1 clamps below 200 Hz anyway
  SetRXASPCWFreq(subrx->channel, freq);
  SetRXASPCWBandwidth(subrx->channel, rx->apf_bw);
  SetRXASPCWGain(subrx->channel, rx->apf_gain);
  SetRXASPCWRun(subrx->channel, (rx->apf_enable && cw) ? 1 : 0);
}

void subrx_mode_changed(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  subrx_set_mode(rx);
  subrx_filter_changed(rx);
  subrx_set_apf(rx);
  // ...and re-push the AGC, because subrx_set_mode() went through SetRXAMode(),
  // which owns the AGC run flag on the way into FM. Same reason
  // receiver_mode_changed() calls set_agc() after set_mode().
  subrx_set_agc(rx);
}

void subrx_set_agc(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  // Same FM rule as set_agc() on the main channel, keyed on VFO-B's mode: the
  // sub-channel is a full RXA and subrx_set_mode() runs the same SetRXAMode()
  // that clears the AGC run flag for FM/WFM. See set_agc() in receiver.c.
  SetRXAAGCRun(subrx->channel, agc_run_for(rx->mode_b, rx->agc));
  SetRXAAGCMode(subrx->channel, rx->agc);
  SetRXAAGCSlope(subrx->channel,rx->agc_slope);
  SetRXAAGCTop(subrx->channel,rx->agc_gain);
  switch(rx->agc) {
    case AGC_OFF:
      break;
    case AGC_LONG:
      SetRXAAGCAttack(subrx->channel,2);
      SetRXAAGCHang(subrx->channel,2000);
      SetRXAAGCDecay(subrx->channel,2000);
      SetRXAAGCHangThreshold(subrx->channel,(int)rx->agc_hang_threshold);
      break;
    case AGC_SLOW:
      SetRXAAGCAttack(subrx->channel,2);
      SetRXAAGCHang(subrx->channel,1000);
      SetRXAAGCDecay(subrx->channel,500);
      SetRXAAGCHangThreshold(subrx->channel,(int)rx->agc_hang_threshold);
      break;
    case AGC_MEDIUM:
      SetRXAAGCAttack(subrx->channel,2);
      SetRXAAGCHang(subrx->channel,0);
      SetRXAAGCDecay(subrx->channel,250);
      SetRXAAGCHangThreshold(subrx->channel,100);
      break;
    case AGC_FAST:
      SetRXAAGCAttack(subrx->channel,2);
      SetRXAAGCHang(subrx->channel,0);
      SetRXAAGCDecay(subrx->channel,50);
      SetRXAAGCHangThreshold(subrx->channel,100);
      break;
  }
}

void create_subrx(RECEIVER *rx) {
log_info("%s: rx=%d\n",__FUNCTION__,rx->channel);
  SUBRX *subrx=g_new(SUBRX,1);
  rx->subrx=subrx;
  subrx->channel=rx->channel+SUBRX_BASE_CHANNEL;
  g_mutex_init(&subrx->mutex);
  subrx->audio_output_buffer=g_new0(gdouble,2*rx->output_samples);
  // The same geometry as the main channel: when the span is decimated in front
  // of WDSP (RECEIVER.dsp_feed), VFO-B's channel is opened at that decimated
  // rate too -- with its OWN feed below, because it listens somewhere else in
  // the same span.  With no feed these are the span and the full block, i.e.
  // exactly what this call passed before.
  subrx->feed=(rx->dsp_feed!=NULL)
              ? rx_feed_create(rx->sample_rate,rx->dsp_in_rate,rx->dsp_in_block)
              : NULL;
  OpenChannel(subrx->channel,
              rx->dsp_in_block,
              (subrx->feed!=NULL)?rx->dsp_size:rx->fft_size,
              rx->dsp_in_rate,
              48000, // dsp rate
              48000, // output rate
              0, // receive
              1, // run
              0.010, 0.025, 0.0, 0.010, 0);

  // The blankers belong to this channel's INPUT, which is the feed's output
  // when there is one and the span when there is not -- the same rule as the
  // main receiver's (rx_nb_apply/rx_nb_rematch in receiver.c).  They were
  // created at the span and never applied at all, so VFO-B was blanked only as
  // a side effect of the main path mutating the shared buffer in place.
  create_anbEXT(subrx->channel,1,rx->dsp_in_block,rx->dsp_in_rate,0.0001,0.0001,0.0001,0.05,20);
  create_nobEXT(subrx->channel,1,0,rx->dsp_in_block,rx->dsp_in_rate,0.0001,0.0001,0.0001,0.05,20);
  RXASetNC(subrx->channel, rx->fft_size);
  RXASetMP(subrx->channel, rx->low_latency);

  // make sure the subrx frequency is within the passband
  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
  rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);
  if(rx->frequency_b<rx->ctun_min || rx->frequency_b>rx->ctun_max) {
    rx->frequency_b=rx->frequency_a;	  
  }

  subrx_frequency_changed(rx);
  subrx_mode_changed(rx);

  // rx->volume alone here left VFO B playing through Mute: the main channel's
  // gain goes to zero and the sub-channel's did not, so muting the receiver
  // silenced one ear of it.  One gain function for both channels.
  SetRXAPanelGain1(subrx->channel, receiver_panel_gain(rx));
  SetRXAPanelSelect(subrx->channel, 3);
  SetRXAPanelPan(subrx->channel, 0.5);
  SetRXAPanelCopy(subrx->channel, 0);
  SetRXAPanelBinaural(subrx->channel, 0);
  SetRXAPanelRun(subrx->channel, 1);

  if(rx->enable_equalizer) {
    SetRXAGrphEQ(subrx->channel, rx->equalizer);
    SetRXAEQRun(subrx->channel, 1);
  } else {
    SetRXAEQRun(subrx->channel, 0);
  }

  subrx_set_agc(rx);

  SetEXTANBRun(subrx->channel, rx->nb);
  SetEXTNOBRun(subrx->channel, rx->nb2);

  SetRXAEMNRPosition(subrx->channel, rx->nr_agc);
  SetRXAEMNRgainMethod(subrx->channel, rx->nr2_gain_method);
  SetRXAEMNRnpeMethod(subrx->channel, rx->nr2_npe_method);
  SetRXAEMNRRun(subrx->channel, rx->nr2);
  SetRXAEMNRaeRun(subrx->channel, rx->nr2_ae);

  SetRXAANRVals(subrx->channel, 64, 16, 16e-4, 10e-7); // defaults
  SetRXAANRRun(subrx->channel, rx->nr);
  SetRXARNNRRun(subrx->channel, rx->nr3);
  SetRXASBNRRun(subrx->channel, rx->nr4);
  SetRXAANFRun(subrx->channel, rx->anf);
  SetRXASNBARun(subrx->channel, rx->snb);

}

void subrx_iq_buffer(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  gint error;
  fexchange0(subrx->channel, rx->iq_input_buffer, subrx->audio_output_buffer, &error);
}

// The two halves of the same thing when the main receiver has a feed: the whole
// span goes in once per block, and the sub-channel is exchanged whenever a full
// decimated block has come out.  Called from full_rx_buffer around the main
// channel's own exchange, so the two stay in step.
void subrx_iq_push(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  if(subrx==NULL || subrx->feed==NULL) return;
  rx_feed_push(subrx->feed,rx->iq_input_buffer,rx->buffer_size);
}

void subrx_iq_take(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  if(subrx==NULL) return;
  gint error;
  gdouble *blk;
  if(subrx->feed==NULL) {
    fexchange0(subrx->channel, rx->iq_input_buffer, subrx->audio_output_buffer, &error);
    return;
  }
  if(rx_feed_take(subrx->feed,&blk)) {
    // VFO-B's own blankers, on VFO-B's own block: the main path no longer
    // mutates the shared full-span buffer, so this is the only thing left that
    // blanks the sub-receiver.
    if(rx->nb)  xanbEXT(subrx->channel,blk,blk);
    if(rx->nb2) xnobEXT(subrx->channel,blk,blk);
    fexchange0(subrx->channel, blk, subrx->audio_output_buffer, &error);
  }
}

void subrx_update_noise(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  if(subrx==NULL) return;
  SetEXTANBRun(subrx->channel, rx->nb);
  SetEXTNOBRun(subrx->channel, rx->nb2);
}

// Reached from receiver_set_volume(), i.e. from every volume AND mute change.
void subrx_volume_changed(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  SetRXAPanelGain1(subrx->channel, receiver_panel_gain(rx));
}

void subrx_change_sample_rate(RECEIVER *rx) {
  SUBRX *subrx=(SUBRX *)rx->subrx;
  g_free(subrx->audio_output_buffer);
  subrx->audio_output_buffer=g_new0(gdouble,2*rx->output_samples);
  // The sub-channel is a full WDSP channel of its own and it was NOT re-rated
  // here: it kept the input rate and block the span had when SUBRX was switched
  // on, so changing the span left VFO-B demodulating at the wrong clock.  Its
  // feed belongs to the new span too.
  if(subrx->feed!=NULL) {
    rx_feed_destroy(subrx->feed);
    subrx->feed=NULL;
  }
  if(rx->dsp_feed!=NULL) {
    subrx->feed=rx_feed_create(rx->sample_rate,rx->dsp_in_rate,rx->dsp_in_block);
  }
  SetInputBuffsize(subrx->channel,rx->dsp_in_block);
  SetDSPBuffsize(subrx->channel,(subrx->feed!=NULL)?rx->dsp_size:rx->fft_size);
  SetAllRates(subrx->channel,rx->dsp_in_rate,48000,48000);
  SetEXTANBBuffsize(subrx->channel,rx->dsp_in_block);
  SetEXTNOBBuffsize(subrx->channel,rx->dsp_in_block);
  SetEXTANBSamplerate(subrx->channel,rx->dsp_in_rate);
  SetEXTNOBSamplerate(subrx->channel,rx->dsp_in_rate);
  subrx_frequency_changed(rx);
}

void destroy_subrx(RECEIVER *rx) {
log_info("%s\n",__FUNCTION__);
  SUBRX *subrx=(SUBRX *)rx->subrx;
  if(subrx==NULL) return;
  // The sub-channel is a full WDSP channel of its own (create_subrx opens one at
  // rx->channel + SUBRX_BASE_CHANNEL, with its own noise blankers).  Freeing the
  // SUBRX block without closing it leaked the whole DSP chain on every SUBRX
  // off/on -- and the next create_subrx re-opened the same channel number over
  // the top of it.
  destroy_anbEXT(subrx->channel);
  destroy_nobEXT(subrx->channel);
  CloseChannel(subrx->channel);
  if(subrx->feed!=NULL) {
    rx_feed_destroy(subrx->feed);
    subrx->feed=NULL;
  }
  g_mutex_clear(&subrx->mutex);
  g_free(subrx->audio_output_buffer);
  g_free(subrx);
  rx->subrx=NULL;      // every caller did this itself; do it once, here
}
