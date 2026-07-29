/*  sbnr.h

    libspecbleach-based spectral receive noise reduction (NR4) for WDSP.

    Wraps Luciano Dato's libspecbleach adaptive denoiser (LGPL-2.1+, vendored
    under libspecbleach/) as a WDSP RXA block.  libspecbleach does its own STFT
    buffering, so this block only feeds it the DSP block (any dsp_size) and
    copies the real result back.  Scratch buffers are sized to the block, not a
    fixed stack array, so a 5120-sample block is safe.

    This file is part of the MacHPSDR fork; libspecbleach itself is unmodified.
*/

#ifndef _sbnr_h
#define _sbnr_h

#include <specbleach_adenoiser.h>

typedef struct _sbnr
{
	int run;
	int position;
	int channel;
	int size;			// dsp block size (complex samples per xsbnr)
	double *in;
	double *out;
	float reduction_amount;		// 0..20 dB
	float smoothing_factor;		// 0..100 %
	float whitening_factor;		// 0..100 %
	float noise_rescale;		// 0..12 dB
	float post_filter_threshold;	// -10..+10 dB
	float *fin;			// size scratch (real in)
	float *fout;			// size scratch (real out)
	SpectralBleachHandle st;
} sbnr, *SBNR;

extern SBNR create_sbnr (int channel, int run, int position, int size, double *in, double *out);
extern void destroy_sbnr (SBNR a);
extern void flush_sbnr (SBNR a);
extern void setBuffers_sbnr (SBNR a, double *in, double *out);
extern void xsbnr (SBNR a, int pos);

extern void SetRXASBNRRun (int channel, int run);
extern void SetRXASBNRreductionAmount (int channel, float amount);
extern void SetRXASBNRsmoothingFactor (int channel, float factor);
extern void SetRXASBNRwhiteningFactor (int channel, float factor);
extern void SetRXASBNRnoiseRescale (int channel, float factor);
extern void SetRXASBNRpostFilterThreshold (int channel, float threshold);

#endif // _sbnr_h
