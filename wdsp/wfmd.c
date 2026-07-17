/*  wfmd.c

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

Phase 1 - mono:
    quadrature discriminator (d/dt of the IQ phase) -> FM multiplex (MPX)
    -> one-pole de-emphasis (RC, time constant tau)
    -> audio low-pass (~15 kHz, fircore)
The demodulator runs at the channel dsp_rate, which for broadcast FM must be
wide enough to pass the ~200 kHz signal (>= 192 kHz recommended).  L and R
carry the same signal until the stereo decoder is added.

*/

#include "comm.h"

void calc_wfmd (WFMD a)
{
	double dt = 1.0 / a->rate;
	double w0, cw, sw, alpha, a0;
	double fn, zeta, th;
	// discriminator output scale: normalise to +/-1 at peak deviation
	a->again = a->rate / (a->deviation * TWOPI);
	// one-pole de-emphasis coefficient for time constant tau
	a->deemph_alpha = dt / (a->tau + dt);
	a->prev_i = 0.0;
	a->prev_j = 0.0;
	a->deemph_y_l = 0.0;
	a->deemph_y_r = 0.0;

	// ---- stereo decoder coefficients (all rate-dependent) ----
	// pilot band-pass: RBJ constant-skirt biquad at 19 kHz, Q ~= 6 (~3 kHz BW,
	// matching the offline prototype's pilot filter).  0 deg phase at f0.
	w0 = TWOPI * 19000.0 / a->rate;
	cw = cos (w0);
	sw = sin (w0);
	alpha = sw / (2.0 * 6.0);			// Q = 6
	a0 = 1.0 + alpha;
	a->bq_b0 = alpha / a0;				// b1 = 0, b2 = -b0
	a->bq_a1 = (-2.0 * cw) / a0;
	a->bq_a2 = (1.0 - alpha) / a0;
	a->bq_x1 = a->bq_x2 = a->bq_y1 = a->bq_y2 = 0.0;
	// pilot amplitude envelope tracker (~10 ms one-pole)
	a->env = 1e-9;
	a->env_alpha = 1.0 / (a->rate * 0.010);
	// 19 kHz PLL: fn = 80 Hz, zeta = 0.707 (as verified offline)
	a->pll_w0 = w0;
	fn = 80.0;
	zeta = 0.707;
	th = TWOPI * fn / a->rate;
	a->pll_alpha = 2.0 * zeta * th;
	a->pll_beta = th * th;
	a->pll_phase = 0.0;
	a->pll_freq = a->pll_w0;
	// pilot-lock coherence smoother (~50 ms) + reset gate
	a->lock = 0.0;
	a->lock_alpha = 1.0 / (a->rate * 0.050);
}

WFMD create_wfmd (int run, int size, double* in, double* out, int rate,
	double deviation, double f_low, double f_high, double afgain, double tau,
	int nc_aud, int mp_aud)
{
	WFMD a = (WFMD) malloc0 (sizeof (wfmd));
	double* impulse;
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = (double) rate;
	a->deviation = deviation;
	a->f_low = f_low;
	a->f_high = f_high;
	a->afgain = afgain;
	a->tau = tau;
	a->stereo = 1;						// decode stereo; auto-blends to mono w/o pilot
	// fircore requires nc >= size (nfor = nc/size must be >= 1); when the DSP
	// block (dsp_size) is larger than the requested tap count, raise nc to size
	// (both are powers of two, so nc stays an integer multiple of size).
	a->nc_aud = (nc_aud >= size) ? nc_aud : size;
	a->mp_aud = mp_aud;
	calc_wfmd (a);
	a->audio = (double *) malloc0 (a->size * sizeof (complex));
	// audio low-pass filter (0 .. f_high), realised as a symmetric band-pass
	impulse = fir_bandpass (a->nc_aud, -a->f_high, a->f_high, a->rate, 0, 1, a->afgain / (2.0 * a->size));
	a->paud = create_fircore (a->size, a->audio, a->out, a->nc_aud, a->mp_aud, impulse);
	_aligned_free (impulse);
	return a;
}

void destroy_wfmd (WFMD a)
{
	destroy_fircore (a->paud);
	_aligned_free (a->audio);
	_aligned_free (a);
}

void flush_wfmd (WFMD a)
{
	memset (a->audio, 0, a->size * sizeof (complex));
	flush_fircore (a->paud);
	a->prev_i = 0.0;
	a->prev_j = 0.0;
	a->deemph_y_l = 0.0;
	a->deemph_y_r = 0.0;
	// reset stereo decoder state (coeffs kept)
	a->bq_x1 = a->bq_x2 = a->bq_y1 = a->bq_y2 = 0.0;
	a->env = 1e-9;
	a->pll_phase = 0.0;
	a->pll_freq = a->pll_w0;
	a->lock = 0.0;
}

void xwfmd (WFMD a)
{
	if (a->run)
	{
		int i;
		double I, Q, re, im, d;
		for (i = 0; i < a->size; i++)
		{
			I = a->in[2 * i + 0];
			Q = a->in[2 * i + 1];
			// instantaneous frequency = arg( cur * conj(prev) ) -> FM multiplex
			re = I * a->prev_i + Q * a->prev_j;
			im = Q * a->prev_i - I * a->prev_j;
			a->prev_i = I;
			a->prev_j = Q;
			if ((re == 0.0) && (im == 0.0)) re = 1.0;
			d = a->again * atan2 (im, re);	// MPX (baseband + 19k pilot + L-R DSB)

			if (a->stereo)
			{
				double pil, piln, s, c, perr, ref38, gate, lr, left, right;
				// isolate the 19 kHz pilot (RBJ band-pass biquad, b1 = 0, b2 = -b0)
				pil = a->bq_b0 * (d - a->bq_x2) - a->bq_a1 * a->bq_y1 - a->bq_a2 * a->bq_y2;
				a->bq_x2 = a->bq_x1; a->bq_x1 = d;
				a->bq_y2 = a->bq_y1; a->bq_y1 = pil;
				// track pilot envelope, normalise so PLL loop gain is level-independent
				a->env += a->env_alpha * (fabs (pil) - a->env);
				piln = (a->env > 1e-9) ? pil / (a->env * 1.41421356) : 0.0;
				// 2nd-order PLL locking the NCO to the pilot
				s = sin (a->pll_phase);
				c = cos (a->pll_phase);
				perr = piln * (-s);				// phase detector
				a->pll_freq += a->pll_beta * perr;
				a->pll_phase += a->pll_freq + a->pll_alpha * perr;
				if (a->pll_phase >  PI) a->pll_phase -= TWOPI;
				if (a->pll_phase < -PI) a->pll_phase += TWOPI;
				// coherence: when locked piln ~ cos(phase) so 2*piln*cos -> ~1;
				// on noise/no-pilot it averages to ~0.  Drives the stereo gate.
				a->lock += a->lock_alpha * (2.0 * piln * c - a->lock);
				gate = a->lock;
				if (gate < 0.0) gate = 0.0;
				if (gate > 1.0) gate = 1.0;
				// 38 kHz reference (frequency-doubled pilot) demodulates the L-R DSB.
				// L-R = 2*(MPX*ref38); fold the matrix into the pre-LPF samples so the
				// shared 15k fircore yields L and R directly (LPF is linear):
				//   L = LPF(MPX*(1 + 2*gate*ref38)),  R = LPF(MPX*(1 - 2*gate*ref38))
				ref38 = cos (2.0 * a->pll_phase);
				lr = 2.0 * gate * ref38;
				left  = d * (1.0 + lr);
				right = d * (1.0 - lr);
				// per-channel de-emphasis (LTI, commutes with the following LPF)
				a->deemph_y_l += a->deemph_alpha * (left  - a->deemph_y_l);
				a->deemph_y_r += a->deemph_alpha * (right - a->deemph_y_r);
				a->audio[2 * i + 0] = a->deemph_y_l;
				a->audio[2 * i + 1] = a->deemph_y_r;
			}
			else
			{
				// mono: one-pole de-emphasis, L == R
				a->deemph_y_l += a->deemph_alpha * (d - a->deemph_y_l);
				a->audio[2 * i + 0] = a->deemph_y_l;
				a->audio[2 * i + 1] = a->deemph_y_l;
			}
		}
		// audio band-limiting low-pass -> a->out (also removes the 38k L-R images)
		xfircore (a->paud);
	}
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_wfmd (WFMD a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->paud, a->audio, a->out);
}

void setSamplerate_wfmd (WFMD a, int rate)
{
	double* impulse;
	a->rate = rate;
	calc_wfmd (a);
	impulse = fir_bandpass (a->nc_aud, -a->f_high, a->f_high, a->rate, 0, 1, a->afgain / (2.0 * a->size));
	setImpulse_fircore (a->paud, impulse, 1);
	_aligned_free (impulse);
}

void setSize_wfmd (WFMD a, int size)
{
	double* impulse;
	_aligned_free (a->audio);
	a->size = size;
	if (a->nc_aud < a->size) a->nc_aud = a->size;	// keep fircore nc >= size
	calc_wfmd (a);
	a->audio = (double *) malloc0 (a->size * sizeof (complex));
	destroy_fircore (a->paud);
	impulse = fir_bandpass (a->nc_aud, -a->f_high, a->f_high, a->rate, 0, 1, a->afgain / (2.0 * a->size));
	a->paud = create_fircore (a->size, a->audio, a->out, a->nc_aud, a->mp_aud, impulse);
	_aligned_free (impulse);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetRXAWFMDeviation (int channel, double deviation)
{
	WFMD a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].wfmd.p;
	a->deviation = deviation;
	a->again = a->rate / (a->deviation * TWOPI);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAWFMDeemphasisTau (int channel, double tau)
{
	WFMD a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].wfmd.p;
	a->tau = tau;
	a->deemph_alpha = (1.0 / a->rate) / (a->tau + 1.0 / a->rate);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAWFMStereo (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].wfmd.p->stereo = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

// pilot-lock coherence in [0,1]: ~0 = no stereo pilot (mono), ~1 = solid stereo
PORT
double GetRXAWFMPilotLock (int channel)
{
	return rxa[channel].wfmd.p->lock;
}
