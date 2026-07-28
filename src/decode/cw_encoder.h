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
 * CW (Morse) transmit engine (Phase 4.4a).
 *
 * Synthesizes a keyed sidetone waveform for a text message (PARIS-standard
 * timing at the operator's configured WPM/weight/sidetone) and clocks it into
 * the WDSP TX chain via add_mic_sample() when the receiver is in CWL/CWU,
 * mirroring the ft8_encoder.c / sstv_encoder.c MOX-keying pattern: raise MOX
 * if we weren't already keyed, clock samples out, drop MOX again when the
 * waveform ends (watchdog-protected).
 *
 * cw_encode_to_audio() is a pure computational core (no RADIO/GTK/MOX
 * dependency) so it can be exercised by a small headless test harness against
 * cw_decoder.c for a round-trip encode/decode proof.
 */

#ifndef _CW_ENCODER_H
#define _CW_ENCODER_H

#include <glib.h>

// Build the keyed-sidetone waveform for `text` and start transmitting: keys MOX
// (if radio->can_transmit, mode is CWL/CWU, not already transmitting) and begins
// clocking samples out via add_mic_sample(). WPM/weight/sidetone read from RADIO.
// Returns FALSE if untransmittable or already sending.
extern gboolean cw_tx_send_text(const char *text);

// TRUE while a CW waveform is actively being clocked out (TX keyed by us).
extern gboolean cw_tx_active(void);

// Next 48 kHz sample for the TX chain; 0.0 when idle/exhausted. Lock-free.
extern float cw_tx_next_sample(void);

// Abort any in-progress transmission and drop MOX if we raised it.
extern void cw_tx_abort(void);

// --- PURE encoder (NO radio globals, NO MOX/GTK) — used by the headless test ---
// Render `text` to a keyed sidetone float buffer at sample_rate. Returns a newly
// g_malloc'd buffer (caller g_free's) and sets *n_samples; NULL on failure.
extern float *cw_encode_to_audio(const char *text, int wpm, int weight,
                                 double sidetone_hz, int sample_rate,
                                 double amplitude, int *n_samples);

#endif
