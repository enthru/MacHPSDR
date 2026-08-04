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
 * HFDL receive front-end DSP — parity 4.5, phase 2a.
 *
 * Conditions the raw off-air complex I/Q into the HFDL symbol domain:
 *
 *   NCO carrier downmix (−HFDL_CARRIER_OFFSET_HZ)  →  bring the SSB carrier to DC
 *   msresamp (multi-stage arbitrary resampler)     →  HFDL_SYM_RATE*HFDL_SPS S/s
 *   AGC                                            →  level-normalise
 *   matched filter (root-raised-cosine, 19 taps)   →  ISI-shaped baseband
 *
 * This is a straight port of the front half of dumphfdl's (GPL-3.0)
 * `hfdl_channel_create()` / demod loop (src/hfdl.c): the same 1800-baud / SPS=3
 * / 1440 Hz-offset parameters and the same liquid-dsp blocks. dumphfdl's FFT
 * channelizer is dropped — our WDSP receiver already delivers a narrow slice
 * centred on the dial, so a single NCO mixer replaces it.
 *
 * What phase 2a delivers is exactly this conditioned symbol-domain stream plus a
 * signal-level (AGC RSSI) readout. The coupled demod CORE — symbol-timing
 * (symsync), carrier recovery (Costas), LMS equalizer, M-PSK modem and preamble
 * correlation → framing → FEC — is the next phase (it needs libfec's Viterbi and
 * can only be meaningfully verified against a real HFDL recording).
 *
 * Self-contained + liquid-dsp only; no GTK/RADIO deps, so it links into a
 * headless unit test (see hfdl_demod_selftest()).
 */

#ifndef _HFDL_DEMOD_H
#define _HFDL_DEMOD_H

#include <glib.h>

#define HFDL_SYM_RATE          1800   // HFDL symbol rate (baud)
#define HFDL_SPS               3      // samples/symbol in the resampled domain
#define HFDL_CARRIER_OFFSET_HZ 1440   // SSB carrier offset (dumphfdl HFDL_SSB_CARRIER_OFFSET_HZ)
// Conditioned output sample rate = symbol rate × samples-per-symbol.
#define HFDL_BASEBAND_RATE     (HFDL_SYM_RATE * HFDL_SPS)   // 5400 S/s

typedef struct hfdl_demod hfdl_demod;

// Create the front-end for an off-air complex input at input_rate Hz. Returns
// NULL on failure (e.g. input_rate below the symbol-domain rate).
hfdl_demod *hfdl_demod_create(double input_rate);

// Same, for a channel whose centre sits channel_offset_hz away from the input's
// centre — the receiver passband usually holds several HFDL channels, and one
// front-end per channel is what lets them all be decoded at once. Offset 0 is
// exactly hfdl_demod_create().
hfdl_demod *hfdl_demod_create_at(double input_rate, double channel_offset_hz);
void hfdl_demod_destroy(hfdl_demod *d);

// Feed nframes complex input samples (interleaved I/Q doubles: I=iq[2i],
// Q=iq[2i+1]); write up to max_out conditioned baseband samples (HFDL_BASEBAND_RATE)
// into out as interleaved float I/Q (needs room for 2*max_out floats). Returns
// the number of complex samples written (≤ max_out; excess is dropped this call).
int hfdl_demod_process(hfdl_demod *d, const double *iq, int nframes,
                       float *out, int max_out);

// Current AGC signal level (RSSI, dB) — slowly tracking, for the panel readout.
double hfdl_demod_level_db(hfdl_demod *d);

// Move the front-end's channel offset without rebuilding it. Only the downmix
// NCO changes; the resampler, AGC, matched filter and symbol recovery keep their
// state, so this is cheap enough to do while hunting for the carrier. Used by
// the decoder's coarse search: the Costas loop only pulls in ~±100 Hz, which is
// far finer than an operator can point a mouse.
void hfdl_demod_set_channel_offset(hfdl_demod *d, double channel_offset_hz);

// Symbol recovery (phase 2b): take conditioned baseband (HFDL_BASEBAND_RATE,
// interleaved float I/Q from hfdl_demod_process) and run symbol-timing recovery
// (symsync) + carrier recovery (Costas loop, decision-directed against BPSK),
// emitting one carrier-locked complex symbol per HFDL symbol (~HFDL_SYM_RATE/s)
// into out_syms (interleaved float I/Q; room for 2*max_out floats). Returns the
// symbol count. The LMS equalizer and adaptive BPSK/PSK4/PSK8 selection are
// driven by the frame state machine (a later phase); this stage recovers the
// clean/lightly-impaired constellation the synthetic self-test exercises.
int hfdl_demod_symbols(hfdl_demod *d, const float *baseband, int nbb,
                       float *out_syms, int max_out);

// Modulation the decision-directed carrier loop slices against: 1=BPSK, 2=PSK4,
// 3=PSK8. MUST track what the frame is actually carrying — a BPSK phase error on
// a QPSK/8-PSK symbol pulls constellation points onto the real axis and drags
// the carrier phase off (dumphfdl's `current_mod_arity`, switched by its framer
// on entering/leaving the data section). Preamble, mode and training fields are
// always BPSK; only the data segments use the frame's own modulation.
void hfdl_demod_set_slicer(hfdl_demod *d, int arity);

// Per-symbol variant of hfdl_demod_symbols(): calls cb for each recovered symbol
// instead of filling a buffer. This is what lets the carrier loop follow the
// frame's modulation — the caller pushes the symbol into the framer and sets the
// slicer for the NEXT symbol from the framer's state, symbol by symbol, the way
// dumphfdl's single combined loop does. Returns the symbol count.
typedef void (*hfdl_symbol_cb)(void *ctx, float re, float im);
int hfdl_demod_symbols_cb(hfdl_demod *d, const float *baseband, int nbb,
                          hfdl_symbol_cb cb, void *ctx);

// Headless self-test (no GTK/RADIO deps — links into a standalone binary):
//   (a) front-end: a tone at the carrier offset lands at DC (coherent) at the
//       exact HFDL_BASEBAND_RATE;
//   (b) symbol recovery: synthetic RRC-shaped BPSK (with a carrier offset) is
//       recovered end-to-end through process()+symbols() at ~0 differential BER.
// Returns TRUE only if both pass.
gboolean hfdl_demod_selftest(void);

#endif
