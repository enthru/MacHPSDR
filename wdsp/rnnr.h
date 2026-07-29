/*  rnnr.h

    RNNoise-based receive noise reduction (NR3) for WDSP.

    This wraps Xiph's RNNoise (BSD-3, vendored under rnnoise/) as a WDSP RXA
    block, alongside anf/anr/emnr.  RNNoise processes fixed 480-sample frames
    at 48 kHz; the WDSP DSP block (ch[channel].dsp_size) is 2048 or 5120 and is
    not a multiple of 480, so this block does its own per-instance ring
    buffering (state lives in the struct, NOT in file-static buffers like the
    reference port) so it is both correct for any dsp_size and safe with
    multiple receivers.

    This file is part of the MacHPSDR fork; RNNoise itself is unmodified.
*/

#ifndef _rnnr_h
#define _rnnr_h

#include "rnnoise.h"

typedef struct _rnnr
{
	int run;			// 0/1 enable
	int position;			// pre-AGC (0) or post-AGC (1) slot
	int channel;			// owning RXA channel
	int size;			// dsp block size (complex samples per xrnnr)
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

extern RNNR create_rnnr (int channel, int run, int position, int size, double *in, double *out);
extern void destroy_rnnr (RNNR a);
extern void flush_rnnr (RNNR a);
extern void setBuffers_rnnr (RNNR a, double *in, double *out);
extern void xrnnr (RNNR a, int pos);

extern void SetRXARNNRRun (int channel, int run);

#endif //_rnnr_h
