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

#ifndef SUBRX_H
#define SUBRX_H

#include "receiver.h"

#define SUBRX_BASE_CHANNEL 16

typedef struct _subrx {
  gint channel;
  // VFO-B's own NCO + decimator when the main receiver has one (see
  // RECEIVER.dsp_feed): the sub-channel listens somewhere else in the same
  // span, so it cannot share the main feed's output.
  void *feed;
  gint buffer_size;
  gint fft_size;
  gdouble *audio_output_buffer;
  GMutex mutex;
} SUBRX;

// Called from full_rx_buffer while the main feed is active: push the full-span
// I/Q into VFO-B's own feed, then exchange one decimated block if one is ready.
extern void subrx_iq_push(RECEIVER *rx);
extern void subrx_iq_take(RECEIVER *rx);

extern void subrx_change_sample_rate(RECEIVER *rx);
extern void create_subrx(RECEIVER *rx);
extern void destroy_subrx(RECEIVER *rx);
extern void subrx_iq_buffer(RECEIVER *rx);
extern void subrx_frequency_changed(RECEIVER *rx);
extern void subrx_mode_changed(RECEIVER *rx);
extern void subrx_set_agc(RECEIVER *rx);
extern void subrx_filter_changed(RECEIVER *rx);
extern void subrx_volume_changed(RECEIVER *rx);
extern void subrx_set_apf(RECEIVER *rx);
// Mirror the main receiver's NB/NB2 switches onto the sub-channel's own
// blankers, which subrx_iq_take applies (see update_noise).
extern void subrx_update_noise(RECEIVER *rx);

#endif
