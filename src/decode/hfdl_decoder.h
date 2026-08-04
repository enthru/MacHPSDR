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
 * HFDL (ARINC 635 aviation HF Data Link) receive decoder — SCAFFOLD (parity 4.5,
 * phase 1).
 *
 * HFDL is a coherent M-PSK data link on an 1800 Hz carrier (1800 baud, adaptive
 * BPSK/QPSK/8-PSK), demodulated with a channel equalizer on a fading HF path —
 * the hardest decoder in the project. Unlike the amplitude-independent
 * FM-discriminator decoders (SSTV/WEFAX/CW) it taps the RAW off-air complex I/Q
 * (not the listen-audio path), like the recorder / vectorscope, so it is fed
 * from receiver.c:full_rx_buffer(), NOT process_rx_buffer(), and is independent
 * of decoder_taps_audio().
 *
 * This file is the phase-1 scaffold only: the I/Q tap, enable/disable gate,
 * status readout and message ring are wired end-to-end (visible in the Decode
 * block on the faker), but no demodulation/framing happens yet. The demod is
 * built up iteratively in later phases (liquid-dsp M-PSK + equalizer, then the
 * dumphfdl framing port, then libacars for message text). liquid-dsp is already
 * linked (phase 0) and its version is logged on first enable to prove linkage.
 *
 * Global singleton (like cw_decoder.c). Compiled only under the HFDL build flag.
 */

#ifndef _HFDL_DECODER_H
#define _HFDL_DECODER_H

#include <glib.h>

// Enable/disable the decoder. Driven from the active RX's I/Q tap in
// receiver.c; an off->on edge resets all state to a fresh start and logs the
// liquid-dsp version. No-op fast path when disabled.
void hfdl_decoder_set_enabled(gboolean on);

// Feed raw off-air complex I/Q: `nframes` complex samples, interleaved doubles
// (I = iq[2*i], Q = iq[2*i+1]) at `sample_rate` Hz. No-op unless enabled. Runs
// on the RX audio thread. (Phase 1: only accumulates a throughput counter; the
// demod chain lands in later phases.)
void hfdl_decoder_add_iq(const double *iq, int nframes, int sample_rate);

// Drain newly-decoded HFDL message lines into buf (NUL-terminated, buflen
// includes room for the NUL). Returns the number of bytes copied. GTK thread
// only. (Phase 1: never produces anything yet.)
int hfdl_decoder_get_messages(char *buf, int buflen);

// Peek at the newest decoded text WITHOUT consuming it (the bottom Decode block
// readout; the panel drains hfdl_decoder_get_messages and the two must not steal
// from each other).
int hfdl_decoder_get_recent(char *buf, int buflen);

// Snapshot of decoder status for the panel readout. GTK thread only.
//   listening   - TRUE while the tap is receiving I/Q (enabled + fed)
//   sample_rate - the off-air sample rate currently seen (Hz), 0 if idle
//   blocks      - conditioned symbol-domain samples produced since the last reset
void hfdl_decoder_get_status(gboolean *listening, int *sample_rate, glong *blocks);

// Current front-end AGC signal level (RSSI, dB) for the readout. GTK thread only.
double hfdl_decoder_get_level_db(void);

// Number of frames decoded since the last reset. GTK thread only.
glong hfdl_decoder_get_frames(void);

// Clear decoded text + reset state (panel "Clear" button). Safe from the GTK
// thread; the state reset is applied on the next audio-thread feed.
void hfdl_decoder_reset(void);

#endif
