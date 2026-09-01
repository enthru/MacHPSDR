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
#include "filter.h"
#include "log.h"
#include "qo100.h"

extern RADIO *radio;   // global application state

// ---------------------------------------------------------------------------
// Band plan
// ---------------------------------------------------------------------------
//
// BOTH of the satellite's amateur transponders, transcribed from AMSAT-DL's own
// published plans:
//
//   * the narrow-band plan (rev5 of 14 Feb 2020) together with the broadcast,
//     emergency and multimedia-beacon slots added to the top segment afterwards.
//     Checked against the source on 2026-08-09; the two renderings that carry it
//     (the PDF and the AMSAT-DL page) agree on everything they share.
//   * the wideband (DATV) plan, "QO-100 Wideband Transponder — 2021 Operating
//     Guidelines and Bandplan", version 3 final of 6 Feb 2021, cross-checked on
//     2026-08-23 against the BATC wiki's rendering of the same document (its
//     channel table and its bandplan graphic, which is where the section edges
//     are — the PDF states them only in the picture).
//
// This is one table and the plan lives nowhere else — if AMSAT-DL revises it,
// edit here.  What is worth knowing before doing so: the plans HAVE been revised,
// substantially, and the superseded figures for both are still in wide
// circulation.  The narrow transponder was 250 kHz wide at launch and was
// extended to 500 kHz in Feb 2020; the wideband one is still widely quoted as
// "8 MHz, 10491.0–10499.0", which is neither what V3 allocates nor what it names
// as the transponder (10490.5–10499.5).  A source that disagrees is far more
// likely to be old than right.
//
// One table for two transponders is deliberate and costs nothing: the plans
// cannot overlap (narrow ends at 10490.000, wideband starts at 10490.500), so
// the overlay draws whichever the dial is in front of and there is no mode to
// get wrong.
//
// Spans are drawn as tinted bands under the trace, beacons as full-height
// markers and DATV channels as ticks above the strip — so the operator can see
// at a glance that they are calling CQ in the CW section, or that their picture
// is sitting between two channels rather than on one.

static const QO100_SEGMENT segments[] = {
  /* --- narrow-band transponder, 10489.500…10490.000 --- */
  { QO100_BEACON_LOWER,  QO100_BEACON_LOWER,  "Beacon",   1.00, 0.85, 0.20, QO100_SEG_BEACON, 0 },
  { 10489505000LL, 10489540000LL, "CW",        0.30, 0.75, 1.00, QO100_SEG_SPAN, 0 },
  { 10489540000LL, 10489580000LL, "Digi 500",  0.55, 0.45, 1.00, QO100_SEG_SPAN, 0 },
  { 10489580000LL, 10489650000LL, "Digi 2k7",  0.55, 0.45, 1.00, QO100_SEG_SPAN, 0 },
  { 10489650000LL, 10489745000LL, "SSB",       0.30, 1.00, 0.50, QO100_SEG_SPAN, 0 },
  { QO100_BEACON_MIDDLE, QO100_BEACON_MIDDLE, "Beacon",   1.00, 0.85, 0.20, QO100_SEG_BEACON, 0 },
  { 10489755000LL, 10489850000LL, "SSB",       0.30, 1.00, 0.50, QO100_SEG_SPAN, 0 },
  { 10489850000LL, 10489858000LL, "News",      0.40, 0.80, 0.90, QO100_SEG_SPAN, 0 },
  { 10489858000LL, 10489865000LL, "Emerg",     1.00, 0.45, 0.35, QO100_SEG_SPAN, 0 },
  { 10489865000LL, 10489990000LL, "Mixed",     0.60, 0.60, 0.60, QO100_SEG_SPAN, 0 },
  { 10489990000LL, 10489997000LL, "MM bcn",    1.00, 0.85, 0.20, QO100_SEG_SPAN, 0 },
  { QO100_BEACON_UPPER,  QO100_BEACON_UPPER,  "Beacon",   1.00, 0.85, 0.20, QO100_SEG_BEACON, 0 },

  /* --- wideband (DATV) transponder, 10490.500…10499.500 ---
     The four sections of the V3 plan, in its own words: beacon only; all DATV
     modes and symbol rates (this is the 1.5 MHz where DVB-T and other
     experiments are allowed); DVB-S/S2 at any symbol rate; and 333 kS and
     lower.  10494.000…10497.000 doubles as the occasional maintenance uplink,
     which users are asked to give absolute priority — it is not drawn
     separately because it is not a standing allocation. */
  { QO100_WB_BEACON,     QO100_WB_BEACON,     "WB beacon", 1.00, 0.85, 0.20, QO100_SEG_BEACON, 0 },
  { 10490500000LL, 10492500000LL, "Beacon only", 1.00, 0.85, 0.20, QO100_SEG_SPAN, 0 },
  { 10492500000LL, 10494000000LL, "All modes",   0.55, 0.45, 1.00, QO100_SEG_SPAN, 0 },
  { 10494000000LL, 10497000000LL, "DVB-S/S2",    0.30, 0.70, 1.00, QO100_SEG_SPAN, 0 },
  { 10497000000LL, 10499500000LL, "333 kS max",  0.30, 1.00, 0.50, QO100_SEG_SPAN, 0 },

  /* The three wide channels: 1 MS and 1.5 MS transmissions centre here. */
  { 10493250000LL, 10493250000LL, "1MS", 0.55, 0.80, 1.00, QO100_SEG_CHANNEL, 2 },
  { 10494750000LL, 10494750000LL, "1MS", 0.55, 0.80, 1.00, QO100_SEG_CHANNEL, 2 },
  { 10496250000LL, 10496250000LL, "1MS", 0.55, 0.80, 1.00, QO100_SEG_CHANNEL, 2 },

  /* The narrow grid.  The V3 plan lists two overlapping channel sets and they
     are ONE grid: the 27 "very narrow" channels (125/66/33 kS) run from
     10492.750 in 250 kHz steps, and the 14 "narrow" channels (500/333/250 kS)
     are its alternate members — every *.250 and *.750 — in 500 kHz steps over
     the same range.  So they are written once, with rank 1 marking the members
     that are also a 333 kS channel; qo100_offline checks the arithmetic against
     the published table rather than trusting the transcription.

     No labels on these: the overlay gives each rank its own row, which says what
     a tick is without thirty pieces of text over the trace, and only the three
     1 MS channels are named. */
  { 10492750000LL, 10492750000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10493000000LL, 10493000000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10493250000LL, 10493250000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10493500000LL, 10493500000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10493750000LL, 10493750000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10494000000LL, 10494000000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10494250000LL, 10494250000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10494500000LL, 10494500000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10494750000LL, 10494750000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10495000000LL, 10495000000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10495250000LL, 10495250000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10495500000LL, 10495500000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10495750000LL, 10495750000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10496000000LL, 10496000000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10496250000LL, 10496250000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10496500000LL, 10496500000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10496750000LL, 10496750000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10497000000LL, 10497000000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10497250000LL, 10497250000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10497500000LL, 10497500000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10497750000LL, 10497750000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10498000000LL, 10498000000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10498250000LL, 10498250000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10498500000LL, 10498500000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10498750000LL, 10498750000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
  { 10499000000LL, 10499000000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 0 },
  { 10499250000LL, 10499250000LL, NULL,  0.45, 0.65, 1.00, QO100_SEG_CHANNEL, 1 },
};

int qo100_segment_count(void) {
  return (int)(sizeof(segments)/sizeof(segments[0]));
}

const QO100_SEGMENT *qo100_segment(int index) {
  if(index<0 || index>=qo100_segment_count()) return NULL;
  return &segments[index];
}

gboolean qo100_in_nb_transponder(long long f) {
  return f>=QO100_NB_DOWN_LOW && f<=QO100_NB_DOWN_HIGH;
}

gboolean qo100_in_wb_transponder(long long f) {
  return f>=QO100_WB_DOWN_LOW && f<=QO100_WB_DOWN_HIGH;
}

gboolean qo100_in_transponder(long long f) {
  return qo100_in_nb_transponder(f) || qo100_in_wb_transponder(f);
}

long long qo100_beacon_frequency(int sel) {
  // Only the two CW beacons are offered.  The middle beacon is 400 bd BPSK,
  // which is a SUPPRESSED-carrier signal — there is no spectral line for a
  // carrier tracker to lock to, and peak-picking its sidebands would measure the
  // modulation rather than the LNB.  It is still drawn in the band plan above.
  //
  // CAVEAT, not yet characterised against the real satellite: the lower beacon
  // is F1A — frequency-shift keyed, 400 Hz shift — so while it sends its ident
  // the line moves between two frequencies 400 Hz apart, and the published
  // figure does not say which of them it names.  The loop degrades safely
  // (acquisition needs three frames agreeing within LOCK_TOL 200 Hz, so a shift
  // simply stops the update rather than dragging it), but whether the settled
  // reading lands on the nominal frequency or 400 Hz off it is unknown until
  // this is used on air.
  //
  // The WIDEBAND beacon is here for a different job entirely.  It is DVB-S2 and
  // so has no carrier either, but it is the reference the wideband transponder's
  // power rule is written against ("at least 1 dB below the beacon"), and while
  // the operator is on that transponder it is the only beacon anywhere near the
  // span — the CW beacons are one to nine megahertz below it, which no span this
  // application can open will reach.  So it is offered for the panadapter's
  // level line and refused by the lock; qo100_beacon_has_carrier() is the split.
  switch(sel) {
    case QO100_BEACON_SEL_UPPER: return QO100_BEACON_UPPER;
    case QO100_BEACON_SEL_WB:    return QO100_WB_BEACON;
    default:                     return QO100_BEACON_LOWER;
  }
}

gboolean qo100_beacon_has_carrier(int sel) {
  return sel!=QO100_BEACON_SEL_WB;
}

// ---------------------------------------------------------------------------
// Converters
// ---------------------------------------------------------------------------
//
// Typing two transverter rows by hand is exactly the friction this page exists
// to remove, and it is also where the numbers get fat-fingered: the band edges
// and the LO have to agree or the radio is commanded to a nonsense IF and
// nothing at all is heard, with no clue as to why.  Only the two LOs are the
// operator's own hardware; everything else follows from the band plan.

// Three working spots for the band-stack, as downlink frequencies: the narrow
// modes near the bottom, the SSB stretch below the middle beacon, and one on the
// wideband transponder.
//
// This is the one place that does NOT follow save_xvtr()'s convention, and
// deliberately: that seeds min / middle / max, which here would be the lower
// beacon, the middle beacon and the upper beacon — the three frequencies an
// operator must specifically NOT transmit on. Landing a band-stack recall on a
// beacon is worse than any consistency argument for keeping the convention.
//
// The last entry is the wideband transponder's first wide channel, and the two
// indices below are what qo100_transponder_setup() recalls for each transponder.
static const long long qo100_stack_rx[3] = {
  10489560000LL,   // CW / narrow digimodes
  10489700000LL,   // SSB, below the middle beacon
  10493250000LL,   // wideband: wide channel 1 (1 MS)
};
#define QO100_STACK_NB  1        // the entry "set up transponder mode" recalls...
#define QO100_STACK_WB  2        // ...for each transponder

// Fill one transverter band. Otherwise mirrors save_xvtr() in xvtr_dialog.c, so
// an entry made here is indistinguishable from a hand-typed one.
static void fill_xvtr(BAND *b, const char *name,
                      long long fmin, long long fmax, long long lo,
                      long long shift) {
  g_strlcpy(b->title,name,sizeof(b->title));
  b->frequencyMin=fmin;
  b->frequencyMax=fmax;
  b->frequencyLO=lo;
  BANDSTACK *bs=b->bandstack;
  if(bs!=NULL) {
    for(int i=0;i<bs->entries;i++) {
      BANDSTACK_ENTRY *e=&bs->entry[i];
      long long f=qo100_stack_rx[(i<3)?i:2]-shift;   // shift = 0 for RX, the offset for TX
      if(f<fmin) f=fmin;
      if(f>fmax) f=fmax;
      e->frequency=f;
      e->mode=USB;          // the transponder is non-inverting: USB both ways
      e->filter=F6;
    }
  }
}

// Find the slot already holding this entry, else the first empty one, else -1.
static int find_xvtr_slot(const char *name) {
  int free_slot=-1;
  for(int i=BANDS;i<BANDS+XVTRS;i++) {
    BAND *b=band_get_band(i);
    if(b==NULL) continue;
    if(strcmp(b->title,name)==0) return i;
    if(free_slot<0 && strlen(b->title)==0) free_slot=i;
  }
  return free_slot;
}

gboolean qo100_create_transverters(RADIO *r, char *msg, int msgsz) {
  if(r==NULL) return FALSE;

  int rxs=find_xvtr_slot(QO100_XVTR_RX_TITLE);
  int txs=find_xvtr_slot(QO100_XVTR_TX_TITLE);
  // Both names are resolved before anything is written, and the two must not
  // land in the same empty slot.
  if(rxs>=0 && txs==rxs) {
    txs=-1;
    for(int i=BANDS;i<BANDS+XVTRS;i++) {
      BAND *b=band_get_band(i);
      if(i!=rxs && b!=NULL && strlen(b->title)==0) { txs=i; break; }
    }
  }
  if(rxs<0 || txs<0) {
    if(msg!=NULL) snprintf(msg,msgsz,
      "No free transverter slots — clear two rows above first");
    return FALSE;
  }

  long long lnb=(r->qo100_lnb_lo!=0)?r->qo100_lnb_lo:QO100_DEFAULT_LNB_LO;
  long long txlo=r->qo100_tx_lo;      // 0 is legitimate: the radio reaches 2.4 GHz itself
  long long offset=(r->qo100_offset!=0)?r->qo100_offset:QO100_TP_OFFSET;

  BAND *rb=band_get_band(rxs), *tb=band_get_band(txs);
  // Keep whatever LO error is already there: on the receive entry that is the
  // beacon lock's accumulated measurement of this very LNB, and throwing it away
  // because the operator pressed the button again would be the one destructive
  // thing this function could do.
  long long rx_err=rb->errorLO, tx_err=tb->errorLO;

  // ONE pair of entries covers BOTH transponders — from the bottom of the narrow
  // one to the top of the wideband one — rather than a pair each.  Two reasons,
  // and the second is the load-bearing one:
  //
  //   * there is one dish, one LNB and one uplink converter, so there is one
  //     conversion; splitting it into two bands would be describing the same
  //     hardware twice, in half the transverter slots the operator has;
  //   * the beacon lock writes its measurement into the LO error of THE BAND THE
  //     RECEIVER IS ON.  With a separate wideband band that band would never
  //     receive a correction — the lock cannot run there at all, the CW beacons
  //     being megahertz outside any span this application can open — so the
  //     wideband dial would stay as wrong as the LNB is, while the narrow one
  //     read true.  Sharing the band shares the measurement, which is right,
  //     because it is a measurement of the LNB and not of a band.
  //
  // The half-megahertz gap between the two transponders comes along with it. It
  // is not amateur allocation, so nothing is drawn there; it is also not
  // reachable by accident, since tuning through it is a deliberate act.
  long long down_low =QO100_NB_DOWN_LOW;
  long long down_high=QO100_WB_DOWN_HIGH;

  fill_xvtr(rb,QO100_XVTR_RX_TITLE,down_low,down_high,lnb,0);
  fill_xvtr(tb,QO100_XVTR_TX_TITLE,down_low-offset,down_high-offset,txlo,offset);
  rb->errorLO=rx_err;
  tb->errorLO=tx_err;

  if(msg!=NULL) snprintf(msg,msgsz,
    "RX %.3f\342\200\223%.3f MHz (LO %.3f) \342\200\242 TX %.3f\342\200\223%.3f MHz (LO %.3f)",
    (double)down_low/1e6,(double)down_high/1e6,(double)lnb/1e6,
    (double)(down_low-offset)/1e6,(double)(down_high-offset)/1e6,
    (double)txlo/1e6);
  log_info("qo100: transverters in slots %d/%d, LNB LO %lld Hz, uplink LO %lld Hz\n",
           rxs,txs,lnb,txlo);
  return TRUE;
}

// ---------------------------------------------------------------------------
// Transponder mode
// ---------------------------------------------------------------------------

// The frequency the operator is actually listening on: under ctun or freetune
// that is the CURSOR, not the span centre -- the same rule vfo_apply_frequency()
// and receiver_notch_anchor() follow. Anchoring the uplink to frequency_a put
// VFO B up to half a span away from the downlink being listened to (~90 kHz on a
// 192 kHz span, on a transponder 500 kHz wide), and SAT split then carried that
// error along for ever, since it moves both VFOs by the same delta.
static long long qo100_dial(RECEIVER *rx) {
  return (rx->ctun || rx->freetune) ? rx->ctun_frequency : rx->frequency_a;
}

gboolean qo100_transponder_setup(RADIO *r) {
  if(r==NULL) return FALSE;
  RECEIVER *rx=r->active_receiver;
  if(rx==NULL) return FALSE;

  // Do the whole job rather than refusing half of it. An earlier version bailed
  // out unless the operator had already tuned to the downlink and built the two
  // transverters by hand — but "set up transponder mode" is an unambiguous
  // instruction, and there is nothing here the button cannot do itself:
  //
  //   * the converters are a known shape once the two LOs are known, and those
  //     have sensible defaults (a standard LNB, and no uplink converter);
  //   * the tuning ceiling only rises once a transverter covering 10.49 GHz
  //     exists (see receiver_max_frequency), so the converters MUST be created
  //     before the dial is moved — which is exactly the ordering below and the
  //     reason doing this by hand was easy to get stuck on;
  //   * set_band() then restores the band-stack entry, which carries the right
  //     LO, mode and filter with it.
  // Which transponder the operator is set up for. This is the ONLY thing the
  // selection changes: the band plan on the panadapter follows the dial, and the
  // converters cover both, so nothing else has to be switched.
  gboolean wb=(r->qo100_transponder==QO100_TRANSPONDER_WB);
  long long tp_low =wb?QO100_WB_DOWN_LOW :QO100_NB_DOWN_LOW;
  long long tp_high=wb?QO100_WB_DOWN_HIGH:QO100_NB_DOWN_HIGH;

  int rxs=find_xvtr_slot(QO100_XVTR_RX_TITLE);
  BAND *rb=(rxs>=0)?band_get_band(rxs):NULL;
  // Rebuild the rows when they are missing OR when they do not reach the
  // transponder that was asked for. The second half is the upgrade path and it
  // is not hypothetical: a receive row written before this application knew about
  // the wideband transponder stops at 10490.000, and its band-stack entries are
  // all narrow-band spots — so without this, selecting Wideband would quietly
  // land the operator back on the narrow transponder and look like a dead
  // feature. Rewriting keeps the LO error, which is the one thing in those rows
  // that is a measurement rather than a setting.
  if(rb==NULL || strcmp(rb->title,QO100_XVTR_RX_TITLE)!=0 ||
     rb->frequencyMin>tp_low || rb->frequencyMax<tp_high) {
    if(!qo100_create_transverters(r,NULL,0)) return FALSE;   // no free slots
    rxs=find_xvtr_slot(QO100_XVTR_RX_TITLE);
    rb=(rxs>=0)?band_get_band(rxs):NULL;
    if(rb==NULL) return FALSE;
  }

  // A little slack past each edge so being parked on a beacon still counts as
  // "already there" and the operator's own frequency is not thrown away. The
  // slack cannot make the two transponders meet: they are half a megahertz
  // apart, ten times this.
  const long long slack=50000LL;
  long long dial=qo100_dial(rx);
  if(dial < tp_low-slack || dial > tp_high+slack) {
    // For the narrow transponder, band-stack entry 1 is the SSB stretch below
    // the middle beacon — a place to land that is neither a beacon nor the CW
    // section. For the wideband one it is the first wide channel, which is where
    // a 1 MS picture belongs; landing on the wideband BEACON would be the same
    // mistake, since it is the one signal on that transponder nobody may sit on.
    set_band(rx,rxs,wb?QO100_STACK_WB:QO100_STACK_NB);
  }

  long long offset=(r->qo100_offset!=0) ? r->qo100_offset : QO100_TP_OFFSET;
  // Re-read the dial: set_band() may have moved it just above, and it clears
  // ctun on the way through.
  dial=qo100_dial(rx);
  rx->frequency_b=dial-offset;

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

  log_info("qo100: %s transponder mode, downlink %lld Hz -> uplink %lld Hz (offset %lld Hz)\n",
           wb?"wideband":"narrow-band",
           (long long)dial,(long long)rx->frequency_b,offset);
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
// How far off a converter may plausibly be, and therefore how far acquisition
// looks: an LNB's error is its crystal's tolerance times 9750 MHz, so 30 ppm --
// worse than any consumer part has a right to be -- is 292 kHz. Sweeping the
// WHOLE span instead was tried and is worse: on a real dish it found a station
// 398 kHz from the expectation, stronger than the beacon (1353x the window mean
// against 900x), and every frame that picked it reset the agreement run.
#define QO100_ACQ_HZ      500000.0   // acquisition half-width around the expectation
#define QO100_PAIR_TOL_HZ   1000.0   // how exactly the two CW beacons must be 500 kHz apart
#define QO100_PAIR_MAX        512    // candidate lines considered for the pairing
#define QO100_TRACK_HZ      2000.0   // ...and once locked
#define QO100_DC_GUARD_HZ    300.0   // the I/Q DC-offset spike lives here
#define QO100_CLUSTER_HZ     600.0   // lines this close belong to one signal
// ...and when a signal has TWO lines this far apart, it is a CW beacon identing.
// WHICH of them the dial must be trued to is not a matter of taste: IARU
// Region 1 publishes an FSK beacon's MARK -- the key-down tone, which is also
// where the carrier rests between messages -- with space that much LOWER
// (AMSAT-DL forum, PA3FYM quoting the standard: "the carrier is on the nominal
// frequency. If it transmits its message, first the carrier goes to 'space'
// (400 Hz lower)"). Locking to the space is a dial 400 Hz off with nothing on
// air to say so, and it is what the operator's second log was. How the loop
// tells which one it is on is in beacon_frame: the mark is the tone that is
// there when the other is not.
#define QO100_FSK_SHIFT_HZ   400.0   // the published shift of both CW beacons
#define QO100_FSK_TOL_HZ      80.0   // ...how exactly a pair must show it
#define QO100_TONE_WIN          20   // measurements judging which tone we are on
#define QO100_TONE_RUN           4   // ...answering one shift up before a locked
                                     // loop accepts that it is on the space
#define QO100_MIN_SNR         12.0   // peak/mean power in the window to trust a frame
// Agreement is counted in MEASUREMENTS now, and each already integrates a second
// of stream: two of them agreeing is two seconds of evidence, which is what the
// old "three frames" was trying and failing to be (three frames is 512 ms at a
// 192 kHz span and 43 ms at 2 304 000).
#define QO100_LOCK_RUN           2   // agreeing measurements to declare a lock, AND
#define QO100_AGREE_MS      2000.0   // ...that much stream time between them
// Acquisition tolerates the beacon's OWN keying shift: the lower one is F1A and
// its carrier hops 400 Hz, so demanding 200 Hz of agreement to declare a lock
// asks the beacon to stop identing. Corrections still wait for the tight
// tracking tolerance below, so tolerating it here buys a lock, not a wrong dial.
#define QO100_LOCK_TOL_HZ    500.0   // how closely they must agree while acquiring
#define QO100_TRACK_TOL_HZ   40.0  // ...and once locked, which is what sees the ident.
                                   // It has to sit between the two: a warming LNB
                                   // moves 10-15 Hz between measurements and the
                                   // ident jumps 400, and 20 Hz sat on top of the
                                   // drift -- half a fast converter's readings were
                                   // thrown away as "unsteady" and never corrected
#define QO100_DEADBAND_HZ      1.0   // below this, leave the radio alone
#define QO100_COARSE_HZ      100.0   // above this the reading is a real offset, not jitter
#define QO100_GAIN             0.5   // fraction of a SMALL error applied per step
// One correction may cover any error an LNB can actually have (30 ppm of
// 9750 MHz is 292 kHz), because the agreement rule above now stands in front of
// it: a reading that moved the radio this far had to hold still for a second
// first. At the old 20 kHz a 250 kHz LNB pulled in at 20 kHz per 1.5 s and the
// operator watched the dial crawl for twenty seconds.
#define QO100_MAX_STEP_HZ 400000.0   // never jump further than this in one go
#define QO100_SETTLED_HZ    1000.0   // residual below which the narrow window is safe
#define QO100_HOLD_DIV           2   // ...and 1/N s of stream discarded after each one
#define QO100_LOST_MS       10000.0  // locked, but nothing found for this long => let go
#define QO100_AVG_MS         1000.0  // stream integrated into one measurement
#define QO100_SLEW_HZ          50.0  // most a LOCKED reading may move in one of those
#define QO100_SLEW_RATE_HZ_S   20.0  // ...and how fast that allowance then AGES
#define QO100_SLEW_MAX_HZ     250.0  // ...to, staying clear of the 400 Hz ident
#define QO100_SLEW_LOST         30   // ...consecutive refusals before the lock goes
#define QO100_MED_N             7  // fine readings are acted on through this many
                                   // (seconds, one each). Five is too few to tell a
                                   // drift from a wobble: four differences agreeing
                                   // in sign happens to white noise once in eight
                                   // windows, and the loop then fed a made-up drift
                                   // forward -- 18 Hz of dial movement, measured
#define QO100_FF_S             2.0   // drift fed forward this far, being about
                                     // how long until the next trim
#define QO100_SCATTER          0.5   // ...and only if the median beats this much
                                     // of the spread of the window behind it
// A retune is NOT free. Measured on a Pluto with a live 768 kHz stream:
// SoapySDRDevice_setFrequency takes 14.4 ms on average and up to 97 ms, on USB,
// and it runs on the GTK thread -- so every correction is a visible hitch in the
// waterfall and a disturbance in the audio. (Over a network Pluto it is worse.)
//
// What that buys has to be worth it, and how much it buys is the ERROR, so the
// wait is not a flat interval -- a flat one delays a five-hertz error exactly as
// long as it delays half a hertz, which is merely slow. A trim is allowed once
// the error and the time since the last one multiply out to QO100_TRIM_HZ_S:
//
//     10 Hz -> 2 s (the floor)   3 Hz -> 3.3 s   1 Hz -> 10 s   0.5 Hz -> 20 s
//
// with QO100_MIN_APPLY_S as a hard floor so a noisy minute cannot become a storm
// of retunes. A COARSE reading is a real offset, not a trim, and goes straight
// through with no wait at all.
#define QO100_MIN_APPLY_S        2.0
#define QO100_TRIM_HZ_S         10.0
// A retune is a step in the audio, and ft8_lib demodulates each candidate at a
// FIXED frequency -- it does no drift tracking at all -- so a step inside a
// transmission smears it. FT8 sends from ~0.5 s to ~13.4 s of its 15 s UTC slot
// and FT4 from ~0.5 s to ~5.5 s of its 7.5 s one, so there is a quiet tail in
// each, and that is when the loop is allowed to move the radio.
#define QO100_FT8_SLOT_S      15.0
#define QO100_FT8_QUIET_S     13.4
#define QO100_FT4_SLOT_S       7.5
#define QO100_FT4_QUIET_S      5.6
#define QO100_STUCK_RUN          3   // coarse steps that changed nothing => not our radio
#define QO100_MAX_APPLIED_HZ 1000000.0 // total correction one session may ever apply

static GMutex   bmtx;
static RECEIVER *track_rx;
static int      track_fs;
static long long track_beacon;

static double   bre[QO100_FFT_N];
static double   bim[QO100_FFT_N];
static double   bpow[QO100_FFT_N];   // |X|^2 summed over QO100_AVG_MS of stream
static double   b_avg_samples;       // ...how much stream is in it
static int      bacc;

static gboolean b_locked;
static gboolean b_settled;           // residual small enough for the narrow window
static int      b_run;               // consecutive agreeing frames
static double   b_last;              // previous frame's residual, for the log's delta
static double   b_run_ref;           // ...and the run's ANCHOR, for the agreement test
static double   b_residual;          // published: last accepted residual
static double   b_applied;           // published: total correction pushed into the LO error
static char     b_status[128]="Off";

// The loop refuses to keep correcting when its corrections do not arrive. Both
// are latched: only an operator action (Re-acquire, another beacon, the lock
// switched off and on) clears them, since re-acquiring by itself would simply
// walk into the same wall three frames later.
static gboolean b_stopped;
static double   b_step_prev;         // the last step queued, 0 if none
static double   b_res_prev;          // ...and the residual it was measured from
static int      b_stuck;             // consecutive coarse steps that changed nothing
static double   b_run_samples;       // stream time the current agreement run covers
static double   b_say_samples;        // ...and since the last spoken diagnosis
static double   b_dry_samples;        // ...and since anything at all was found
static double   b_gone_samples;       // ...and since a LOCKED loop last saw its beacon
static double   b_expect;             // where the next measurement should land
static double   b_expect_samples;     // ...and how much stream ago that was decided
static int      b_reject;             // consecutive measurements refused as impossible
static gboolean b_tone_up;            // ...and the verdict "we are on the space"
static int      b_tone_win;           // measurements in the window judging that
static int      b_tone_hits;          // ...and how many answered one shift up
static double   b_med[QO100_MED_N];   // recent fine residuals, for the median
static int      b_med_n;
// Counted in STREAM time, like the hold and the agreement: that is the clock the
// loop lives on, and it is what makes the offline stand -- which feeds far faster
// than real time -- measure the behaviour the operator gets.
static double   b_since_apply;        // stream since the radio was last moved
// Samples to throw away after a correction. A step takes time to reach the radio
// and time to come back: the driver's transfers already in flight, the FIFO
// between the receive thread and the DSP thread, and the retune itself. Frames
// measured inside that window still carry the OLD frequency, and at a 2 304 000
// span -- a Pluto's own rate -- an FFT frame is 14 ms, so the pipeline is TEN of
// them. Believing them means correcting twice for one error, and reading "that
// step changed nothing" three times running, which latches the stuck guard on a
// loop that is working perfectly. Counted in SAMPLES rather than wall clock:
// that is what the latency is actually made of, and it makes the offline
// harness (which feeds far faster than real time) measure the same thing.
static int      b_hold;

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

// One frame's contribution to the accumulated power spectrum. Window, transform,
// add |X|^2. Destroys re/im. Caller holds bmtx.
static void spectrum_add(double *re, double *im, double *pow_acc) {
  const int N=QO100_FFT_N;
  for(int i=0;i<N;i++) {
    double w=0.5-0.5*cos(2.0*M_PI*(double)i/(double)(N-1));   // Hann
    re[i]*=w; im[i]*=w;
  }
  fft_radix2(re,im,N);
  for(int m=0;m<N;m++) pow_acc[m]+=re[m]*re[m]+im[m]*im[m];
}

// Locate the beacon within [lo_hz, hi_hz] of an ACCUMULATED power spectrum and
// return its position in Hz. Caller holds bmtx.
//
// Two rules, and each was paid for on air.
//
// The spectrum is integrated over about a second before this is called, because
// the discriminator that separates a beacon from everything else on the
// transponder is TIME, not level: the beacon is continuous, a QSO is not. A
// single 43 ms frame of the operator's dish read the beacon's own line at -22,
// -295, -336 and -667 Hz in successive reports -- 644 Hz of wander against a
// 20 Hz tracking tolerance -- because the lower beacon is F1A and its carrier
// hops 400 Hz while it keys, and one frame is shorter than one element. Summed
// over a second, the line the beacon spends most of its time on is simply the
// tallest, and it stands still.
//
// And it is NOT the strongest line that is taken but the one CLOSEST TO WHERE
// THE BEACON SHOULD BE, among those standing far enough out of the window mean
// to be a carrier at all. The transponder is busy by definition: on that same
// dish the loudest things in a +/-300 kHz window were carriers at 10489.336,
// 10489.432 and 10489.440 MHz, every one of them somebody else's, picked in turn
// frame after frame. Which is loudest is not information about the beacon; how
// far each is from where the beacon must be, is.
//
// snr_out, bins_out and cand_out are for the operator: "Searching for the
// beacon" is the one status that explains nothing by itself, and the difference
// between "the strongest thing here is 3 dB out of the noise" and "the window
// holds four bins" is the difference between a dish problem and a settings
// problem. All are filled in even when the search fails.
// partner_hz: where the OTHER CW beacon would be relative to this one, or 0 to
// skip the pairing. The two CW beacons ARE the transponder's edges, exactly
// 500.000 kHz apart and both continuous, and no pair of QSO carriers holds that
// spacing over an integrated second. It is the one thing on this band that
// identifies a beacon absolutely, rather than by being near where the operator's
// settings say it should be -- which is what a converter 350 kHz out makes
// worthless. (Measured on the operator's dish: the whole transponder drawn
// 350 kHz low, and everything the loop had been locking to was somebody's QSO
// sitting where the beacon was expected.)
static gboolean find_carrier(const double *pow_acc, int fs,
                             double lo_hz, double hi_hz, double expected,
                             double partner_hz, gboolean dominant,
                             double *found, double *snr_out, int *bins_out,
                             int *cand_out) {
  const int N=QO100_FFT_N;
  const double bin_hz=(double)fs/(double)N;
  double sum=0.0, peak=-1.0;
  int pk=-1, count=0;
  // First pass: the window's mean power, and its loudest bin -- the latter only
  // so the diagnostics can say how strong the strongest thing was.
  for(int m=0;m<N;m++) {
    double sf=((m<=N/2)?(double)m:(double)(m-N))*bin_hz;   // signed baseband Hz
    if(fabs(sf)<QO100_DC_GUARD_HZ) continue;
    if(sf<lo_hz || sf>hi_hz) continue;
    double p=pow_acc[m];
    sum+=p; count++;
    if(p>peak) { peak=p; pk=m; }
  }
  if(bins_out!=NULL) *bins_out=count;
  if(pk<1 || pk>=N-1 || count<8) return FALSE;
  double mean=sum/(double)count;
  if(snr_out!=NULL && mean>0.0) *snr_out=peak/mean;
  if(mean<=0.0) return FALSE;

  // Second pass: every LOCAL maximum that stands QO100_MIN_SNR out of that mean
  // is a candidate carrier. Collect them, then choose.
  static int cand_bin[QO100_PAIR_MAX];
  static double cand_sf[QO100_PAIR_MAX];
  int cands=0, held=0;
  for(int m=1;m<N-1;m++) {
    double sf=((m<=N/2)?(double)m:(double)(m-N))*bin_hz;
    if(fabs(sf)<QO100_DC_GUARD_HZ) continue;
    if(sf<lo_hz || sf>hi_hz) continue;
    double p=pow_acc[m];
    if(p/mean<QO100_MIN_SNR) continue;
    if(p<pow_acc[m-1] || p<pow_acc[m+1]) continue;   // a shoulder, not a line
    cands++;
    if(held<QO100_PAIR_MAX) { cand_bin[held]=m; cand_sf[held]=sf; held++; }
  }
  if(cand_out!=NULL) *cand_out=cands;
  if(held<1) return FALSE;

  int sel=-1;
  double best=1e18;

  // The pairing first, when both edges could be in the window: a candidate with
  // a partner exactly 500 kHz away IS a beacon, however far the operator's
  // settings say it should not be. Among such candidates -- there is normally
  // exactly one pair -- take the nearest to the expectation.
  if(partner_hz!=0.0) {
    for(int i=0;i<held;i++) {
      double want=cand_sf[i]+partner_hz;
      if(want<lo_hz || want>hi_hz) continue;         // the partner is out of view
      gboolean paired=FALSE;
      for(int j=0;j<held && !paired;j++)
        if(fabs(cand_sf[j]-want)<QO100_PAIR_TOL_HZ) paired=TRUE;
      if(!paired) continue;
      double d=fabs(cand_sf[i]-expected);
      if(d<best) { best=d; sel=cand_bin[i]; }
    }
  }

  // Failing that -- one edge out of the span, one beacon off the air -- the
  // nearest candidate to the expectation, which is right whenever the converter
  // is roughly believed.
  if(sel<0) {
    for(int i=0;i<held;i++) {
      double d=fabs(cand_sf[i]-expected);
      if(d<best) { best=d; sel=cand_bin[i]; }
    }
  }
  if(sel<1 || sel>=N-1) return FALSE;

  // Third pass, and it is what makes an F1A beacon ACQUIRABLE. The nearest
  // candidate locates the SIGNAL; within one signal's width the line to measure
  // is the one it spends most of its time on, which in an integrated spectrum is
  // simply the tallest. Nearest alone is bistable while acquiring: the beacon's
  // two lines are 400 Hz apart, whichever is nearer changes as the correction
  // proceeds, and the loop ends up hopping between them 400 Hz at a time --
  // measured, before this pass existed.
  //
  // It is exactly wrong once the loop is TRACKING, and that is the caller's
  // `dominant` flag. A settled lock knows where its beacon is to within a few
  // hertz, so the nearest line IS the beacon and the tallest is whatever is
  // momentarily loudest within 600 Hz of it -- the ident's other line, or
  // somebody tuning up across it. Following that is how a loop reading -0.0 Hz
  // walked out to -111.8 and applied it as a coarse step, reported from air.
  // The bistability this pass exists for cannot happen here: nothing moves the
  // expectation by 400 Hz once the tracking window is only 2 kHz wide.
  double sel_sf=((sel<=N/2)?(double)sel:(double)(sel-N))*bin_hz;
  if(dominant) {
    for(int m=1;m<N-1;m++) {
      double sf=((m<=N/2)?(double)m:(double)(m-N))*bin_hz;
      if(fabs(sf-sel_sf)>QO100_CLUSTER_HZ) continue;
      if(sf<lo_hz || sf>hi_hz) continue;
      double pw=pow_acc[m];
      if(pw/mean<QO100_MIN_SNR) continue;
      if(pw<pow_acc[m-1] || pw<pow_acc[m+1]) continue;
      if(pw>pow_acc[sel]) sel=m;
    }
  }
  pk=sel;
  if(snr_out!=NULL) *snr_out=pow_acc[pk]/mean;

  double y0=sqrt(pow_acc[pk-1]);
  double y1=sqrt(pow_acc[pk]);
  double y2=sqrt(pow_acc[pk+1]);
  double denom=y0-2.0*y1+y2;
  double delta=(denom!=0.0)?0.5*(y0-y2)/denom:0.0;
  if(delta> 0.5) delta= 0.5;
  if(delta<-0.5) delta=-0.5;
  double pos=(double)pk+delta;
  double sf=(pos<=N/2)?pos:pos-(double)N;
  *found=sf*bin_hz;
  return TRUE;
}

// May the radio be moved right now? Only outside an FT8/FT4 transmission when
// one of those decoders is running on this receiver: a correction is a step in
// the audio, and ft8_lib demodulates at a fixed frequency, so a step inside a
// transmission costs that decode. The loop measures about once a second, so it
// simply waits for the slot's quiet tail -- at most one slot of delay, against a
// converter that drifts hertz per minute.
static gboolean qo100_may_retune_now(RECEIVER *rx) {
  if(radio==NULL || rx==NULL) return TRUE;
  if(radio->decode_mode!=DECODE_FT8 && radio->decode_mode!=DECODE_FT4) return TRUE;
  if(rx->mode_a!=DIGU && rx->mode_a!=DIGL) return TRUE;   // the tap is mode-gated
  double slot =(radio->decode_mode==DECODE_FT4)?QO100_FT4_SLOT_S :QO100_FT8_SLOT_S;
  double quiet=(radio->decode_mode==DECODE_FT4)?QO100_FT4_QUIET_S:QO100_FT8_QUIET_S;
  double utc=(double)(g_get_real_time()/1000)/1000.0;     // UTC seconds
  return fmod(utc,slot)>=quiet;
}

// The median of the recent fine residuals -- used to REJECT outliers, not to
// replace the reading. Acting on the median directly was measured on the stand
// and is worse: it lags the drift by half its own length, so at 0.5 Hz/s the
// movement inside a 15 s slot went 1.95 -> 3.62 Hz. Rejecting instead keeps the
// latest value (no lag) and throws away the beacon fades and neighbours that
// made it jump; a reading more than QO100_MED_TOL_HZ from the median of the last
// five seconds is not the converter, which drifts hertz per MINUTE.
static double median_of(const double *v, int n) {
  double t[QO100_MED_N];
  memcpy(t,v,sizeof(double)*(size_t)n);
  for(int i=1;i<n;i++) {                       // insertion sort, n is 5
    double x=t[i]; int j=i-1;
    while(j>=0 && t[j]>x) { t[j+1]=t[j]; j--; }
    t[j+1]=x;
  }
  return t[n/2];
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
    gint64 t0=g_get_monotonic_time();
    rx->error_a += (gint64)llround(step);
    BAND *band=band_get_band(rx->band_a);
    if(band!=NULL) band->errorLO=rx->error_a;   // so it survives a band change and a restart
    frequency_changed(rx);
    update_frequency(rx);
    // The retune runs on the GTK thread and a SoapySDR device can take tens of
    // milliseconds over it -- measured on a Pluto at 14.4 ms average, 97 ms
    // worst, on a live stream. That is a visible hitch in the waterfall, so when
    // it happens the log says which of the two it was rather than leaving the
    // operator to blame the receiver.
    double ms=(double)(g_get_monotonic_time()-t0)/1000.0;
    if(ms>20.0)
      log_info("qo100: the %+.1f Hz retune took %.0f ms on the GTK thread\n",step,ms);
  }
  g_atomic_int_set(&apply_queued,0);
  return G_SOURCE_REMOVE;
}

static void spectrum_clear(void) {
  memset(bpow,0,sizeof(bpow));
  b_avg_samples=0.0;
}

static void beacon_reset_locked(void) {
  bacc=0;
  spectrum_clear();
  b_run=0;
  b_last=0.0;
  b_run_ref=0.0;
  b_locked=FALSE;
  b_settled=FALSE;
  b_step_prev=0.0;
  b_res_prev=0.0;
  b_stuck=0;
  b_run_samples=0.0;
  b_say_samples=0.0;
  b_dry_samples=0.0;
  b_gone_samples=0.0;
  b_expect=0.0;
  b_expect_samples=0.0;
  b_reject=0;
  b_tone_up=FALSE;
  b_tone_win=0;
  b_tone_hits=0;
  b_med_n=0;
  b_since_apply=1.0e12;                 // a fresh lock may trim at once
  b_hold=0;
}

void qo100_beacon_reset(void) {
  g_mutex_lock(&bmtx);
  beacon_reset_locked();
  b_stopped=FALSE;
  b_applied=0.0;
  b_residual=0.0;
  track_rx=NULL;
  g_strlcpy(b_status,"Off",sizeof(b_status));
  g_mutex_unlock(&bmtx);
}

// The tracked receiver is being freed (delete_receiver). Same shape as
// qo100_beacon_reset(), but only when it is THIS receiver: the lock follows the
// receiver it was measuring on, and the correction already written into the
// band's errorLO stays, being a measurement of the converter rather than
// session state.
void qo100_beacon_forget_receiver(RECEIVER *rx) {
  if(rx==NULL) return;
  g_mutex_lock(&bmtx);
  if(track_rx==rx) {
    beacon_reset_locked();
    b_residual=0.0;
    track_rx=NULL;
    g_strlcpy(b_status,"Off (receiver closed)",sizeof(b_status));
  }
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
  if(b_stopped) return;                 // latched refusal, see below
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
  //
  // ACQUISITION searches as much of the span as it can reach, and the reason is
  // arithmetic an LNB's data sheet makes plain: the error is the CRYSTAL's
  // tolerance multiplied by 9750 MHz, so 5 ppm is 48.8 kHz, 10 ppm is 97.5 and
  // 20 ppm — an ordinary consumer part — is 195 kHz. The +/-60 kHz this used to
  // hunt in cannot see any of those, and the operator gets "Searching for the
  // beacon" for ever with nothing to tell them the beacon is simply outside the
  // window. A wide search is only safe because of the agreement rule above: a
  // strong SSB or CW station in the span is not a steady line for a whole
  // second, so it cannot be mistaken for a beacon.
  // Explicit bounds, and each end is clipped to the span SEPARATELY: the old
  // code shrank a symmetric window until both ends fitted, so with the beacon
  // expected 40 kHz below centre it searched -86..+6 kHz and an LNB erring the
  // other way was invisible.
  double half=(b_locked && b_settled)?QO100_TRACK_HZ:QO100_ACQ_HZ;
  double lo_hz=expected-half, hi_hz=expected+half;
  if(lo_hz<-span_half) lo_hz=-span_half;
  if(hi_hz> span_half) hi_hz= span_half;

  // Integrate this frame and stop here unless a full averaging window has been
  // collected: the measurement is made on a second of stream, not on 43 ms of
  // it. Everything downstream therefore runs about once a second.
  spectrum_add(bre,bim,bpow);
  b_avg_samples+=(double)QO100_FFT_N;
  if(b_avg_samples<(double)track_fs*(QO100_AVG_MS/1000.0)) return;
  double avg_samples=b_avg_samples;

  double found, snr=0.0;
  int bins=0, cands=0;
  b_say_samples+=avg_samples;
  b_expect_samples+=avg_samples;
  gboolean say=(b_say_samples>=(double)track_fs*5.0);   // at most once per 5 s of stream
  if(say) b_say_samples=0.0;

  // While acquiring, let the pairing work; once locked the window is narrow and
  // the partner is not in it.
  double partner=0.0;
  if(!(b_locked && b_settled)) {
    if(radio->qo100_beacon_sel==QO100_BEACON_SEL_LOWER)
      partner=(double)(QO100_BEACON_UPPER-QO100_BEACON_LOWER);
    else if(radio->qo100_beacon_sel==QO100_BEACON_SEL_UPPER)
      partner=-(double)(QO100_BEACON_UPPER-QO100_BEACON_LOWER);
  }
  gboolean tracking=(b_locked && b_settled);
  if(!find_carrier(bpow,track_fs,lo_hz,hi_hz,expected,partner,!tracking,
                   &found,&snr,&bins,&cands)) {
    spectrum_clear();
    b_run=0;
    b_run_samples=0.0;
    b_dry_samples+=avg_samples;
    if(!b_locked) {
      // After ten seconds of finding nothing, stop repeating "Searching" and
      // name the limit the operator can actually act on. An LNB's error is its
      // crystal's tolerance times 9750 MHz -- 195 kHz at 20 ppm -- and a span of
      // 192 kHz only reaches +/-96 kHz, so with a narrow span the beacon can be
      // outside the RECEIVER, where no search width helps. The two cures are the
      // operator's: a wider span while acquiring, or an LNB LO nearer the truth.
      if(b_dry_samples>(double)track_fs*10.0)
        snprintf(b_status,sizeof(b_status),
                 "Nothing found in the Â±%.0f kHz this span covers â "
                 "widen the span, or set the LNB LO closer",span_half/1000.0);
      else
        g_strlcpy(b_status,"Searching for the beacon\342\200\246",sizeof(b_status));
    }
    else {
      // A lock is a claim about where the beacon IS, and once settled it is
      // searched for in a window +/-2 kHz wide. If the beacon then goes -- it
      // was never the beacon, the dish moved, the transponder is quiet -- that
      // window is a 4 kHz slit with nothing in it and no way out: measured on
      // the operator's radio, TWO MINUTES of "0 candidate lines" in a row while
      // the real beacon sat 14 kHz away, because nothing ever let the lock go.
      // Ten seconds of stream without a carrier is enough to stop believing it.
      b_gone_samples+=avg_samples;
      if(b_gone_samples>=(double)track_fs*(QO100_LOST_MS/1000.0)) {
        beacon_reset_locked();
        g_strlcpy(b_status,
                  "Re-acquiring â the beacon was gone for 10 s",
                  sizeof(b_status));
        log_info("qo100: lock given up — no carrier for %.0f s, searching wide "
                 "again\n",QO100_LOST_MS/1000.0);
      } else {
        snprintf(b_status,sizeof(b_status),
                 "Locked (beacon gone %.0f s)",
                 b_gone_samples/(double)track_fs);
      }
    }
    // Say why, at INFO, because this is the status an operator gets stuck on and
    // it is the one that explains nothing by itself.
    if(say)
      log_info("qo100: no carrier — beacon expected %+.1f kHz from centre, "
               "searched %+.1f..%+.1f kHz (%d bins of %.1f Hz), %d candidate "
               "lines, best peak %.1f x mean (needs %.0f), span %d Hz\n",
               expected/1000.0,lo_hz/1000.0,hi_hz/1000.0,bins,
               (double)track_fs/(double)QO100_FFT_N,cands,snr,QO100_MIN_SNR,
               track_fs);
    return;
  }
  spectrum_clear();
  b_since_apply+=avg_samples;
  b_dry_samples=0.0;
  b_gone_samples=0.0;

  double residual=found-expected;

  // Which of the beacon's two tones is the loop sitting on? THE MARK IS THE ONE
  // THAT IS THERE WHEN THE OTHER IS NOT -- the carrier rests on the published
  // frequency and drops to the space only while the beacon is sending -- so a
  // loop on the space finds its own line missing in every idle second and reads
  // the mark instead, one shift up. Count those against the measurements as a
  // whole (this is before the agreement gate on purpose: a reading that breaks
  // the run IS the evidence), and a window in which the other tone answered
  // often enough settles it. A station parked one shift away cannot produce
  // this, whatever its strength, because it does not make the beacon's own line
  // vanish -- which is why the pairing in find_carrier is left to acquisition
  // and the tracking answer is counted in time instead.
  if(b_locked) {
    b_tone_win++;
    if(fabs(residual-b_expect-QO100_FSK_SHIFT_HZ)<QO100_FSK_TOL_HZ) b_tone_hits++;
    if(b_tone_win>=QO100_TONE_WIN) {
      if(b_tone_hits>=QO100_TONE_RUN) b_tone_up=TRUE;
      b_tone_win=0;
      b_tone_hits=0;
    }
  }
  // Per frame, at DEBUG: the only way to tell a beacon whose line MOVES (the
  // lower one is F1A and hops 400 Hz while it keys) from one that is steady but
  // competing with another carrier. The INFO summary below is five seconds
  // apart and cannot show either.
  log_debug("qo100: frame carrier %+.1f Hz residual %+.1f Hz snr %.0f "
            "(previous residual %+.1f Hz, delta %+.1f Hz, run %d)\n",
            found,residual,snr,b_last,residual-b_last,b_run);

  // Agreement, and it is measured in STREAM TIME rather than in frames. The
  // search takes a maximum over hundreds of trial positions, so noise alone
  // produces a confident-looking peak somewhere every frame; a real beacon comes
  // back to the same place. (Same lesson as the APT sync detector.) But an FFT
  // frame is 170 ms at a 192 kHz span and 14 ms at 2 304 000 -- a Pluto's own
  // rate -- so "three frames agreed" is half a second of evidence on one radio
  // and 43 ms on another, which is shorter than a single CW element and is
  // precisely the thing that must not be believed.
  //
  // Because the lower beacon is F1A: its carrier HOPS 400 Hz while it keys its
  // ident, and every reading taken during that is one of two frequencies. The
  // same test therefore guards every correction, not just the first -- it used
  // to guard acquisition alone, and the loop dragged the radio 400.9 Hz back and
  // forth at 0.6 retunes a second (measured against a modelled ident), which is
  // audible on SSB and fatal to FT8, whose decoder has no drift tracking at all
  // and wants the frequency to hold still across a 15 s slot. An unsteady beacon
  // now buys nothing: the run resets, no correction is applied, and the loop
  // waits for the ident to end. A beacon that has genuinely MOVED is steady in
  // its new place, so half a second later it is followed like any other reading.
  //
  // And agreement is with the run's own ANCHOR -- its first reading -- not with
  // the one before it. Chained to the previous reading, a run is not evidence
  // that anything held still: every consecutive pair can be inside the 20 Hz
  // tolerance while the run itself walks as far as it likes. That is not a
  // theoretical hole, it is the shape of the report this test came from -- a
  // locked loop sitting on -0.0 Hz walked out through -68 to -111.8 Hz, twenty
  // hertz at a time, and applied the -111.8 as a coarse step. A beacon holds
  // ONE frequency for a second at a time; anything that walks now breaks the
  // run at the point it leaves the anchor, and buys nothing.
  double tol=b_locked?QO100_TRACK_TOL_HZ:QO100_LOCK_TOL_HZ;
  if(b_run>0 && fabs(residual-b_run_ref)<tol) {
    b_run++;
    b_run_samples+=avg_samples;
  } else {
    b_run=1;
    b_run_samples=avg_samples;
    b_run_ref=residual;
  }
  double delta=residual-b_last;
  b_last=residual;
  gboolean agreed=(b_run>=QO100_LOCK_RUN &&
                   b_run_samples>=(double)track_fs*(QO100_AGREE_MS/1000.0));

  // Five-second summary, and it has to carry the AGREEMENT state: a loop that
  // finds a strong carrier every frame and never locks is one whose readings
  // disagree, and nothing else in the log says by how much.
  if(say)
    log_info("qo100: carrier at %+.1f Hz of an expected %+.1f Hz "
             "(residual %+.1f Hz, %.0f x mean, nearest of %d lines, %s; "
             "agreed on %d frames / %.0f ms of %.0f, last step %+.1f Hz, "
             "tolerance %.0f Hz)\n",
             found,expected,residual,snr,cands,b_locked?"locked":"acquiring",
             b_run,b_run_samples*1000.0/(double)track_fs,QO100_AGREE_MS,
             delta,tol);

  if(!b_locked) {
    if(!agreed) {
      snprintf(b_status,sizeof(b_status),"Acquiring\342\200\246 %+.0f Hz",residual);
      return;
    }
    b_locked=TRUE;
    b_expect=residual;                  // the lock starts believing from here
    b_expect_samples=0.0;
  } else if(!agreed) {
    snprintf(b_status,sizeof(b_status),
             "Locked  (beacon unsteady %+.0f Hz -- ident?)",residual);
    b_step_prev=0.0;
    return;
  }

  b_residual=residual;
  b_settled=(fabs(residual)<QO100_SETTLED_HZ);

  // A reading that is not where the last one said it would be has exactly two
  // explanations, and they are opposite faults, so they are told apart before
  // either is acted on.
  //
  //   * it did not MOVE when a correction should have moved it -- our step is
  //     not reaching the radio. That is not hypothetical: frequency_changed()
  //     pushed a new LO to Protocol 2 / SoapySDR only in its non-ctun branch, so
  //     under ctun or freetune the loop corrected error_a for ever against a
  //     radio that never budged.
  //   * it moved further than an oscillator can. A locked loop measures once a
  //     second and an LNB drifting a fast 10 Hz/s moves ten hertz in that time,
  //     so a 400 Hz jump is not the converter -- it is the beacon's own keying.
  //     The lower one is F1A and shifts 400 Hz, and a whole integrating second
  //     landing inside a burst of it makes the KEYED line the taller. Two such
  //     seconds in a row satisfied the agreement rule and applied the lot as a
  //     coarse step: the dial jumping forward and coming back, reported from air
  //     in exactly those words.
  //
  // Only COARSE steps can prove the first, since a fine one is half of a few
  // hertz of noise; and the second stops believing after QO100_SLEW_LOST
  // measurements, at which point the beacon really has moved and the lock goes,
  // putting the wide search and the 500 kHz pairing back in front of it.
  // The allowance is a RATE, not a fixed offset, and that distinction is the
  // whole of "прыгает как горный козёл". An LNB warming on its dish moves
  // several hertz a second -- measured on the operator's at 5-7 -- so what a
  // reading may not do is jump further than the converter can have MOVED since
  // the loop last believed anything. Frozen at 50 Hz it worked only while every
  // second produced a reading: F1A is two frequencies 400 Hz apart and neither
  // is an idle carrier, so whole integrated seconds hold the other line alone,
  // and each of those is refused (rightly) while the converter keeps going. Ten
  // seconds of that put the real beacon 60 Hz from an anchor that had not moved
  // since, so it was refused too -- and then everything was, for ever, until 30
  // refusals dropped the lock and the re-acquisition applied the whole
  // accumulated error as one step. Measured on air: 25 s with no correction at
  // all while the residual walked 114 -> 240 Hz, then a +691.9 Hz retune; in
  // the harness the loop falls off this cliff between 6 and 8 Hz/s of drift and
  // lands a 1098 Hz step at 10.
  //
  // The cap stays well clear of the 400 Hz ident, so no amount of waiting ever
  // makes the beacon's OTHER line acceptable. A converter that really has run
  // further than that while the loop was blind is what QO100_SLEW_LOST is for:
  // the lock goes, and the wide search and the 500 kHz pairing get it back.
  // ...with one exception, and it is the only reading in this file allowed past
  // the guard. THE MARK IS THE TONE THAT IS THERE WHEN THE OTHER IS NOT: the
  // carrier rests on the published frequency and drops to the space only while
  // the beacon is sending, so a loop sitting on the space finds its own line
  // missing in every idle second and reads the mark instead, one shift up.
  // That is what QO100_TONE_RUN counts -- readings exactly one shift above the
  // anchor, against readings that land on it -- and four of them net is a claim
  // no station parked 400 Hz away can make, because a parked carrier does not
  // make the beacon's own line vanish.
  //
  // Without this the loop can be right and unable to act on it: acquisition
  // landing in a second where only the space was on air locks to the space, the
  // mark is then refused as a 400 Hz jump for 30 measurements, and the
  // correction arrives only because the lock is finally dropped altogether.
  // Half a minute of "ignoring +400 Hz" in the log, and a dial 400 Hz off the
  // whole time.
  gboolean tone_swap=(b_tone_up &&
                      fabs(residual-b_expect-QO100_FSK_SHIFT_HZ)<QO100_FSK_TOL_HZ);
  double slew_allow=QO100_SLEW_HZ+
                    QO100_SLEW_RATE_HZ_S*(b_expect_samples/(double)track_fs);
  if(slew_allow>QO100_SLEW_MAX_HZ) slew_allow=QO100_SLEW_MAX_HZ;
  if(b_locked && !tone_swap && fabs(residual-b_expect)>slew_allow) {
    if(fabs(b_step_prev)>QO100_COARSE_HZ &&
       fabs(residual-b_res_prev)<0.25*fabs(b_step_prev)) {
      b_stuck++;
      if(b_stuck>=QO100_STUCK_RUN) {
        double last=b_step_prev;        // beacon_reset_locked() clears it
        beacon_reset_locked();
        b_stopped=TRUE;
        g_strlcpy(b_status,
                  "Stopped: the correction is not reaching the radio",
                  sizeof(b_status));
        log_error("qo100: beacon lock stopped -- %.0f Hz applied and the residual "
                  "stayed at %.0f Hz; the radio is not being retuned\n",
                  last,residual);
        return;
      }
      snprintf(b_status,sizeof(b_status),
               "Locked  (%+.0f Hz applied, nothing moved)",b_step_prev);
      return;                            // b_step_prev/b_res_prev kept on purpose
    }
    b_reject++;
    if(b_reject<QO100_SLEW_LOST) {
      snprintf(b_status,sizeof(b_status),
               "Locked  (ignoring %+.0f Hz \342\200\224 beacon ident?)",residual-b_expect);
      b_step_prev=0.0;
      return;
    }
    beacon_reset_locked();
    g_strlcpy(b_status,"Re-acquiring \342\200\224 the beacon moved",sizeof(b_status));
    return;
  }
  b_reject=0;
  b_stuck=0;
  b_step_prev=0.0;

  // Two speeds, because the two situations are not the same measurement. A large
  // reading is a genuine converter offset that has been measured directly, so
  // there is nothing to gain by approaching it in fractions — take all of it, and
  // do not smooth it either. A small one is mostly measurement noise on a fading
  // path, so what is acted on is the MEDIAN of the last QO100_MED_N seconds and
  // not the reading itself, before the damping gain on top of that.
  //
  // The median used to be a gate instead — a reading more than 3 Hz off it was
  // refused, the reading itself applied — and that is a worse instrument in both
  // directions at once. Against jitter it is leakier: a reading that happens to
  // land near the median is applied in full, so the loop chases whichever wobble
  // agrees with itself (measured, +/-8 Hz of wobble: 17 retunes and 7 Hz of dial
  // movement, against 8 and 2 for the median). And against DRIFT it is a wall:
  // an LNB warming at 2 Hz/s puts every reading 4 Hz off the median of the last
  // five by arithmetic alone, so a +/-3 Hz gate refuses the lot for ever and
  // nothing is trimmed until the error passes QO100_COARSE_HZ and the radio
  // moves in one 100 Hz jump — the fault the fine loop exists to avoid. Taking
  // the median as the measurement is immune to a single wild reading by
  // construction and lags a drift by two seconds, which at any rate an LNB can
  // manage is a few hertz.
  double act, ff=0.0;                   // ...and the drift to feed forward
  if(fabs(residual)>QO100_COARSE_HZ) {
    act=residual;
    b_med_n=0;                          // a real offset invalidates the history
  } else {
    if(b_med_n<QO100_MED_N) b_med[b_med_n++]=residual;
    else {
      for(int i=1;i<QO100_MED_N;i++) b_med[i-1]=b_med[i];
      b_med[QO100_MED_N-1]=residual;
    }
    // b_expect is deliberately NOT moved by this return. It is the anchor the
    // slew guard above measures against -- where the beacon was last seen to
    // really BE -- and a window that is still filling has not established that
    // yet. Moving it here (and, in the code this replaced, on every refused
    // reading) makes the two guards cancel out: each reading teaches the slew
    // guard to accept its successor twenty hertz further on, and a chain of
    // those is what walked a locked loop from -0.0 Hz out to -111.8 Hz and then
    // applied it as a coarse step. The anchor moves when a reading is BELIEVED,
    // and when the radio itself is moved.
    if(b_med_n<QO100_MED_N) {           // the window is not full yet
      snprintf(b_status,sizeof(b_status),"Locked  %+.1f Hz  (settling %d/%d)",
               residual,b_med_n,QO100_MED_N);
      return;
    }
    // The window's own SLOPE first, robustly: the median of its consecutive
    // differences, and one entry is one second of stream, so it is Hz per
    // second. It is believed only when every difference agrees with it in sign
    // -- a converter drifting is a run of readings all moving the same way, and
    // jitter, however wide, is not. Nothing here needs a bare gradient, and
    // that is deliberate: a slope taken off two endpoints is one wild reading
    // away from claiming any rate you like.
    double diff[QO100_MED_N-1];
    for(int i=1;i<b_med_n;i++) diff[i-1]=b_med[i]-b_med[i-1];
    double slope=median_of(diff,b_med_n-1);
    for(int i=0;i<b_med_n-1;i++) if(diff[i]*slope<=0.0) { slope=0.0; break; }

    // The median is the middle of the window, so under a drift it is already
    // two seconds stale by the time it is computed, and the correction lands
    // later still. Both are known now, so both are taken out: what is acted on
    // is where the error will BE, not where it was.
    double med=median_of(b_med,b_med_n);
    act=med+slope*0.5*(double)(QO100_MED_N-1);
    ff=slope*QO100_FF_S;

    // ...and it is only acted on when it stands out of the window's OWN
    // scatter. A median is an estimate and the spread of the readings behind it
    // is that estimate's uncertainty: on a fading path the line wanders several
    // hertz from one integrated second to the next -- a bin is 23 Hz at the
    // 768 kHz span this is used at -- and a loop that applies half of every
    // wobble simply moves the operator's dial about for no gain. The estimate
    // must therefore beat the half-spread of the window behind it, which is the
    // wobble's own amplitude. Measured, with the reading wobbling +/-12 Hz:
    // 8 Hz of dial movement and 14 retunes a minute without this test, 0 Hz and
    // 2 with it -- and a drift of 2 Hz/s is tracked either way, because a drift
    // biases the median far more than it widens the window.
    // The spread is measured with the trend taken out, or a drift would widen
    // the very gate it has to pass: at 10 Hz/s the window spans 40 Hz, and half
    // of that is a threshold no honest reading can beat until the error is
    // already 20 Hz.
    double lo_m=1e18, hi_m=-1e18;
    for(int i=0;i<b_med_n;i++) {
      double dt=b_med[i]-slope*(double)i;
      if(dt<lo_m) lo_m=dt;
      if(dt>hi_m) hi_m=dt;
    }
    if(fabs(act)+fabs(ff)<QO100_SCATTER*(hi_m-lo_m)) {
      b_expect=residual;
      b_expect_samples=0.0;
      snprintf(b_status,sizeof(b_status),
               "Locked  %+.1f Hz  (inside the %.1f Hz the readings scatter by)",
               act,hi_m-lo_m);
      return;
    }
  }

  if(fabs(act)<QO100_DEADBAND_HZ) {
    b_expect=residual;                  // nothing moves, so nothing should change
    b_expect_samples=0.0;
    snprintf(b_status,sizeof(b_status),"Locked  %+.1f Hz  (LO %+.0f Hz)",
             act,b_applied);
    return;
  }

  // A step is a jump in the audio, so it waits for a gap in the traffic that
  // cares: see qo100_may_retune_now().
  if(!qo100_may_retune_now(rx)) {
    b_expect=residual;
    b_expect_samples=0.0;
    snprintf(b_status,sizeof(b_status),
             "Locked  %+.1f Hz  (holding for the decoder's slot)",act);
    return;
  }

  // ...and a FINE step waits for the rate limit as well, because the retune
  // itself costs the operator something (see QO100_MIN_APPLY_S).
  if(fabs(act)<=QO100_COARSE_HZ) {
    double since=b_since_apply/(double)track_fs;
    double need=(fabs(act)>0.0)?QO100_TRIM_HZ_S/fabs(act):1.0e9;
    if(need<QO100_MIN_APPLY_S) need=QO100_MIN_APPLY_S;
    if(since<need) {
      b_expect=residual;
      b_expect_samples=0.0;
      snprintf(b_status,sizeof(b_status),
               "Locked  %+.1f Hz  (trim in %.0f s)",act,need-since);
      return;
    }
  }

  // The damping gain is on the ERROR only. The drift term is not an error to
  // approach carefully, it is the converter's known rate multiplied by the time
  // until the next trim -- feeding it forward is what stops the loop running a
  // standing lag proportional to how fast the LNB is warming, and it costs no
  // extra retunes because it rides on one that was going to happen anyway.
  double step=(fabs(act)>QO100_COARSE_HZ)?act:(act*QO100_GAIN+ff);
  if(step> QO100_MAX_STEP_HZ) step= QO100_MAX_STEP_HZ;
  if(step<-QO100_MAX_STEP_HZ) step=-QO100_MAX_STEP_HZ;

  // A second stop, for the same reason from the other end. An LNB's error is its
  // crystal's tolerance times 9750 MHz -- 292 kHz at 30 ppm, and a badly out
  // consumer part perhaps twice that -- while the search can only see half a
  // span either way in the first place. A megahertz of accumulated correction in
  // one session is therefore not a converter being measured, it is a loop that
  // has stopped converging on something.
  if(fabs(b_applied+step)>QO100_MAX_APPLIED_HZ) {
    beacon_reset_locked();
    b_stopped=TRUE;
    snprintf(b_status,sizeof(b_status),
             "Stopped: %+.0f kHz of correction is not an LNB",(b_applied+step)/1000.0);
    log_error("qo100: beacon lock stopped -- %.0f Hz accumulated, residual %.0f Hz\n",
              b_applied+step,residual);
    return;
  }
  // The history is not thrown away across a step -- that would make every fine
  // correction wait five seconds for a new one, and at 0.5 Hz/s of drift the
  // error grows 2.5 Hz in that gap (measured: 1.95 -> 3.62 Hz of movement inside
  // a 15 s slot). It is SHIFTED instead: after the radio moves by `step`, every
  // stored residual means `residual - step` in the new frame.
  for(int i=0;i<b_med_n;i++) b_med[i]-=step;

  snprintf(b_status,sizeof(b_status),"Locked  %+.1f Hz  (LO %+.0f Hz)",
           residual,b_applied+step);

  // Queue the retune for the GTK thread, at most one at a time: if the applier
  // has not run yet the next frame's measurement is stale anyway.
  if(g_atomic_int_compare_and_exchange(&apply_queued,0,1)) {
    pend_step=step;
    b_tone_up=FALSE;                    // ...spent, whether or not it was one
    b_applied+=step;
    b_step_prev=step;                   // ...and what it must be worth next frame
    b_res_prev=residual;
    b_expect=residual-step;             // where the next measurement must land
    b_expect_samples=0.0;
    b_since_apply=0.0;
    b_hold=track_fs/QO100_HOLD_DIV;     // wait for data that has the step in it
    spectrum_clear();                   // ...and it must not be averaged with this
    g_idle_add(apply_idle,NULL);
  } else {
    b_expect=residual;                  // nothing was applied, so nothing moves
    b_expect_samples=0.0;
  }
}

void qo100_beacon_iq_feed(RECEIVER *rx, const double *iq, int n_frames) {
  if(radio==NULL || !radio->qo100_beacon_lock) return;     // cheap fast path
  if(rx==NULL || rx!=radio->active_receiver) return;

  // The wideband beacon is DVB-S2: a suppressed-carrier signal, so there is no
  // spectral line to measure and peak-picking it would track the modulation.
  // Refusing it in words matters more than refusing it quietly — the operator
  // has selected it for the level reference line, where it is the right answer,
  // and a lock that simply never acquired would look like a broken lock.
  if(!qo100_beacon_has_carrier(radio->qo100_beacon_sel)) {
    g_mutex_lock(&bmtx);
    beacon_reset_locked();
    g_strlcpy(b_status,
              "The WB beacon is DVB-S2 \342\200\224 no carrier to lock to; pick a CW beacon",
              sizeof(b_status));
    g_mutex_unlock(&bmtx);
    return;
  }

  g_mutex_lock(&bmtx);

  long long beacon=qo100_beacon_frequency(radio->qo100_beacon_sel);
  if(rx!=track_rx || track_fs!=rx->sample_rate || track_beacon!=beacon) {
    track_rx=rx;
    track_fs=rx->sample_rate;
    track_beacon=beacon;
    beacon_reset_locked();
  }

  for(int k=0;k<n_frames;k++) {
    if(b_hold>0) { b_hold--; bacc=0; continue; }   // stale: a step is in flight
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
