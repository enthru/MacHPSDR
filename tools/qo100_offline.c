/* QO-100 beacon-lock offline harness.
 *
 * The beacon lock is a closed loop that RETUNES THE RADIO, so a sign error does
 * not show up as a slightly wrong number — it shows up as the receiver walking
 * away from the band at 20 kHz a second.  That is not something to discover on
 * air with a dish pointed at a geostationary satellite, and it is not something
 * the running application can be asked about cheaply either (see the standing
 * rule about not launching the GUI to test).  So the whole loop runs here
 * against synthetic I/Q, in the real qo100.c code path.
 *
 * The trick that makes this possible without linking the entire application: the
 * four app-side functions qo100.c calls are stubbed below, and the two structs it
 * touches are ordinary C structs that can simply be allocated and filled.  The
 * measurement, the acquisition logic, the loop arithmetic and the retune are all
 * the shipped ones.
 *
 *   make qo100-offline && ./qo100_offline
 *
 * Exit status 0 = every case passed.
 */

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
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
#include "qo100.h"

// ---- the application, reduced to what qo100.c actually reaches for ----------

RADIO *radio;                       // the global qo100.c reads

// A real bands table: the transverter-creation code hunts for free slots across
// it, so a single shared band would make that logic untestable.
static BAND      test_bands[BANDS+XVTRS];
static BANDSTACK test_stacks[BANDS+XVTRS];
static BANDSTACK_ENTRY test_entries[BANDS+XVTRS][3];
static int  retunes;                // how many times the loop moved the radio

#define test_band test_bands[0]

static void bands_init(void) {
  memset(test_bands,0,sizeof(test_bands));
  memset(test_stacks,0,sizeof(test_stacks));
  memset(test_entries,0,sizeof(test_entries));
  for(int i=0;i<BANDS+XVTRS;i++) {
    test_stacks[i].entries=3;
    test_stacks[i].entry=test_entries[i];
    test_bands[i].bandstack=&test_stacks[i];
  }
}

BAND *band_get_band(int b) {
  if(b<0 || b>=BANDS+XVTRS) return NULL;
  return &test_bands[b];
}
void frequency_changed(RECEIVER *rx) { (void)rx; retunes++; }
void update_frequency(RECEIVER *rx) { (void)rx; }
void receiver_sync_vfo_b_lo(RECEIVER *rx) { (void)rx; }
void transmitter_set_mode(TRANSMITTER *tx, int m) { (void)tx; (void)m; }

// Stands in for band.c:set_band(): restore the band-stack entry and the band's
// LO, which is the part qo100.c depends on.
void set_band(RECEIVER *rx, int band, int bs_entry) {
  BAND *b=band_get_band(band);
  if(b==NULL || rx==NULL) return;
  int e=(bs_entry>=0)?bs_entry:0;
  if(b->bandstack!=NULL && e<b->bandstack->entries) {
    rx->frequency_a=b->bandstack->entry[e].frequency;
    rx->mode_a=b->bandstack->entry[e].mode;
    rx->filter_a=b->bandstack->entry[e].filter;
  }
  rx->band_a=band;
  rx->lo_a=b->frequencyLO;
  rx->error_a=b->errorLO;
}

// ---- signal generation ------------------------------------------------------

// Fill a receiver-format I/Q block. The buffer order is (Q, I) — the receiver's
// own, which is the whole point: writing it as (I, Q) here would mirror the
// spectrum and the test would then "prove" a decoder that is wrong on air.
static void gen_block(double *iq, int n, double baseband_hz, int fs,
                      double amp, double noise, double *phase, guint32 *seed) {
  double dph=2.0*M_PI*baseband_hz/(double)fs;
  for(int k=0;k<n;k++) {
    double i=amp*cos(*phase);
    double q=amp*sin(*phase);
    *phase+=dph;
    if(*phase>2.0*M_PI) *phase-=2.0*M_PI;
    if(noise>0.0) {
      // Cheap deterministic LCG noise: reproducible runs matter more than
      // spectral purity here.
      for(int t=0;t<2;t++) {
        *seed=(*seed)*1103515245u+12345u;
        double u=((double)((*seed>>8)&0xFFFF)/32768.0)-1.0;
        if(t==0) i+=noise*u; else q+=noise*u;
      }
    }
    iq[2*k]  =q;      // Q first
    iq[2*k+1]=i;
  }
}

// ---- test rig ---------------------------------------------------------------

#define FS       192000
#define BLOCK      2048

static RECEIVER *mk_rx(long long freq_a, int sample_rate) {
  RECEIVER *rx=g_new0(RECEIVER,1);
  rx->frequency_a=freq_a;
  rx->sample_rate=sample_rate;
  rx->error_a=0;
  rx->band_a=0;
  return rx;
}

static RADIO *mk_radio(RECEIVER *rx) {
  RADIO *r=g_new0(RADIO,1);
  r->active_receiver=rx;
  r->qo100_beacon_lock=TRUE;
  r->qo100_beacon_sel=0;
  r->qo100_offset=QO100_TP_OFFSET;
  return r;
}

// Run the loop for at most `max_blocks` blocks against an LNB whose LO is out by
// `lo_error` Hz, and return the residual LO error left over.
//
// The model: the true LNB LO is lo_a + D, so a signal at RF F lands at baseband
//   F - frequency_a - D - error_a
// (derived in qo100.c). error_a is what the loop is allowed to move; when the
// loop has done its job, D + error_a == 0.
static double run_loop(double lo_error, double noise, int max_blocks,
                       gboolean *locked_out, int fs, long long freq_a) {
  RECEIVER *rx=mk_rx(freq_a, fs);              // parked inside the transponder
  radio=mk_radio(rx);
  test_band.frequencyLO=9750000000LL;
  test_band.errorLO=0;
  retunes=0;
  qo100_beacon_reset();
  radio->qo100_beacon_lock=TRUE;               // reset() clears the status only

  double *iq=g_new0(double,BLOCK*2);
  double phase=0.0;
  guint32 seed=12345;
  long long beacon=qo100_beacon_frequency(0);

  for(int b=0;b<max_blocks;b++) {
    double baseband=(double)(beacon-rx->frequency_a)-lo_error-(double)rx->error_a;
    gen_block(iq,BLOCK,baseband,fs,1.0,noise,&phase,&seed);
    qo100_beacon_iq_feed(rx,iq,BLOCK);
    // The retune is queued onto the main loop, exactly as it is in the app.
    while(g_main_context_iteration(NULL,FALSE)) ;
  }

  double residual=lo_error+(double)rx->error_a;
  if(locked_out!=NULL) *locked_out=qo100_beacon_locked();
  g_free(iq);
  g_free(rx);
  g_free(radio);
  radio=NULL;
  return residual;
}

static int failures;

static void check(const char *name, gboolean ok, const char *detail) {
  printf("%-42s %s   %s\n", name, ok?"PASS":"FAIL", detail?detail:"");
  if(!ok) failures++;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  char d[160];
  bands_init();

  // How many blocks one FFT frame needs, plus the settling the loop takes: the
  // frame is 32768 samples and the correction is fractional, so allow plenty.
  const int blocks_per_frame=32768/BLOCK;
  const int blocks=blocks_per_frame*40;

  // ---- 1. the sign. The single most important assertion in this file: an LNB
  //         3 kHz high must be corrected TOWARD zero, not away from it.
  {
    gboolean locked=FALSE;
    double res=run_loop(3000.0,0.0,blocks,&locked,FS,10489540000LL);
    snprintf(d,sizeof(d),"3000 Hz -> %+.1f Hz left, locked=%d",res,locked);
    check("sign: positive LNB error converges",
          locked && fabs(res)<10.0, d);
  }

  // ---- 2. ...and the other way round, which is what a wrong sign would still
  //         pass if the test only ever pushed one direction.
  {
    gboolean locked=FALSE;
    double res=run_loop(-4500.0,0.0,blocks,&locked,FS,10489540000LL);
    snprintf(d,sizeof(d),"-4500 Hz -> %+.1f Hz left, locked=%d",res,locked);
    check("sign: negative LNB error converges",
          locked && fabs(res)<10.0, d);
  }

  // ---- 3. a big cold-start error, the realistic case for a cheap LNB.
  {
    gboolean locked=FALSE;
    double res=run_loop(35000.0,0.0,blocks,&locked,FS,10489540000LL);
    snprintf(d,sizeof(d),"35 kHz -> %+.1f Hz left, locked=%d",res,locked);
    check("wide acquisition (35 kHz off)", locked && fabs(res)<20.0, d);
  }

  // ---- 4. with noise on it, since a real beacon is not a clean tone.
  {
    gboolean locked=FALSE;
    double res=run_loop(2000.0,0.30,blocks,&locked,FS,10489540000LL);
    snprintf(d,sizeof(d),"2000 Hz + noise -> %+.1f Hz left, locked=%d",res,locked);
    check("converges with noise", locked && fabs(res)<25.0, d);
  }

  // ---- 5. NO SIGNAL AT ALL must not produce a lock, and above all must not
  //         move the radio. This is the failure that would be catastrophic in
  //         use: a loop that chases noise walks the receiver off the band.
  //         (The same class of bug the APT sync detector had.)
  {
    RECEIVER *rx=mk_rx(10489540000LL,FS);
    radio=mk_radio(rx);
    test_band.frequencyLO=9750000000LL;
    retunes=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    double *iq=g_new0(double,BLOCK*2);
    double phase=0.0; guint32 seed=999;
    for(int b=0;b<blocks;b++) {
      gen_block(iq,BLOCK,0.0,FS,0.0,1.0,&phase,&seed);   // amplitude 0 = noise only
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    snprintf(d,sizeof(d),"locked=%d retunes=%d error_a=%lld",
             qo100_beacon_locked(),retunes,(long long)rx->error_a);
    check("noise alone: no lock, radio untouched",
          !qo100_beacon_locked() && retunes==0 && rx->error_a==0, d);
    g_free(iq); g_free(rx); g_free(radio); radio=NULL;
  }

  // ---- 6. a lower sample rate still covering the beacon.
  {
    gboolean locked=FALSE;
    double res=run_loop(1500.0,0.0,blocks,&locked,96000,10489520000LL);
    snprintf(d,sizeof(d),"96 kHz, 1500 Hz -> %+.1f Hz left, locked=%d",res,locked);
    check("works at 96 kHz", locked && fabs(res)<10.0, d);
  }

  // ---- 7. the band plan and the transponder arithmetic, which are pure data.
  {
    gboolean ok=qo100_in_transponder(10489750000LL) &&
                !qo100_in_transponder(10489400000LL) &&
                qo100_beacon_frequency(0)==QO100_BEACON_LOWER &&
                qo100_beacon_frequency(1)==QO100_BEACON_UPPER &&
                (QO100_BEACON_LOWER-QO100_TP_OFFSET)==2400000000LL &&
                (QO100_BEACON_UPPER-QO100_TP_OFFSET)==2400500000LL;
    snprintf(d,sizeof(d),"uplink edges %lld / %lld",
             (long long)(QO100_BEACON_LOWER-QO100_TP_OFFSET),
             (long long)(QO100_BEACON_UPPER-QO100_TP_OFFSET));
    check("transponder offset maps the band edges", ok, d);
  }

  // ---- 8. every band-plan segment must be inside the transponder and ordered,
  //         so the overlay cannot draw a band that does not exist.
  {
    gboolean ok=TRUE;
    long long prev=0, prev_high=0;
    for(int i=0;i<qo100_segment_count();i++) {
      const QO100_SEGMENT *s=qo100_segment(i);
      if(s->low>s->high) ok=FALSE;
      if(s->low<QO100_NB_DOWN_LOW || s->high>QO100_NB_DOWN_HIGH) ok=FALSE;
      if(s->low<prev) ok=FALSE;
      // Two mode segments must never overlap — the plan assigns each slice of the
      // transponder to exactly one use, and an overlap would draw two tinted
      // bands over each other and tell the operator nothing. (Beacons are
      // zero-width markers and legitimately sit inside a guard band, so they are
      // exempt from the ordering-against-the-previous-end test.)
      if(!s->beacon) {
        if(prev_high>s->low) ok=FALSE;
        prev_high=s->high;
      }
      prev=s->low;
    }
    snprintf(d,sizeof(d),"%d segments spanning %.0f kHz",qo100_segment_count(),
             (double)(QO100_NB_DOWN_HIGH-QO100_NB_DOWN_LOW)/1000.0);
    check("band plan ordered, non-overlapping, in range", ok, d);
  }

  // ---- 9. the two transverter entries are created correctly. These are the
  //         numbers that, typed wrong by hand, produce a radio that hears
  //         nothing and gives no clue why — which is the reason the button
  //         exists at all.
  {
    bands_init();
    RADIO *r=g_new0(RADIO,1);
    r->qo100_lnb_lo=QO100_DEFAULT_LNB_LO;
    r->qo100_tx_lo=1968000000LL;          // 432 MHz IF, a common uplink converter
    r->qo100_offset=QO100_TP_OFFSET;
    char msg[160];
    gboolean ok=qo100_create_transverters(r,msg,sizeof(msg));
    BAND *rb=NULL,*tb=NULL;
    for(int i=BANDS;i<BANDS+XVTRS;i++) {
      if(strcmp(test_bands[i].title,QO100_XVTR_RX_TITLE)==0) rb=&test_bands[i];
      if(strcmp(test_bands[i].title,QO100_XVTR_TX_TITLE)==0) tb=&test_bands[i];
    }
    gboolean good = ok && rb!=NULL && tb!=NULL &&
      rb->frequencyMin==QO100_NB_DOWN_LOW && rb->frequencyMax==QO100_NB_DOWN_HIGH &&
      rb->frequencyLO==QO100_DEFAULT_LNB_LO &&
      tb->frequencyMin==QO100_NB_DOWN_LOW-QO100_TP_OFFSET &&
      tb->frequencyMax==QO100_NB_DOWN_HIGH-QO100_TP_OFFSET &&
      tb->frequencyLO==1968000000LL &&
      // and the uplink band must be exactly the published 2400.000-2400.500
      tb->frequencyMin==2400000000LL && tb->frequencyMax==2400500000LL;
      // (what the band-stack is seeded with is case 14's business)
    snprintf(d,sizeof(d),"rx %lld..%lld tx %lld..%lld",
             rb?(long long)rb->frequencyMin:0, rb?(long long)rb->frequencyMax:0,
             tb?(long long)tb->frequencyMin:0, tb?(long long)tb->frequencyMax:0);
    check("transverter entries written correctly", good, d);
    g_free(r);
  }

  // ---- 10. pressing the button again must UPDATE the same two rows, not eat
  //          two more slots — and must not throw away the LO error, which on the
  //          receive row is the beacon lock's accumulated measurement.
  {
    bands_init();
    RADIO *r=g_new0(RADIO,1);
    r->qo100_lnb_lo=QO100_DEFAULT_LNB_LO;
    char msg[160];
    qo100_create_transverters(r,msg,sizeof(msg));
    // pretend the beacon lock has been running
    for(int i=BANDS;i<BANDS+XVTRS;i++)
      if(strcmp(test_bands[i].title,QO100_XVTR_RX_TITLE)==0) test_bands[i].errorLO=-12345;
    r->qo100_lnb_lo=9749800000LL;         // operator corrects the LNB figure
    qo100_create_transverters(r,msg,sizeof(msg));
    int nrx=0,ntx=0; long long kept=0,lo=0;
    for(int i=BANDS;i<BANDS+XVTRS;i++) {
      if(strcmp(test_bands[i].title,QO100_XVTR_RX_TITLE)==0) {
        nrx++; kept=test_bands[i].errorLO; lo=test_bands[i].frequencyLO;
      }
      if(strcmp(test_bands[i].title,QO100_XVTR_TX_TITLE)==0) ntx++;
    }
    snprintf(d,sizeof(d),"rows rx=%d tx=%d, errorLO kept %lld, LO now %lld",nrx,ntx,kept,lo);
    check("second press updates in place, keeps LO error",
          nrx==1 && ntx==1 && kept==-12345 && lo==9749800000LL, d);
    g_free(r);
  }

  // ---- 11. no free slots: refuse, and do not half-write.
  {
    bands_init();
    for(int i=BANDS;i<BANDS+XVTRS;i++) g_strlcpy(test_bands[i].title,"taken",16);
    RADIO *r=g_new0(RADIO,1);
    char msg[160];
    gboolean ok=qo100_create_transverters(r,msg,sizeof(msg));
    int written=0;
    for(int i=BANDS;i<BANDS+XVTRS;i++)
      if(strcmp(test_bands[i].title,"taken")!=0) written++;
    snprintf(d,sizeof(d),"returned %d, rows touched %d",ok,written);
    check("full transverter table is refused, not clobbered",
          !ok && written==0, d);
    g_free(r);
    bands_init();
  }

  // ---- 12. cold start: nothing configured, receiver on 20 m. One press must
  //          build the converters, tune to the downlink and set the split — the
  //          ORDER matters, because the tuning ceiling only rises once a
  //          transverter covering 10.49 GHz exists.
  {
    bands_init();
    RECEIVER *rx=mk_rx(14200000LL,FS);   // parked on 20 m, nowhere near the satellite
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_lock=FALSE;
    radio=r;
    gboolean ok=qo100_transponder_setup(r);
    gboolean on_downlink=qo100_in_transponder(rx->frequency_a);
    long long expect_b=rx->frequency_a-QO100_TP_OFFSET;
    int made=0;
    for(int i=BANDS;i<BANDS+XVTRS;i++)
      if(strcmp(test_bands[i].title,QO100_XVTR_RX_TITLE)==0 ||
         strcmp(test_bands[i].title,QO100_XVTR_TX_TITLE)==0) made++;
    snprintf(d,sizeof(d),"A=%lld B=%lld split=%d rows=%d",
             (long long)rx->frequency_a,(long long)rx->frequency_b,rx->split,made);
    check("cold start: one press does converters+tune+split",
          ok && made==2 && on_downlink && rx->frequency_b==expect_b &&
          rx->split==SPLIT_SAT && rx->mode_a==USB, d);
    g_free(rx); g_free(r); radio=NULL;
  }

  // ---- 13. ...but an operator already on the downlink keeps their frequency:
  //          the button must not yank them off the QSO they are in.
  {
    bands_init();
    RADIO *r0=g_new0(RADIO,1);
    char msg[160];
    qo100_create_transverters(r0,msg,sizeof(msg));
    g_free(r0);
    RECEIVER *rx=mk_rx(10489743000LL,FS);      // mid-QSO, an odd frequency
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_lock=FALSE;
    radio=r;
    qo100_transponder_setup(r);
    snprintf(d,sizeof(d),"A stayed at %lld",(long long)rx->frequency_a);
    check("already on the downlink: frequency untouched",
          rx->frequency_a==10489743000LL &&
          rx->frequency_b==10489743000LL-QO100_TP_OFFSET, d);
    g_free(rx); g_free(r); radio=NULL;
  }

  // ---- 14. the band-stack must not park the operator on a beacon, which is
  //          precisely where min/middle/max would have put all three entries.
  {
    bands_init();
    RADIO *r=g_new0(RADIO,1);
    char msg[160];
    qo100_create_transverters(r,msg,sizeof(msg));
    gboolean clear=TRUE;
    BAND *rb=NULL;
    for(int i=BANDS;i<BANDS+XVTRS;i++)
      if(strcmp(test_bands[i].title,QO100_XVTR_RX_TITLE)==0) rb=&test_bands[i];
    for(int e=0;rb!=NULL && e<rb->bandstack->entries;e++) {
      long long f=rb->bandstack->entry[e].frequency;
      if(f==QO100_BEACON_LOWER || f==QO100_BEACON_MIDDLE || f==QO100_BEACON_UPPER) clear=FALSE;
      if(!qo100_in_transponder(f)) clear=FALSE;
    }
    snprintf(d,sizeof(d),"%lld / %lld / %lld",
             rb?(long long)rb->bandstack->entry[0].frequency:0,
             rb?(long long)rb->bandstack->entry[1].frequency:0,
             rb?(long long)rb->bandstack->entry[2].frequency:0);
    check("band-stack lands on working spots, not beacons", clear, d);
    g_free(r);
  }

  printf("\n%s\n", failures==0 ? "all cases passed" : "FAILURES ABOVE");
  return failures==0 ? 0 : 1;
}
