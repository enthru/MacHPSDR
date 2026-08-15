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

// How often the GTK-thread poll checks whether the hang has run out. The hang
// itself keeps its millisecond resolution (it is a deadline, not a count of
// ticks); this only bounds how late the un-key can be, and 50 ms is far below
// anything an operator hears on a VOX tail.
#define VOX_POLL_MS 50

// Monotonic deadline (µs): while the mic stays above threshold this is pushed
// forward, and the poll below un-keys once it passes. Written by the mic thread,
// read on the GTK thread — a benign cross-thread scalar, the same shape as
// fake_play_pos_frames.
static volatile gint64 vox_hang_until_us = 0;

// Drop VOX and tell the radio. GTK thread only; caller has already decided.
static void vox_release(RADIO *r) {
  r->vox = 0;
  if (r->vox_timeout != 0) {
    g_source_remove(r->vox_timeout);
    r->vox_timeout = 0;
  }
  vox_changed(r);
}

// GTK-thread poll, alive only while VOX holds the transmitter.
//
// The un-key MUST NOT depend on the mic thread coming back: an audio device that
// stops delivering is exactly when the radio would otherwise be left keyed with
// nothing to release it.
static gboolean vox_poll_cb(gpointer data) {
  RADIO *r = (RADIO *)data;
  if (!r->vox) { r->vox_timeout = 0; return G_SOURCE_REMOVE; }
  if (!r->vox_enabled || g_get_monotonic_time() >= vox_hang_until_us) {
    r->vox = 0;
    r->vox_timeout = 0;
    vox_changed(r);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

// The 0->1 transition, moved to the GTK thread: it is where the poll source is
// created, and every GLib source operation in this file happens here or in the
// poll itself. update_vox() runs on the mic thread, and the old code armed and
// removed a GSource from there on every buffer -- racing the main loop that was
// dispatching that very source, and churning ~47 sources a second while anyone
// was speaking.
static gboolean vox_key_idle(gpointer data) {
  RADIO *r = (RADIO *)data;
  if (r->vox && r->vox_timeout == 0)
    r->vox_timeout = g_timeout_add(VOX_POLL_MS, vox_poll_cb, r);
  vox_changed(r);
  return G_SOURCE_REMOVE;
}

// Mic thread (add_mic_sample -> full_tx_buffer_process). Runs whether or not
// VOX is enabled: vox_peak is the mic bar's reading (mic_level.c) as well as the
// VOX trigger, so the measurement is not conditional on the feature.
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
      double hang = r->vox_hang > 0.0 ? r->vox_hang : 0.0;
      vox_hang_until_us = g_get_monotonic_time() + (gint64)(hang * 1000.0);
      if(!r->vox) {
        r->vox=1;
        g_idle_add(vox_key_idle,r);
      }
    }
  }
}

// The single choke point for the VOX enable, the way receiver_set_nr_mode() is
// for NR: the UI button, CAT (VX) and MIDI all reach it here.
//
// Turning VOX off has to release it. isTransmitting() ORs in r->vox, so a plain
// `vox_enabled = FALSE` while VOX was holding the key left the transmitter ON
// with nothing able to drop it -- the hang callback returned early on exactly
// that condition, so the radio transmitted until something else keyed or the
// app exited.
void vox_set_enabled(RADIO *r, gboolean enabled) {
  if (r == NULL) return;
  r->vox_enabled = enabled ? TRUE : FALSE;
  if (!r->vox_enabled) vox_cancel(r);
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

// GTK thread. Unconditional teardown: the source id is cleared on every path
// that ends the timer, so nothing later removes an id GLib has already dropped.
void vox_cancel(RADIO *r) {
  if(r->vox || r->vox_timeout != 0) vox_release(r);
}
