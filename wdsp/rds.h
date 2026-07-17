/*  rds.h

RDS (Radio Data System) decoder for WDSP broadcast-FM demodulation.

This file is part of a program that implements a Software-Defined Radio.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Streaming RDS decoder fed one MPX (FM multiplex) sample at a time together with
the 19 kHz stereo-pilot phase (cos/sin) recovered by the WFM demodulator.  The
57 kHz RDS subcarrier is the 3rd harmonic of the pilot, so the reference is
generated coherently from the pilot phase.  Chain:

    57 kHz coherent demod (ref = 3*pilot phase) -> 2.4 kHz biquad low-pass
    -> Costas carrier tracking (residual phase) -> biphase matched filter
    -> Mueller&Muller symbol timing (1187.5 bps) -> differential decode
    -> 26-bit block sync (syndrome + burst error correction) -> group decode

Decoded PI code, PS (programme service name, 8 chars) and RadioText (RT, up to
64 chars, group 2A/2B) are exposed via accessor fields.
*/

#ifndef _rds_h
#define _rds_h

typedef struct _rds
{
	double rate;						// input (MPX / dsp) sample rate
	// ---- 57 kHz coherent demod -> 2.4 kHz low-pass (2 cascaded biquads, I&Q) ----
	double lp_b0, lp_b1, lp_b2, lp_a1, lp_a2;		// shared biquad coefficients
	double lpi_x1[2], lpi_x2[2], lpi_y1[2], lpi_y2[2];	// I path, 2 stages
	double lpq_x1[2], lpq_x2[2], lpq_y1[2], lpq_y2[2];	// Q path, 2 stages
	// ---- AGC (level normalisation for level-independent loop gains) ----
	double co_env;						// running magnitude envelope
	// ---- Costas loop (trig-free rotating phasor) ----
	double co_cc, co_ss;				// cos/sin of carrier phase
	double co_freq;						// tracked residual frequency (rad/sample)
	double co_a, co_b;					// loop gains
	long   co_norm;						// renormalise phasor counter
	// ---- biphase matched filter (boxcar +half/-half, O(1) running sums) ----
	int    L, Lh;						// symbol length / half in input samples
	double* ring;						// delay line of length L (data samples)
	int    ring_pos;
	double sum1, sum2;					// running sums of the two half-windows
	// ---- Mueller&Muller symbol timing ----
	double mm_acc;						// sample accumulator toward next symbol
	double mm_period;					// samples per symbol (adaptive)
	double mm_a, mm_b;					// timing loop gains
	double mm_prev_mf;					// previous matched-filter sample (for interp)
	double mm_prev_y, mm_prev_dec;		// previous symbol / decision (M&M TED)
	int    mm_have;						// have a previous mf sample yet
	// ---- differential decode ----
	int    prev_sym;
	int    have_sym;
	// ---- block synchroniser ----
	unsigned int reg;					// 26-bit sliding block register
	int    synced;
	int    blk_idx;						// 0=A 1=B 2=C 3=D within group
	int    blk_nbit;					// bit counter within current block (0..25)
	int    badrun;						// consecutive bad blocks
	unsigned int ginfo[4];				// decoded 16-bit info of the 4 blocks
	int    gvalid[4];					// per-block validity in the current group
	// ---- decoded output ----
	unsigned int pi;					// programme identification (16-bit)
	int    pi_valid;
	char   ps[9];						// programme service name (8 chars + NUL)
	int    ps_valid;					// at least one full PS assembled
	char   ps_work[9];					// PS being assembled
	int    ps_seg_ok;					// bitmask of segments seen (bits 0..3)
	// ---- RadioText (group 2A/2B) ----
	char   rt[65];						// RadioText message (up to 64 chars + NUL)
	int    rt_valid;					// at least one full RadioText assembled
	char   rt_work[65];					// RadioText being assembled
	int    rt_ab, rt_have_ab;			// last Text A/B flag (a toggle = new message)
	unsigned int rt_seg_ok;				// bitmask of RT segments seen (addr 0..15)
	int    rt_len;						// current message length (lowered by 0x0D)
	// ---- programme flags (from block B / group 0) ----
	int    pty;							// Programme Type code (0..31)
	int    tp;							// Traffic Programme flag
	int    ta;							// Traffic Announcement flag (group 0 only)
	int    flags_valid;					// a clean block B has been seen
	// ---- clock-time / date (group 4A), converted to LOCAL time ----
	int    ct_valid;					// a plausible CT has been decoded
	int    ct_year, ct_month, ct_day;	// local calendar date
	int    ct_hour, ct_minute;			// local wall-clock time
	// ---- group-0 extras: Music/Speech, Decoder Info, Alternative Frequencies ----
	int    ms;							// 1 = Music, 0 = Speech
	int    di;							// decoder-info bits: b0 stereo .. b3 dynamic-PTY
	int    msdi_valid;					// a clean group 0 has been seen
	// ---- Extended Country Code (group 1A slow-labelling, variant 0) ----
	int    ecc;							// 8-bit ECC (with PI's country nibble = country)
	int    ecc_valid;
	int    af_mhz10[25];				// alternative frequencies, units of 0.1 MHz
	int    af_count;					// number of AFs collected
	// ---- RadioText+ (ODA, AID 0x4BD7): "now playing" title / artist ----
	int    rtp_app_group;				// announced data group-type code, -1 if none
	int    rtp_toggle, rtp_have_toggle;	// item-toggle tracking (flip = new item)
	char   rtp_title[65];
	char   rtp_artist[65];
	int    rtp_valid;					// a title or artist tag has been extracted
	long   groups;						// total valid groups decoded (stats)
} rds, *RDS;

extern RDS create_rds (int rate);
extern void destroy_rds (RDS a);
extern void flush_rds (RDS a);					// reset all decoder state
extern void setSize_rds (RDS a, int rate);		// re-rate (rebuild rate-dependent parts)
// process one MPX sample; pc/ps = cos/sin of the 19 kHz pilot phase
extern void xrds (RDS a, double mpx, double pilot_cos, double pilot_sin);

#endif
