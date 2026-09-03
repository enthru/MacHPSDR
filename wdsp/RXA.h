/*  RXA.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2014, 2015, 2016 Warren Pratt, NR0V

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

The author can be reached by email at  

warren@wpratt.com

*/

#ifndef _rxa_h
#define _rxa_h
#include "comm.h"

enum rxaMode
{
	RXA_LSB,
	RXA_USB,
	RXA_DSB,
	RXA_CWL,
	RXA_CWU,
	RXA_FM,
	RXA_AM,
	RXA_DIGU,
	RXA_SPEC,
	RXA_DIGL,
	RXA_SAM,
	RXA_DRM,
	RXA_WFM
};

enum rxaMeterType
{
	RXA_S_PK,
	RXA_S_AV,
	RXA_ADC_PK,
	RXA_ADC_AV,
	RXA_AGC_GAIN,
	RXA_AGC_PK,
	RXA_AGC_AV,
	RXA_METERTYPE_LAST
};

struct _rxa
{
	double* inbuff;
	double* outbuff;
	double* midbuff;
	// The stream tap.  xrxa() forks the signal into this ring the moment it is
	// demodulated -- before SNB, the EQ, ANF/ANR/EMNR/NR3/NR4 and the AGC -- and
	// filters the copy with bp_tap below.  That is what a consumer which is not
	// the operator's speaker wants: the signal, with none of the listening
	// chain, and without taking any of it away from the operator.
	// NULL means untapped and no copy is made.  The ring is written by the DSP
	// thread and read by whoever set it, using pretap_w as the sequence number.
	double* pretap;         // ring of pretap_cap complex samples, or NULL
	int     pretap_cap;     // capacity in complex samples
	long    pretap_w;       // total complex samples ever written (monotonic)
	double* tapbuff;        // the tap's own working buffer (dsp_size complex)
	int mode;
	double meter[RXA_METERTYPE_LAST];
	CRITICAL_SECTION* pmtupdate[RXA_METERTYPE_LAST];
	struct
	{
		METER p;
	} smeter, adcmeter, agcmeter;
	struct
	{
		SHIFT p;
	} shift;
	struct
	{
		RESAMPLE p;
	} rsmpin, rsmpout;
	struct
	{
		GEN p;
	} gen0;
	struct
	{
		BANDPASS p;
	} bp1;
	// The tap's own passband filter. bp1 cannot be shared: it is one object at a
	// fixed position whose gain compensates for the NR blocks ahead of it having
	// made the signal real, and the tap is taken BEFORE those. Same coefficients,
	// its own overlap state, gain 1.0.
	struct
	{
		BANDPASS p;
	} bp_tap;
	struct
	{
		NOTCHDB p;
	} ndb;
	struct
	{
		NBP p;
	} nbp0;
	struct
	{
		BPSNBA p;
	} bpsnba;
	struct
	{
		SNBA p;
	} snba;
	struct
	{
		SENDER p;
	} sender;
	struct
	{
		AMSQ p;
	} amsq;
	struct
	{
		AMD p;
	} amd;
	struct
	{
		FMD p;
	} fmd;
	struct
	{
		WFMD p;
	} wfmd;
	struct
	{
		FMSQ p;
	} fmsq;
	struct
	{
		EQP p;
	} eqp;
	struct
	{
		ANF p;
	} anf;
	struct
	{
		ANR p;
	} anr;
	struct
	{
		EMNR p;
	} emnr;
	struct
	{
		RNNR p;
	} rnnr;
	struct
	{
		SBNR p;
	} sbnr;
	struct
	{
		WCPAGC p;
	} agc;
	struct
	{
		SPEAK p;
	} speak;
	struct
	{
		MPEAK p;
	} mpeak;
	struct
	{
		PANEL p;
	} panel;
	struct
	{
		SIPHON p;
	} sip1;
	struct
	{
		CBL p;
	} cbl;

};

extern struct _rxa rxa[];

extern void create_rxa (int channel);

extern void destroy_rxa (int channel);

extern void flush_rxa (int channel);

extern void xrxa (int channel);

extern void setInputSamplerate_rxa (int channel);

extern void setOutputSamplerate_rxa (int channel);

extern void setDSPSamplerate_rxa (int channel);

extern void setDSPBuffsize_rxa (int channel);

// RXA Properties

extern __declspec (dllexport) void SetRXAMode (int channel, int mode);

extern void RXAResCheck (int channel);

extern void RXAbp1Check (int channel, int amd_run, int snba_run, int emnr_run, int anf_run, int anr_run, int rnnr_run, int sbnr_run);

extern void RXAbp1Set (int channel);

extern void RXAbpsnbaCheck (int channel, int mode, int notch_run);

extern void RXAbpsnbaSet (int channel);

// The stream tap.  `ring` holds `cap` complex samples and must stay allocated until
// the channel is closed or the tap is cleared with a NULL ring; clearing is
// enough to stop the copy, but the memory may only be freed once nothing can be
// inside xrxa() with the old pointer (in practice: after CloseChannel).
extern __declspec (dllexport) void SetRXAPreAgcTap (int channel, double* ring, int cap);

// Total complex samples ever written to that ring.  A reader keeps its own read
// position and takes the difference; more than `cap` behind means it lost data.
extern __declspec (dllexport) long GetRXAPreAgcTapPos (int channel);

#endif
