/*  sbnr.c  --  libspecbleach spectral noise reduction (NR4) for WDSP.  See sbnr.h. */

#define _CRT_SECURE_NO_WARNINGS
#include <specbleach_adenoiser.h>
#include "comm.h"

#if defined(linux) || defined(__APPLE__) || defined(__MINGW32__)
#include "calculus.h"
#endif

void setBuffers_sbnr (SBNR a, double *in, double *out)
{
	a->in = in;
	a->out = out;
}

void flush_sbnr (SBNR a)
{
	// no external state to clear; libspecbleach keeps its own STFT history
	(void) a;
}

SBNR create_sbnr (int channel, int run, int position, int size, double *in, double *out)
{
	SBNR a = (SBNR) malloc0 (sizeof (sbnr));
	a->run = run;
	a->position = position;
	a->channel = channel;
	a->size = size;
	a->in = in;
	a->out = out;
	a->st = specbleach_adaptive_initialize (48000, 20);
	a->reduction_amount = 10.f;
	a->smoothing_factor = 0.f;
	a->whitening_factor = 0.f;
	a->noise_rescale = 2.f;
	a->post_filter_threshold = -10.f;
	a->fin  = (float *) malloc0 (size * sizeof (float));
	a->fout = (float *) malloc0 (size * sizeof (float));
	return a;
}

void destroy_sbnr (SBNR a)
{
	specbleach_adaptive_free (a->st);
	_aligned_free (a->fout);
	_aligned_free (a->fin);
	_aligned_free (a);
}

void xsbnr (SBNR a, int pos)
{
	if (a->run && pos == a->position)
	{
		int n = a->size;
		int i;
		SpectralBleachParameters parameters =
			(SpectralBleachParameters){.residual_listen = false,
				.reduction_amount = a->reduction_amount,
				.smoothing_factor = a->smoothing_factor,
				.whitening_factor = a->whitening_factor,
				.noise_scaling_type = 0,
				.noise_rescale = a->noise_rescale,
				.post_filter_threshold = a->post_filter_threshold};
		specbleach_adaptive_load_parameters (a->st, parameters);

		for (i = 0; i < n; i++)
			a->fin[i] = (float) a->in[2 * i];
		specbleach_adaptive_process (a->st, (uint32_t) n, a->fin, a->fout);
		for (i = 0; i < n; i++)
		{
			a->out[2 * i] = (double) a->fout[i];
			a->out[2 * i + 1] = 0.0;
		}
	}
	else if (a->out != a->in)
	{
		memcpy (a->out, a->in, a->size * sizeof (complex));
	}
}

PORT
void SetRXASBNRRun (int channel, int run)
{
	SBNR a = rxa[channel].sbnr.p;
	if (a->run != run)
	{
		RXAbp1Check (channel, rxa[channel].amd.p->run, rxa[channel].snba.p->run,
			rxa[channel].emnr.p->run, rxa[channel].anf.p->run, rxa[channel].anr.p->run,
			rxa[channel].rnnr.p->run, run);
		EnterCriticalSection (&ch[channel].csDSP);
		a->run = run;
		RXAbp1Set (channel);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

PORT
void SetRXASBNRreductionAmount (int channel, float amount)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].sbnr.p->reduction_amount = amount;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXASBNRsmoothingFactor (int channel, float factor)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].sbnr.p->smoothing_factor = factor;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXASBNRwhiteningFactor (int channel, float factor)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].sbnr.p->whitening_factor = factor;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXASBNRnoiseRescale (int channel, float factor)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].sbnr.p->noise_rescale = factor;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXASBNRpostFilterThreshold (int channel, float threshold)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].sbnr.p->post_filter_threshold = threshold;
	LeaveCriticalSection (&ch[channel].csDSP);
}
