/*  rnnr.c  --  RNNoise receive noise reduction (NR3) for WDSP.  See rnnr.h. */

#define _CRT_SECURE_NO_WARNINGS
#include "comm.h"

#if defined(linux) || defined(__APPLE__) || defined(__MINGW32__)
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

// RNNoise's 480-sample frame IS 10 ms at 48 kHz and nothing else; the network's
// features are cut from that frame.  A channel running at another dsp_rate (WFM
// runs the whole chain at the span) is not a slower RNNoise, it is a different
// signal, so the block stands down there instead of pretending.
//
// The gate has to live in the RUN FLAG, not in xrnnr's body: bp1 is switched on
// and given a gain of 2.0 whenever an NR block runs, because these blocks emit
// a REAL signal in the I slot and bp1's one-sided passband is what makes it
// analytic again.  A block that leaves the buffer complex while bp1 still
// doubles it is +6 dB, measured.  So a->run is the EFFECTIVE flag every other
// file reads, a->want is the operator's wish, and both this and
// setSamplerate_rnnr re-derive one from the other.
#define RNNR_RATE 48000

// RNNoise has no strength of its own: rnnoise_process_frame returns the whole
// mask applied, and on a signal the network does not read as speech -- weak SSB
// through a transponder is not what it was trained on -- that is the voice going
// with the noise.  So the block carries a wet/dry mix, and the dry side has to
// be delayed by exactly what the wet side costs or the two comb-filter each
// other.  MEASURED, not assumed: rnnoise_process_frame's output lags its input
// by one whole frame (an impulse fed at sample 9700 came out at 10180), because
// frame_analysis windows the previous frame together with this one and
// frame_synthesis overlap-adds.  So the dry frame pushed alongside frame_out is
// the PREVIOUS input frame, which is what frame_dry holds.
#define RNNR_DEFAULT_DEPTH 1.0

static int rnnr_effective (RNNR a) { return a->want && a->rate == RNNR_RATE; }

void setBuffers_rnnr (RNNR a, double *in, double *out)
{
	a->in = in;
	a->out = out;
}

// Prime the OUTPUT ring with one frame of silence.  Without it the first blocks
// after every flush come up short -- a block of n samples drains
// floor((n + leftover) / 480) frames, which is fewer than n whenever the
// leftover is small -- and xrnnr filled the shortfall with zeros spliced into
// the audio: measured at 2048, seven of the first eleven blocks carried 32..128
// zero samples each.  One frame of latency (10 ms) costs nothing and makes the
// deficit unreachable at every block size this tree uses (1024..25600).
// RNNoise's own state is reset too: a flush means the stream was discontinued
// (a mode or span change, or NR3 being switched on), and carrying the network's
// recurrent state and its noise estimate across that gap makes the first
// seconds after every flush depend on what was being received before it.
void flush_rnnr (RNNR a)
{
	a->inbuf_n = 0;
	memset (a->inbuf, 0, a->bufcap * sizeof (float));
	memset (a->outbuf, 0, a->bufcap * sizeof (float));
	a->outbuf_n = a->frame_size;
	// Recreate rather than rnnoise_init(): upstream's init memsets the state and
	// calloc()s the three GRU buffers afresh, so the pointers it would have to
	// free are gone by the time it allocates -- calling it to reset leaks 672
	// bytes every time. That is per flush, i.e. per mode change, per span change
	// and per NR3 switch-on, and it is what LeakSanitizer reports out of
	// agc_offline on CI. rnnoise/ is unmodified upstream, so the fix belongs
	// here; a flush is rare and this is three small allocations.
	if (a->st)
	{
		rnnoise_destroy (a->st);
		a->st = rnnoise_create (NULL);
	}
}

// The two ring buffers, sized from the block.  Split out of create_rnnr so
// setSize_rnnr is the same allocation and cannot drift from it.
static void rnnr_alloc_bufs (RNNR a)
{
	// worst case a full block plus a partial frame can queue before draining
	a->bufcap = a->size + 4 * a->frame_size;
	a->inbuf  = (float *) malloc0 (a->bufcap * sizeof (float));
	a->outbuf = (float *) malloc0 (a->bufcap * sizeof (float));
	a->drybuf = (float *) malloc0 (a->bufcap * sizeof (float));
}

RNNR create_rnnr (int channel, int run, int position, int size, int rate, double *in, double *out)
{
	RNNR a = (RNNR) malloc0 (sizeof (rnnr));
	a->want = run;
	a->position = position;
	a->channel = channel;
	a->size = size;
	a->rate = rate;
	a->frame_size = 480;			// RNNoise fixed frame
	a->scale = RNNR_DEFAULT_SCALE;
	a->depth = RNNR_DEFAULT_DEPTH;
	a->st = rnnoise_create (NULL);
	a->run = rnnr_effective (a);
	a->in = in;
	a->out = out;
	rnnr_alloc_bufs (a);
	a->frame_in  = (float *) malloc0 (a->frame_size * sizeof (float));
	a->frame_out = (float *) malloc0 (a->frame_size * sizeof (float));
	a->frame_dry = (float *) malloc0 (a->frame_size * sizeof (float));
	flush_rnnr (a);
	return a;
}

void setSize_rnnr (RNNR a, int size)
{
	if (a->size == size) return;
	a->size = size;
	_aligned_free (a->drybuf);
	_aligned_free (a->outbuf);
	_aligned_free (a->inbuf);
	rnnr_alloc_bufs (a);
	flush_rnnr (a);
}

// Called from setSamplerate_rxa with the channel stopped, so the bp1 coupling
// is re-derived here rather than left to the next SetRXARNNRRun that may never
// come: leaving bp1 running at gain 2.0 over a block that has just stood down
// is +6 dB of audio.
void setSamplerate_rnnr (RNNR a, int rate)
{
	if (a->rate == rate) return;
	a->rate = rate;
	flush_rnnr (a);
	if (a->run != rnnr_effective (a))
	{
		a->run = rnnr_effective (a);
		RXAbp1Check (a->channel, rxa[a->channel].amd.p->run, rxa[a->channel].snba.p->run,
			rxa[a->channel].emnr.p->run, rxa[a->channel].anf.p->run,
			rxa[a->channel].anr.p->run, a->run, rxa[a->channel].sbnr.p->run);
		RXAbp1Set (a->channel);
	}
}

void destroy_rnnr (RNNR a)
{
	rnnoise_destroy (a->st);
	_aligned_free (a->frame_dry);
	_aligned_free (a->frame_out);
	_aligned_free (a->frame_in);
	_aligned_free (a->drybuf);
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

		// 2. drain full 480-sample frames through RNNoise, pushing the
		//    frame-aligned dry copy alongside each processed frame
		while (a->inbuf_n >= fs)
		{
			memcpy (a->frame_in, a->inbuf, fs * sizeof (float));
			rnnoise_process_frame (a->st, a->frame_out, a->frame_in);
			if (a->outbuf_n + fs <= a->bufcap)
			{
				memcpy (a->outbuf + a->outbuf_n, a->frame_out, fs * sizeof (float));
				memcpy (a->drybuf + a->outbuf_n, a->frame_dry, fs * sizeof (float));
				a->outbuf_n += fs;
			}
			memcpy (a->frame_dry, a->frame_in, fs * sizeof (float));
			a->inbuf_n -= fs;
			if (a->inbuf_n > 0)
				memmove (a->inbuf, a->inbuf + fs, a->inbuf_n * sizeof (float));
		}

		// 3. emit n samples.  flush_rnnr primes a frame of output, so the
		//    deficit below is a backstop, not a path the audio takes.
		{
			int avail = a->outbuf_n < n ? a->outbuf_n : n;
			int deficit = n - avail;
			for (i = 0; i < deficit; i++)
			{
				a->out[2 * i] = 0.0;
				a->out[2 * i + 1] = 0.0;
			}
			double w = a->depth;
			for (i = 0; i < avail; i++)
			{
				double dry = (double) a->drybuf[i];
				double wet = (double) a->outbuf[i];
				a->out[2 * (deficit + i)] = (dry + w * (wet - dry)) / sc;
				a->out[2 * (deficit + i) + 1] = 0.0;
			}
			a->outbuf_n -= avail;
			if (a->outbuf_n > 0)
			{
				memmove (a->outbuf, a->outbuf + avail, a->outbuf_n * sizeof (float));
				memmove (a->drybuf, a->drybuf + avail, a->outbuf_n * sizeof (float));
			}
		}
	}
	else if (a->out != a->in)
	{
		memcpy (a->out, a->in, a->size * sizeof (complex));
	}
}

PORT
void SetRXARNNRdepth (int channel, double depth)
{
	RNNR a = rxa[channel].rnnr.p;
	if (depth < 0.0) depth = 0.0;
	if (depth > 1.0) depth = 1.0;
	EnterCriticalSection (&ch[channel].csDSP);
	a->depth = depth;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXARNNRRun (int channel, int run)
{
	RNNR a = rxa[channel].rnnr.p;
	a->want = run;
	int eff = rnnr_effective (a);
	if (a->run != eff)
	{
		RXAbp1Check (channel, rxa[channel].amd.p->run, rxa[channel].snba.p->run,
			rxa[channel].emnr.p->run, rxa[channel].anf.p->run, rxa[channel].anr.p->run,
			eff, rxa[channel].sbnr.p->run);
		EnterCriticalSection (&ch[channel].csDSP);
		a->run = eff;
		if (eff) flush_rnnr (a);
		RXAbp1Set (channel);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}
