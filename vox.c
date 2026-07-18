/* Copyright (C)
* 2016 - John Melton, G0ORX/N6LYT
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

#include "discovered.h"
#include "bpsk.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "vox.h"
#include "vfo.h"
#include "ext.h"

static int vox_timeout_cb(gpointer data) {
  RADIO *r=(RADIO *)data;
  if(r->vox_enabled) {
    r->vox=0;
    g_idle_add(ext_vox_changed,NULL);
  }
  return FALSE;
}

void update_vox(RADIO *r) {
  // calculate peak microphone input
  // assumes it is interleaved left and right channel with length samples
  int i;
  double sample;
  r->vox_peak=0.0;
  for(i=0;i<r->transmitter->buffer_size;i++) {
    sample=(double)r->transmitter->mic_input_buffer[i];
    if(sample<0.0) {
      sample=-sample;
    }
    if(sample>r->vox_peak) {
      r->vox_peak=sample;
    }
  }

  if(r->vox_enabled) {
    if(r->vox_peak>r->vox_threshold) {
      if(r->vox) {
        g_source_remove(r->vox_timeout);
      } else {
        r->vox=1;
        g_idle_add(ext_vox_changed,NULL);
      }
      r->vox_timeout=g_timeout_add((int)r->vox_hang,vox_timeout_cb,r);
    }
  }
}

void set_cwvox(RADIO *r, gboolean cw_key_state) {
  r->hang_time_ctr = 0;
  if (cw_key_state == 1) {
    if(!r->mox) {
      MOX_STATE *m=g_new0(MOX_STATE,1);
      m->radio=r;
      m->state=1;
      g_idle_add(ext_set_mox,(gpointer)m);      
    }
  }
  else {
    r->hang_time_ctr = 0;      
  }
}

void update_cwvox(RADIO *r) {
  r->hang_time_ctr += r->protocol1_timer;
        
  if (r->hang_time_ctr > (double)r->cw_keyer_hang_time) {    
    MOX_STATE *m=g_new0(MOX_STATE,1);
    m->radio=r;
    m->state=0;
    g_idle_add(ext_set_mox,(gpointer)m); 
  }
}

void vox_cancel(RADIO *r) {
  if(r->vox) {
    g_source_remove(r->vox_timeout);
    r->vox=0;
    g_idle_add(ext_vox_changed,NULL);
  }
}
