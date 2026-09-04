/* Copyright (C)
* 2026 - MacHPSDR contributors
*
*   This program is free software; you can redistribute it and/or
*   modify it under the terms of the GNU General Public License
*   as published by the Free Software Foundation; either version 2
*   of the License, or (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 51 Franklin Street, Boston, MA  02110-1301, USA.
*
*/

#ifndef _DC_BLOCK_H
#define _DC_BLOCK_H

/* The line in the middle of the panadapter that every zero-IF front end draws.
   A direct-conversion receiver puts its own LO leakage, the ADC's offset and
   the mixer's self-mixing product at exactly 0 Hz of the baseband, which is the
   frequency the device is tuned to -- so on a HackRF or a PlutoSDR the centre
   of the span carries a carrier that is not on the band, tens of dB above the
   noise floor, and it moves with the dial because it IS the dial.

   Neither driver offers a correction of its own: SoapyPlutoSDR advertises no
   DC-offset mode at all (probed 2026-09-04 on an ADALM-PLUTO Rev.C -- the
   "Corrections" line SoapySDRUtil prints for a device that has one is absent),
   and SoapyHackRF is the same, so soapy_discovery.c's hasDCOffsetMode probe
   reads 0 on both.  That leaves the host, which is this.

   The estimator is a leaky mean per component, subtracted from the sample it
   was just updated with -- the standard one-pole DC blocker,
   H(z) = R(1 - z^-1)/(1 - R z^-1), a transmission ZERO at 0 Hz and a pole just
   inside it.  Two properties are what make it the right shape here:

   - the null at 0 Hz is EXACT, so the offset is removed whole rather than
     attenuated.  That matters more than the notch width: the spike is drawn
     wide only because an FFT leaks a strong DC component across its window,
     and a component that is gone leaks nothing;
   - it TRACKS.  LO leakage moves with temperature, gain and frequency, so a
     constant measured once at start-up would be wrong an hour later.

   The corner is stated in HERTZ and the coefficient derived from the sample
   rate, so the notch is the same width on a 192 kHz stream and on a 9.6 MHz
   one.  20 Hz settles in ~8 ms and is narrower than one bin of any panadapter
   this application can draw, so what the operator sees is the spike gone and
   not a dip where it was. */
#define DC_BLOCK_CORNER_HZ 20.0

typedef struct {
  double i;                     /* the tracked offset, per component */
  double q;
  double alpha;                 /* per-sample tracking coefficient */
  int rate;                     /* the rate alpha was derived from; 0 = uninitialised */
} DCBLOCK;

/* Build for this sample rate and clear the estimate.  Idempotent, and cheap
   enough to call per block: a caller that has to notice a rate change compares
   d->rate itself. */
extern void dc_block_init(DCBLOCK *d,int rate);

/* Forget the estimate, keep the coefficient.  For a discontinuity in the
   stream -- a restart, a retune, a dropped queue -- where the old offset is no
   longer the new one and re-converging from zero is quicker than walking. */
extern void dc_block_reset(DCBLOCK *d);

/* In place, over `samples` INTERLEAVED complex samples (2 floats each).
   The pair order does not matter: the two components are tracked separately,
   so this is one of the very few places in this tree where the (Q, I) order
   WDSP reads (see CLAUDE.md) is not a trap -- swapping them swaps which mean
   is subtracted from which sample, and both are subtracted. */
extern void dc_block_run(DCBLOCK *d,float *iq,int samples);

#endif
