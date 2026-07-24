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
 * FT8 transmit engine (Phase 2).
 *
 * Encodes a message to the 79 FT8 tones (ft8_lib), synthesizes the GFSK audio
 * waveform at the 48 kHz mic rate, and clocks it into the WDSP TX chain via
 * add_mic_sample() when the receiver is in DIGU.  A slot scheduler keys MOX at
 * the top of the chosen UTC 15-second slot and drops it when the waveform ends.
 *
 * Uses the vendored ft8_lib (Karlis Goba, MIT) under ft8_lib/.
 */

#ifndef _FT8_ENCODER_H
#define _FT8_ENCODER_H

#include <glib.h>

// Prepare (encode + synthesize) an FT8 message for transmission at the given
// audio offset (Hz).  Returns FALSE if the text cannot be encoded.  Does not
// key TX; call ft8_tx_arm() to schedule the actual transmission.
extern gboolean ft8_tx_prepare(const char *text, float offset_hz);

// Arm a one-shot transmission on the next matching UTC slot.  tx_even selects
// the slot parity: TRUE => slots beginning at an even 15-second index
// (…:00, :30), FALSE => odd (…:15, :45).  Requires a prepared waveform.
// Starts the slot scheduler; MOX is raised automatically at the slot boundary
// and dropped when the waveform finishes.
extern void ft8_tx_arm(gboolean tx_even);

// Cancel any pending or in-progress transmission and drop MOX if we raised it.
extern void ft8_tx_disarm(void);

// TRUE while an FT8 waveform is actively being clocked out (TX keyed by us).
extern gboolean ft8_tx_active(void);

// Next 48 kHz waveform sample for the TX chain; 0.0 when idle/exhausted.
// Called from the audio/TX thread via add_mic_sample().  Cheap, lock-free.
extern float ft8_tx_next_sample(void);

#endif
