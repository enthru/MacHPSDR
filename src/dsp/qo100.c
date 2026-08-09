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
#include "bandstack.h"
#include "band.h"
#include "radio.h"
#include "mode.h"
#include "log.h"
#include "qo100.h"

extern RADIO *radio;   // global application state

// ---------------------------------------------------------------------------
// Band plan
// ---------------------------------------------------------------------------
//
// AMSAT-DL's narrow-band transponder plan.  The three beacons and the two
// transponder edges are exact and are what everything else here is anchored to;
// the boundaries BETWEEN the mode segments are the published guidance and get
// revised from time to time, so they live in this one table and nowhere else —
// if the plan changes, edit here.
//
// Segments are drawn as tinted bands under the trace and beacons as single
// vertical markers, so the operator can see at a glance that they are calling CQ
// in the CW section.

static const QO100_SEGMENT segments[] = {
  { QO100_BEACON_LOWER,  QO100_BEACON_LOWER,  "Beacon",  1.00, 0.85, 0.20, TRUE  },
  { 10489555000LL, 10489580000LL, "CW",        0.30, 0.75, 1.00, FALSE },
  { 10489580000LL, 10489650000LL, "NB digi",   0.55, 0.45, 1.00, FALSE },
  { 10489650000LL, 10489675000LL, "Mixed",     0.60, 0.60, 0.60, FALSE },
  { QO100_BEACON_MIDDLE, QO100_BEACON_MIDDLE, "Beacon",  1.00, 0.85, 0.20, TRUE  },
  { 10489675000LL, 10489745000LL, "SSB",       0.30, 1.00, 0.50, FALSE },
  { 10489745000LL, 10489770000LL, "Digi",      0.55, 0.45, 1.00, FALSE },
  { 10489770000LL, 10489795000LL, "Mixed",     0.60, 0.60, 0.60, FALSE },
  { QO100_BEACON_UPPER,  QO100_BEACON_UPPER,  "Beacon",  1.00, 0.85, 0.20, TRUE  },
};

int qo100_segment_count(void) {
  return (int)(sizeof(segments)/sizeof(segments[0]));
}

const QO100_SEGMENT *qo100_segment(int index) {
  if(index<0 || index>=qo100_segment_count()) return NULL;
  return &segments[index];
}

gboolean qo100_in_transponder(long long f) {
  return f>=QO100_NB_DOWN_LOW && f<=QO100_NB_DOWN_HIGH;
}

long long qo100_beacon_frequency(int sel) {
  // Only the two CW beacons are offered.  The middle beacon is 400 bd BPSK,
  // which is a SUPPRESSED-carrier signal — there is no spectral line for a
  // carrier tracker to lock to, and peak-picking its sidebands would measure the
  // modulation rather than the LNB.  It is still drawn in the band plan above.
  return (sel==1) ? QO100_BEACON_UPPER : QO100_BEACON_LOWER;
}

// ---------------------------------------------------------------------------
// Transponder mode
// ---------------------------------------------------------------------------

gboolean qo100_transponder_setup(RADIO *r) {
  if(r==NULL) return FALSE;
  RECEIVER *rx=r->active_receiver;
  if(rx==NULL) return FALSE;

  // Refuse politely rather than silently dragging VFO B somewhere absurd: this
  // only means anything when the receiver is actually on the downlink.  A little
  // slack past each edge so being parked on a beacon still counts.
  const long long slack=50000LL;
  if(rx->frequency_a < QO100_NB_DOWN_LOW-slack ||
     rx->frequency_a > QO100_NB_DOWN_HIGH+slack) return FALSE;

  long long offset=(r->qo100_offset!=0) ? r->qo100_offset : QO100_TP_OFFSET;
  rx->frequency_b=rx->frequency_a-offset;

  // The transponder is non-inverting, so the uplink keeps the downlink's
  // sideband — and SPLIT_SAT is the mode that moves both VFOs the same way,
  // which is exactly a constant translation.  (SPLIT_RSAT, the inverting one, is
  // for the linear transponders that mirror the passband; it would walk the
  // uplink the wrong way here.)
  rx->split=SPLIT_SAT;
  rx->mode_b=rx->mode_a;
  rx->filter_b=rx->filter_a;

  receiver_sync_vfo_b_lo(rx);
  if(r->transmitter!=NULL && r->transmitter->rx==rx) {
    transmitter_set_mode(r->transmitter,rx->mode_b);
  }
  frequency_changed(rx);
  update_frequency(rx);

  log_info("qo100: transponder mode, downlink %lld Hz -> uplink %lld Hz (offset %lld Hz)",
           (long long)rx->frequency_a,(long long)rx->frequency_b,offset);
  return TRUE;
}

// ---------------------------------------------------------------------------
// Beacon lock — continuous LNB drift correction
// ---------------------------------------------------------------------------
//
// The problem this solves: a QO-100 downlink arrives through an LNB whose local
// oscillator is nominally 9750 MHz but in practice is out by anywhere from a few
// to some tens of kilohertz, and MOVES — a few kHz over the first half hour as
// the dish warms, and again when the sun goes off it.  Everything the radio
// displays is wrong by that amount, and since the uplink goes through a
// completely different converter the operator cannot even discover the error by
// listening to themselves without first being on frequency.
//
// The transponder carries its own reference for exactly this: the two CW beacons
// mark the band edges and are on frequency by definition.  So: find one in the
// off-air spectrum, compare where it IS with where it SHOULD be, and feed the
// difference into the band's LO error.
//
// This differs from ppm_cal.c (which does the same trick against an HF time
// standard) in two ways that matter:
//   * it is CONTINUOUS, not a one-shot calibration, because the thing it
//     measures drifts while you use it; and
//   * it must never retune the operator's dial.  ppm_cal parks the receiver on
//     the carrier because it can; here the operator is working a QSO and the
//     beacon simply has to be found wherever it happens to fall in the span.
//
// What it deliberately does NOT touch is error_b / the uplink.  The 2.4 GHz
// transverter is a different box with a different error, and a downlink
// measurement says nothing about it — which is precisely why VFO B grew its own
// LO (see receiver_sync_vfo_b_lo()).
//
// Sign: the receiver's I/Q buffer is (Q, I) — WDSP reads it that way, so that is
// the sense the panadapter is in, and reading it as (I, Q) here would mirror the
// baseband and drive the loop the wrong way. Same note as in ppm_cal.c.
// With true_LO = lo_a + D (D = the LNB's error) the beacon lands at baseband
//   measured = beacon - frequency_a - D - error_a
// against an expected beacon - frequency_a, so
//   residual = measured - expected = -(D + error_a)
// and stepping error_a by +residual drives D + error_a to zero, which is the
// condition for the dial to be telling the truth.

#define QO100_FFT_N        32768     // ~5.9 Hz/bin at 192 kHz, ~170 ms per frame
#define QO100_ACQ_HZ       60000.0   // search half-width while hunting
#define QO100_TRACK_HZ      2000.0   // ...and once locked
#define QO100_DC_GUARD_HZ    300.0   // the I/Q DC-offset spike lives here
#define QO100_MIN_SNR         12.0   // peak/mean power in the window to trust a frame
#define QO100_LOCK_RUN           3   // consecutive agreeing frames to declare a lock
#define QO100_LOCK_TOL_HZ    200.0   // ...how closely they must agree
#define QO100_DEADBAND_HZ      3.0   // below this, leave the radio alone
#define QO100_COARSE_HZ      100.0   // above this the reading is a real offset, not jitter
#define QO100_GAIN             0.5   // fraction of a SMALL error applied per step
#define QO100_MAX_STEP_HZ  20000.0   // never jump further than this in one go
#define QO100_SETTLED_HZ    1000.0   // residual below which the narrow window is safe

static GMutex   bmtx;
static RECEIVER *track_rx;
static int      track_fs;
static long long track_beacon;

static double   bre[QO100_FFT_N];
static double   bim[QO100_FFT_N];
static int      bacc;

static gboolean b_locked;
static gboolean b_settled;           // residual small enough for the narrow window
static int      b_run;               // consecutive agreeing frames
static double   b_last;              // previous frame's residual, for the agreement test
static double   b_residual;          // published: last accepted residual
static double   b_applied;           // published: total correction pushed into the LO error
static char     b_status[128]="Off";

static volatile gint apply_queued;   // one applier in flight at a time
static double   pend_step;           // Hz to add to error_a, under bmtx

// In-place iterative radix-2 FFT (forward). Self-contained for the same reason
// ppm_cal.c keeps its own: this module must not depend on which FFT the FT8 or
// WDSP builds happen to provide.
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
      for(int i2=0;i2<len/2;i2++) {
        int a=i+i2, b=i+i2+len/2;
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

// Locate the strongest line within [expected-win, expected+win] and return its
// position in Hz. Destroys re/im. Caller holds bmtx.
static gboolean find_carrier(double *re, double *im, int fs,
                             double expected, double win, double *found) {
  const int N=QO100_FFT_N;
  for(int i=0;i<N;i++) {
    double w=0.5-0.5*cos(2.0*M_PI*(double)i/(double)(N-1));   // Hann
    re[i]*=w; im[i]*=w;
  }
  fft_radix2(re,im,N);

  const double bin_hz=(double)fs/(double)N;
  double sum=0.0, peak=-1.0;
  int pk=-1, count=0;
  for(int m=0;m<N;m++) {
    double sf=((m<=N/2)?(double)m:(double)(m-N))*bin_hz;   // signed baseband Hz
    if(fabs(sf)<QO100_DC_GUARD_HZ) continue;
    if(sf<expected-win || sf>expected+win) continue;
    double p=re[m]*re[m]+im[m]*im[m];
    sum+=p; count++;
    if(p>peak) { peak=p; pk=m; }
  }
  if(pk<1 || pk>=N-1 || count<8) return FALSE;
  double mean=sum/(double)count;
  if(mean<=0.0 || peak/mean<QO100_MIN_SNR) return FALSE;

  double y0=sqrt(re[pk-1]*re[pk-1]+im[pk-1]*im[pk-1]);
  double y1=sqrt(peak);
  double y2=sqrt(re[pk+1]*re[pk+1]+im[pk+1]*im[pk+1]);
  double denom=y0-2.0*y1+y2;
  double delta=(denom!=0.0)?0.5*(y0-y2)/denom:0.0;
  if(delta> 0.5) delta= 0.5;
  if(delta<-0.5) delta=-0.5;
  double pos=(double)pk+delta;
  double sf=(pos<=N/2)?pos:pos-(double)N;
  *found=sf*bin_hz;
  return TRUE;
}

// GTK-thread half: push the queued step into the receiver and the band it is on.
// Everything here touches WDSP / the protocol layer and so cannot run on the
// audio thread the measurement lives on.
static gboolean apply_idle(gpointer data) {
  double step;
  RECEIVER *rx;

  g_mutex_lock(&bmtx);
  step=pend_step;
  pend_step=0.0;
  rx=track_rx;
  g_mutex_unlock(&bmtx);

  // The receiver may have gone away, or the operator may have switched away from
  // it, between the measurement and this callback.
  if(radio!=NULL && rx!=NULL && rx==radio->active_receiver && step!=0.0) {
    rx->error_a += (gint64)llround(step);
    BAND *band=band_get_band(rx->band_a);
    if(band!=NULL) band->errorLO=rx->error_a;   // so it survives a band change and a restart
    frequency_changed(rx);
    update_frequency(rx);
  }
  g_atomic_int_set(&apply_queued,0);
  return G_SOURCE_REMOVE;
}

static void beacon_reset_locked(void) {
  bacc=0;
  b_run=0;
  b_last=0.0;
  b_locked=FALSE;
  b_settled=FALSE;
}

void qo100_beacon_reset(void) {
  g_mutex_lock(&bmtx);
  beacon_reset_locked();
  b_residual=0.0;
  track_rx=NULL;
  g_strlcpy(b_status,"Off",sizeof(b_status));
  g_mutex_unlock(&bmtx);
}

// Display-only: a benign read of a scalar the audio thread writes. Worst case a
// readout is one frame (~170 ms) stale, which no caller cares about.
gboolean qo100_beacon_locked(void) {
  return b_locked;
}

double qo100_beacon_residual_hz(void) { return b_residual; }
double qo100_beacon_applied_hz(void)  { return b_applied; }

void qo100_beacon_status(char *buf, int size) {
  if(buf==NULL || size<=0) return;
  g_mutex_lock(&bmtx);
  g_strlcpy(buf,b_status,size);
  g_mutex_unlock(&bmtx);
}

// Analyse one filled frame. Caller holds bmtx.
static void beacon_frame(RECEIVER *rx) {
  double expected=(double)(track_beacon-rx->frequency_a);
  double span_half=0.45*(double)track_fs;

  if(fabs(expected)>span_half) {
    beacon_reset_locked();
    snprintf(b_status,sizeof(b_status),
             "Beacon outside the span (%.0f kHz away)",expected/1000.0);
    return;
  }
  // If the beacon is expected to sit on top of the receiver's own DC spike there
  // is no way to tell the two apart, and a lock on the spike would read a
  // permanent zero error and quietly do nothing. Say so instead.
  if(fabs(expected)<2.0*QO100_DC_GUARD_HZ) {
    beacon_reset_locked();
    g_strlcpy(b_status,"Beacon too close to the centre \342\200\224 tune off it",
              sizeof(b_status));
    return;
  }

  // The narrow tracking window may only be used once the beacon is actually
  // WHERE IT SHOULD BE — not merely once a lock has been declared. Narrowing on
  // the lock alone strands the loop: a cold LNB 35 kHz out gets one correction,
  // the beacon is then still 17 kHz from the centre of a +/-2 kHz window, the
  // search stops finding it, and the radio sits reporting "locked" while being
  // 17 kHz wrong for ever. (Found by tools/qo100_offline.c, not on air.)
  double win=(b_locked && b_settled)?QO100_TRACK_HZ:QO100_ACQ_HZ;
  if(win>span_half-fabs(expected)) win=span_half-fabs(expected);
  if(win<50.0) win=50.0;

  double found;
  if(!find_carrier(bre,bim,track_fs,expected,win,&found)) {
    b_run=0;
    if(!b_locked) g_strlcpy(b_status,"Searching for the beacon\342\200\246",sizeof(b_status));
    else g_strlcpy(b_status,"Locked (beacon momentarily gone)",sizeof(b_status));
    return;
  }

  double residual=found-expected;

  // Acquisition needs agreement, not just a strong bin: the search takes a
  // maximum over hundreds of trial positions, so noise alone produces a
  // confident-looking peak somewhere every frame. A real beacon comes back to
  // the same place; noise does not. (Same lesson as the APT sync detector.)
  if(!b_locked) {
    if(b_run>0 && fabs(residual-b_last)<QO100_LOCK_TOL_HZ) b_run++;
    else b_run=1;
    b_last=residual;
    if(b_run<QO100_LOCK_RUN) {
      snprintf(b_status,sizeof(b_status),"Acquiring\342\200\246 %+.0f Hz",residual);
      return;
    }
    b_locked=TRUE;
  }

  b_residual=residual;
  b_settled=(fabs(residual)<QO100_SETTLED_HZ);

  if(fabs(residual)<QO100_DEADBAND_HZ) {
    snprintf(b_status,sizeof(b_status),"Locked  %+.1f Hz  (LO %+.0f Hz)",
             residual,b_applied);
    return;
  }

  // Two speeds, because the two situations are not the same measurement. A large
  // reading is a genuine converter offset that has been measured directly, so
  // there is nothing to gain by approaching it in fractions — take all of it. A
  // small one is mostly measurement noise on a fading path, so damp it, or the
  // radio twitches a few Hz every frame for ever.
  double step=(fabs(residual)>QO100_COARSE_HZ)?residual:residual*QO100_GAIN;
  if(step> QO100_MAX_STEP_HZ) step= QO100_MAX_STEP_HZ;
  if(step<-QO100_MAX_STEP_HZ) step=-QO100_MAX_STEP_HZ;

  snprintf(b_status,sizeof(b_status),"Locked  %+.1f Hz  (LO %+.0f Hz)",
           residual,b_applied+step);

  // Queue the retune for the GTK thread, at most one at a time: if the applier
  // has not run yet the next frame's measurement is stale anyway.
  if(g_atomic_int_compare_and_exchange(&apply_queued,0,1)) {
    pend_step=step;
    b_applied+=step;
    g_idle_add(apply_idle,NULL);
  }
}

void qo100_beacon_iq_feed(RECEIVER *rx, const double *iq, int n_frames) {
  if(radio==NULL || !radio->qo100_beacon_lock) return;     // cheap fast path
  if(rx==NULL || rx!=radio->active_receiver) return;

  g_mutex_lock(&bmtx);

  long long beacon=qo100_beacon_frequency(radio->qo100_beacon_sel);
  if(rx!=track_rx || track_fs!=rx->sample_rate || track_beacon!=beacon) {
    track_rx=rx;
    track_fs=rx->sample_rate;
    track_beacon=beacon;
    beacon_reset_locked();
  }

  for(int k=0;k<n_frames;k++) {
    // (Q, I) — the receiver buffer's order; see the sign note above.
    bre[bacc]=iq[2*k+1];
    bim[bacc]=iq[2*k];
    bacc++;
    if(bacc>=QO100_FFT_N) {
      bacc=0;
      beacon_frame(rx);
    }
  }

  g_mutex_unlock(&bmtx);
}
