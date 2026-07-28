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
 * CW (Morse) receive decoder.
 *
 * A self-contained audio->text CW decoder: tracks the keyed tone with a small
 * Goertzel filter bank (no FFT/WDSP dependency), forms an adaptive on/off
 * envelope (Schmitt-triggered against a floor/peak follower), classifies
 * mark/space runs against an adaptively-estimated dot length, and resolves
 * completed dot/dash symbols through a static Morse table. Runs inline on the
 * RX audio thread; the decoded text + status are drained/polled from the GTK
 * thread through a mutex (same model as sstv_decoder.c).
 */

#ifndef _CW_DECODER_H
#define _CW_DECODER_H

#include <glib.h>

// Enable/disable the decoder. Called on every RX audio buffer from the tap in
// receiver.c; an off->on edge resets all DSP + text state to a fresh start.
// No-op fast path when disabled.
void cw_decoder_set_enabled(gboolean on);

// Feed demodulated audio: 48 kHz, interleaved-stereo doubles (left channel
// used, samples[i*2]). No-op unless enabled. Runs on the RX audio thread.
void cw_decoder_add_audio(const double *samples, int nframes);

// Drain newly-decoded characters (letters/digits/punctuation + word-gap
// spaces) into buf (NUL-terminated, buflen includes room for the NUL).
// Returns the number of characters copied. GTK thread only.
int cw_decoder_get_text(char *buf, int buflen);

// Snapshot of decoder status for the panel's readout line. GTK thread only.
//   wpm     - estimated words-per-minute (from the adaptive dot length)
//   tone_hz - the currently-tracked keyed-tone frequency
//   locked  - TRUE once enough elements have been seen for wpm/tone to be
//             considered a stable estimate
void cw_decoder_get_status(int *wpm, double *tone_hz, gboolean *locked);

// Clear decoded text + reset DSP state (panel "Clear" button). Safe to call
// from the GTK thread; the DSP reset itself is deferred to the audio thread
// (mirrors sstv_decoder_reset()).
void cw_decoder_reset(void);

#endif
