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
 * SSTV transmit engine (the mirror of sstv_decoder.c).
 *
 * Encodes an image to an analogue-SSTV audio timeline — a VIS header followed by
 * per-line sync / porch / separator tones and the FM-encoded pixel scans (1500 Hz
 * = black .. 2300 Hz = white, 1200 Hz = sync) — and clocks it out at the 48 kHz
 * mic rate through the WDSP TX chain via add_mic_sample() (transmitter.c), exactly
 * as the FT8 encoder does.  The normal DIGU (USB) / FMN (FM) TX chain then
 * modulates the tone up to the dial frequency, so this is universal across
 * protocol1/2/soapy.  Self-contained: no external DSP dependency.
 *
 * Supported modes match the decoder's MODES[] table (N7CXI timings): Martin
 * M1/M2, Scottie S1/S2/DX, Robot 36/72, PD50/90/120/160/180/240.
 */

#ifndef _SSTV_ENCODER_H
#define _SSTV_ENCODER_H

#include <gtk/gtk.h>

// Prepare (encode) `img` as SSTV mode `vis` into an internal tone timeline.  The
// image is scaled to the mode's native geometry.  Returns FALSE on a bad VIS /
// NULL image, or if a transmission is already in progress.  Does not key TX;
// call sstv_tx_start() to begin.
gboolean sstv_tx_prepare(int vis, GdkPixbuf *img);

// Duration of the prepared waveform, in seconds (0 if nothing prepared).
double sstv_tx_duration(void);

// Begin transmitting the prepared waveform: raises MOX and starts clocking the
// tone into the TX chain.  No-op (with a logged reason) if nothing is prepared,
// the radio cannot transmit, or the active receiver is not in a phone mode that
// carries SSTV (DIGU / DIGL / FMN).
void sstv_tx_start(void);

// Cancel an in-progress transmission and drop MOX if we raised it.
void sstv_tx_stop(void);

// TRUE while a waveform is actively being clocked out (TX keyed by us).
gboolean sstv_tx_active(void);

// Fraction of the waveform sent so far, 0.0..1.0 (for a progress bar).
double sstv_tx_progress(void);

// Next 48 kHz waveform sample for the TX chain; 0.0 when idle / exhausted.
// Called from the audio/TX thread via add_mic_sample().  Cheap, lock-free.
float sstv_tx_next_sample(void);

#endif
