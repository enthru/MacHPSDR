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
// A radio that takes the retune and does not move: the shape of the bug the
// stuck guard exists for (frequency_changed() pushed a new LO to Protocol 2 /
// SoapySDR only in its non-ctun branch, so under ctun or freetune the correction
// stayed in error_a and never reached the hardware).
static gboolean deaf_radio;
// The lower beacon is F1A: its carrier HOPS 400 Hz while it keys its ident, and
// an FFT frame is longer than an element, so most frames hold both lines and the
// peak search picks whichever is momentarily stronger.
static gboolean f1a_ident;
// ...and, where the test is about TRACKING rather than acquisition, only after
// the loop has settled: which line acquisition picks out of a burst is the
// documented ambiguity, and a test that turns on the ident before there is a
// lock measures that instead of what it means to.
static double   f1a_from_s;
// The ident shaped as the log reads it: whole seconds on one line, whole
// seconds on the other, keying in between (see the hop expression).
static gboolean f1a_shape;
// An LNB warms up and drifts while it does it: Hz per minute normally, and the
// loop's own guards are written around a "fast 10 Hz/s". The ident test needs
// it, because a loop that cannot measure during the ident is only visible as
// one that falls behind something moving.
static double   lo_drift_hz_s;
// ...and the measurement itself is not clean on a fading path: the line the
// loop reads moves a few hertz from one integrated second to the next. It has
// mean zero, so a loop that chases it only moves the operator's dial about.
static double   lo_wobble_hz;
// Blocks of delay between the loop writing error_a and the signal showing it:
// the driver's transfers in flight, the FIFO in front of the DSP thread and the
// retune itself. Real, and much longer than one FFT frame at a wide span.
static int      lag_blocks;
// Peak-to-peak MOVEMENT of the correction once the loop has settled, not its
// distance from nominal: which of the two F1A lines the published figure names
// is unknown (see the note), so sitting 400 Hz off is a documented ambiguity,
// while moving 400 Hz back and forth is the bug -- that is what a decode or an
// SSB listener hears.
static double   worst_pull;
// ...and how far the dial ever sat from the beacon once settled, which is the
// number an operator hears.
static double   worst_track;
// ...and the most it ever moved inside ONE FT8 slot, which is the number that
// decides whether a decode survives: ft8_lib does no drift tracking at all.
static double   worst_slot;
// ...and the biggest single step it ever applied, which is what "it jumps about"
// means when an operator says it.
static double   worst_step;

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

  worst_pull=0.0;
  worst_track=0.0;
  worst_slot=0.0;
  worst_step=0.0;
  double slot_lo=1e18, slot_hi=-1e18, slot_t0=-1.0;
  long long last_err=0;
  double pull_lo=1e18, pull_hi=-1e18;
  for(int b=0;b<max_blocks;b++) {
    double t=(double)b*(double)BLOCK/(double)fs;
    // Second half of the run only: the first correction is the LNB's real error
    // being taken out, which is the loop doing its job.
    if(b>max_blocks/2) {
      double e=(double)rx->error_a;
      if(slot_t0<0.0 || t-slot_t0>=15.0) { slot_t0=t; slot_lo=slot_hi=e; }
      if(e<slot_lo) slot_lo=e;
      if(e>slot_hi) slot_hi=e;
      if(slot_hi-slot_lo>worst_slot) worst_slot=slot_hi-slot_lo;
      double off=fabs(lo_error+lo_drift_hz_s*t+e);   // dial vs the real beacon
      if(off>worst_track) worst_track=off;
      if(e<pull_lo) pull_lo=e;
      if(e>pull_hi) pull_hi=e;
      worst_pull=pull_hi-pull_lo;
      double st=fabs((double)(rx->error_a-last_err));
      if(st>worst_step) worst_step=st;
    }
    last_err=rx->error_a;
    // The ident as a beacon actually sends it: bursts of keying with idle
    // carrier between them. A carrier that hops for ever is not a beacon, it is
    // a loop-breaker -- and asserting against it would only pin the loop's
    // refusal to lock at all.
    //
    // The burst is SIX seconds and its mark duty 60 %, which is what makes this
    // case bite: the loop integrates one second at a time, so two whole
    // measurements land inside the keying with the keyed line the taller of the
    // two, they agree with each other, and 400 Hz is applied as a coarse step.
    // That is the "jumps forward and comes back" reported from air.
    // F1A as the standard defines it, which is the thing the loop has to get
    // right: the carrier RESTS on the published (mark) frequency and drops
    // 400 Hz for the space between elements. So the nominal line is the one
    // that is nearly always there, and the shifted one appears only while the
    // beacon is sending.
    double hop=(f1a_ident && t>=f1a_from_s &&
                fmod(t,10.0)<6.0 && fmod(t,0.2)<0.12)?-400.0:0.0;
    // ...and the guard's worst case, which is what the operator's log turned
    // out to be: whole integrated seconds in which the loop's OWN line is
    // absent and the only candidate is 400 Hz away -- "nearest of 1 lines".
    // A beacon does not really rest on its space for three seconds; a loop
    // sitting on the WRONG line of the pair sees exactly this, because then
    // the line it is tracking is the one that only appears between elements.
    // Three seconds shifted, three keying, three on the nominal, over and
    // over, which is a harder version of that than the air produces.
    if(f1a_shape && t>=f1a_from_s) {
      double ph=fmod(t,9.0);
      hop=(ph<3.0)?-400.0:((ph<6.0)?((fmod(t,0.2)<0.1)?-400.0:0.0):0.0);
    }
    // What the radio is ACTUALLY tuned to right now: what the loop asked for
    // lag_blocks ago.
    double applied=(double)rx->error_a;
    if(lag_blocks>0) {
      static double hist[4096];
      int slot=b%(lag_blocks+1);
      applied=(b>lag_blocks)?hist[(b-lag_blocks)%(lag_blocks+1)]:0.0;
      hist[slot]=(double)rx->error_a;
    }
    double wob=0.0;
    if(lo_wobble_hz>0.0) {
      guint32 h=(guint32)(int)t*2654435761u;
      wob=lo_wobble_hz*((double)((h>>16)&0xFF)/127.5-1.0);
    }
    double baseband=(double)(beacon-rx->frequency_a)-lo_error-lo_drift_hz_s*t+wob
                    -(deaf_radio?0.0:applied)+hop;
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
  //         The offset is the check that settles a disputed set of edges: every
  //         published figure for either transponder, old or current, has to map
  //         to its uplink through the SAME constant translation, because that is
  //         what a non-inverting transponder is.
  {
    gboolean ok=qo100_in_transponder(10489750000LL) &&
                qo100_in_nb_transponder(10489750000LL) &&
                !qo100_in_wb_transponder(10489750000LL) &&
                !qo100_in_transponder(10489400000LL) &&
                // the half-megahertz between the two transponders is neither
                !qo100_in_transponder(10490250000LL) &&
                qo100_in_wb_transponder(10495000000LL) &&
                !qo100_in_nb_transponder(10495000000LL) &&
                !qo100_in_transponder(10499750000LL) &&
                qo100_beacon_frequency(QO100_BEACON_SEL_LOWER)==QO100_BEACON_LOWER &&
                qo100_beacon_frequency(QO100_BEACON_SEL_UPPER)==QO100_BEACON_UPPER &&
                qo100_beacon_frequency(QO100_BEACON_SEL_WB)==QO100_WB_BEACON &&
                qo100_beacon_has_carrier(QO100_BEACON_SEL_LOWER) &&
                qo100_beacon_has_carrier(QO100_BEACON_SEL_UPPER) &&
                !qo100_beacon_has_carrier(QO100_BEACON_SEL_WB) &&
                (QO100_BEACON_LOWER-QO100_TP_OFFSET)==2400000000LL &&
                (QO100_BEACON_UPPER-QO100_TP_OFFSET)==2400500000LL &&
                // AMSAT-DL WB bandplan V3 note 5: uplink 2401.0-2410.0 MHz
                (QO100_WB_DOWN_LOW -QO100_TP_OFFSET)==2401000000LL &&
                (QO100_WB_DOWN_HIGH-QO100_TP_OFFSET)==2410000000LL &&
                // ...and its beacon row: 10491.5 down against 2402.0 up
                (QO100_WB_BEACON  -QO100_TP_OFFSET)==2402000000LL;
    snprintf(d,sizeof(d),"NB %lld/%lld  WB %lld/%lld",
             (long long)(QO100_BEACON_LOWER-QO100_TP_OFFSET),
             (long long)(QO100_BEACON_UPPER-QO100_TP_OFFSET),
             (long long)(QO100_WB_DOWN_LOW-QO100_TP_OFFSET),
             (long long)(QO100_WB_DOWN_HIGH-QO100_TP_OFFSET));
    check("transponder offset maps both band edges", ok, d);
  }

  // ---- 8. every band-plan entry must be inside ONE of the transponders and
  //         ordered, so the overlay cannot draw a band that does not exist.
  {
    gboolean ok=TRUE;
    long long prev=0, prev_high=0;
    int nb=0, wb=0, chans=0;
    for(int i=0;i<qo100_segment_count();i++) {
      const QO100_SEGMENT *s=qo100_segment(i);
      if(s->low>s->high) ok=FALSE;
      if(qo100_in_nb_transponder(s->low) && qo100_in_nb_transponder(s->high)) nb++;
      else if(qo100_in_wb_transponder(s->low) && qo100_in_wb_transponder(s->high)) wb++;
      else ok=FALSE;                       // outside both, or straddling the gap
      // Two spans must never overlap, and they must be in order — the plan
      // assigns each slice of a transponder to exactly one use, an overlap would
      // draw two tinted bands over each other and tell the operator nothing, and
      // a span transcribed out of order is how a table silently grows a hole.
      //
      // The ordering test is over the SPANS only, deliberately. Beacons and
      // channels are zero-width markers that sit inside a span rather than
      // between spans, and grouping them by what they are (the three wide
      // channels together, then the narrow grid) is worth more in a table meant
      // to be checked against a published one than a single sorted sequence
      // would be. Nothing reads this table in order; the panadapter asks each
      // entry where it is. What the markers ARE is checked exhaustively by the
      // two cases either side of this one.
      if(s->kind==QO100_SEG_SPAN) {
        if(s->low<prev) ok=FALSE;
        if(prev_high>s->low) ok=FALSE;
        prev_high=s->high;
        prev=s->low;
      } else {
        if(s->low!=s->high) ok=FALSE;      // a marker is one frequency
        if(s->kind==QO100_SEG_CHANNEL) chans++;
      }
    }
    snprintf(d,sizeof(d),"%d entries: %d narrow, %d wideband (%d channels)",
             qo100_segment_count(),nb,wb,chans);
    check("band plan spans ordered, non-overlapping, in range", ok && nb>0 && wb>0, d);
  }

  // ---- 8b. the wideband channel grid, against the published table rather than
  //          against itself. The V3 plan lists two overlapping channel sets and
  //          they are one grid: 27 "very narrow" channels from 10492.750 in
  //          250 kHz steps, of which the alternate members (every *.250 and
  //          *.750, 14 of them) are also the "narrow" 333 kS channels, plus
  //          three "wide" 1 MS channels. Transcribing 30 rows of numbers by hand
  //          is exactly the job a machine should be checking.
  {
    gboolean ok=TRUE;
    int grid=0, narrow=0, wide=0;
    long long first_grid=0, last_grid=0;
    static const long long wide_ch[3]={10493250000LL,10494750000LL,10496250000LL};
    int wide_seen[3]={0,0,0};
    for(int i=0;i<qo100_segment_count();i++) {
      const QO100_SEGMENT *s=qo100_segment(i);
      if(s->kind!=QO100_SEG_CHANNEL) continue;
      if(s->rank>=2) {
        wide++;
        int m=0;
        for(int k=0;k<3;k++) if(s->low==wide_ch[k]) { wide_seen[k]=1; m=1; }
        if(!m) ok=FALSE;
        continue;
      }
      // ...the 250 kHz grid, and rank 1 exactly on its alternate members.
      long long off=s->low-10492750000LL;
      if(off<0 || off>6500000LL || (off%250000LL)!=0) ok=FALSE;
      int k=(int)(off/250000LL);
      if(s->rank!=((k%2==0)?1:0)) ok=FALSE;
      if(s->rank==1) narrow++;
      if(grid==0) first_grid=s->low;
      last_grid=s->low;
      grid++;
    }
    for(int k=0;k<3;k++) if(!wide_seen[k]) ok=FALSE;
    snprintf(d,sizeof(d),"%d grid (%.3f..%.3f), %d narrow, %d wide",
             grid,(double)first_grid/1e6,(double)last_grid/1e6,narrow,wide);
    check("wideband channels match the published table",
          ok && grid==27 && narrow==14 && wide==3 &&
          first_grid==10492750000LL && last_grid==10499250000LL, d);
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
    // ONE pair of rows covering BOTH transponders: bottom of the narrow one to
    // top of the wideband one. Anything narrower and the wideband dial cannot be
    // reached; anything split in two and the beacon lock's LNB measurement stops
    // reaching the wideband band (see qo100_create_transverters).
    gboolean good = ok && rb!=NULL && tb!=NULL &&
      rb->frequencyMin==QO100_NB_DOWN_LOW && rb->frequencyMax==QO100_WB_DOWN_HIGH &&
      rb->frequencyLO==QO100_DEFAULT_LNB_LO &&
      tb->frequencyMin==QO100_NB_DOWN_LOW-QO100_TP_OFFSET &&
      tb->frequencyMax==QO100_WB_DOWN_HIGH-QO100_TP_OFFSET &&
      tb->frequencyLO==1968000000LL &&
      // and the uplink band must be exactly the published 2400.000-2410.000
      tb->frequencyMin==2400000000LL && tb->frequencyMax==2410000000LL;
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
      if(f==QO100_WB_BEACON) clear=FALSE;
      if(!qo100_in_transponder(f)) clear=FALSE;
    }
    snprintf(d,sizeof(d),"%lld / %lld / %lld",
             rb?(long long)rb->bandstack->entry[0].frequency:0,
             rb?(long long)rb->bandstack->entry[1].frequency:0,
             rb?(long long)rb->bandstack->entry[2].frequency:0);
    check("band-stack lands on working spots, not beacons", clear, d);
    g_free(r);
  }

  // ---- 15. the wideband transponder: one press with it selected must land on
  //          the wideband downlink, not the narrow one, and pair it with the
  //          matching uplink. The two transponders share an offset, so the whole
  //          risk here is landing on the wrong one — which reads as "it works"
  //          on any check that only looks at the uplink arithmetic.
  {
    bands_init();
    RECEIVER *rx=mk_rx(14200000LL,FS);
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_lock=FALSE;
    r->qo100_transponder=QO100_TRANSPONDER_WB;
    radio=r;
    gboolean ok=qo100_transponder_setup(r);
    snprintf(d,sizeof(d),"A=%lld B=%lld split=%d",
             (long long)rx->frequency_a,(long long)rx->frequency_b,rx->split);
    check("wideband: setup lands on the WB downlink",
          ok && qo100_in_wb_transponder(rx->frequency_a) &&
          !qo100_in_nb_transponder(rx->frequency_a) &&
          rx->frequency_b==rx->frequency_a-QO100_TP_OFFSET &&
          rx->frequency_b>=2401000000LL && rx->frequency_b<=2410000000LL &&
          rx->split==SPLIT_SAT, d);
    g_free(rx); g_free(r); radio=NULL;
  }

  // ---- 16. ...and it must not land on the wideband BEACON, which is the one
  //          signal on that transponder nobody may sit on — the same mistake the
  //          narrow band-stack avoids, in the place it is easiest to make again.
  {
    bands_init();
    RECEIVER *rx=mk_rx(14200000LL,FS);
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_lock=FALSE;
    r->qo100_transponder=QO100_TRANSPONDER_WB;
    radio=r;
    qo100_transponder_setup(r);
    // 10490.5-10492.5 is the beacon-only section: not a place to transmit.
    gboolean clear=rx->frequency_a>10492500000LL;
    snprintf(d,sizeof(d),"A=%lld (beacon-only section ends 10492.500)",
             (long long)rx->frequency_a);
    check("wideband: lands clear of the beacon section", clear, d);
    g_free(rx); g_free(r); radio=NULL;
  }

  // ---- 17. switching transponders moves the operator. Being parked on the
  //          narrow transponder is NOT "already there" once wideband is asked
  //          for, and the ±50 kHz slack that keeps an operator on their own QSO
  //          must not be wide enough to bridge the half-megahertz between them.
  {
    bands_init();
    RADIO *r0=g_new0(RADIO,1);
    char msg[160];
    qo100_create_transverters(r0,msg,sizeof(msg));
    g_free(r0);
    RECEIVER *rx=mk_rx(10489980000LL,FS);      // top of the narrow transponder
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_lock=FALSE;
    r->qo100_transponder=QO100_TRANSPONDER_WB;
    radio=r;
    qo100_transponder_setup(r);
    snprintf(d,sizeof(d),"10489.980 -> %.3f MHz",(double)rx->frequency_a/1e6);
    check("wideband: a narrow-band dial is moved, not kept",
          qo100_in_wb_transponder(rx->frequency_a), d);
    g_free(rx); g_free(r); radio=NULL;
  }

  // ---- 18. the wideband beacon must never drive the lock. It is DVB-S2 — a
  //          suppressed carrier — so a peak search there measures the modulation
  //          and would walk the radio off frequency at whatever rate the picture
  //          happened to fade. Fed a strong clean carrier exactly where that
  //          beacon is, the loop must still refuse to lock or to retune.
  {
    bands_init();
    RECEIVER *rx=mk_rx(QO100_WB_BEACON-20000LL,FS);
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_sel=QO100_BEACON_SEL_WB;
    radio=r;
    test_band.frequencyLO=9750000000LL;
    test_band.errorLO=0;
    retunes=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    radio->qo100_beacon_sel=QO100_BEACON_SEL_WB;
    double *iq=g_new0(double,BLOCK*2);
    double phase=0.0; guint32 seed=4242;
    for(int b=0;b<blocks;b++) {
      gen_block(iq,BLOCK,20000.0,FS,1.0,0.0,&phase,&seed);   // a perfect carrier
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    char st[128];
    qo100_beacon_status(st,sizeof(st));
    snprintf(d,sizeof(d),"locked=%d retunes=%d \342\200\224 %s",
             qo100_beacon_locked(),retunes,st);
    check("WB beacon refused by the lock, radio untouched",
          !qo100_beacon_locked() && retunes==0 && rx->error_a==0, d);
    g_free(iq); g_free(rx); g_free(r); radio=NULL;
  }

  // ---- 19. the upgrade path. A receive row written before this application
  //          knew about the wideband transponder stops at 10490.000 and carries
  //          three narrow-band band-stack spots. Selecting wideband must widen it
  //          rather than quietly landing the operator back on the narrow
  //          transponder — and must keep the LO error, which is the beacon
  //          lock's measurement of that LNB and not a setting.
  {
    bands_init();
    RECEIVER *rx=mk_rx(10489700000LL,FS);
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_lock=FALSE;
    r->qo100_transponder=QO100_TRANSPONDER_WB;
    radio=r;
    // an "old" row, by hand: narrow edges, narrow spots, a measured LO error
    BAND *rb=&test_bands[BANDS];
    g_strlcpy(rb->title,QO100_XVTR_RX_TITLE,sizeof(rb->title));
    rb->frequencyMin=QO100_NB_DOWN_LOW;
    rb->frequencyMax=QO100_NB_DOWN_HIGH;
    rb->frequencyLO =QO100_DEFAULT_LNB_LO;
    rb->errorLO=-8321;
    for(int e=0;e<rb->bandstack->entries;e++) rb->bandstack->entry[e].frequency=10489800000LL;
    g_strlcpy(test_bands[BANDS+1].title,QO100_XVTR_TX_TITLE,sizeof(rb->title));
    test_bands[BANDS+1].frequencyMin=QO100_NB_DOWN_LOW-QO100_TP_OFFSET;
    test_bands[BANDS+1].frequencyMax=QO100_NB_DOWN_HIGH-QO100_TP_OFFSET;

    qo100_transponder_setup(r);
    snprintf(d,sizeof(d),"row now %lld..%lld, errorLO %lld, A=%lld",
             (long long)rb->frequencyMin,(long long)rb->frequencyMax,
             (long long)rb->errorLO,(long long)rx->frequency_a);
    check("old narrow-only converter row is widened, error kept",
          rb->frequencyMax==QO100_WB_DOWN_HIGH && rb->errorLO==-8321 &&
          qo100_in_wb_transponder(rx->frequency_a), d);
    g_free(rx); g_free(r); radio=NULL;
  }

  // ---- 20. the radio that never moves. A correction the hardware ignores is
  //          indistinguishable, frame by frame, from an LNB that drifted back by
  //          exactly as much — so the loop applies it again, and again, at up to
  //          20 kHz a frame into error_a and into the band's PERSISTED errorLO
  //          with it. It must stop instead, and say why. (This is the failure
  //          that was reported from air: ctun on, and the correction reaching
  //          error_a but never the radio.)
  {
    bands_init();
    gboolean locked=FALSE;
    deaf_radio=TRUE;
    // Long enough for the loop to lock (two measurements) and then be caught
    // out three times: each measurement integrates a second of stream and each
    // correction discards half a second after it.
    double res=run_loop(3000.0,0.0,blocks*6,&locked,FS,10489540000LL);
    deaf_radio=FALSE;
    double err=res-3000.0;                 // what the loop pushed into error_a
    char st[128];
    qo100_beacon_status(st,sizeof(st));
    snprintf(d,sizeof(d),"error_a %+.0f Hz, locked=%d — %s",err,locked,st);
    check("a radio that never moves stops the loop",
          fabs(err)<20000.0 && !locked && strstr(st,"Stopped")!=NULL, d);
    qo100_beacon_reset();
  }

  // ---- 21. the beacon's own ident must not be followed. F1A hops the carrier
  //          400 Hz, and the loop applied every post-lock reading, so it dragged
  //          the radio back and forth by that much: measured on the shipped loop
  //          at 400.9 Hz of movement inside a 15 s window, 0.6 retunes a second.
  //          Audible on SSB, and fatal to FT8 — ft8_lib does no drift tracking,
  //          so the frequency has to hold still across the whole slot.
  {
    bands_init();
    gboolean locked=FALSE;
    f1a_ident=TRUE;
    f1a_from_s=20.0;                       // acquisition is case 21g's business
    double res=run_loop(3000.0,0.0,blocks*9,&locked,FS,10489540000LL);
    f1a_from_s=0.0;
    f1a_ident=FALSE;
    snprintf(d,sizeof(d),"dragged %.1f Hz, %+.1f Hz left, locked=%d",
             worst_pull,res,locked);
    check("the beacon's F1A ident is not followed",
          locked && worst_pull<5.0 && fabs(res)<10.0, d);
    qo100_beacon_reset();
  }

  // ---- 21c. ...and the ident must not stop the loop TRACKING either. Refusing
  //           a bad reading is only half the job: while the beacon keys, the
  //           taller of its two lines is the keyed one, and a loop that measures
  //           whatever is tallest within 600 Hz of its beacon spends the whole
  //           ident 400 Hz out and refusing itself -- so nothing is trimmed for
  //           as long as the ident lasts, and a converter that is drifting
  //           meanwhile simply walks away. Once the loop is settled it knows
  //           where its beacon is to within a few hertz, so the NEAREST line is
  //           the beacon by construction and the tallest is only whatever is
  //           momentarily loudest beside it (see find_carrier's `dominant`).
  //           An LNB warming at 2 Hz/s under a beacon that idents 60 % of the
  //           time is the case that separates the two.
  {
    bands_init();
    gboolean locked=FALSE;
    f1a_ident=TRUE;
    f1a_from_s=20.0;                       // the loop is settled before it starts
    lo_drift_hz_s=2.0;
    double res=run_loop(3000.0,0.0,blocks*9,&locked,FS,10489540000LL);
    lo_drift_hz_s=0.0;
    f1a_from_s=0.0;
    f1a_ident=FALSE;
    snprintf(d,sizeof(d),"wandered %.1f Hz, %d retunes, locked=%d",
             worst_track,retunes,locked);
    check("a drifting LNB is still tracked through the ident",
          locked && worst_track<25.0, d);
    qo100_beacon_reset();
  }

  // ---- 21b. ...and neither must anything else that MOVES. Reported from air,
  //           with the log to go with it: a locked loop reading a steady -0.0 Hz
  //           walked out to -68, then -111.8, and applied the -111.8 as a coarse
  //           step -- the dial jumping a hundred hertz off a beacon it was
  //           sitting on. Nothing in it was a single bad reading: every step was
  //           inside the 20 Hz tracking tolerance and inside the 50 Hz slew
  //           guard, so a chain of them walked the loop as far as it liked. The
  //           three holes that made it possible are each fixed and each is
  //           exercised here (see qo100.c): agreement was chained to the
  //           PREVIOUS reading rather than to the run's own anchor; a reading
  //           the median threw out still moved the slew reference to itself; and
  //           the tallest-line-in-the-cluster rule kept following the mover even
  //           once the loop knew to within a few hertz where its beacon was.
  //
  //           The geometry that reproduces it: a station 6 dB louder than the
  //           beacon appears on top of it and tunes away at 20 Hz a second --
  //           one tracking tolerance per measurement, which is the fastest a
  //           chain can be walked -- then parks 400 Hz off, where the F1A ident
  //           puts its other line anyway.
  {
    bands_init();
    RECEIVER *rx=mk_rx(10489540000LL,FS);
    RADIO *r=mk_radio(rx);
    radio=r;
    test_band.frequencyLO=9750000000LL;
    test_band.errorLO=0;
    retunes=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    double *iq=g_new0(double,BLOCK*2);
    double *walker=g_new0(double,BLOCK*2);
    double p1=0.0,p2=0.0; guint32 s1=21,s2=22;
    long long beacon=qo100_beacon_frequency(0);
    const double lo_err=3000.0;
    const int settle=blocks*3;                  // lock and settle on the beacon
    const int total=settle+(int)(40.0*(double)FS/(double)BLOCK);
    long long settled_err=0;
    double drag=0.0;
    for(int b=0;b<total;b++) {
      double base=(double)(beacon-rx->frequency_a)-lo_err-(double)rx->error_a;
      gen_block(iq,BLOCK,base,FS,1.0,0.02,&p1,&s1);
      if(b>=settle) {
        if(b==settle) settled_err=rx->error_a;
        double t=(double)(b-settle)*(double)BLOCK/(double)FS;
        double off=20.0*t;                      // 20 Hz a second...
        if(off>400.0) off=400.0;                // ...and then it parks
        gen_block(walker,BLOCK,base+off,FS,2.0,0.0,&p2,&s2);
        for(int k=0;k<BLOCK*2;k++) iq[k]+=walker[k];
        double moved=fabs((double)(rx->error_a-settled_err));
        if(moved>drag) drag=moved;
      }
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    double left=lo_err+(double)rx->error_a;
    snprintf(d,sizeof(d),"dragged %.1f Hz, %+.1f Hz left, locked=%d",
             drag,left,qo100_beacon_locked());
    check("a signal walking across the beacon does not drag the lock",
          drag<30.0 && fabs(left)<30.0, d);
    g_free(iq); g_free(walker); g_free(rx); g_free(r); radio=NULL;
    qo100_beacon_reset();
  }


  // ---- 21d. a converter that DRIFTS must be trimmed, not jumped. The fine
  //           loop used to refuse any reading more than 3 Hz off the median of
  //           the last five -- and an LNB warming at 2 Hz/s puts every reading
  //           4 Hz off that median by arithmetic alone, so it refused the lot
  //           and nothing was corrected until the error passed
  //           QO100_COARSE_HZ, at which point the dial moved 100 Hz in one
  //           step. Measured on the shipped loop: 101 Hz of movement inside a
  //           15 s slot and 2 retunes a minute, against 32 Hz -- the drift
  //           itself -- and 30 now. The FT8 number is the slot: ft8_lib does no
  //           drift tracking, so what a decode feels is how far the dial moved
  //           while it was listening.
  {
    bands_init();
    gboolean locked=FALSE;
    lo_drift_hz_s=2.0;
    double res=run_loop(3000.0,0.0,blocks*12,&locked,FS,10489540000LL);
    lo_drift_hz_s=0.0;
    snprintf(d,sizeof(d),"%.0f Hz inside a slot, %.1f Hz of wander, %d retunes, "
             "locked=%d",worst_slot,worst_track,retunes,locked);
    check("a drifting LNB is trimmed, not jumped",
          locked && worst_slot<40.0 && worst_track<30.0 && retunes>10, d);
    qo100_beacon_reset();
  }

  // ---- 21e. ...and the other side of that trade, which is what the 3 Hz gate
  //           was buying: the reading itself wobbles on a fading path, several
  //           hertz from one integrated second to the next (a bin is 23 Hz at
  //           the 768 kHz span this runs at), and a loop that applies half of
  //           every wobble moves the operator's dial about for nothing. Acting
  //           on the median only when it beats the spread of the window behind
  //           it keeps both: +/-12 Hz of wobble moves the dial 0 Hz here, where
  //           acting on every median moved it 8.
  {
    bands_init();
    gboolean locked=FALSE;
    lo_wobble_hz=12.0;
    double res=run_loop(3000.0,0.0,blocks*9,&locked,FS,10489540000LL);
    lo_wobble_hz=0.0;
    snprintf(d,sizeof(d),"dial moved %.1f Hz, %+.1f Hz left, %d retunes, locked=%d",
             worst_pull,res,retunes,locked);
    check("a wobbling reading is not chased",
          locked && worst_pull<4.0 && fabs(res)<15.0, d);
    qo100_beacon_reset();
  }

  // ---- 21f. the two together, which is the case reported from air: an LNB
  //           warming fast under a beacon whose own line is absent for whole
  //           seconds at a time. F1A is two frequencies 400 Hz apart and
  //           neither is an idle carrier, so an integrated second often holds
  //           only the OTHER one -- "nearest of 1 lines" in the log, 400 Hz
  //           out. Refusing those is right; what must not happen is what did:
  //           each refusal froze the slew anchor while the converter kept
  //           moving, so when the beacon came back it was already further from
  //           that stale anchor than the guard allows, every later reading was
  //           refused too, and 30 refusals later the lock was dropped and
  //           re-acquired -- which applies the whole accumulated error as one
  //           coarse step. Measured on air: 25 s with no correction at all
  //           while the residual walked 114 -> 240 Hz, then a +691.9 Hz retune.
  {
    bands_init();
    gboolean locked=FALSE;
    f1a_ident=TRUE; f1a_shape=TRUE;
    f1a_from_s=20.0;
    lo_drift_hz_s=6.0;                     // measured on the operator's LNB
    double res=run_loop(3000.0,0.0,blocks*18,&locked,FS,10489540000LL);
    lo_drift_hz_s=0.0;
    f1a_from_s=0.0; f1a_shape=FALSE; f1a_ident=FALSE;
    snprintf(d,sizeof(d),"biggest step %.0f Hz, %.0f Hz of wander, %d retunes, "
             "locked=%d",worst_step,worst_track,retunes,locked);
    check("a fast-drifting LNB survives the ident",
          locked && worst_step<60.0 && worst_track<80.0, d);
    qo100_beacon_reset();
  }

  // ---- 21g. the dial is only truthful if the loop knows WHICH of the beacon's
  //           two tones is the published one, and that is not a matter of
  //           taste: IARU Region 1 publishes an FSK beacon's mark, the carrier
  //           rests there between messages and drops 400 Hz to the space while
  //           sending (AMSAT-DL forum, PA3FYM quoting the standard). So the
  //           mark is the tone that is on the air when the other is not, and a
  //           lock on the space is a dial 400 Hz off with nothing on air to say
  //           so -- which is what the operator's second log was.
  //           This starts the loop in the worst place for it: acquisition
  //           lands in a stretch where ONLY the space is transmitting, so it
  //           locks 400 Hz low and every guard in the file is then against
  //           moving that far. It has to end up on the mark anyway.
  {
    bands_init();
    gboolean locked=FALSE;
    f1a_ident=TRUE; f1a_shape=TRUE; f1a_from_s=0.0;
    double res=run_loop(3000.0,0.0,blocks*9,&locked,FS,10489540000LL);
    f1a_shape=FALSE; f1a_ident=FALSE;
    snprintf(d,sizeof(d),"%+.1f Hz from the published tone, %d retunes, locked=%d",
             res,retunes,locked);
    check("the lock lands on the published tone, not the space",
          locked && fabs(res)<10.0, d);
    qo100_beacon_reset();
  }

  // ---- 22. the LNB's error is the crystal's tolerance times 9750 MHz: 5 ppm is
  //          48.8 kHz, 20 ppm -- an ordinary consumer part -- is 195 kHz. The
  //          +/-60 kHz this used to hunt in could not see either, and the
  //          operator got "Searching for the beacon" for ever. Run it at a
  //          Pluto's own span, where the beacon is a quarter of a megahertz out.
  {
    bands_init();
    gboolean locked=FALSE;
    double res=run_loop(250000.0,0.0,blocks*8,&locked,2304000,10489540000LL);
    snprintf(d,sizeof(d),"250 kHz -> %+.1f Hz left, locked=%d",res,locked);
    check("a 250 kHz LNB error is still acquired",
          locked && fabs(res)<10.0, d);
    qo100_beacon_reset();
  }

  // ---- 22b. ...but a strong station well outside any LNB's error must NOT be
  //           taken for the beacon. Measured on a real dish: with the beacon
  //           64.8 kHz out, a carrier 398 kHz from the expectation was STRONGER
  //           (1353x the window mean against 900x), and a sweep of the whole
  //           span picked it often enough to reset the agreement run for ever.
  {
    bands_init();
    RECEIVER *rx=mk_rx(10489540000LL,1920000);
    RADIO *r=mk_radio(rx);
    radio=r;
    test_band.frequencyLO=9750000000LL;
    test_band.errorLO=0;
    retunes=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    double *iq=g_new0(double,BLOCK*2);
    double phase=0.0; guint32 seed=99;
    long long beacon=qo100_beacon_frequency(0);
    // Only the interloper: 398 kHz from where the beacon is expected, and the
    // strongest thing in the span. Nothing may lock to it.
    double interloper=(double)(beacon-rx->frequency_a)+398000.0;
    for(int b=0;b<blocks*2;b++) {
      gen_block(iq,BLOCK,interloper,1920000,1.0,0.05,&phase,&seed);
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    snprintf(d,sizeof(d),"locked=%d retunes=%d error_a=%lld",
             qo100_beacon_locked(),retunes,(long long)rx->error_a);
    check("a strong station 398 kHz away is not the beacon",
          !qo100_beacon_locked() && retunes==0 && rx->error_a==0, d);
    g_free(iq); g_free(rx); g_free(r); radio=NULL;
    qo100_beacon_reset();
  }

  // ---- 22c. the transponder is BUSY -- that is what it is for -- so the beacon
  //           is routinely not the loudest thing in the window. Measured on the
  //           operator's dish: carriers at 10489.336, .432 and .440 MHz all
  //           louder than the beacon at .500, picked in turn frame after frame,
  //           so the agreement run never lasted a fifth of a second. The line
  //           NEAREST the expectation is the beacon; which is loudest says
  //           nothing about it.
  {
    bands_init();
    RECEIVER *rx=mk_rx(10489540000LL,768000);
    RADIO *r=mk_radio(rx);
    radio=r;
    test_band.frequencyLO=9750000000LL;
    test_band.errorLO=0;
    retunes=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    double *iq=g_new0(double,BLOCK*2);
    double *hog=g_new0(double,BLOCK*2);
    double ph1=0.0, ph2=0.0; guint32 s1=7, s2=8;
    long long beacon=qo100_beacon_frequency(0);
    const double lo_err=3000.0;                       // a sane LNB, 3 kHz out
    for(int b=0;b<blocks*3;b++) {
      double base=(double)(beacon-rx->frequency_a)-lo_err-(double)rx->error_a;
      gen_block(iq,BLOCK,base,768000,1.0,0.05,&ph1,&s1);          // the beacon
      gen_block(hog,BLOCK,base-50000.0,768000,3.0,0.0,&ph2,&s2);  // 9.5 dB louder
      for(int k=0;k<BLOCK*2;k++) iq[k]+=hog[k];
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    double left=lo_err+(double)rx->error_a;
    snprintf(d,sizeof(d),"%+.1f Hz left, locked=%d (a 9.5 dB louder station sat "
             "50 kHz away)",left,qo100_beacon_locked());
    check("the loudest line is not the beacon",
          qo100_beacon_locked() && fabs(left)<10.0, d);
    g_free(iq); g_free(hog); g_free(rx); g_free(r); radio=NULL;
    qo100_beacon_reset();
  }

  // ---- 22e. the operator's own dish, reproduced: a converter 350 kHz out, so
  //           the whole transponder is drawn 350 kHz low and the frequency the
  //           settings call "the beacon" holds somebody's QSO instead -- louder
  //           than the beacon, and the loop locked onto it every time. What
  //           identifies a beacon absolutely is that the two CW ones ARE the
  //           transponder's edges, exactly 500.000 kHz apart and both
  //           continuous. Nothing else on the band holds that spacing.
  {
    bands_init();
    RECEIVER *rx=mk_rx(10489313792LL,768000);
    RADIO *r=mk_radio(rx);
    radio=r;
    test_band.frequencyLO=9750000000LL;
    test_band.errorLO=0;
    retunes=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    double *iq=g_new0(double,BLOCK*2);
    double *t2=g_new0(double,BLOCK*2);
    double p1=0.0,p2=0.0,p3=0.0; guint32 s1=11,s2=12,s3=13;
    long long beacon=qo100_beacon_frequency(0);
    const double lo_err=350000.0;
    const double expected=(double)(beacon-rx->frequency_a);
    for(int b=0;b<blocks*12;b++) {
      double low=expected-lo_err-(double)rx->error_a;      // the lower beacon
      gen_block(iq,BLOCK,low,768000,1.0,0.05,&p1,&s1);
      gen_block(t2,BLOCK,low+500000.0,768000,1.0,0.0,&p2,&s2);   // the upper one
      for(int k=0;k<BLOCK*2;k++) iq[k]+=t2[k];
      // ...and a QSO sitting exactly where the settings say the beacon is,
      // 9.5 dB louder than it.
      gen_block(t2,BLOCK,expected-(double)rx->error_a,768000,3.0,0.0,&p3,&s3);
      for(int k=0;k<BLOCK*2;k++) iq[k]+=t2[k];
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    double left=lo_err+(double)rx->error_a;
    snprintf(d,sizeof(d),"350 kHz out, a louder QSO on the expectation -> "
             "%+.1f Hz left, locked=%d",left,qo100_beacon_locked());
    check("the two CW beacons are 500 kHz apart, and that is the tell",
          qo100_beacon_locked() && fabs(left)<50.0, d);
    g_free(iq); g_free(t2); g_free(rx); g_free(r); radio=NULL;
    qo100_beacon_reset();
  }

  // ---- 22d. a lock is a claim about where the beacon IS, and once settled the
  //           search narrows to +/-2 kHz around it. If the thing it locked to
  //           then goes -- it was never the beacon, or the transponder went
  //           quiet -- that window is a slit with nothing in it. Measured on the
  //           operator's radio: two minutes of "0 candidate lines" in a row
  //           while the real beacon sat 14 kHz away, because nothing ever let
  //           the lock go.
  {
    bands_init();
    RECEIVER *rx=mk_rx(10489540000LL,FS);
    RADIO *r=mk_radio(rx);
    radio=r;
    test_band.frequencyLO=9750000000LL;
    test_band.errorLO=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    double *iq=g_new0(double,BLOCK*2);
    double phase=0.0; guint32 seed=31;
    long long beacon=qo100_beacon_frequency(0);
    // Lock onto an impostor sitting 500 Hz from where the beacon is expected --
    // close enough to be declared settled, which is what narrows the window.
    for(int b=0;b<blocks;b++) {
      gen_block(iq,BLOCK,(double)(beacon-rx->frequency_a)+500.0-(double)rx->error_a,
                FS,1.0,0.05,&phase,&seed);
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    gboolean was=qo100_beacon_locked();
    // ...then it stops transmitting, and the REAL beacon is 14 kHz away, far
    // outside the tracking window.
    for(int b=0;b<blocks*8;b++) {
      gen_block(iq,BLOCK,(double)(beacon-rx->frequency_a)+14000.0-(double)rx->error_a,
                FS,1.0,0.05,&phase,&seed);
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    char st[128];
    qo100_beacon_status(st,sizeof(st));
    // The loop drives error_a to the offset it is looking at, so following the
    // real line means error_a == 14000.
    double left=(double)rx->error_a-14000.0;
    snprintf(d,sizeof(d),"locked first=%d, now %+.0f Hz from the real line — %s",
             was,left,st);
    check("a lock whose carrier vanishes is given up",
          was && fabs(left)<200.0, d);
    g_free(iq); g_free(rx); g_free(r); radio=NULL;
    qo100_beacon_reset();
  }

  // ---- 22f. a correction is a STEP in the audio, and ft8_lib demodulates each
  //           candidate at a fixed frequency -- it does no drift tracking -- so a
  //           step inside a transmission costs that decode. With FT8 selected the
  //           loop must wait for the slot's quiet tail. Deterministic: the case
  //           starts by waiting for the BUSY part of a slot, which is where 89 %
  //           of wall-clock time is anyway, and the run takes about a second.
  {
    bands_init();
    for(;;) {
      double utc=(double)(g_get_real_time()/1000)/1000.0;
      double ph=fmod(utc,15.0);
      if(ph<10.0) break;                   // room for the run to finish inside
      g_usleep(200000);
    }
    RECEIVER *rx=mk_rx(10489540000LL,FS);
    rx->mode_a=DIGU;                       // the decoder taps only in DIGU/DIGL
    RADIO *r=mk_radio(rx);
    r->decode_mode=DECODE_FT8;
    radio=r;
    test_band.frequencyLO=9750000000LL;
    test_band.errorLO=0;
    retunes=0;
    qo100_beacon_reset();
    radio->qo100_beacon_lock=TRUE;
    radio->decode_mode=DECODE_FT8;
    double *iq=g_new0(double,BLOCK*2);
    double phase=0.0; guint32 seed=77;
    long long beacon=qo100_beacon_frequency(0);
    for(int b=0;b<blocks*3;b++) {
      gen_block(iq,BLOCK,(double)(beacon-rx->frequency_a)-3000.0-(double)rx->error_a,
                FS,1.0,0.05,&phase,&seed);
      qo100_beacon_iq_feed(rx,iq,BLOCK);
      while(g_main_context_iteration(NULL,FALSE)) ;
    }
    char st[128];
    qo100_beacon_status(st,sizeof(st));
    snprintf(d,sizeof(d),"retunes=%d error_a=%lld — %s",
             retunes,(long long)rx->error_a,st);
    check("no retune inside an FT8 transmission",
          retunes==0 && rx->error_a==0 && strstr(st,"holding")!=NULL, d);
    g_free(iq); g_free(rx); g_free(r); radio=NULL;
    qo100_beacon_reset();
  }

  // ---- 23. the search band is the RECEIVER's, not a window around the guess.
  //          With the beacon expected 40 kHz below centre, a symmetric window
  //          reached -86 kHz on one side and +6 kHz on the other -- so an LNB
  //          erring the other way put the carrier inside the SPAN and outside
  //          the SEARCH, which reads as a beacon that is not there.
  {
    bands_init();
    gboolean locked=FALSE;
    // With noise, so the case cannot be passed by a Hann sidelobe leaking into
    // the window edge: on a noiseless carrier the old symmetric window found its
    // own leakage at +6.4 kHz, stepped 46 kHz on that, and stumbled into the
    // real line from there.
    double res=run_loop(-60000.0,1.0,blocks*2,&locked,FS,10489540000LL);
    snprintf(d,sizeof(d),"-60 kHz (beacon lands +20 kHz) -> %+.1f Hz, locked=%d",
             res,locked);
    check("a carrier on the far side of centre is found",
          locked && fabs(res)<10.0, d);
    qo100_beacon_reset();
  }

  // ---- 24. a radio that answers LATE must not be mistaken for one that never
  //          answers. A correction takes the driver's queued transfers, the FIFO
  //          in front of the DSP thread and the retune itself to come back, and
  //          at a 2 304 000 span -- a Pluto's own rate -- an FFT frame is 14 ms,
  //          so the pipeline is ten of them. Without the post-correction hold the
  //          loop reads its own stale frames, corrects twice for one error, and
  //          latches the stuck guard on a radio that is working.
  {
    bands_init();
    gboolean locked=FALSE;
    lag_blocks=48;                       // ~0.5 s at 192 kHz: three FFT frames
    double res=run_loop(3000.0,0.0,blocks*2,&locked,FS,10489540000LL);
    lag_blocks=0;
    char st[128];
    qo100_beacon_status(st,sizeof(st));
    snprintf(d,sizeof(d),"%+.1f Hz left, locked=%d — %s",res,locked,st);
    check("a radio that answers late still converges",
          locked && fabs(res)<10.0 && strstr(st,"Stopped")==NULL, d);
    qo100_beacon_reset();
  }

  // ---- 23. ctun/freetune: the dial the operator reads is the CURSOR, so that
  //          is what the uplink must be paired with. Anchoring VFO B to the span
  //          centre put it half a span out — 60 kHz here, on a transponder
  //          500 kHz wide — and SAT split then carried the error along for ever.
  {
    bands_init();
    RADIO *r0=g_new0(RADIO,1);
    char msg[160];
    qo100_create_transverters(r0,msg,sizeof(msg));
    g_free(r0);
    RECEIVER *rx=mk_rx(10489650000LL,FS);      // span centre
    rx->ctun=TRUE;
    rx->ctun_frequency=10489710000LL;          // ...and the cursor, 60 kHz up
    RADIO *r=mk_radio(rx);
    r->qo100_beacon_lock=FALSE;
    radio=r;
    qo100_transponder_setup(r);
    snprintf(d,sizeof(d),"cursor %lld -> B %lld",
             (long long)rx->ctun_frequency,(long long)rx->frequency_b);
    check("ctun: the uplink pairs with the cursor",
          rx->frequency_b==10489710000LL-QO100_TP_OFFSET &&
          rx->ctun_frequency==10489710000LL, d);
    g_free(rx); g_free(r); radio=NULL;
  }

  printf("\n%s\n", failures==0 ? "all cases passed" : "FAILURES ABOVE");
  return failures==0 ? 0 : 1;
}
