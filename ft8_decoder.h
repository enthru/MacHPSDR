/* Copyright (C)
* 2026 - MacHPSDR fork
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
 * FT8 receive decoder.
 *
 * Taps the active receiver's demodulated 48 kHz audio, decimates it to the
 * 12 kHz mono stream the ft8_lib codec expects, buffers one 15-second UTC
 * time slot, and decodes it in a background thread on each slot boundary.
 * The decoder is enabled automatically whenever the active receiver's mode is
 * DIGU (see receiver_mode_changed()).
 *
 * Uses the vendored ft8_lib (Karlis Goba, MIT) under ft8_lib/.
 */

#ifndef _FT8_DECODER_H
#define _FT8_DECODER_H

#include <glib.h>

// One decoded FT8 message from a completed 15-second slot.
typedef struct _ft8_decode {
  char  utc[8];    // "hhmmss" slot start (UTC)
  float snr;       // approximate signal report (dB-ish, sync-score based)
  float dt;        // time offset within the slot, seconds
  float freq;      // audio frequency, Hz
  char  text[40];  // decoded message text
} FT8_DECODE;

// Create the worker thread and buffers.  Call once at start-up.
extern void ft8_decoder_init(void);

// Enable/disable decoding.  Enabling resets the current slot accumulator.
extern void ft8_decoder_set_enabled(gboolean enabled);
extern gboolean ft8_decoder_is_enabled(void);

// Feed one block of the active receiver's demodulated audio (48 kHz,
// interleaved stereo doubles; only the left channel is used).  Cheap: called
// from the RX/audio thread.  No-op unless the decoder is enabled.
extern void ft8_decoder_add_audio(const gdouble *samples, int nframes);

// Copy the most recent slot's decode list (thread-safe).  Returns the count,
// filling up to max entries.  utc7 (>=8 bytes), if non-NULL, receives the
// "hhmmss" of the slot those decodes belong to.
extern int ft8_decoder_get_decodes(FT8_DECODE *out, int max, char *utc7);

#endif
