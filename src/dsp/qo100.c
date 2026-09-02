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
  // The MIDDLE beacon is offered as a lock source now, and it is the one that
  // settles arguments.  It is 400 bd BPSK — a suppressed carrier with no line to
  // peak-search — so the loop makes one by squaring (line_near() on a squared
  // spectrum), and the reason to go to that trouble is that a BPSK spectrum
  // carries NO TONE CONVENTION: it is symmetric about its own published
  // frequency, whoever is right about the CW beacons' two lines.  It is also
  // what the rest of the QO-100 world calibrates against.
  //
  // WHICH of an F1A beacon's two lines the published figure names is the whole
  // difference between a truthful dial and a dial 400 Hz off, and this tree has
  // taken it from a document twice and been wrong twice.  It is settled by
  // measurement now — see QO100_BEACON_REST_HZ in qo100.h and
  // QO100_KEYED_FROM_REST_HZ below — and the two instruments that settled it are
  // both still here: the middle beacon checks a CW lock, and a CW beacon checks
  // a middle-beacon lock, which is the one that reads the convention off the
  // air rather than assuming it.
  //
  // qo100_beacon_track_frequency() is the line to MEASURE and this function is
  // what the operator tunes to, what the band plan draws and what the dial is
  // trued against.  They are the same number today; the seam stays because the
  // question lives on it.
  //
  // The WIDEBAND beacon is here for a different job entirely.  DVB-S2's carrier
  // is suppressed the same way, but 1.5 MS/s of shaped modulation leaves nothing
  // a squaring loop can use either, and it is the reference the wideband
  // transponder's power rule is written against ("at least 1 dB below the
  // beacon").  While the operator is on that transponder it is the only beacon
  // anywhere near the span — the CW beacons are one to nine megahertz below it,
  // which no span this application can open will reach.  So it is offered for
  // the panadapter's level line and refused by the lock; qo100_beacon_lockable()
  // is the split.
  switch(sel) {
    case QO100_BEACON_SEL_UPPER:  return QO100_BEACON_UPPER;
    case QO100_BEACON_SEL_MIDDLE: return QO100_BEACON_MIDDLE;
    case QO100_BEACON_SEL_WB:     return QO100_WB_BEACON;
    default:                      return QO100_BEACON_LOWER;
  }
}

// A bare carrier the peak search can find: the two CW beacons, and only those.
gboolean qo100_beacon_has_carrier(int sel) {
  return sel==QO100_BEACON_SEL_LOWER || sel==QO100_BEACON_SEL_UPPER;
}

// ...a suppressed one a squaring loop can recover: the middle beacon.
gboolean qo100_beacon_is_bpsk(int sel) {
  return sel==QO100_BEACON_SEL_MIDDLE;
}

// ...and either of those is something the lock can be run against.
gboolean qo100_beacon_lockable(int sel) {
  return qo100_beacon_has_carrier(sel) || qo100_beacon_is_bpsk(sel);
}

// The line the LOOP measures, as against the line the dial is trued to.  A
// beacon spends most of its life resting, so the resting tone is the one that is
// always on air and the only one a tracker can follow without gaps; tracking
// anything else means whole integrated seconds in which the loop's own line is
// simply absent.  Measured, that tone IS the published figure
// (QO100_BEACON_REST_HZ, zero) — so this function is an identity today and
// stays because it is the seam the whole question lives on.  The 500 kHz
// pairing that identifies a CW beacon absolutely is unaffected either way, both
// tones being offset the same.
//
// Only the LOWER beacon's tones have been measured; the upper is assumed to be
// the same generator convention, which is also what QO100_FSK_SHIFT_HZ already
// assumes of both.
long long qo100_beacon_track_frequency(int sel) {
  long long f=qo100_beacon_frequency(sel);
  return qo100_beacon_has_carrier(sel)?f+QO100_BEACON_REST_HZ:f;
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
// WHICH of them the dial must be trued to is not a matter of taste, and it is
// not a matter of documents either: it was read out of the IARU convention
// ("the carrier is on the nominal frequency … first the carrier goes to 'space'
// (400 Hz lower)"), and on air that is simply not what this beacon does. The
// operator's dish, from the state the dial was truthful in: the published
// figure is the LOWER line and the beacon RESTS on the upper one. That is
// QO100_BEACON_REST_HZ in qo100.h, and it is applied where the loop decides
// what to MEASURE -- so what is tracked is the resting tone (always on air,
// no gaps) while the dial is still trued to the published one.
//
// The keyed tone therefore shows up one shift BELOW the tracked line, and the
// discriminator below reads that as confirmation rather than as a jump. It is
// the reading one shift ABOVE that says the loop has landed on the keyed tone
// and has to climb.
#define QO100_FSK_SHIFT_HZ   400.0   // the published shift of both CW beacons
// ...and WHICH SIDE the keyed tone is on, signed, which is the other half of the
// same question and is equally a measurement. The beacon rests on its published
// frequency and its keyed elements are one shift ABOVE it: the operator's log,
// every window of it, counted lines one shift above the anchor and NEVER one
// below, which places the loop on the lower of the two and the ident above it.
// Written the other way round, the discriminator below reads the ident as
// "you are on the keyed tone, climb" in every window it ever sees — it climbed,
// lost the line it climbed to, dropped the lock, re-acquired on the line it
// started from and climbed again: ±400 Hz of dial, about twice a minute, for
// ever. That is what "нихера не синкается" looked like from inside.
#define QO100_KEYED_FROM_REST_HZ (+QO100_FSK_SHIFT_HZ)
// How exactly a pair must show that shift. It was 80 Hz, which is not a test:
// the operator's log had +366.1 and +377.9 Hz counted as "exactly one shift"
// -- 34 and 22 Hz out on a spacing that comes from ONE synthesiser and is
// therefore exact. What the reading is worth is set by the tracking tolerance,
// since a reading too unsteady to be trimmed on cannot identify a tone either.
#define QO100_FSK_TOL_HZ      40.0
// The narrow transponder's three beacons are 250 kHz apart by construction, and
// the middle one is the confirmation a bare carrier cannot fake.
#define QO100_BPSK_OFFSET_HZ 250000.0
// How near the middle beacon's measured centre must land, and it is TIGHTER
// than one FSK shift on purpose: that is what makes the geometry identify the
// TONE as well as the beacon. The hump sits 250 kHz from the published tone, so
// a candidate on the keyed tone finds it 400 Hz out and is refused -- no tone
// discriminator, no history, no ratchet, just the distance between two beacons.
#define QO100_BPSK_CONFIRM_HZ   200.0
#define QO100_BPSK_CONFIRM_MAX      8  // candidates tried before giving up
#define QO100_TONE_WIN          20   // measurements judging which tone we are on
#define QO100_TONE_RUN           4   // ...answering one shift up before a locked
                                     // loop accepts that it is on the keyed tone
#define QO100_MIN_SNR         12.0   // peak/mean power in the window to trust a frame
// The dial's one INDEPENDENT check, and the reason it exists is the 400 Hz
// above. WHICH of the CW beacon's two tones the published figure names is a
// convention, and no measurement of that beacon can settle it: the loop reads
// +/-2 Hz on the wrong line exactly as it does on the right one, which is how a
// dial 400 Hz off shipped for a release with nothing on air to argue with it.
// The MIDDLE beacon is not a convention. It is 400 bd BPSK, so its spectrum is
// symmetric about its own carrier whether or not that carrier is suppressed,
// and where the middle of it lands is a direct reading of what the dial is
// worth -- through the same LNB, the same span and the same FFT as the lock.
// It is a CHECK and never a correction: a centroid over an 800 Hz-wide
// modulated signal is worth tens of hertz where the CW lock is worth two.
#define QO100_MID_WIN_HZ    4000.0   // half-window searched around the middle beacon
#define QO100_MID_SNR          4.0   // a bin joins the beacon at this x the floor
#define QO100_MID_MIN_BINS       6   // ...and this many of them make a signal
#define QO100_MID_MIN_WIDTH_HZ 150.0 // ...that is wider than one carrier, or it is
                                     // a carrier somebody parked there
#define QO100_MID_VERDICT_HZ  150.0  // how near 0 or one shift a reading must be
                                     // before it is called either
// ...and a verdict is never ONE reading. A centroid over an 800 Hz-wide
// modulated signal on a fading path is worth tens of hertz, and the decision
// resting on it -- which of two lines 400 Hz apart the published figure names
// -- is one this application gets to make once and then saves into
// band->errorLO. So the readings are collected and the MEDIAN of them answers,
// with the spread quoted beside it: a number nobody can check is worth less
// than a number that says how much it wobbled.
// Acquisition for the BPSK source is DELIBERATELY narrow, where the CW one
// sweeps half a megahertz. Squaring doubles every offset, so a +/-500 kHz hunt
// would be a megahertz of squared spectrum in a span that has not got one -- and
// squaring is not selective: every CW carrier on the transponder makes a line of
// its own at twice ITS offset, so a wide window there is a field of candidates
// that all look like beacons. A converter further out than this is brought in
// with a CW beacon first, and the status says so rather than searching for ever.
#define QO100_BPSK_ACQ_HZ  25000.0
#define QO100_MID_MED_N          5   // readings behind a verdict (one per 5 s)
#define QO100_MID_MIN_N          3   // ...and the fewest that may speak at all
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
// ...and with a decoder running the floor is one MEASUREMENT, not two seconds.
// The loop cannot act faster than it measures anyway, and while the converter
// is warming, every trim it skips is drift left in the audio -- see the gate
// below for why that is the thing to spend retunes on.
#define QO100_DECODER_APPLY_S    (QO100_AVG_MS/1000.0)
// A step at or below this is not a discontinuity, it is the CANCELLATION of one:
// the converter has drifted that far since the last trim and the correction puts
// the audio back where the decoder found it. Bigger than this and it is a jump
// worth waiting for a quiet moment.
#define QO100_FT8_TRIM_HZ       15.0
// ...and NOTHING may hold a correction longer than this. The decoder gate is
// allowed to DELAY a step to a quieter moment; it is not allowed to cancel it,
// and a loop that waits several slots for a quiet tail it keeps missing is a
// loop that does not correct at all. Operator's requirement, in his words: a
// correction has to happen at least once every fifteen seconds, FT8 or no FT8.
#define QO100_MAX_HOLD_S        15.0
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
static double   bsqr[QO100_FFT_N];   // ...and of z^2, for the BPSK source
static double   bsq_re[QO100_FFT_N]; // ...whose own working buffers these are,
static double   bsq_im[QO100_FFT_N]; // since spectrum_add consumes what it is given
static gboolean track_bpsk;          // the selected beacon is the BPSK one
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
static char     b_check[192]="";      // the middle beacon's verdict on the dial
static double   b_mid[QO100_MID_MED_N];  // ...and the readings behind it
static double   b_mid_w[QO100_MID_MED_N];
static int      b_mid_n;

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
static int      b_tone_dn;            // ...against how many answered one down
static double   b_med[QO100_MED_N];   // recent residuals, for the median
static double   b_med_t[QO100_MED_N];  // ...and the stream second each was read at
static int      b_med_n;
static double   b_stream_samples;      // the loop's own clock, in samples of stream
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
// probe_hz / probe_out: is there a candidate line at this baseband frequency in
// the same integrated second? The tone discriminator asks it about the position
// one shift BELOW the line it is tracking, and the answer is what makes "the
// resting tone is the one that is there when the other is not" a measurement
// instead of an assumption -- see beacon_frame.
static gboolean middle_beacon_centre(const double *pow_acc, int fs,
                                     double expected, double half,
                                     double *centre, double *width, int *nbins);

static gboolean find_carrier(const double *pow_acc, int fs,
                             double lo_hz, double hi_hz, double expected,
                             double partner_hz, double bpsk_off_hz,
                             gboolean dominant,
                             double probe_hz, gboolean *probe_out,
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
    // ...asked of EVERY candidate, not only of the ones the pairing array had
    // room for: a truncated list would answer "nothing there" on a busy band,
    // which is the answer that unlocks a 400 Hz step.
    if(probe_out!=NULL && fabs(sf-probe_hz)<QO100_FSK_TOL_HZ)
      *probe_out=TRUE;
    if(held<QO100_PAIR_MAX) { cand_bin[held]=m; cand_sf[held]=sf; held++; }
  }
  if(cand_out!=NULL) *cand_out=cands;
  if(held<1) return FALSE;

  int sel=-1;
  double best=1e18;
  gboolean confirmed=FALSE;

  // A BEACON IS KNOWN BY ITS NEIGHBOURS, and that is the only test here that a
  // bare carrier cannot pass. Every rule in front of the lock -- the agreement
  // run, the tracking tolerance, the slew guard -- rewards a line that stays
  // put, and an F1A beacon is the one signal on the band that by definition
  // does not: it hops 400 Hz while it idents. So a steady station parked near
  // the beacon wins every one of those tests, which is exactly what happened on
  // air (reported with a picture of it: an unmodulated carrier 2.3 kHz from the
  // beacon, the loop sitting on it, the beacon's own keyed pair drawn plainly
  // beside it).
  //
  // The narrow transponder's three beacons are 250 kHz apart by construction --
  // 10489.500 / .750 / 10490.000 -- so the MIDDLE one is the confirmation:
  // 400 bd BPSK is a hump about 800 Hz wide that nothing else on this band
  // looks like, and it is on air continuously. A candidate with that hump
  // exactly 250 kHz away (above it for the lower beacon, below for the upper)
  // IS the beacon.
  //
  // It is the middle one and not the other CW beacon because of the SPAN. The
  // two CW beacons are 500 kHz apart, so seeing both needs the dial within
  // ~130 kHz of 10489.750 on a 768 kHz span -- an operator sitting on the FT8
  // spot at 10489.540 has the far beacon 460 kHz away and out of the receiver
  // entirely, while the middle beacon is 210 kHz away and in plain view.
  // Nearest candidates first, and the first one confirmed wins: the test costs a
  // window sort, so it is not run against a hundred lines.
  if(dominant && bpsk_off_hz!=0.0) {
    static gboolean tried[QO100_PAIR_MAX];
    memset(tried,0,sizeof(gboolean)*(size_t)held);
    for(int pass=0;pass<QO100_BPSK_CONFIRM_MAX && !confirmed;pass++) {
      int    pick=-1;
      double pd=1e18;
      for(int i=0;i<held;i++) {
        if(tried[i]) continue;
        double d=fabs(cand_sf[i]-expected);
        if(d<pd) { pd=d; pick=i; }
      }
      if(pick<0) break;
      tried[pick]=TRUE;
      double c=0.0, w=0.0;
      int nb=0;
      double want=cand_sf[pick]+bpsk_off_hz;
      if(fabs(want)+QO100_MID_WIN_HZ>0.5*(double)fs) continue;   // outside the span
      if(middle_beacon_centre(pow_acc,fs,want,QO100_MID_WIN_HZ,&c,&w,&nb) &&
         fabs(c-want)<QO100_BPSK_CONFIRM_HZ && w>=QO100_MID_MIN_WIDTH_HZ) {
        sel=cand_bin[pick];
        best=pd;
        confirmed=TRUE;
      }
    }
  }

  // Failing that, the 500 kHz pairing, when both edges are in the window: a
  // candidate with a partner exactly that far away IS a beacon too. Among such
  // candidates -- there is normally exactly one pair -- take the nearest to the
  // expectation.
  if(sel<0 && partner_hz!=0.0) {
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
  if(dominant && !confirmed) {
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
// UTC seconds, and the only wall clock this file reads. Behind a hook because
// the question "how OFTEN does the loop get to move the radio" is answered in
// minutes of drift, and the offline harness runs a minute of stream in well
// under a second -- a case about the cadence cannot be written against a clock
// it does not drive.
static double qo100_clock_real(void) {
  return (double)(g_get_real_time()/1000)/1000.0;
}
static double (*qo100_clock)(void)=qo100_clock_real;

void qo100_beacon_set_clock(double (*fn)(void)) {
  qo100_clock=(fn!=NULL)?fn:qo100_clock_real;
}

// Is a decoder that cares about a step in the audio running on this receiver?
static gboolean qo100_decoder_slot(RECEIVER *rx, double *slot, double *quiet) {
  if(radio==NULL || rx==NULL) return FALSE;
  if(radio->decode_mode!=DECODE_FT8 && radio->decode_mode!=DECODE_FT4) return FALSE;
  if(rx->mode_a!=DIGU && rx->mode_a!=DIGL) return FALSE;   // the tap is mode-gated
  if(slot!=NULL)  *slot =(radio->decode_mode==DECODE_FT4)?QO100_FT4_SLOT_S :QO100_FT8_SLOT_S;
  if(quiet!=NULL) *quiet=(radio->decode_mode==DECODE_FT4)?QO100_FT4_QUIET_S:QO100_FT8_QUIET_S;
  return TRUE;
}

// How long until the loop's NEXT chance to move the radio -- which is what the
// control law's two time-shaped decisions are made of: how far to feed the
// drift forward, and how much of the measured error to take now. It is the trim
// rate limiter's own wait, and a whole slot ON TOP of it for a step big enough
// that the gate below makes it wait for one.
static double qo100_next_chance_s(RECEIVER *rx, double act) {
  double floor_s=qo100_decoder_slot(rx,NULL,NULL)?QO100_DECODER_APPLY_S
                                                 :QO100_MIN_APPLY_S;
  double need=(fabs(act)>0.0)?QO100_TRIM_HZ_S/fabs(act):QO100_FF_S;
  if(need<floor_s) need=floor_s;
  double slot=0.0;
  if(fabs(act)>QO100_FT8_TRIM_HZ && fabs(act)<=QO100_COARSE_HZ &&
     qo100_decoder_slot(rx,&slot,NULL))
    need+=slot;
  return need;
}

// Where the middle beacon's spectrum is CENTRED, in baseband Hz, out of the same
// accumulated power the lock is measured from. Deliberately a centroid and not a
// peak search: a BPSK signal has no peak to find, and its shape is what carries
// the frequency. The floor is the window's own median, so the transponder's
// noise slope does not drag the answer; only bins standing QO100_MID_SNR out of
// it are weighted, and by their excess over it rather than their power, or the
// loudest bin would decide a measurement whose whole point is the shape.
static gboolean middle_beacon_centre(const double *pow_acc, int fs,
                                     double expected, double half,
                                     double *centre, double *width, int *nbins) {
  const int N=QO100_FFT_N;
  const double bin_hz=(double)fs/(double)N;
  static double win[QO100_FFT_N];
  static double wsf[QO100_FFT_N];
  int n=0;
  for(int m=0;m<N;m++) {
    double sf=((m<=N/2)?(double)m:(double)(m-N))*bin_hz;   // signed baseband Hz
    if(fabs(sf)<QO100_DC_GUARD_HZ) continue;
    if(sf<expected-half || sf>expected+half) continue;
    wsf[n]=sf;
    win[n]=pow_acc[m];
    n++;
  }
  if(n<4*QO100_MID_MIN_BINS) return FALSE;

  static double sorted[QO100_FFT_N];
  memcpy(sorted,win,sizeof(double)*(size_t)n);
  for(int i=1;i<n;i++) {                       // insertion sort: n is a few hundred
    double x=sorted[i]; int j=i-1;
    while(j>=0 && sorted[j]>x) { sorted[j+1]=sorted[j]; j--; }
    sorted[j+1]=x;
  }
  double floor_p=sorted[n/2];
  if(floor_p<=0.0) return FALSE;

  double sw=0.0, sf1=0.0, sf2=0.0;
  int hit=0;
  for(int i=0;i<n;i++) {
    if(win[i]<QO100_MID_SNR*floor_p) continue;
    double w=win[i]-floor_p;
    sw+=w; sf1+=w*wsf[i]; sf2+=w*wsf[i]*wsf[i];
    hit++;
  }
  if(nbins!=NULL) *nbins=hit;
  if(hit<QO100_MID_MIN_BINS || sw<=0.0) return FALSE;
  double c=sf1/sw;
  double var=sf2/sw-c*c;
  if(centre!=NULL) *centre=c;
  if(width!=NULL) *width=(var>0.0)?2.0*sqrt(var):0.0;
  return TRUE;
}

// ...and what that reading MEANS, in the one sentence an operator can act on.
// The published figure is 10489.750: the check is what the dial says about it
// after the CW lock has had its way, so a reading near zero is the whole
// convention confirmed and a reading near one FSK shift is the convention
// inverted -- which is not a subtlety, it is every frequency this radio shows
// being 400 Hz out, self-consistently, with the beacon lock reporting +/-2 Hz
// throughout.
// The BPSK beacon has no line to find, so one is MADE. A 400 bd BPSK signal is
// one carrier multiplied by +/-1, and (+/-1)^2 is 1 -- so z^2 is that carrier at
// TWICE its baseband offset with the modulation gone entirely. This is the
// standard suppressed-carrier recovery and it is what the rest of the QO-100
// world tracks; the point of using it here is that a BPSK spectrum carries no
// tone convention, so a dial trued to it cannot be one 400 Hz shift out however
// the argument about the CW beacons' two tones ends.
//
// The expectation is kept UNWRAPPED (`expect2` may exceed the span) and only the
// comparison with a bin is wrapped, which is what removes the ambiguity that
// squaring would otherwise introduce: a line at 2f and one at 2f+fs are the same
// bin, and halving them gives answers fs/2 apart. Working relative to the
// expectation, the answer is expect2 + (small), so halving it lands where it
// should. It also HALVES the frequency error: a hertz of error in the squared
// line is half a hertz in the carrier.
static gboolean line_near(const double *pow_acc, int fs, double expect2,
                          double half2, double *off2, double *snr_out) {
  const int N=QO100_FFT_N;
  const double bin_hz=(double)fs/(double)N;
  if(half2>0.45*(double)fs) half2=0.45*(double)fs;   // never wrap onto itself
  double sum=0.0, peak=-1.0, peak_d=0.0;
  int pk=-1, count=0;
  for(int m=0;m<N;m++) {
    double sf=((m<=N/2)?(double)m:(double)(m-N))*bin_hz;
    if(fabs(sf)<QO100_DC_GUARD_HZ) continue;      // z^2 puts any DC offset here
    double d=sf-expect2;
    while(d> 0.5*(double)fs) d-=(double)fs;       // the squared line wraps; the
    while(d<-0.5*(double)fs) d+=(double)fs;       // expectation does not
    if(fabs(d)>half2) continue;
    double p=pow_acc[m];
    sum+=p; count++;
    if(p>peak) { peak=p; pk=m; peak_d=d; }
  }
  if(pk<1 || pk>=N-1 || count<8) return FALSE;
  double mean=sum/(double)count;
  if(mean<=0.0) return FALSE;
  if(snr_out!=NULL) *snr_out=peak/mean;
  if(peak/mean<QO100_MIN_SNR) return FALSE;

  // Parabolic interpolation on the three bins around the peak, in dB-less power
  // exactly as find_carrier does it.
  double y1=pow_acc[(pk-1+N)%N], y2=pow_acc[pk], y3=pow_acc[(pk+1)%N];
  double den=y1-2.0*y2+y3;
  double corr=(den!=0.0)?0.5*(y1-y3)/den:0.0;
  if(corr>1.0) corr=1.0;
  if(corr<-1.0) corr=-1.0;
  if(off2!=NULL) *off2=peak_d+corr*bin_hz;
  return TRUE;
}

static double median_of(const double *v, int n);   // ...defined with the loop below

// ...and the reading the other way round: locked to the BPSK beacon, which
// carries no tone convention, where the CW beacon's line falls IS the
// convention. The resting tone is the answer that keeps coming back -- the
// beacon spends most of its life on it -- so the median of the readings is the
// resting tone and a keyed second is an outlier it survives.
static void cw_tone_verdict(double off) {
  const double shift=QO100_FSK_SHIFT_HZ;
  if(b_mid_n<QO100_MID_MED_N) b_mid[b_mid_n++]=off;
  else {
    for(int i=1;i<QO100_MID_MED_N;i++) b_mid[i-1]=b_mid[i];
    b_mid[QO100_MID_MED_N-1]=off;
  }
  if(b_mid_n<QO100_MID_MIN_N) {
    snprintf(b_check,sizeof(b_check),
             "Lower CW beacon %+.0f Hz from its published figure, measuring "
             "(%d of %d)",off,b_mid_n,QO100_MID_MIN_N);
    return;
  }
  double med=median_of(b_mid,b_mid_n);
  int k=(int)llround(med/shift);
  if(fabs(med-(double)k*shift)>=QO100_MID_VERDICT_HZ)
    snprintf(b_check,sizeof(b_check),
             "Lower CW beacon %+.0f Hz from its published figure (%d readings) — "
             "not a whole %.0f Hz shift; the dial or the LNB is adrift",
             med,b_mid_n,shift);
  else if(k==0)
    snprintf(b_check,sizeof(b_check),
             "Lower CW beacon rests %+.0f Hz from its published figure "
             "(%d readings) — the tone model is right",med,b_mid_n);
  else
    snprintf(b_check,sizeof(b_check),
             "Lower CW beacon rests %+.0f Hz from its published figure "
             "(%d readings) — %+d shift%s: QO100_BEACON_REST_HZ is wrong by that "
             "much",med,b_mid_n,k,(k==1||k==-1)?"":"s");
}

static void middle_beacon_verdict(double off, double width, int bins) {
  const double shift=QO100_FSK_SHIFT_HZ;

  if(b_mid_n<QO100_MID_MED_N) {
    b_mid[b_mid_n]=off;
    b_mid_w[b_mid_n]=width;
    b_mid_n++;
  } else {
    for(int i=1;i<QO100_MID_MED_N;i++) { b_mid[i-1]=b_mid[i]; b_mid_w[i-1]=b_mid_w[i]; }
    b_mid[QO100_MID_MED_N-1]=off;
    b_mid_w[QO100_MID_MED_N-1]=width;
  }
  double med=median_of(b_mid,b_mid_n), medw=median_of(b_mid_w,b_mid_n);
  double lo=1e18, hi=-1e18;
  for(int i=0;i<b_mid_n;i++) {
    if(b_mid[i]<lo) lo=b_mid[i];
    if(b_mid[i]>hi) hi=b_mid[i];
  }

  if(medw<QO100_MID_MIN_WIDTH_HZ) {
    snprintf(b_check,sizeof(b_check),
             "Middle beacon: %d bins only %.0f Hz wide — that is a carrier, "
             "not the BPSK beacon; no verdict",bins,medw);
    return;
  }
  if(b_mid_n<QO100_MID_MIN_N) {
    snprintf(b_check,sizeof(b_check),
             "Middle beacon: %+.0f Hz, measuring (%d of %d readings)",
             off,b_mid_n,QO100_MID_MIN_N);
    return;
  }
  // The answer is counted in SHIFTS, not in the two cases that were expected.
  // There are three tones in play, not two: where the beacon rests, where it
  // keys, and where the published figure sits between them -- and the sources
  // disagree about the last. AMSAT-DL's own text has the carrier resting on
  // 'space', 400 Hz BELOW nominal, and the keyed information ON nominal, which
  // if true makes this loop TWO shifts out, not one. A verdict that only knew
  // 0 and +/-1 would have called that "neither" and told the operator nothing.
  int k=(int)llround(med/shift);
  if(fabs(med-(double)k*shift)>=QO100_MID_VERDICT_HZ)
    snprintf(b_check,sizeof(b_check),
             "Middle beacon %+.0f Hz off (%d readings, spread %.0f Hz) — not a "
             "whole %.0f Hz shift; check the LNB and the lock",
             med,b_mid_n,hi-lo,shift);
  else if(k==0)
    snprintf(b_check,sizeof(b_check),
             "Middle beacon (BPSK) centred %+.0f Hz — the dial agrees "
             "(%d readings, spread %.0f Hz, %.0f Hz wide)",
             med,b_mid_n,hi-lo,medw);
  else
    snprintf(b_check,sizeof(b_check),
             "Middle beacon %+.0f Hz off (%d readings, spread %.0f Hz) — the dial "
             "reads %.0f Hz %s, %+d shift%s of %.0f Hz: the lock is truing the "
             "wrong tone",
             med,b_mid_n,hi-lo,fabs(med),(med>0.0)?"HIGH":"LOW",
             k,(k==1||k==-1)?"":"s",shift);
}

static gboolean qo100_may_retune_now(RECEIVER *rx, double act, double since_s) {
  if(radio==NULL || rx==NULL) return TRUE;
  // The ceiling first: whatever else this function thinks, a correction that has
  // been waiting this long goes.
  if(since_s>=QO100_MAX_HOLD_S) return TRUE;
  // A correction bigger than a trim is not what this gate is for. It exists so
  // that a step does not land inside a transmission ft8_lib is demodulating at
  // a fixed frequency -- but a dial that is hundreds of hertz out is not
  // producing decodes worth protecting, it is producing decodes REPORTED at the
  // wrong frequency, and acquisition is a run of such steps: cold, at the
  // 35 kHz an LNB is easily out by, every one of them waited for a slot's quiet
  // tail and the first lock took minutes. Acquisition goes through at once;
  // trims, which is everything this gate was written for, wait.
  if(fabs(act)>QO100_COARSE_HZ) return TRUE;
  // ...and NOR is a trim, which is the correction this gate was hurting most.
  // Holding the dial still through a transmission does not hold the AUDIO
  // still: the converter keeps drifting, and to ft8_lib -- which demodulates
  // each candidate at one fixed frequency for the whole slot -- a signal
  // sliding 30 Hz through the decode is indistinguishable from the dial being
  // dragged 30 Hz. Measured against a 2 Hz/s converter with the gate holding
  // every trim: 32 Hz of audio swept inside one slot, and 96 Hz at 6 Hz/s,
  // where an FT8 tone is 6.25 Hz wide. That is what a bent trace on the
  // waterfall is. A trim of a few hertz CANCELS that slide; it is not a
  // discontinuity, and the decoder is better off with it than without it.
  if(fabs(act)<=QO100_FT8_TRIM_HZ) return TRUE;
  double slot=0.0, quiet=0.0;
  if(!qo100_decoder_slot(rx,&slot,&quiet)) return TRUE;
  double utc=qo100_clock();                              // UTC seconds
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
  memset(bsqr,0,sizeof(bsqr));
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
  b_tone_dn=0;
  b_med_n=0;
  b_stream_samples=0.0;
  b_check[0]='\0';
  b_mid_n=0;
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

// The middle beacon's verdict on the dial, empty until there is one. Separate
// from the lock's own status on purpose: the lock reports how steady it is,
// which it can do perfectly while being 400 Hz wrong, and this is the only
// line in the application that can tell the operator otherwise.
void qo100_beacon_check(char *buf, int size) {
  g_mutex_lock(&bmtx);
  g_strlcpy(buf,b_check,(gsize)size);
  g_mutex_unlock(&bmtx);
}

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
  if(track_bpsk) {
    // z^2, before spectrum_add consumes the raw block. The plain spectrum is
    // accumulated too: the middle beacon cannot be its own independent check,
    // so when it is the source the CW beacon becomes one, and that is read off
    // bpow (see the check below).
    for(int i=0;i<QO100_FFT_N;i++) {
      double r=bre[i], m=bim[i];
      bsq_re[i]=r*r-m*m;
      bsq_im[i]=2.0*r*m;
    }
    spectrum_add(bsq_re,bsq_im,bsqr);
  }
  spectrum_add(bre,bim,bpow);
  b_avg_samples+=(double)QO100_FFT_N;
  if(b_avg_samples<(double)track_fs*(QO100_AVG_MS/1000.0)) return;
  double avg_samples=b_avg_samples;

  double found, snr=0.0;
  int bins=0, cands=0;
  b_say_samples+=avg_samples;
  b_expect_samples+=avg_samples;
  b_stream_samples+=avg_samples;
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
  // Where the beacon's KEYED tone would be if the loop is on the resting one,
  // asked of the same second's spectrum -- see the tone discriminator below.
  gboolean tone_other=FALSE;
  double probe=expected+b_expect+QO100_KEYED_FROM_REST_HZ;
  // Where the MIDDLE beacon must be, relative to whichever CW beacon is
  // selected: above the lower one, below the upper one. Zero when there is
  // nothing to confirm against.
  double bpsk_off=0.0;
  if(radio->qo100_beacon_sel==QO100_BEACON_SEL_LOWER)
    bpsk_off= QO100_BPSK_OFFSET_HZ;
  else if(radio->qo100_beacon_sel==QO100_BEACON_SEL_UPPER)
    bpsk_off=-QO100_BPSK_OFFSET_HZ;
  gboolean got;
  if(track_bpsk) {
    // The squared domain: everything doubles, including the search width and
    // the error, and the answer comes back halved. There is no pairing and no
    // tone question here -- that is the whole reason this source exists.
    double off2=0.0;
    got=line_near(bsqr,track_fs,2.0*(expected+b_expect),
                     2.0*(tracking?QO100_TRACK_HZ:QO100_BPSK_ACQ_HZ),&off2,&snr);
    found=expected+b_expect+0.5*off2;
    bins=0;
    cands=got?1:0;
  } else
    got=find_carrier(bpow,track_fs,lo_hz,hi_hz,expected,partner,bpsk_off,!tracking,
                     probe,b_locked?&tone_other:NULL,
                     &found,&snr,&bins,&cands);
  if(!got) {
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
                 "Nothing found in the ±%.0f kHz this span covers — "
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
                  "Re-acquiring — the beacon was gone for 10 s",
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
  // The check RUNS BOTH WAYS, because whichever beacon the loop is locked to
  // cannot be the one that checks it.
  //
  //   * locked to a CW beacon, the middle one is the arbiter: BPSK is symmetric
  //     about its own published frequency and knows nothing of tone conventions.
  //   * locked to the middle one, the dial needs no convention at all -- so the
  //     CW beacon's own line becomes the MEASUREMENT of that convention, which
  //     is the argument this whole file has had twice. Where the resting tone
  //     sits relative to 10489.500 is then read straight off the spectrum,
  //     against a dial that has no opinion about it.
  //
  // Asked of the SAME second's spectrum, therefore before it is cleared, and
  // only once the lock has settled: before that the dial is knowingly wrong and
  // the reading would only say by how much. Every five seconds, like the
  // summary it sits beside.
  if(say && b_locked && b_settled && track_bpsk) {
    double cw=(double)(QO100_BEACON_LOWER-rx->frequency_a);
    double off=0.0, sn=0.0;
    if(fabs(cw)>span_half-2.0*QO100_FSK_SHIFT_HZ)
      g_strlcpy(b_check,"Lower CW beacon outside the span — nothing to check "
                        "the tone convention against",sizeof(b_check));
    else if(line_near(bpow,track_fs,cw,2.5*QO100_FSK_SHIFT_HZ,&off,&sn))
      cw_tone_verdict(off);
    else
      g_strlcpy(b_check,"Lower CW beacon not heard — tone convention "
                        "unchecked",sizeof(b_check));
    if(b_check[0]!='\0') log_info("qo100: %s\n",b_check);
  } else if(say && b_locked && b_settled) {
    double mid=(double)(QO100_BEACON_MIDDLE-rx->frequency_a);
    double c=0.0, w=0.0;
    int nb=0;
    if(fabs(mid)+QO100_MID_WIN_HZ>span_half)
      g_strlcpy(b_check,"Middle beacon outside the span — no independent "
                        "check of the dial (widen the span to 500 kHz)",
                sizeof(b_check));
    else if(fabs(mid)<2.0*QO100_DC_GUARD_HZ)
      g_strlcpy(b_check,"Middle beacon sits on the centre — no independent "
                        "check of the dial from there",sizeof(b_check));
    else if(middle_beacon_centre(bpow,track_fs,mid,QO100_MID_WIN_HZ,&c,&w,&nb)) {
      middle_beacon_verdict(c-mid,w,nb);
      log_info("qo100: %s\n",b_check);
    } else
      g_strlcpy(b_check,"Middle beacon not heard — no independent check "
                        "of the dial",sizeof(b_check));
  }

  spectrum_clear();
  b_since_apply+=avg_samples;
  b_dry_samples=0.0;
  b_gone_samples=0.0;

  double residual=found-expected;

  // Which of the beacon's two tones is the loop sitting on? THE RESTING TONE IS
  // THE ONE THAT IS THERE WHEN THE OTHER IS NOT, and it is the one this loop
  // tracks (qo100_beacon_track_frequency) -- so a loop that has landed on the
  // KEYED tone instead finds its own line missing in every idle second and
  // reads the resting one, one shift AWAY on the side the resting tone lies
  // (QO100_KEYED_FROM_REST_HZ: the ident is above, so the resting tone is
  // below). Count those against the measurements as a whole (this is before the
  // agreement gate on purpose: a reading that breaks the run IS the evidence).
  //
  // And a line one shift on the OTHER side of the anchor is the same evidence
  // pointing the other way -- that is the beacon's own keyed tone, i.e. proof
  // the loop is already where it belongs -- so it is looked for in the SAME
  // second's spectrum rather than waited for as a reading of its own
  // (`tone_other`). Both halves matter:
  //
  //  * counted one way only -- which is how this was written -- the swap is a
  //    RATCHET. It can fire, nothing can ever argue back, and the misfire is
  //    permanent: it goes into band->errorLO and is saved to the props file, so
  //    the next session starts on it. With the two tones the wrong way round in
  //    the model it fired for ever: the operator's log counted lines one shift
  //    above the anchor in every window and none below, so the loop climbed,
  //    lost the line it climbed to, dropped the lock and did it again.
  //  * and the argument has to come out of the SPECTRUM, not out of which line
  //    happened to be picked. When something sits one shift beyond the resting
  //    tone, the beacon's own keyed tone is one shift the other way, both are
  //    equally far from the expectation, and which of the two the nearest-line
  //    rule returns is decided by where the FFT grid falls -- a coin whose bias
  //    does not change all session. Asking the spectrum instead is not a coin:
  //    three lines a shift apart mean the middle one has a neighbour, and a
  //    beacon has two tones, not three.
  if(b_locked && !track_bpsk) {
    b_tone_win++;
    if(tone_other) b_tone_dn++;
    else if(fabs(residual-b_expect+QO100_KEYED_FROM_REST_HZ)<QO100_FSK_TOL_HZ)
      b_tone_hits++;
    if(b_tone_win>=QO100_TONE_WIN) {
      if(b_tone_hits>=QO100_TONE_RUN && b_tone_dn==0) b_tone_up=TRUE;
      b_tone_win=0;
      b_tone_hits=0;
      b_tone_dn=0;
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
  //
  // ...and while LOCKED that tolerance AGES, for the same reason the slew
  // guard's allowance does and at the same rate. The anchor is the run's FIRST
  // reading, so a converter drifting at 6 Hz/s has legitimately walked 42 Hz
  // from it by the seventh second -- and frozen at 40 Hz the run then broke,
  // every seventh second, and stayed broken for the two it takes to build
  // another one. That is invisible on a loop that may act every second and
  // fatal on one gated by a decoder: the single chance in fifteen seconds kept
  // landing in the gap. Measured at 6 Hz/s with FT8 selected, before the
  // ageing: two slots in three missed and the dial 143 Hz out, where the
  // sawtooth of one correction per slot is 45. The cap is the slew guard's own
  // and stays well clear of the 400 Hz ident, so no amount of ageing ever makes
  // the beacon's other line an agreement.
  double tol=b_locked?QO100_TRACK_TOL_HZ:QO100_LOCK_TOL_HZ;
  if(b_locked) {
    tol+=QO100_SLEW_RATE_HZ_S*(b_run_samples/(double)track_fs);
    if(tol>QO100_SLEW_MAX_HZ) tol=QO100_SLEW_MAX_HZ;
  }
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
             "tolerance %.0f Hz; tone %d on the resting side / %d on the keyed "
             "side of %d%s)\n",
             found,expected,residual,snr,cands,b_locked?"locked":"acquiring",
             b_run,b_run_samples*1000.0/(double)track_fs,QO100_AGREE_MS,
             delta,tol,
             b_tone_hits,b_tone_dn,b_tone_win,b_tone_up?", armed":"");

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
  // the guard. THE RESTING TONE IS THE ONE THAT IS THERE WHEN THE OTHER IS NOT,
  // and it is the tone this loop tracks, so a loop that has landed on the keyed
  // one instead finds its own line missing in every idle second and reads the
  // resting tone one shift away, on the side QO100_KEYED_FROM_REST_HZ says it
  // lies (the ident is above, so the resting tone is below). That is what
  // QO100_TONE_RUN counts, and QO100_FSK_TOL_HZ is what makes "exactly one
  // shift" mean something, the shift coming from one synthesiser.
  //
  // Without this the loop can be right and unable to act on it: acquisition
  // landing in a second where only the keyed tone was on air locks to it, the
  // resting tone is then refused as a 400 Hz jump for 30 measurements, and the
  // correction arrives only because the lock is finally dropped altogether.
  // Half a minute of "ignoring +400 Hz" in the log, and a dial 400 Hz off the
  // whole time.
  gboolean tone_swap=(b_tone_up &&
                      fabs(residual-b_expect+QO100_KEYED_FROM_REST_HZ)<QO100_FSK_TOL_HZ);
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
  // How long until this loop may move the radio again -- a second, or a whole
  // FT8 slot if a decoder is gating it. Both halves of the control law below
  // are that number: what a drift will have added by then, and whether there is
  // another chance soon enough to make a half step worth taking.
  double now_s=b_stream_samples/(double)track_fs;

  // EVERY believed reading goes into the window, and each carries the stream
  // second it was read at. Two things were wrong with the window this replaces,
  // and both of them only bite once something makes the loop skip measurements:
  //
  //  * a COARSE reading emptied it. That is right when a coarse step is applied
  //    -- the radio moves and the history is about the old dial -- but the wipe
  //    happened on the READING, and with a decoder slot gate in front of the
  //    loop a coarse reading is refused fourteen seconds out of fifteen. So the
  //    window was cleared over and over by readings that changed nothing, and
  //    the one moment the loop was allowed to act it was still "settling 1/7".
  //    The step itself does not need the wipe either: the shift at the bottom
  //    of this function already re-expresses every stored residual in the new
  //    frame, and it does so without throwing the drift estimate away.
  //  * and an entry was assumed to be one second old. A measurement integrates
  //    QO100_AVG_MS of stream, a step costs another half second of hold, and a
  //    gated loop lets whole seconds pass -- so the slope, which is the drift
  //    in Hz per SECOND, was computed by dividing by an entry count. It is
  //    divided by the clock now.
  if(b_med_n<QO100_MED_N) {
    b_med[b_med_n]=residual;
    b_med_t[b_med_n]=now_s;
    b_med_n++;
  } else {
    for(int i=1;i<QO100_MED_N;i++) { b_med[i-1]=b_med[i]; b_med_t[i-1]=b_med_t[i]; }
    b_med[QO100_MED_N-1]=residual;
    b_med_t[QO100_MED_N-1]=now_s;
  }

  // The window's own SLOPE, robustly, and BEFORE the two branches: the drift is
  // the converter's rate and has nothing to do with how big the error happens
  // to be, so a coarse step gets it fed forward exactly as a trim does. Without
  // that, a correction taken while the error is large lands the dial on where
  // the beacon was at that instant and it starts falling behind again the same
  // second -- and with a slot gate at 6 Hz/s the error grows past
  // QO100_COARSE_HZ inside every slot, so EVERY correction was that kind.
  // It is the median of the consecutive rates, believed only when every one of
  // them agrees with it in sign: a converter drifting is a run of readings all
  // moving the same way, and jitter, however wide, is not. Nothing here takes a
  // bare gradient off two endpoints -- that is one wild reading away from
  // claiming any rate you like.
  double slope=0.0;
  if(b_med_n>=QO100_MED_N) {
    double diff[QO100_MED_N-1];
    int nd=0;
    for(int i=1;i<b_med_n;i++) {
      double dt=b_med_t[i]-b_med_t[i-1];
      if(dt>0.0) diff[nd++]=(b_med[i]-b_med[i-1])/dt;
    }
    if(nd>0) {
      slope=median_of(diff,nd);
      for(int i=0;i<nd;i++) if(diff[i]*slope<=0.0) { slope=0.0; break; }
    }
  }

  // The drift is fed forward to the MIDDLE of the wait ahead -- half of it, not
  // all: feeding the whole wait forward was measured and is unstable, because
  // the wait itself shrinks as the error it is derived from grows (161.9 Hz of
  // audio swept inside a slot at 6 Hz/s, against 11.9 for half). The wait is
  // asked for rather than assumed: QO100_FF_S was "about how long until the next
  // trim", which is only true when nothing is holding the loop back. Half the
  // wait puts the dial in the middle of the error the interval sweeps through
  // (+/-d*W/2) instead of at one end of it (0..d*W). It needs `act`, so both
  // branches below compute their own.
  double act, ff=0.0, chance=QO100_FF_S;
  if(fabs(residual)>QO100_COARSE_HZ) {
    act=residual;                       // a real offset, measured directly
    chance=qo100_next_chance_s(rx,act);
    ff=slope*0.5*chance;
  } else {
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
    // The median is the middle of the window, so under a drift it is stale by
    // the age of the middle READING -- which is a clock reading now, not
    // half the entry count -- and the correction lands later still. Both are
    // known, so both are taken out: what is acted on is where the error will
    // BE, not where it was.
    double med=median_of(b_med,b_med_n);
    act=med+slope*(now_s-b_med_t[b_med_n/2]);
    chance=qo100_next_chance_s(rx,act);
    ff=slope*0.5*chance;

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
      double dt=b_med[i]-slope*(b_med_t[i]-b_med_t[0]);
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
  if(!qo100_may_retune_now(rx,act,b_since_apply/(double)track_fs)) {
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
    double floor_s=qo100_decoder_slot(rx,NULL,NULL)?QO100_DECODER_APPLY_S
                                                   :QO100_MIN_APPLY_S;
    if(need<floor_s) need=floor_s;
    if(need>QO100_MAX_HOLD_S) need=QO100_MAX_HOLD_S;   // the ceiling, again
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
  // The damping gain is a bet that another chance is coming shortly: take half,
  // measure again a second later, take half of what is left. With a decoder
  // slot gate in front of the loop that bet is off -- the next chance is a
  // whole slot away, so a half step is half the error left standing for fifteen
  // seconds while the converter keeps drifting into it. Measured on the shipped
  // loop with FT8 selected and an LNB at 2 Hz/s: a standing 56 Hz, and the
  // steady state is 26x the drift rate, so a warming converter at 6 Hz/s sat
  // 170 Hz off. What made the half safe is still there -- the estimate is the
  // median of seven seconds and has to beat that window's own scatter -- so the
  // loop that only gets one chance takes the whole of it.
  double gain=(chance>2.0*QO100_AVG_MS/1000.0)?1.0:QO100_GAIN;
  double step=(fabs(act)>QO100_COARSE_HZ)?act:(act*gain+ff);
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
  snprintf(b_status,sizeof(b_status),"Locked  %+.1f Hz  (LO %+.0f Hz)",
           residual,b_applied+step);

  // Queue the retune for the GTK thread, at most one at a time: if the applier
  // has not run yet the next frame's measurement is stale anyway.
  if(g_atomic_int_compare_and_exchange(&apply_queued,0,1)) {
    // The history is not thrown away across a step -- that would make every
    // correction wait a whole window for a new one, and at 0.5 Hz/s of drift
    // the error grows 2.5 Hz in that gap (measured: 1.95 -> 3.62 Hz of movement
    // inside a 15 s slot). It is SHIFTED instead: after the radio moves by
    // `step`, every stored residual means `residual - step` in the new frame.
    // Inside this branch, because a step that was NOT queued moves nothing and
    // a window shifted for it would be describing a dial that never existed.
    for(int i=0;i<b_med_n;i++) b_med[i]-=step;
    pend_step=step;
    b_tone_up=FALSE;                    // ...spent, whether or not it was one
    b_applied+=step;
    b_step_prev=step;                   // ...and what it must be worth next frame
    b_res_prev=residual;
    b_expect=residual-step;             // where the next measurement must land
    b_expect_samples=0.0;
    b_since_apply=0.0;
    b_hold=track_fs/QO100_HOLD_DIV;     // wait for data that has the step in it
    b_stream_samples+=(double)b_hold;   // ...which is stream the clock still counts
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
  if(!qo100_beacon_lockable(radio->qo100_beacon_sel)) {
    g_mutex_lock(&bmtx);
    beacon_reset_locked();
    g_strlcpy(b_status,
              "The WB beacon is DVB-S2 \342\200\224 no carrier to lock to; pick a CW or the middle beacon",
              sizeof(b_status));
    g_mutex_unlock(&bmtx);
    return;
  }

  g_mutex_lock(&bmtx);

  // The line to MEASURE, which is not the published one: see
  // qo100_beacon_track_frequency().
  long long beacon=qo100_beacon_track_frequency(radio->qo100_beacon_sel);
  gboolean bpsk=qo100_beacon_is_bpsk(radio->qo100_beacon_sel);
  if(rx!=track_rx || track_fs!=rx->sample_rate || track_beacon!=beacon ||
     bpsk!=track_bpsk) {
    track_rx=rx;
    track_fs=rx->sample_rate;
    track_beacon=beacon;
    track_bpsk=bpsk;       // ...which decides what is even measured, so a change
    beacon_reset_locked(); // of source starts the whole loop again
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
