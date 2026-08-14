/* Copyright (C)
*
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

#include <gtk/gtk.h>
#include "log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <wdsp.h>

#include "agc.h"
#include "mode.h"
#include "filter.h"
#include "bandstack.h"
#include "band.h"
#include "discovered.h"
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
#include "bpsk.h"

#include "diversity_mixer.h"

void diversity_add_buffer(DIVMIXER *dmix) {
  memcpy(dmix->iq_input[0], dmix->rx_visual->iq_input_buffer, 2*dmix->iq_buffer_size * sizeof(gdouble));
}

void diversity_mix_full_buffers(DIVMIXER *dmix) {
  // Mix the iq streams
  memcpy(dmix->iq_input[1], dmix->rx_hidden->iq_input_buffer, 2*dmix->iq_buffer_size * sizeof(gdouble));

  // Diversity phase-scope tap: both coherent streams are now aligned in
  // iq_input[0] (visual/main) and iq_input[1] (hidden) BEFORE the mix, so this
  // is the one point where a main-vs-hidden Lissajous can be built. Plot the
  // main channel's I on X against the hidden channel's I on Y (the classic
  // diversity alignment scope): a common signal with relative complex gain
  // g=A*e^{jphi} traces a line (phi=0, slope=A) or an ellipse (phi!=0), so the
  // operator collapses the ellipse to the 45-deg reference with the diversity
  // Phase then Gain controls. Only fires when the visual RX's phase scope is in
  // Diversity source; publish is under scope_mutex, matching scope_iq_feed.
  RECEIVER *rxv = dmix->rx_visual;
  if(rxv->panadapter_phase && rxv->panadapter_phase_source==2 && rxv->scope_iq!=NULL) {
    int n = dmix->iq_buffer_size < rxv->scope_iq_cap ? dmix->iq_buffer_size : rxv->scope_iq_cap;
    g_mutex_lock(&rxv->scope_mutex);
    for(int i=0;i<n;i++) {
      rxv->scope_iq[i*2]   = (gfloat)dmix->iq_input[0][i*2];   // main  I -> X
      rxv->scope_iq[i*2+1] = (gfloat)dmix->iq_input[1][i*2];   // hidden I -> Y
    }
    rxv->scope_iq_n = n;
    g_mutex_unlock(&rxv->scope_mutex);
  }

  xdivEXT(dmix->id, dmix->iq_buffer_size, dmix->iq_input, dmix->iq_output_buffer);

  // Copy the output from the diversity mixer to our rx that we have visuals display for
  memcpy(dmix->rx_visual->diviq_input_buffer, dmix->iq_output_buffer, 2*dmix->iq_buffer_size * sizeof(gdouble));

  // Now proceed as normal for a receiver
  full_diviqrx_buffer(dmix->rx_visual);
}

void set_gain_phase(DIVMIXER *dmix) {
  double amplitude = pow(10.0, 0.05 * (dmix->gain + dmix->gain_fine));
  double arg = (dmix->phase + dmix->phase_fine) * (M_PI / 180);
  
  dmix->i_rotate[1] = amplitude*cos(arg);
  dmix->q_rotate[1] = amplitude*sin(arg);

  //dmix->i_rotate[0] = 1.0;
  //dmix->q_rotate[0] = 0;

  dmix->i_rotate[0] = 0;
  dmix->q_rotate[0] = 1.0;

//fprintf(stderr,"GAIN=%f PHASE=%f\n", dmix->gain, dmix->phase);

//fprintf(stderr,"0 - Irot=%f Qrot=%f\n", dmix->i_rotate[0], dmix->q_rotate[0]);  
//fprintf(stderr,"1 - Irot=%f Qrot=%f\n", dmix->i_rotate[1], dmix->q_rotate[1]);  
      
  SetEXTDIVRotate(dmix->id, dmix->num_streams, dmix->i_rotate, dmix->q_rotate);
}

// Just ADC1, Just ADC2, or diveristy mixer for ADC1+ADC2
void SetNumStreams(DIVMIXER *dmix) {
  SetEXTDIVOutput(dmix->id, dmix->num_streams);
}

void diversity_mix_calibrate_gain_visuals(DIVMIXER *dmix) {
  dmix->rx_hidden->panadapter_width = dmix->rx_visual->panadapter_width;
  dmix->rx_hidden->panadapter_height = dmix->rx_visual->panadapter_height;   
  dmix->rx_hidden->pixels = dmix->rx_visual->pixels;
  dmix->rx_hidden->fps = dmix->rx_visual->fps;
  dmix->rx_hidden->display_average_time = dmix->rx_visual->display_average_time;
  receiver_init_analyzer(dmix->rx_hidden);    
}


DIVMIXER *create_diversity_mixer(int id, RECEIVER *rxa, RECEIVER *rxb) {
  DIVMIXER *dmix=g_new0(DIVMIXER,1);
  
  char name [80];
  char *value;
  gint x=-1;
  gint y=-1;
  gint width;
  gint height;

  dmix->rx_visual = rxa;
  dmix->rx_hidden = rxb;
  
  // Samples counters for each RX must be set to 0 to allow them
  // to synchronise with add_iq_samples in receiver.c
  rxa->samples = 0;
  rxb->samples = 0;

log_info("create_diversity_mixer: id=%d\n", id);
  dmix->id=id;
  dmix->num_streams = 2;
  // Must match the receivers' actual buffer size: the mix copies/processes
  // exactly iq_buffer_size samples and the result is handed to
  // full_diviqrx_buffer(), which reads rx->buffer_size. Classic HPSDR (P1) uses
  // 1024, but the fake device (and any other rate) uses a different size (5120
  // at 192k), so hardcoding 1024 left 4/5 of each block stale -> broken stream.
  dmix->iq_buffer_size = rxa->buffer_size;

  dmix->gain = 0;
  dmix->phase = 0;
  dmix->gain_fine = 0;
  dmix->phase_fine = 0;
  
  dmix->flip = FALSE;
  
  dmix->calibrate_gain = FALSE;
  
  dmix->iq_output_buffer = g_new0(gdouble, 2*dmix->iq_buffer_size);
  
  dmix->iq_input = (gdouble **) g_malloc0(dmix->num_streams * sizeof(gdouble *));
  for (int i = 0; i < dmix->num_streams; i++) {
    dmix->iq_input[i] = (gdouble *) g_malloc0(2*dmix->iq_buffer_size * sizeof(gdouble)); 
  }

  dmix->i_rotate = g_new0(gdouble, dmix->num_streams);
  dmix->q_rotate = g_new0(gdouble, dmix->num_streams); 
  
  
  for (int i = 0; i < dmix->num_streams; i++) {  
    dmix->i_rotate[i] = 1.0;
    dmix->q_rotate[i] = 0.0;
  }

  // Create the WDSP diversity mixer
  create_divEXT(dmix->id, 0, dmix->num_streams, dmix->iq_buffer_size);
  // Initialise all the parameters
  SetEXTDIVBuffsize(dmix->id, dmix->iq_buffer_size);
  SetEXTDIVNr(dmix->id, dmix->num_streams);
  SetEXTDIVOutput(dmix->id, dmix->num_streams); 
  SetEXTDIVRotate(dmix->id, dmix->num_streams, dmix->i_rotate, dmix->q_rotate);
  // Now set the mixer to run
  SetEXTDIVRun(dmix->id, 1);

  return dmix;
}

// The mirror of create_diversity_mixer, and it exists for the same reason
// receiver_destroy() does.  Switching diversity off used to clear the slot in
// radio->divmixer[] and free NOTHING: not this struct, not iq_output_buffer,
// not the per-stream iq_input[] blocks, not i_rotate/q_rotate -- and, worst of
// the five, it never called destroy_divEXT(), so the next create_divEXT() ran
// over the top of the dead one on the same integer id.  That is exactly the
// OpenChannel-over-the-top pattern that made a closed receiver cost 1.2 GB.
// Measured at ~180 kB per off-cycle by LeakSanitizer, driving the toggle with
// MACHPSDR_DIVERSITY against tools/p2_emu.c.
//
// NOT protocol-specific, despite where it was found: the same teardown runs on
// Protocol 1 and on the faker.  Nothing had ever switched diversity off under a
// leak checker before, because doing it needed a click.
//
// Order mirrors create: stop the WDSP block first so nothing can be mixing into
// buffers that are about to go, then the buffers, then the struct.
void destroy_diversity_mixer(DIVMIXER *dmix) {
  if(dmix==NULL) return;
log_info("destroy_diversity_mixer: id=%d\n", dmix->id);

  SetEXTDIVRun(dmix->id, 0);
  destroy_divEXT(dmix->id);

  if(dmix->iq_input!=NULL) {
    for (int i = 0; i < dmix->num_streams; i++) {
      g_clear_pointer(&dmix->iq_input[i], g_free);
    }
    g_clear_pointer(&dmix->iq_input, g_free);
  }
  g_clear_pointer(&dmix->iq_output_buffer, g_free);
  g_clear_pointer(&dmix->i_rotate, g_free);
  g_clear_pointer(&dmix->q_rotate, g_free);

  g_free(dmix);
}
