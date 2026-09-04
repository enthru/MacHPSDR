/* Copyright (C)
* 2026 - MacHPSDR contributors
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Boston, MA  02110-1301, USA.
*
*/

#include <math.h>
#include <stddef.h>

#include "dc_block.h"

void dc_block_init(DCBLOCK *d,int rate) {
  if(d==NULL) return;
  d->i=0.0;
  d->q=0.0;
  d->rate=rate;
  if(rate<=0) {
    /* Nothing to derive a corner from.  A zero coefficient is the identity,
       which is the right answer for "the rate is not known yet": a stream is
       never altered on a guess. */
    d->alpha=0.0;
    return;
  }
  /* Exact one-pole, not the 2*pi*f/fs small-angle form: the ratio spans a
     factor of fifty across the offered rates and there is no reason to take
     the approximation's error at the narrow end. */
  d->alpha=1.0-exp(-2.0*M_PI*DC_BLOCK_CORNER_HZ/(double)rate);
  if(d->alpha>1.0) d->alpha=1.0;
  if(d->alpha<0.0) d->alpha=0.0;
}

void dc_block_reset(DCBLOCK *d) {
  if(d==NULL) return;
  d->i=0.0;
  d->q=0.0;
}

void dc_block_run(DCBLOCK *d,float *iq,int samples) {
  if(d==NULL || iq==NULL || samples<=0) return;
  const double a=d->alpha;
  if(a<=0.0) return;                    /* not built for a rate: pass through */
  /* The state is held in a register for the block and written back once.  It
     is DOUBLE on purpose: the coefficient is 5e-5 at 2.3 MS/s and the recursion
     runs for the life of the stream, which is where a float accumulator's
     ulp starts to fight the update it is being given. */
  double mi=d->i;
  double mq=d->q;
  for(int n=0;n<samples;n++) {
    const double x=(double)iq[2*n];
    const double y=(double)iq[(2*n)+1];
    mi+=a*(x-mi);
    mq+=a*(y-mq);
    iq[2*n]    =(float)(x-mi);
    iq[(2*n)+1]=(float)(y-mq);
  }
  d->i=mi;
  d->q=mq;
}
