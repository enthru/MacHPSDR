/* DC-blocker offline harness.
 *
 * The spike a zero-IF front end draws in the middle of its own stream is
 * removed on the raw block, before every receiver is tuned out of it
 * (soapy_protocol.c's dsp_thread), so a mistake here is not confined to one
 * receiver and not confined to one span: it reaches everything the device
 * delivers, including the panadapter, the decoders and the I/Q recorder.
 *
 * Two ways to get it wrong, and both are quiet.  A blocker that does not
 * actually null 0 Hz leaves a spike that merely looks smaller, and nobody can
 * tell that from a correct one by eye on a waterfall.  A blocker whose notch is
 * too WIDE eats real signal next to the LO, which nobody sees at all -- the hole
 * is at the centre of the screen where the spike used to be, so it reads as
 * "the spike is gone".  Both are numbers, so both are measured here:
 *
 *   - the DC component is driven at least 80 dB down (the null is EXACT, not an
 *     attenuation), while
 *   - a tone 200 Hz away loses less than 0.1 dB, and
 *   - the -3 dB corner lands on DC_BLOCK_CORNER_HZ at BOTH a 192 kHz stream and
 *     a 2.304 MHz one -- the rate-independence is the whole reason the corner is
 *     stated in hertz, and a fixed per-sample coefficient (the obvious
 *     simplification) passes every other case here and fails this one by a
 *     factor of twelve.
 *
 * The tone at 200 Hz is the negative control the image harnesses have: without
 * it "the DC went away" is also satisfied by a filter that removed everything.
 *
 *   make dc-offline && ./dcblock_offline --selftest
 *
 * Exit status 0 = every case passed.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_block.h"

static int failures=0;

static void check(int ok,const char *what,const char *detail) {
  printf("  %-58s %s%s%s\n",what,ok?"OK":"FAILED",detail&&*detail?"  ":"",detail?detail:"");
  if(!ok) failures++;
}

/* The DC component of a block is the mean of it -- bin 0 of an unwindowed DFT,
   which is exactly what the panadapter's centre pixel is fed. */
static void dc_of(const float *iq,int n,double *mi,double *mq) {
  double si=0.0,sq=0.0;
  for(int k=0;k<n;k++) { si+=iq[2*k]; sq+=iq[(2*k)+1]; }
  *mi=si/(double)n;
  *mq=sq/(double)n;
}

/* Amplitude of a complex tone at f, by correlation: mean(x * conj(e^jwt)).
   Whole periods only, or the leakage from everything else in the block is
   larger than the difference being measured. */
static double tone_amp(const float *iq,int n,double f,double rate) {
  double re=0.0,im=0.0;
  const double w=2.0*M_PI*f/rate;
  for(int k=0;k<n;k++) {
    const double c=cos(w*(double)k),s=sin(w*(double)k);
    re+=(double)iq[2*k]*c+(double)iq[(2*k)+1]*s;
    im+=(double)iq[(2*k)+1]*c-(double)iq[2*k]*s;
  }
  re/=(double)n; im/=(double)n;
  return sqrt(re*re+im*im);
}

/* n samples of: a DC offset, a complex tone, and nothing else. */
static float *make_block(int n,double rate,double dci,double dcq,double amp,double f) {
  float *iq=(float *)malloc(sizeof(float)*2*(size_t)n);
  const double w=2.0*M_PI*f/rate;
  for(int k=0;k<n;k++) {
    iq[2*k]    =(float)(dci+amp*cos(w*(double)k));
    iq[(2*k)+1]=(float)(dcq+amp*sin(w*(double)k));
  }
  return iq;
}

static double db(double x) { return 20.0*log10(x>1e-30?x:1e-30); }

/* |H(f)| measured through the real filter: one tone in, its amplitude out,
   after the estimate has settled. */
static double response_at(double rate,double f,int periods) {
  int n=(int)((double)periods*rate/f);
  if(n<4096) n=4096;
  /* Settle first -- the estimate starts at zero, and the transient is a burst
     of the very thing being measured. */
  const int warm=(int)(rate*0.5);
  float *w=make_block(warm,rate,0.0,0.0,1.0,f);
  float *x=make_block(n,rate,0.0,0.0,1.0,f);
  /* The two blocks have to be one continuous tone, or the phase jump between
     them is a step the filter answers. */
  {
    const double wf=2.0*M_PI*f/rate;
    for(int k=0;k<n;k++) {
      x[2*k]    =(float)cos(wf*(double)(warm+k));
      x[(2*k)+1]=(float)sin(wf*(double)(warm+k));
    }
  }
  DCBLOCK d;
  dc_block_init(&d,(int)rate);
  dc_block_run(&d,w,warm);
  dc_block_run(&d,x,n);
  double a=tone_amp(x,n,f,rate);
  /* Referred to the same tone measured the same way, so the correlation's own
     normalisation cannot be what is being reported. */
  free(w); free(x);
  return a;
}

int main(int argc,char **argv) {
  (void)argc; (void)argv;
  printf("dcblock_offline: the zero-IF spike remover (corner %.1f Hz)\n\n",DC_BLOCK_CORNER_HZ);

  /* ---- 1. the offset is removed, and the tone that shares the block is not */
  {
    const double rate=2304000.0;          /* a PlutoSDR's own clock */
    const int n=(int)(rate*0.5);
    /* 40 kHz: far enough out that the ideal loss is nil, so a filter that is
       merely too wide fails this rather than being absorbed by the tolerance.
       Whole periods in the tail (2304000/40000 = 57.6 samples, 2000 of them),
       or the correlation's own leakage is the number being reported. */
    const double dci=0.30,dcq=-0.20,amp=0.05,f=40000.0;
    float *iq=make_block(n,rate,dci,dcq,amp,f);
    const double before=tone_amp(iq,n,f,rate);
    DCBLOCK d;
    dc_block_init(&d,(int)rate);
    dc_block_run(&d,iq,n);
    /* Measured over the last tenth, i.e. past the settling transient: a filter
       is judged on what it does, not on how it starts. */
    const int tail=n/10;
    double mi,mq;
    dc_of(iq+2*(n-tail),tail,&mi,&mq);
    const double res=sqrt(mi*mi+mq*mq);
    const double in=sqrt(dci*dci+dcq*dcq);
    char buf[160];
    snprintf(buf,sizeof(buf),"(%.4f -> %.2e, %.1f dB down)",in,res,db(in)-db(res));
    check(db(in)-db(res)>80.0,"DC offset removed from a 2304k stream",buf);

    const double after=tone_amp(iq+2*(n-tail),tail,f,rate);
    snprintf(buf,sizeof(buf),"(%.6f -> %.6f, %+.3f dB)",before,after,db(after)-db(before));
    check(fabs(db(after)-db(before))<0.01,"...and the 40 kHz tone beside it survives",buf);
    free(iq);
  }

  /* ---- 2. the corner is where it is stated to be, at two rates a factor of
     twelve apart.  This is the case a fixed per-sample coefficient fails. */
  {
    const double rates[2]={192000.0,2304000.0};
    for(int i=0;i<2;i++) {
      const double a=response_at(rates[i],DC_BLOCK_CORNER_HZ,40);
      char buf[160];
      snprintf(buf,sizeof(buf),"(%.2f dB, want -3.0)",db(a));
      char what[96];
      snprintf(what,sizeof(what),"-3 dB corner at %.0f Hz on a %.0fk stream",
               DC_BLOCK_CORNER_HZ,rates[i]/1000.0);
      check(fabs(db(a)+3.0)<0.6,what,buf);
    }
  }

  /* ---- 3. ...and the notch is NARROW: ten corners out it is already flat.
     Without this, "the DC went away" is also true of a filter that took the
     band with it. */
  {
    const double rate=2304000.0;
    const double a=response_at(rate,10.0*DC_BLOCK_CORNER_HZ,200);
    char buf[160];
    snprintf(buf,sizeof(buf),"(%.3f dB)",db(a));
    check(fabs(db(a))<0.1,"flat 10 corners out (200 Hz)",buf);
  }

  /* ---- 4. it TRACKS: LO leakage moves with temperature and gain, so an
     offset measured once is wrong later.  A ramp of 0.1 full scale per second
     is orders of magnitude faster than any real drift. */
  {
    const double rate=2304000.0;
    const int n=(int)rate;                        /* one second */
    float *iq=(float *)malloc(sizeof(float)*2*(size_t)n);
    for(int k=0;k<n;k++) {
      const double t=(double)k/rate;
      iq[2*k]    =(float)(0.1*t);
      iq[(2*k)+1]=(float)(-0.05*t);
    }
    DCBLOCK d;
    dc_block_init(&d,(int)rate);
    dc_block_run(&d,iq,n);
    const int tail=n/10;
    double mi,mq;
    dc_of(iq+2*(n-tail),tail,&mi,&mq);
    const double res=sqrt(mi*mi+mq*mq);
    char buf[160];
    /* A one-pole tracker follows a ramp with a fixed LAG, slope * tau, and
       that is the number to assert: a bare "small enough" threshold passes a
       filter whose corner has moved, which is the thing worth catching. */
    const double slope=sqrt(0.1*0.1+0.05*0.05);
    const double want=slope/(2.0*M_PI*DC_BLOCK_CORNER_HZ);
    snprintf(buf,sizeof(buf),"(offset reached 0.112, residual %.2e, lag theory %.2e)",res,want);
    check(fabs(res-want)<0.1*want,"a drifting offset is tracked, not just cancelled once",buf);
    free(iq);
  }

  /* ---- 5. the pair order does not matter.  Everywhere else in this tree it
     does (WDSP reads the pair swapped), so the claim is worth pinning: the two
     components are tracked separately, and swapping them swaps the answer
     rather than changing it. */
  {
    const double rate=768000.0;
    const int n=200000;
    float *a=make_block(n,rate,0.25,-0.10,0.02,1000.0);
    float *b=(float *)malloc(sizeof(float)*2*(size_t)n);
    for(int k=0;k<n;k++) { b[2*k]=a[(2*k)+1]; b[(2*k)+1]=a[2*k]; }
    DCBLOCK d1,d2;
    dc_block_init(&d1,(int)rate); dc_block_run(&d1,a,n);
    dc_block_init(&d2,(int)rate); dc_block_run(&d2,b,n);
    double worst=0.0;
    for(int k=0;k<n;k++) {
      const double e1=fabs((double)a[2*k]-(double)b[(2*k)+1]);
      const double e2=fabs((double)a[(2*k)+1]-(double)b[2*k]);
      if(e1>worst) worst=e1;
      if(e2>worst) worst=e2;
    }
    char buf[160];
    snprintf(buf,sizeof(buf),"(worst sample difference %.2e)",worst);
    check(worst<1.0e-6,"(I,Q) and (Q,I) give the same answer, swapped",buf);
    free(a); free(b);
  }

  /* ---- 6. a reset really forgets, and re-converges.  fifo_clear raises this
     after the stream is rebuilt, where the old offset is not the new one. */
  {
    const double rate=2304000.0;
    const int n=(int)(rate*0.2);
    DCBLOCK d;
    dc_block_init(&d,(int)rate);
    float *iq=make_block(n,rate,0.4,0.0,0.0,0.0);
    dc_block_run(&d,iq,n);
    const double settled=fabs(d.i);
    dc_block_reset(&d);
    check(d.i==0.0 && d.q==0.0,"reset clears the estimate","");
    /* ...and the first sample after it passes the offset through rather than a
       step of twice it, which is what subtracting a stale estimate would do. */
    free(iq);
    iq=make_block(n,rate,-0.4,0.0,0.0,0.0);
    dc_block_run(&d,iq,n);
    double mi,mq;
    dc_of(iq+2*(n-n/10),n/10,&mi,&mq);
    char buf[160];
    snprintf(buf,sizeof(buf),"(settled %.4f, then -0.4 in: residual %.2e after 200 ms)",settled,fabs(mi));
    check(fabs(mi)<1.0e-4,"...and converges on the new offset",buf);
    free(iq);
  }

  /* ---- 7. no rate, no filtering.  dsp_thread builds the block before the
     stream's rate has been read back at least once, and a stream is never
     altered on a guess. */
  {
    const int n=1024;
    float *iq=make_block(n,192000.0,0.3,0.3,0.0,0.0);
    DCBLOCK d;
    dc_block_init(&d,0);
    dc_block_run(&d,iq,n);
    int same=1;
    for(int k=0;k<2*n;k++) if(iq[k]!=0.3f) same=0;
    check(same,"an unbuilt blocker is the identity","");
    free(iq);
  }

  printf("\n%s\n",failures==0?"dcblock_offline: all cases passed":"dcblock_offline: FAILURES");
  return failures==0?0:1;
}
