/*  rds.c

RDS (Radio Data System) decoder for WDSP broadcast-FM demodulation.  See rds.h
for the signal chain.  This is the streaming (one sample at a time) counterpart
of an offline prototype that was validated on a real 250 kHz FM recording
(decodes PI and the PS station name, including scrolling/dynamic PS).

This file is part of a program that implements a Software-Defined Radio.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "comm.h"
#include "rds.h"

/********************************************************************************************************
*  RDS block code: shortened cyclic (26,16), g(x)=x10+x8+x7+x5+x4+x3+1 (=0x5B9).                        *
*  A block of type X is error-free iff syndrome(block XOR offset_X) == 0.                                *
********************************************************************************************************/

enum { OA=0, OB, OC, OCp, OD, ONONE };
static const unsigned int OFFW[5] = { 0x0FC, 0x198, 0x168, 0x350, 0x1B4 }; // A,B,C,C',D

static unsigned int rds_syndrome (unsigned int bits)	// bits: 26-bit block
{
	unsigned int reg = 0;
	int i;
	for (i = 25; i >= 0; i--) { reg = (reg << 1) | ((bits >> i) & 1); if (reg & 0x400) reg ^= 0x5B9; }
	return reg & 0x3FF;
}

// syndrome -> error pattern for correctable bursts (length <= 5). Built once.
static unsigned int errtab[1024];
static int errtab_ready = 0;
static void build_errtab (void)
{
	int p; unsigned int mask, s;
	for (s = 0; s < 1024; s++) errtab[s] = 0xFFFFFFFF;
	for (p = 0; p < 26; p++) for (mask = 1; mask < 32; mask++) {
		unsigned int ep = mask << p; if (ep >= (1u << 26)) continue;
		s = rds_syndrome (ep); if (errtab[s] == 0xFFFFFFFF) errtab[s] = ep;
	}
	errtab_ready = 1;
}

// validate/correct a 26-bit block against expected offset; set *info = 16 info bits.
// returns 2 = clean (no error), 1 = corrected, 0 = uncorrectable.
static int rds_chk (unsigned int block, int off, unsigned int* info)
{
	unsigned int w = block ^ OFFW[off];
	unsigned int s = rds_syndrome (w);
	if (s == 0) { *info = (w >> 10) & 0xFFFF; return 2; }
	{ unsigned int ep = errtab[s]; if (ep == 0xFFFFFFFF) return 0; w ^= ep; }
	*info = (w >> 10) & 0xFFFF; return 1;
}

// which block type (offset) makes this 26-bit word error-free, if any
static int rds_match (unsigned int block)
{
	int k; for (k = 0; k < 5; k++) if (rds_syndrome (block ^ OFFW[k]) == 0) return k;
	return ONONE;
}
// map an offset id to its 0..3 position within a group (C and C' share position 2)
static int off_pos (int o) { return (o == OD) ? 3 : (o == OC || o == OCp) ? 2 : o; }

/********************************************************************************************************
*  rate-dependent setup                                                                                 *
********************************************************************************************************/

static void rds_calc (RDS a)
{
	// 2.4 kHz RBJ low-pass biquad (Q = 0.707), applied twice (I and Q paths)
	double w0 = TWOPI * 2400.0 / a->rate, cw = cos (w0), sw = sin (w0);
	double alpha = sw / (2.0 * 0.7071), a0 = 1.0 + alpha;
	a->lp_b0 = (1.0 - cw) / 2.0 / a0;
	a->lp_b1 = (1.0 - cw) / a0;
	a->lp_b2 = a->lp_b0;
	a->lp_a1 = (-2.0 * cw) / a0;
	a->lp_a2 = (1.0 - alpha) / a0;
	// Costas loop gains (fn = 25 Hz, zeta = 0.707)
	{ double th = TWOPI * 25.0 / a->rate; a->co_a = 2.0 * 0.7071 * th; a->co_b = th * th; }
	// symbol timing
	a->mm_period = a->rate / 1187.5;
	a->mm_a = 0.02;			// proportional (phase) gain
	a->mm_b = 2.0e-4;		// integral (period) gain
	a->L  = (int) (a->rate / 1187.5 + 0.5);
	a->Lh = a->L / 2;
}

/********************************************************************************************************
*  block-sync / group decode                                                                            *
********************************************************************************************************/

static char ps_printable (int c) { return (c >= 32 && c < 127) ? (char) c : ' '; }

// store one RadioText character; 0x0D (carriage return) marks the message end.
static void rds_rt_char (RDS a, int pos, int raw)
{
	if (pos < 0 || pos >= 64) return;
	if (raw == 0x0D) { if (a->rt_len > pos) a->rt_len = pos; a->rt_work[pos] = ' '; }
	else             { a->rt_work[pos] = ps_printable (raw); }
}

// Decode group 4A (clock-time and date) into local wall-clock time.
// Layout: MJD(17) = B[1:0]<<15 | C[15:1]; hour(5) = C[0]<<4 | D[15:12];
// minute(6) = D[11:6]; local-offset sense = D[5]; offset(5) in half-hours = D[4:0].
static void rds_decode_ct (RDS a)
{
	unsigned int B = a->ginfo[1], C = a->ginfo[2], D = a->ginfo[3];
	long mjd   = ((long)(B & 0x3) << 15) | ((C >> 1) & 0x7FFF);
	int  hour  = (int)(((C & 0x1) << 4) | ((D >> 12) & 0xF));
	int  minute= (int)((D >> 6) & 0x3F);
	int  sense = (int)((D >> 5) & 0x1);			// 1 = offset is negative
	int  off   = (int)(D & 0x1F);				// units of half an hour

	if (mjd < 40000 || mjd > 80000 || hour > 23 || minute > 59) return;	// implausible

	// apply the local-time offset (in minutes), rolling the day if it wraps
	long tmin = hour * 60 + minute + (sense ? -1 : 1) * off * 30;
	while (tmin < 0)    { tmin += 1440; mjd--; }
	while (tmin >= 1440){ tmin -= 1440; mjd++; }
	hour = (int)(tmin / 60); minute = (int)(tmin % 60);

	// MJD -> Gregorian date (RDS/EN 50067 Annex G algorithm)
	int yp = (int)((mjd - 15078.2) / 365.25);
	int mp = (int)((mjd - 14956.1 - (long)(yp * 365.25)) / 30.6001);
	int day= (int)(mjd - 14956 - (long)(yp * 365.25) - (long)(mp * 30.6001));
	int k  = (mp == 14 || mp == 15) ? 1 : 0;

	a->ct_year  = yp + k + 1900;
	a->ct_month = mp - 1 - k * 12;
	a->ct_day   = day;
	a->ct_hour  = hour;
	a->ct_minute= minute;
	a->ct_valid = 1;
}

// Collect one Alternative-Frequency code (group 0A block C, two per group).
// Only Band-II FM codes (1..204 -> 87.5 MHz + code*0.1) are frequencies; the
// count/filler codes (>=205) are ignored. Frequencies are kept as 0.1-MHz units.
static void rds_af_add (RDS a, int code)
{
	int mhz10, i;
	if (code < 1 || code > 204) return;
	mhz10 = 875 + code;
	for (i = 0; i < a->af_count; i++) if (a->af_mhz10[i] == mhz10) return;	// dedupe
	if (a->af_count < (int)(sizeof(a->af_mhz10)/sizeof(a->af_mhz10[0])))
		a->af_mhz10[a->af_count++] = mhz10;
}

// Apply one RadioText+ tag: a (content-type, start, length) triple that points
// at a slice of the current RadioText. We keep the two classes that make the
// "now playing" line: ITEM.TITLE (1) and ITEM.ARTIST (4).
static void rds_rtplus_tag (RDS a, int ct, int start, int len)
{
	char *dst = (ct == 1) ? a->rtp_title : (ct == 4) ? a->rtp_artist : NULL;
	// length field maps to 1..N characters (value+1), which for the 6-bit tag-1
	// field spans 1..64 == the RadioText length; skip empty/dummy tags.
	int n = len + 1;
	if (dst == NULL || !a->rt_valid) return;
	if (start < 0 || start >= 64) return;
	if (start + n > 64) n = 64 - start;
	if (n <= 0) return;
	memcpy (dst, a->rt + start, n);
	dst[n] = 0;
	a->rtp_valid = 1;
}

// Decode a RadioText+ data group (the group type the station announced via 3A).
// The 37-bit payload carries an item-toggle bit plus two tags.
static void rds_decode_rtplus (RDS a)
{
	unsigned int B = a->ginfo[1], C = a->ginfo[2], D = a->ginfo[3];
	int toggle = (B >> 4) & 1;
	int ct1 = (int)(((B & 0x7) << 3) | ((C >> 13) & 0x7));	// content type 1 (6 bits)
	int st1 = (int)((C >> 7) & 0x3F);						// start 1 (6 bits)
	int ln1 = (int)((C >> 1) & 0x3F);						// length 1 (6 bits)
	int ct2 = (int)(((C & 0x1) << 5) | ((D >> 11) & 0x1F));	// content type 2 (6 bits)
	int st2 = (int)((D >> 5) & 0x3F);						// start 2 (6 bits)
	int ln2 = (int)(D & 0x1F);								// length 2 (5 bits)
	if (a->rtp_have_toggle && toggle != a->rtp_toggle) {	// new item -> clear
		a->rtp_title[0] = 0; a->rtp_artist[0] = 0; a->rtp_valid = 0;
	}
	a->rtp_toggle = toggle; a->rtp_have_toggle = 1;
	rds_rtplus_tag (a, ct1, st1, ln1);
	rds_rtplus_tag (a, ct2, st2, ln2);
}

static void rds_decode_group (RDS a)
{
	a->groups++;
	// Publish only from CLEAN (zero-error) blocks: gvalid holds the checker result
	// (2 = clean, 1 = corrected, 0 = bad). Corrected blocks can be miscorrections
	// (real errors exceeding the burst capacity), so they hold sync but do not feed
	// the displayed PI/PS.
	if (a->gvalid[0] == 2) { a->pi = a->ginfo[0]; a->pi_valid = 1; }
	if (a->gvalid[1] == 2) {
		int gt  = (a->ginfo[1] >> 12) & 0xF;		// group type number
		int ver = (a->ginfo[1] >> 11) & 1;			// version: 0 = A, 1 = B
		int gcode = (gt << 1) | ver;				// group-type code as used by ODA
		// TP (Traffic Programme) and PTY (Programme Type) live in every block B.
		a->tp  = (a->ginfo[1] >> 10) & 1;
		a->pty = (a->ginfo[1] >>  5) & 0x1F;
		a->flags_valid = 1;
		if (gt == 0) {								// group 0A/0B -> PS name, flags, AF
			a->ta = (a->ginfo[1] >> 4) & 1;			// Traffic Announcement
			a->ms = (a->ginfo[1] >> 3) & 1;			// Music (1) / Speech (0)
			{	// Decoder Identification: 1 bit per group, addressed by seg 0..3
				int addr = a->ginfo[1] & 0x3, dibit = (a->ginfo[1] >> 2) & 1;
				a->di = (a->di & ~(1 << (3 - addr))) | (dibit << (3 - addr));
			}
			a->msdi_valid = 1;
			if (!ver && a->gvalid[2] == 2) {		// 0A: block C carries two AF codes
				rds_af_add (a, (a->ginfo[2] >> 8) & 0xFF);
				rds_af_add (a,  a->ginfo[2]       & 0xFF);
			}
			if (a->gvalid[3] == 2) {
				int seg = a->ginfo[1] & 0x3;
				a->ps_work[seg * 2 + 0] = ps_printable ((a->ginfo[3] >> 8) & 0xFF);
				a->ps_work[seg * 2 + 1] = ps_printable ( a->ginfo[3]       & 0xFF);
				a->ps_seg_ok |= (1 << seg);
				if (a->ps_seg_ok == 0xF) {			// a full 8-char frame assembled
					memcpy (a->ps, a->ps_work, 9);
					a->ps_valid = 1;
					a->ps_seg_ok = 0;				// require a fresh set (handles scrolling PS)
				}
			}
		} else if (gt == 2) {						// group 2A/2B -> RadioText
			int ab   = (a->ginfo[1] >>  4) & 1;		// Text A/B flag
			int addr =  a->ginfo[1]        & 0xF;	// text segment address (0..15)
			int cps  = ver ? 2 : 4;					// characters carried per group
			if (a->rt_have_ab && ab != a->rt_ab) {	// flag toggled -> start a new message
				memset (a->rt_work, ' ', 64); a->rt_seg_ok = 0; a->rt_len = 64;
			}
			a->rt_ab = ab; a->rt_have_ab = 1;
			if (!ver) {								// 2A: 4 chars in blocks C and D
				if (a->gvalid[2] == 2 && a->gvalid[3] == 2) {
					rds_rt_char (a, addr * 4 + 0, (a->ginfo[2] >> 8) & 0xFF);
					rds_rt_char (a, addr * 4 + 1,  a->ginfo[2]       & 0xFF);
					rds_rt_char (a, addr * 4 + 2, (a->ginfo[3] >> 8) & 0xFF);
					rds_rt_char (a, addr * 4 + 3,  a->ginfo[3]       & 0xFF);
					a->rt_seg_ok |= (1u << addr);
				}
			} else {								// 2B: 2 chars in block D (C repeats the PI)
				if (a->gvalid[3] == 2) {
					rds_rt_char (a, addr * 2 + 0, (a->ginfo[3] >> 8) & 0xFF);
					rds_rt_char (a, addr * 2 + 1,  a->ginfo[3]       & 0xFF);
					a->rt_seg_ok |= (1u << addr);
				}
			}
			{	// publish once every segment covering [0, len) has been received
				int maxlen = ver ? 32 : 64;
				int len = (a->rt_len < maxlen) ? a->rt_len : maxlen;
				int nseg = (len + cps - 1) / cps;
				unsigned int need = (nseg <= 0) ? 0 : ((1u << nseg) - 1);
				if ((a->rt_seg_ok & need) == need) {
					memcpy (a->rt, a->rt_work, 64);
					a->rt[len] = 0;
					a->rt_valid = 1;
				}
			}
		} else if (gt == 1) {						// group 1A -> slow-labelling codes
			// 1A block C, variant (bits 14-12) == 0 carries the Extended Country
			// Code in its low byte; combined with PI's country nibble = country.
			if (!ver && a->gvalid[2] == 2 && ((a->ginfo[2] >> 12) & 0x7) == 0) {
				a->ecc = a->ginfo[2] & 0xFF; a->ecc_valid = 1;
			}
		} else if (gt == 3) {						// group 3A -> ODA announcement
			// Block D is the Application ID; RadioText+ is 0x4BD7. Block B's low
			// 5 bits name the group type that will carry the application's data.
			if (!ver && a->gvalid[3] == 2 && a->ginfo[3] == 0x4BD7)
				a->rtp_app_group = a->ginfo[1] & 0x1F;
		} else if (gt == 4) {						// group 4A -> clock-time and date
			if (!ver && a->gvalid[2] == 2 && a->gvalid[3] == 2) rds_decode_ct (a);
		}

		// RadioText+ data arrives in whatever group the station announced via 3A.
		if (a->rtp_app_group >= 0 && gcode == a->rtp_app_group &&
		    a->gvalid[2] == 2 && a->gvalid[3] == 2)
			rds_decode_rtplus (a);
	}
}

// validate the just-completed 26-bit block (a->reg) at group position a->blk_idx
static void rds_process_block (RDS a)
{
	int idx = a->blk_idx, good = 0; unsigned int info = 0;
	if (idx == 2) {									// block C: version A uses C, B uses C'
		unsigned int i1, i2; int g1 = rds_chk (a->reg, OC, &i1), g2 = rds_chk (a->reg, OCp, &i2);
		if (g1) { good = g1; info = i1; } else if (g2) { good = g2; info = i2; }
	} else {
		int off = (idx == 0) ? OA : (idx == 1) ? OB : OD;
		good = rds_chk (a->reg, off, &info);
	}
	a->ginfo[idx] = good ? info : 0;
	a->gvalid[idx] = good;							// 2 = clean, 1 = corrected, 0 = bad
	if (good) a->badrun = 0; else a->badrun++;
	if (idx == 3) rds_decode_group (a);				// D completes the group
	a->blk_idx = (idx + 1) & 3;
	if (a->badrun >= 6) a->synced = 0;				// lost sync -> re-acquire
}

static void rds_bit (RDS a, int bit)
{
	a->reg = ((a->reg << 1) | (bit & 1)) & 0x3FFFFFF;
	if (!a->synced) {
		int o = rds_match (a->reg);
		if (o != ONONE) {							// found a block boundary -> acquire
			a->synced = 1; a->badrun = 0;
			a->blk_idx = off_pos (o);
			rds_process_block (a);					// this reg IS that block
			a->blk_nbit = 0;
		}
		return;
	}
	if (++a->blk_nbit >= 26) { a->blk_nbit = 0; rds_process_block (a); }
}

/********************************************************************************************************
*  lifecycle                                                                                            *
********************************************************************************************************/

RDS create_rds (int rate)
{
	RDS a = (RDS) malloc0 (sizeof (rds));
	if (!errtab_ready) build_errtab ();
	a->rate = (double) rate;
	rds_calc (a);
	a->ring = (double *) malloc0 (a->L * sizeof (double));
	flush_rds (a);
	return a;
}

void destroy_rds (RDS a)
{
	_aligned_free (a->ring);
	_aligned_free (a);
}

void flush_rds (RDS a)
{
	int s;
	for (s = 0; s < 2; s++) { a->lpi_x1[s]=a->lpi_x2[s]=a->lpi_y1[s]=a->lpi_y2[s]=0.0;
	                          a->lpq_x1[s]=a->lpq_x2[s]=a->lpq_y1[s]=a->lpq_y2[s]=0.0; }
	a->co_env = 1e-9; a->co_cc = 1.0; a->co_ss = 0.0; a->co_freq = 0.0; a->co_norm = 0;
	if (a->ring) memset (a->ring, 0, a->L * sizeof (double));
	a->ring_pos = 0; a->sum1 = a->sum2 = 0.0;
	a->mm_acc = 0.0; a->mm_period = a->rate / 1187.5;
	a->mm_prev_mf = a->mm_prev_y = a->mm_prev_dec = 0.0; a->mm_have = 0;
	a->prev_sym = 0; a->have_sym = 0;
	a->reg = 0; a->synced = 0; a->blk_idx = 0; a->blk_nbit = 0; a->badrun = 0;
	memset (a->ginfo, 0, sizeof (a->ginfo)); memset (a->gvalid, 0, sizeof (a->gvalid));
	a->pi = 0; a->pi_valid = 0; a->ps_valid = 0; a->ps_seg_ok = 0; a->groups = 0;
	memset (a->ps, ' ', 8); a->ps[8] = 0;
	memset (a->ps_work, ' ', 8); a->ps_work[8] = 0;
	a->rt_valid = 0; a->rt_ab = 0; a->rt_have_ab = 0; a->rt_seg_ok = 0; a->rt_len = 64;
	memset (a->rt, ' ', 64); a->rt[64] = 0;
	memset (a->rt_work, ' ', 64); a->rt_work[64] = 0;
	a->pty = 0; a->tp = 0; a->ta = 0; a->flags_valid = 0;
	a->ct_valid = 0; a->ct_year = a->ct_month = a->ct_day = a->ct_hour = a->ct_minute = 0;
	a->ms = 0; a->di = 0; a->msdi_valid = 0; a->ecc = 0; a->ecc_valid = 0;
	a->af_count = 0; memset (a->af_mhz10, 0, sizeof (a->af_mhz10));
	a->rtp_app_group = -1; a->rtp_toggle = 0; a->rtp_have_toggle = 0; a->rtp_valid = 0;
	a->rtp_title[0] = 0; a->rtp_artist[0] = 0;
}

void setSize_rds (RDS a, int rate)
{
	a->rate = (double) rate;
	_aligned_free (a->ring);
	rds_calc (a);
	a->ring = (double *) malloc0 (a->L * sizeof (double));
	flush_rds (a);
}

/********************************************************************************************************
*  per-sample processing                                                                                *
********************************************************************************************************/

static double biquad (double x, double b0, double b1, double b2, double a1, double a2,
                      double* x1, double* x2, double* y1, double* y2)
{
	double y = b0 * x + b1 * (*x1) + b2 * (*x2) - a1 * (*y1) - a2 * (*y2);
	*x2 = *x1; *x1 = x; *y2 = *y1; *y1 = y;
	return y;
}

void xrds (RDS a, double mpx, double pc, double ps)
{
	double c3, s3, bI, bQ, I, Q, mag, rI, rQ, e, dph, cc, mf, x, out_old, out_mid;

	// 1) 57 kHz coherent demod: reference = 3rd harmonic of the pilot phase
	c3 = 4.0 * pc * pc * pc - 3.0 * pc;			// cos(3*phi)
	s3 = 3.0 * ps - 4.0 * ps * ps * ps;			// sin(3*phi)
	bI = mpx * c3;
	bQ = -mpx * s3;

	// 2) 2.4 kHz low-pass (two cascaded biquads) on each of I, Q
	I = biquad (bI, a->lp_b0, a->lp_b1, a->lp_b2, a->lp_a1, a->lp_a2,
	            &a->lpi_x1[0], &a->lpi_x2[0], &a->lpi_y1[0], &a->lpi_y2[0]);
	I = biquad (I,  a->lp_b0, a->lp_b1, a->lp_b2, a->lp_a1, a->lp_a2,
	            &a->lpi_x1[1], &a->lpi_x2[1], &a->lpi_y1[1], &a->lpi_y2[1]);
	Q = biquad (bQ, a->lp_b0, a->lp_b1, a->lp_b2, a->lp_a1, a->lp_a2,
	            &a->lpq_x1[0], &a->lpq_x2[0], &a->lpq_y1[0], &a->lpq_y2[0]);
	Q = biquad (Q,  a->lp_b0, a->lp_b1, a->lp_b2, a->lp_a1, a->lp_a2,
	            &a->lpq_x1[1], &a->lpq_x2[1], &a->lpq_y1[1], &a->lpq_y2[1]);

	// 3) AGC (normalise so the Costas/timing loop gains are level-independent)
	mag = fabs (I) + fabs (Q);
	a->co_env += 1e-3 * (mag - a->co_env);		// ~slow magnitude envelope
	if (a->co_env > 1e-12) { I /= (a->co_env * 1.4142); Q /= (a->co_env * 1.4142); }

	// 4) Costas loop (trig-free phasor rotation) to track residual carrier phase
	cc = a->co_cc;
	rI =  I * cc + Q * a->co_ss;
	rQ = -I * a->co_ss + Q * cc;
	e  = (rI > 0.0 ? 1.0 : -1.0) * rQ;			// BPSK decision-directed phase detector
	dph = a->co_freq + a->co_a * e;
	a->co_freq += a->co_b * e;
	a->co_cc = cc - a->co_ss * dph;				// rotate by dph (small angle)
	a->co_ss = a->co_ss + cc * dph;
	if (++a->co_norm >= 1024) {					// renormalise the phasor periodically
		double n = sqrt (a->co_cc * a->co_cc + a->co_ss * a->co_ss);
		if (n > 1e-12) { a->co_cc /= n; a->co_ss /= n; }
		a->co_norm = 0;
	}

	// 5) biphase matched filter (boxcar +half/-half) via O(1) running sums
	x = rI;
	out_old = a->ring[a->ring_pos];							// oldest (leaving window)
	out_mid = a->ring[(a->ring_pos - a->Lh + a->L) % a->L];	// crossing sum1 -> sum2
	a->sum1 += x - out_mid;
	a->sum2 += out_mid - out_old;
	a->ring[a->ring_pos] = x;
	a->ring_pos = (a->ring_pos + 1) % a->L;
	mf = (a->sum1 - a->sum2) / a->Lh;

	// 6) Mueller & Muller symbol timing (huge oversampling -> linear interpolation)
	a->mm_acc += 1.0;
	if (a->mm_acc >= a->mm_period) {
		double frac = a->mm_acc - a->mm_period;				// 0..1 past the symbol instant
		double y = mf * (1.0 - frac) + a->mm_prev_mf * frac;
		double dec = y > 0.0 ? 1.0 : -1.0;
		a->mm_acc -= a->mm_period;
		if (a->mm_have) {
			double te = a->mm_prev_dec * y - dec * a->mm_prev_y;	// M&M timing error
			a->mm_period += a->mm_b * te;
			a->mm_acc    += a->mm_a * te;
		}
		a->mm_prev_y = y; a->mm_prev_dec = dec; a->mm_have = 1;
		{
			int sym = (y > 0.0) ? 1 : 0;
			if (a->have_sym) rds_bit (a, sym ^ a->prev_sym);	// differential decode
			a->prev_sym = sym; a->have_sym = 1;
		}
	}
	a->mm_prev_mf = mf;
}
