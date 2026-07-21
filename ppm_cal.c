/* Copyright (C)
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
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "mode.h"
#include "ppm_cal.h"

// Reference stations, ordered most-useful-first for a shortwave SDR.  The HF
// carrier standards (RWM/WWV/CHU/BPM) all radiate a strong continuous carrier
// that is disciplined to a national frequency standard, so a measured offset
// is pure receiver-oscillator error.  The LF/MF entries are only reachable by
// front-ends that tune that low.
static const PPM_STATION stations[] = {
  { "RWM 4.996 MHz (Russia)",   4996000LL },
  { "RWM 9.996 MHz (Russia)",   9996000LL },
  { "RWM 14.996 MHz (Russia)", 14996000LL },
  { "WWV 2.5 MHz (USA)",        2500000LL },
  { "WWV 5 MHz (USA)",          5000000LL },
  { "WWV 10 MHz (USA)",        10000000LL },
  { "WWV 15 MHz (USA)",        15000000LL },
  { "WWV 20 MHz (USA)",        20000000LL },
  { "CHU 3.330 MHz (Canada)",   3330000LL },
  { "CHU 7.850 MHz (Canada)",   7850000LL },
  { "CHU 14.670 MHz (Canada)", 14670000LL },
  { "BPM 5 MHz (China)",        5000000LL },
  { "BPM 10 MHz (China)",      10000000LL },
  { "BPM 15 MHz (China)",      15000000LL },
  { "MSF 60 kHz (UK)",            60000LL },
  { "DCF77 77.5 kHz (Germany)",   77500LL },
  { "Droitwich 198 kHz (UK)",    198000LL },
};

int ppm_station_count(void) {
  return (int)(sizeof(stations)/sizeof(stations[0]));
}

const PPM_STATION *ppm_station(int index) {
  if(index<0 || index>=ppm_station_count()) return NULL;
  return &stations[index];
}

void ppm_cal_tune_to_station(RADIO *r, int index) {
  const PPM_STATION *st=ppm_station(index);
  if(st==NULL) return;
  RECEIVER *rx=r->active_receiver;
  if(rx==NULL) return;

  r->ppm_ref_station=index;

  // Park the exact carrier at the VFO centre and go CW-USB so it beats down to
  // a clean tone at the sidetone offset (mirrors the bookmark-recall retune
  // path in bookmark_dialog.c).
  rx->frequency_a=st->freq;
  rx->ctun=0;
  rx->ctun_frequency=st->freq;
  receiver_mode_changed(rx,CWU);
  frequency_changed(rx);
}

// ---------------- Automatic carrier-offset measurement ----------------
//
// The measurement works on the raw off-air I/Q (independent of the demod mode),
// so it is unaffected by CW/SSB shift conventions.  The active RX is retuned so
// the reference carrier lands at a fixed audio offset (PPM_MEAS_OFFSET) — well
// clear of the DC-offset spike at 0 Hz — then a windowed FFT locates the carrier
// bin, parabolic interpolation refines it to a fraction of a bin, and the result
// is averaged over several frames to beat down ionospheric Doppler jitter.
//
// Closed-loop correction, derived from the mixing model (commanded frequency C
// is produced by the hardware at C·(1+e), e = fractional clock error):
//   measured carrier baseband  = PPM_MEAS_OFFSET − B·(p/1e6 + e),   B = carrier−lo
//   residual                   = measured − PPM_MEAS_OFFSET = −B·(p/1e6 + e)
//   for zero error we need p/1e6 = −e, i.e.
//   p_new = p_old + (residual / B)·1e6
// A positive residual (carrier sits above the intended offset) therefore raises
// the ppm value.  The sign follows the HPSDR non-inverted I/Q convention
// (iq[2k]=I, iq[2k+1]=Q, a signal above the LO at positive baseband frequency);
// re-running the measurement shows the residual collapsing toward 0 when it is
// right.

#define PPM_FFT_N        32768   // FFT size (power of two)
#define PPM_MEAS_OFFSET   1500.0 // Hz the carrier is parked at, away from DC
#define PPM_FRAMES        6      // frames averaged for the result
#define PPM_SETTLE        1      // frames discarded after retune (PLL/AGC settle)
#define PPM_DC_GUARD_HZ   300.0  // ignore |f| below this (I/Q DC-offset spike)
#define PPM_MIN_SNR       10.0   // peak/mean power ratio required to trust a frame

static GMutex ppm_mtx;
static volatile gint ppm_measuring;   // atomic fast-path flag for the tap

static RECEIVER *meas_rx;
static double    meas_base;           // carrier − lo_a (Hz), the ppm scaling base
static double    meas_ppm0;           // ppm value when the run started
static int       meas_fs;             // sample rate captured at start

static double fre[PPM_FFT_N];         // accumulation / FFT scratch (I in re, Q in im)
static double fim[PPM_FFT_N];
static int    acc_n;
static int    frames_done;
static int    valid_frames;
static int    settle_left;
static double offset_sum;

static gboolean have_result;
static gboolean result_ok;
static double   result_offset;        // residual carrier offset, Hz
static double   result_ppm;           // suggested new ppm
static char     status[160];

// In-place iterative radix-2 FFT (forward). Kept self-contained so ppm_cal has
// no dependency on the FT8/kiss or fftw builds.
static void fft_radix2(double *re, double *im, int n) {
  for(int i=1,j=0;i<n;i++) {
    int bit=n>>1;
    for(;j&bit;bit>>=1) j^=bit;
    j^=bit;
    if(i<j) {
      double t;
      t=re[i]; re[i]=re[j]; re[j]=t;
      t=im[i]; im[i]=im[j]; im[j]=t;
    }
  }
  for(int len=2;len<=n;len<<=1) {
    double ang=-2.0*M_PI/(double)len;
    double wr=cos(ang), wi=sin(ang);
    for(int i=0;i<n;i+=len) {
      double cwr=1.0, cwi=0.0;
      for(int k=0;k<len/2;k++) {
        int a=i+k, b=i+k+len/2;
        double vr=re[b]*cwr - im[b]*cwi;
        double vi=re[b]*cwi + im[b]*cwr;
        re[b]=re[a]-vr; im[b]=im[a]-vi;
        re[a]+=vr;      im[a]+=vi;
        double ncwr=cwr*wr - cwi*wi;
        cwi=cwr*wi + cwi*wr; cwr=ncwr;
      }
    }
  }
}

// Analyse one filled frame (destroys re/im). Returns the carrier's baseband
// offset in Hz and whether a carrier was found (peak clears the SNR gate).
static gboolean analyze_frame(double *re, double *im, int fs, double *off_hz) {
  const int N=PPM_FFT_N;
  for(int i=0;i<N;i++) {
    double w=0.5-0.5*cos(2.0*M_PI*(double)i/(double)(N-1));  // Hann
    re[i]*=w; im[i]*=w;
  }
  fft_radix2(re,im,N);

  double guard_bins=PPM_DC_GUARD_HZ*(double)N/(double)fs;
  double sum=0.0, peak=-1.0;
  int pk=-1;
  for(int m=0;m<N;m++) {
    double p=re[m]*re[m]+im[m]*im[m];
    sum+=p;
    double sf=(m<=N/2)?(double)m:(double)(m-N);
    if(fabs(sf)<guard_bins) continue;
    if(p>peak) { peak=p; pk=m; }
  }
  if(pk<1 || pk>=N-1) { *off_hz=0.0; return FALSE; }
  double mean=sum/(double)N;
  if(mean<=0.0 || peak/mean<PPM_MIN_SNR) { *off_hz=0.0; return FALSE; }

  double y0=sqrt(re[pk-1]*re[pk-1]+im[pk-1]*im[pk-1]);
  double y1=sqrt(peak);
  double y2=sqrt(re[pk+1]*re[pk+1]+im[pk+1]*im[pk+1]);
  double denom=y0-2.0*y1+y2;
  double delta=(denom!=0.0)?0.5*(y0-y2)/denom:0.0;
  if(delta> 0.5) delta= 0.5;
  if(delta<-0.5) delta=-0.5;
  double pos=(double)pk+delta;
  double sf=(pos<=N/2)?pos:pos-(double)N;
  *off_hz=sf*(double)fs/(double)N;
  return TRUE;
}

// Compute the averaged result and stop. Caller holds ppm_mtx.
static void finalize(void) {
  double avg=(valid_frames>0)?offset_sum/(double)valid_frames:0.0;
  double residual=avg-PPM_MEAS_OFFSET;
  gboolean ok=(valid_frames>=(PPM_FRAMES+1)/2);   // majority of frames saw a carrier

  double newppm=meas_ppm0;
  if(ok && meas_base>0.0) {
    newppm=meas_ppm0 + residual/meas_base*1.0e6;
    if(newppm> 500.0) newppm= 500.0;
    if(newppm<-500.0) newppm=-500.0;
    snprintf(status,sizeof(status),
             "Done: offset %+.1f Hz   ppm %+.3f \342\206\222 %+.3f",
             residual, meas_ppm0, newppm);
  } else {
    snprintf(status,sizeof(status),"No carrier detected (check station/band/signal)");
  }
  result_offset=residual;
  result_ppm=newppm;
  result_ok=ok;
  have_result=TRUE;
  g_atomic_int_set(&ppm_measuring,0);
}

gboolean ppm_cal_measuring(void) {
  return g_atomic_int_get(&ppm_measuring)!=0;
}

void ppm_cal_measure_cancel(void) {
  g_atomic_int_set(&ppm_measuring,0);
  g_mutex_lock(&ppm_mtx);
  meas_rx=NULL;
  g_mutex_unlock(&ppm_mtx);
}

gboolean ppm_cal_measure_start(RADIO *r) {
  if(g_atomic_int_get(&ppm_measuring)) return FALSE;
  RECEIVER *rx=r->active_receiver;
  if(rx==NULL) return FALSE;
  const PPM_STATION *st=ppm_station(r->ppm_ref_station);
  if(st==NULL) return FALSE;

  g_mutex_lock(&ppm_mtx);
  meas_rx=rx;
  meas_base=(double)(st->freq - rx->lo_a);
  meas_ppm0=r->ppm_correction_value;
  meas_fs=rx->sample_rate;
  acc_n=0; frames_done=0; valid_frames=0; settle_left=PPM_SETTLE; offset_sum=0.0;
  have_result=FALSE; result_ok=FALSE; result_offset=0.0; result_ppm=meas_ppm0;
  snprintf(status,sizeof(status),"Measuring %s\342\200\246",st->name);
  g_mutex_unlock(&ppm_mtx);

  // Retune so the carrier lands at +PPM_MEAS_OFFSET in the baseband spectrum.
  rx->frequency_a=st->freq-(long long)PPM_MEAS_OFFSET;
  rx->ctun=0;
  rx->ctun_frequency=rx->frequency_a;
  receiver_mode_changed(rx,CWU);
  frequency_changed(rx);

  g_atomic_int_set(&ppm_measuring,1);   // arm the tap only after the retune
  return TRUE;
}

void ppm_cal_measure_poll(gboolean *done, char *s, int sz,
                          double *off, double *sugg, gboolean *ok) {
  g_mutex_lock(&ppm_mtx);
  if(s!=NULL && sz>0) g_strlcpy(s,status,sz);
  if(done!=NULL) *done=have_result;
  if(off!=NULL)  *off=result_offset;
  if(sugg!=NULL) *sugg=result_ppm;
  if(ok!=NULL)   *ok=result_ok;
  g_mutex_unlock(&ppm_mtx);
}

void ppm_cal_iq_feed(RECEIVER *rx, const double *iq, int n_frames) {
  if(!g_atomic_int_get(&ppm_measuring)) return;   // cheap fast-path
  g_mutex_lock(&ppm_mtx);
  if(rx!=meas_rx || !g_atomic_int_get(&ppm_measuring)) {
    g_mutex_unlock(&ppm_mtx);
    return;
  }
  for(int k=0;k<n_frames;k++) {
    fre[acc_n]=iq[2*k];
    fim[acc_n]=iq[2*k+1];
    acc_n++;
    if(acc_n>=PPM_FFT_N) {
      if(settle_left>0) {
        settle_left--;
      } else {
        double off;
        if(analyze_frame(fre,fim,meas_fs,&off)) {
          offset_sum+=off;
          valid_frames++;
        }
        frames_done++;
        if(frames_done>=PPM_FRAMES) {
          finalize();
          acc_n=0;
          break;
        }
      }
      acc_n=0;
    }
  }
  g_mutex_unlock(&ppm_mtx);
}
