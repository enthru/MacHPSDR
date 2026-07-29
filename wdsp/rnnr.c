/*  rnnr.c  --  RNNoise receive noise reduction (NR3) for WDSP.  See rnnr.h. */

#define _CRT_SECURE_NO_WARNINGS
#include "comm.h"

#if defined(linux) || defined(__APPLE__)
#include "calculus.h"
#endif

// RNNoise was trained on ~int16-magnitude PCM, WDSP audio is ~+/-1.0 float.
// The signal is scaled up before analysis and back down after; because
// RNNoise applies a spectral gain mask the net level is unchanged, so this
// only positions RNNoise's feature/VAD extraction.  We insert pre-AGC
// (position 0) where the demod level can be well below full scale, so a hot
// pre-scale is wanted; 500000 matches the vu3rdd/pihpsdr reference port (their
// on-air-tuned value).  Tunable if NR3 proves too weak/strong on air.
#define RNNR_DEFAULT_SCALE 500000.0

void setBuffers_rnnr (RNNR a, double *in, double *out)
{
	a->in = in;
	a->out = out;
}

void flush_rnnr (RNNR a)
{
	a->inbuf_n = 0;
	a->outbuf_n = 0;
	memset (a->inbuf, 0, a->bufcap * sizeof (float));
	memset (a->outbuf, 0, a->bufcap * sizeof (float));
}

RNNR create_rnnr (int channel, int run, int position, int size, double *in, double *out)
{
	RNNR a = (RNNR) malloc0 (sizeof (rnnr));
	a->run = run;
	a->position = position;
	a->channel = channel;
	a->size = size;
	a->frame_size = 480;			// RNNoise fixed frame
	a->scale = RNNR_DEFAULT_SCALE;
	a->st = rnnoise_create (NULL);
	a->in = in;
	a->out = out;
	// worst case a full block plus a partial frame can queue before draining
	a->bufcap = size + 4 * a->frame_size;
	a->inbuf  = (float *) malloc0 (a->bufcap * sizeof (float));
	a->outbuf = (float *) malloc0 (a->bufcap * sizeof (float));
	a->frame_in  = (float *) malloc0 (a->frame_size * sizeof (float));
	a->frame_out = (float *) malloc0 (a->frame_size * sizeof (float));
	a->inbuf_n = 0;
	a->outbuf_n = 0;
	return a;
}

void destroy_rnnr (RNNR a)
{
	rnnoise_destroy (a->st);
	_aligned_free (a->frame_out);
	_aligned_free (a->frame_in);
	_aligned_free (a->outbuf);
	_aligned_free (a->inbuf);
	_aligned_free (a);
}

void xrnnr (RNNR a, int pos)
{
	if (a->run && pos == a->position)
	{
		int n = a->size;
		int fs = a->frame_size;
		int i;
		double sc = a->scale;

		// 1. queue this block's real samples (guard against overflow)
		if (a->inbuf_n + n > a->bufcap) a->inbuf_n = 0;
		for (i = 0; i < n; i++)
			a->inbuf[a->inbuf_n + i] = (float) (a->in[2 * i] * sc);
		a->inbuf_n += n;

		// 2. drain full 480-sample frames through RNNoise
		while (a->inbuf_n >= fs)
		{
			memcpy (a->frame_in, a->inbuf, fs * sizeof (float));
			rnnoise_process_frame (a->st, a->frame_out, a->frame_in);
			if (a->outbuf_n + fs <= a->bufcap)
			{
				memcpy (a->outbuf + a->outbuf_n, a->frame_out, fs * sizeof (float));
				a->outbuf_n += fs;
			}
			a->inbuf_n -= fs;
			if (a->inbuf_n > 0)
				memmove (a->inbuf, a->inbuf + fs, a->inbuf_n * sizeof (float));
		}

		// 3. emit n samples; zero-fill the startup latency deficit
		{
			int avail = a->outbuf_n < n ? a->outbuf_n : n;
			int deficit = n - avail;
			for (i = 0; i < deficit; i++)
			{
				a->out[2 * i] = 0.0;
				a->out[2 * i + 1] = 0.0;
			}
			for (i = 0; i < avail; i++)
			{
				a->out[2 * (deficit + i)] = (double) a->outbuf[i] / sc;
				a->out[2 * (deficit + i) + 1] = 0.0;
			}
			a->outbuf_n -= avail;
			if (a->outbuf_n > 0)
				memmove (a->outbuf, a->outbuf + avail, a->outbuf_n * sizeof (float));
		}
	}
	else if (a->out != a->in)
	{
		memcpy (a->out, a->in, a->size * sizeof (complex));
	}
}

PORT
void SetRXARNNRRun (int channel, int run)
{
	RNNR a = rxa[channel].rnnr.p;
	if (a->run != run)
	{
		RXAbp1Check (channel, rxa[channel].amd.p->run, rxa[channel].snba.p->run,
			rxa[channel].emnr.p->run, rxa[channel].anf.p->run, rxa[channel].anr.p->run,
			run, rxa[channel].sbnr.p->run);
		EnterCriticalSection (&ch[channel].csDSP);
		a->run = run;
		if (run) flush_rnnr (a);
		RXAbp1Set (channel);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}
