/*  wfmd.h

Wideband FM (broadcast) demodulator for WDSP.

This file is part of a program that implements a Software-Defined Radio.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Phase 1: quadrature discriminator -> MPX -> audio LPF (~15 kHz) + de-emphasis.
Mono only; L and R carry the same signal.  Stereo (19 kHz pilot / 38 kHz L-R)
and RDS (57 kHz) are added in later phases.

*/

#ifndef _wfmd_h
#define _wfmd_h

#include "firmin.h"

typedef struct _wfmd
{
	int run;
	int size;
	double* in;
	double* out;
	double rate;
	double deviation;					// peak deviation used for output normalisation (Hz)
	double f_low;						// audio low cutoff (Hz)
	double f_high;						// audio high cutoff (Hz)
	double afgain;						// audio gain
	// quadrature discriminator state (previous IQ sample)
	double prev_i;
	double prev_j;
	double again;						// discriminator output scale
	// MPX / audio scratch buffer (interleaved, L==R for now)
	double* audio;
	// de-emphasis (one-pole RC low-pass, time constant tau)
	double tau;							// seconds (50e-6 EU / 75e-6 US)
	double deemph_alpha;
	double deemph_y_l;
	double deemph_y_r;
	// ---- stereo decoder (19 kHz pilot / 38 kHz L-R subcarrier) ----
	int stereo;							// 1 = decode stereo (auto-blends to mono if no pilot)
	// pilot band-pass (single RBJ biquad centred at 19 kHz; 0 deg phase at f0
	// so the recovered pilot phase is unbiased -> correct 38 kHz reference)
	double bq_b0;						// b1 == 0, b2 == -b0 (constant-skirt band-pass)
	double bq_a1;
	double bq_a2;
	double bq_x1, bq_x2, bq_y1, bq_y2;	// biquad state
	// pilot envelope (one-pole on |pilot|) for loop-gain-independent PLL drive
	double env;
	double env_alpha;
	// 19 kHz pilot PLL (2nd order)
	double pll_w0;						// nominal 19 kHz in rad/sample
	double pll_alpha;					// loop proportional gain
	double pll_beta;					// loop integral gain
	double pll_phase;
	double pll_freq;
	// pilot-lock metric (smoothed coherence 0..1) -> stereo/mono blend gate
	double lock;
	double lock_alpha;
	// audio band-limiting filter (fircore low-pass)
	FIRCORE paud;
	int nc_aud;
	int mp_aud;
} wfmd, *WFMD;

extern WFMD create_wfmd (int run, int size, double* in, double* out, int rate,
	double deviation, double f_low, double f_high, double afgain, double tau,
	int nc_aud, int mp_aud);

extern void destroy_wfmd (WFMD a);

extern void flush_wfmd (WFMD a);

extern void xwfmd (WFMD a);

extern void setBuffers_wfmd (WFMD a, double* in, double* out);

extern void setSamplerate_wfmd (WFMD a, int rate);

extern void setSize_wfmd (WFMD a, int size);

// RXA Properties
extern void SetRXAWFMDeviation (int channel, double deviation);
extern void SetRXAWFMDeemphasisTau (int channel, double tau);
extern void SetRXAWFMStereo (int channel, int run);
extern double GetRXAWFMPilotLock (int channel);

#endif
