/*  rnnr.h

    RNNoise-based receive noise reduction (NR3) for WDSP.

    This wraps Xiph's RNNoise (BSD-3, vendored under rnnoise/) as a WDSP RXA
    block, alongside anf/anr/emnr.  RNNoise processes fixed 480-sample frames
    at 48 kHz; the WDSP DSP block (ch[channel].dsp_size) is 2048 or 5120 and is
    not a multiple of 480, so this block does its own per-instance ring
    buffering (state lives in the struct, NOT in file-static buffers like the
    reference port) so it is both correct for any dsp_size and safe with
    multiple receivers.

    setSize_rnnr()/setSamplerate_rnnr() exist because ch[channel].dsp_size and
    dsp_rate MOVE: a span change re-runs SetDSPBuffsize/SetAllRates on a live
    channel.  Without them this block kept the geometry the channel was OPENED
    with -- reading and writing past a midbuff that had since shrunk, or
    processing only the first fifth of each block and passing the rest through
    raw.  Every other RXA block has the pair; these two were the only ones
    setDSPBuffsize_rxa() could not reach.

    This file is part of the MacHPSDR fork; RNNoise itself is unmodified.
*/

#ifndef _rnnr_h
#define _rnnr_h

#include "rnnoise.h"

typedef struct _rnnr
{
	int run;			// 0/1 EFFECTIVE enable: what the chain (and the bp1
					// gain coupling) sees -- want && rate == 48 kHz
	int want;			// 0/1 what the operator asked for
	int position;			// pre-AGC (0) or post-AGC (1) slot
	int channel;			// owning RXA channel
	int size;			// dsp block size (complex samples per xrnnr)
	int rate;			// the CHANNEL's dsp rate; RNNoise is 48 kHz only
	int frame_size;			// RNNoise frame (480)
	double scale;			// pre-scale into RNNoise's operating range
	DenoiseState *st;
	double *in;
	double *out;
	// per-instance buffering (real samples)
	float *inbuf;			// queued input awaiting a full 480 frame
	int    inbuf_n;
	float *outbuf;			// processed output awaiting emission
	int    outbuf_n;
	int    bufcap;			// capacity of inbuf/outbuf
	float *frame_in;		// frame_size scratch
	float *frame_out;
} rnnr, *RNNR;

extern RNNR create_rnnr (int channel, int run, int position, int size, int rate, double *in, double *out);
extern void destroy_rnnr (RNNR a);
extern void flush_rnnr (RNNR a);
extern void setBuffers_rnnr (RNNR a, double *in, double *out);
extern void setSize_rnnr (RNNR a, int size);
extern void setSamplerate_rnnr (RNNR a, int rate);
extern void xrnnr (RNNR a, int pos);

extern void SetRXARNNRRun (int channel, int run);

#endif //_rnnr_h
