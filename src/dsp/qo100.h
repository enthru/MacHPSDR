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

#ifndef _QO100_H
#define _QO100_H

#include <gtk/gtk.h>

#include "receiver.h"

/* QO-100 (Es'hail-2) narrow-band transponder.
 *
 * A geostationary amateur transponder, so — unlike the low-orbit satellites the
 * SAT/RSAT split modes were written for — there is no Doppler worth tracking and
 * the offset between uplink and downlink is a constant.  What there IS instead is
 * a receive chain that lies to you: the downlink at 10.489 GHz is brought down by
 * an LNB whose local oscillator is a free-running (or at best a crystal-PLL)
 * device sitting outdoors, so the frequency the radio reports is wrong by
 * kilohertz and walks as the dish warms up.  That is what qo100_beacon_* is for.
 *
 * Everything here is downlink frequencies unless the name says uplink.
 */

/* ---------------- narrow-band transponder ---------------- */

/* Transponder edges and the three beacons, from AMSAT-DL's own band plan
   (rev5, 14 Feb 2020, plus the later broadcast/emergency/multimedia additions).
   The lower and upper beacons ARE the transponder edges; the middle beacon sits
   in the centre.

   NOTE the 2020 change: the narrow-band transponder was originally 250 kHz
   (2400.050…2400.300 up / 10489.550…10489.800 down) and was EXTENDED to 500 kHz
   on 14 Feb 2020. The older figures are still quoted in plenty of places —
   AMSAT-UK's own satellite page still had them when this was checked on
   2026-08-09 — so a reference that disagrees with these numbers is probably
   just describing the pre-2020 transponder. */
#define QO100_NB_DOWN_LOW    10489500000LL   /* = lower beacon */
#define QO100_NB_DOWN_HIGH   10490000000LL   /* = upper beacon */
#define QO100_BEACON_LOWER   10489500000LL   /* CW, F1A 400 Hz shift */
#define QO100_BEACON_MIDDLE  10489750000LL   /* 400 bd BPSK */
#define QO100_BEACON_UPPER   10490000000LL   /* CW and other modulations */

/* ---------------- wideband (DATV) transponder ---------------- */

/* The satellite's second amateur transponder, half a megahertz above the narrow
   one and eighteen times as wide. Figures from AMSAT-DL/BATC "QO-100 Wideband
   Transponder — 2021 Operating Guidelines and Bandplan", version 3 final,
   6 February 2021, whose note 5 gives the transponder itself as
   "Uplink 2401.0 – 2410.0 MHz RHCP, Downlink 10490.5 – 10499.5 MHz Horizontal".

   TRAP, the same shape as the narrow transponder's 2020 extension: the figure in
   wide circulation for this transponder is "8 MHz, 2401.5–2409.5 / 10491.0–
   10499.0", which is its nominal bandwidth rather than the range the published
   plan allocates — and the plan's own sections and channels run from 10490.5 to
   10499.25, i.e. outside those numbers at BOTH ends. The V3 figures are what is
   used here, and the consistency check that settles any source is the offset:
   both edges must differ from their uplink by exactly QO100_TP_OFFSET, which the
   8 MHz figures also satisfy — so the offset agreeing is NOT evidence that a set
   of edges is the current one.

   What this application can and cannot do with it, because it changes what the
   support is for: MacHPSDR does not demodulate DVB-S2 and never will — the
   wideband transponder carries digital television. What it gives the operator is
   a truthful dial through the same LNB, the channel plan drawn over the
   spectrum, and VFO B tracking the matching uplink for a separate DATV
   transmitter. It is a spectrum-monitoring aid, not a receiver. */
#define QO100_WB_DOWN_LOW    10490500000LL
#define QO100_WB_DOWN_HIGH   10499500000LL
#define QO100_WB_BEACON      10491500000LL   /* DVB-S2, 1500 kS, FEC 4/5, from Qatar */

/* Which transponder the operator is set up for (radio->qo100_transponder). It
   selects what "Set up transponder mode" tunes to and nothing else — the band
   plan drawn on the panadapter follows the frequency on the dial, since the two
   transponders cannot overlap. */
#define QO100_TRANSPONDER_NB  0
#define QO100_TRANSPONDER_WB  1

/* Uplink 2400.000…2400.500 MHz against downlink 10489.500…10490.000 MHz, so the
   narrow transponder is non-inverting with a constant translation of exactly
   this — and the 2020 extension did NOT change it, both edges still differ by
   this figure. The wideband transponder is translated by the same amount
   (2401.000 → 10490.500, 2410.000 → 10499.500), which is why one pair of
   converters serves both. */
#define QO100_TP_OFFSET       8089500000LL   /* downlink - uplink */

/* ---------------- band plan ---------------- */

/* Both transponders' plans live in one table, drawn by frequency: the operator
   sees whichever applies to where the dial is, with no mode to get wrong.
   They cannot collide — the narrow transponder ends at 10490.000 and the
   wideband one starts at 10490.500. */
typedef enum {
  QO100_SEG_SPAN = 0,   /* a stretch of the transponder with one use: tinted band */
  QO100_SEG_BEACON,     /* one frequency: full-height marker */
  QO100_SEG_CHANNEL,    /* a recommended DATV channel centre: tick above the strip */
} QO100_SEG_KIND;

typedef struct _QO100_SEGMENT {
  long long low;        /* downlink Hz, inclusive */
  long long high;       /* == low for BEACON and CHANNEL */
  const char *label;
  double r, g, b;       /* overlay tint */
  QO100_SEG_KIND kind;
  int rank;             /* CHANNEL only: 0 = 125 kS grid, 1 = also 333 kS, 2 = 1 MS */
} QO100_SEGMENT;

extern int  qo100_segment_count(void);
extern const QO100_SEGMENT *qo100_segment(int index);

/* TRUE when f (absolute downlink Hz) is inside the named transponder;
   qo100_in_transponder() is either of them. */
extern gboolean qo100_in_nb_transponder(long long f);
extern gboolean qo100_in_wb_transponder(long long f);
extern gboolean qo100_in_transponder(long long f);

/* ---------------- converters ---------------- */

/* Titles of the two transverter entries the QO-100 page creates. They are also
   how it FINDS them again, so pressing the button twice edits the same two rows
   instead of eating two more of the eight slots. */
#define QO100_XVTR_RX_TITLE "QO-100 RX"
#define QO100_XVTR_TX_TITLE "QO-100 TX"

/* The near-universal LNB: a standard universal LNB's low band. Worth a default
   because almost every QO-100 station starts here; the uplink converter has no
   equivalent (it depends entirely on the operator's IF), so its default is 0 =
   no converter, i.e. a radio that reaches 2.4 GHz by itself. */
#define QO100_DEFAULT_LNB_LO  9750000000LL

/* Create (or update) the two transverter bands the satellite needs, from the LO
   frequencies persisted on RADIO. Returns FALSE with a reason in `msg` if there
   are not enough free transverter slots. An existing entry's LO ERROR is kept —
   it is the beacon lock's accumulated measurement, not something to reset
   because the operator pressed the button again. */
extern gboolean qo100_create_transverters(RADIO *r, char *msg, int msgsz);

/* ---------------- transponder mode ---------------- */

/* Put VFO B on the uplink that matches VFO A's downlink and link the two with the
   non-inverting SAT split, so tuning the receiver drags the transmitter along.
   Creates the converters first if they are missing and moves the dial onto
   radio->qo100_transponder's downlink unless it is already there. Returns FALSE
   (and changes nothing) only when there are no transverter slots to be had. */
extern gboolean qo100_transponder_setup(RADIO *r);

/* ---------------- beacon lock (LNB drift correction) ---------------- */

/* Raw off-air I/Q tap, called from receiver.c:full_rx_buffer on the RX audio
   thread. Returns immediately unless the lock is enabled and rx is the receiver
   being tracked. Buffer order is the receiver's own (Q, I) — see the note in
   qo100.c and in ppm_cal.c. */
extern void qo100_beacon_iq_feed(RECEIVER *rx, const double *iq, int n_frames);

/* Display-only readouts, safe to call from the GTK thread. */
extern gboolean qo100_beacon_locked(void);
extern double   qo100_beacon_residual_hz(void);  /* last measured error, Hz */
extern double   qo100_beacon_applied_hz(void);   /* correction currently applied */
extern void     qo100_beacon_status(char *buf, int size);

/* ...and the MIDDLE beacon's verdict on the dial, empty until there is one.
   The CW lock cannot check itself: which of an F1A beacon's two tones the
   published figure names is a convention, and the loop reads the same +/-2 Hz
   on the wrong line as on the right one. The middle beacon is 400 bd BPSK and
   its spectrum is symmetric about its own published frequency, so where it is
   centred is a direct reading of what the dial is worth. Needs a span reaching
   10489.750 (500 kHz from the narrow transponder's edges). */
extern void     qo100_beacon_check(char *buf, int size);

/* Forget the lock state (e.g. when the operator switches beacon or band). The
   correction already written into the band's LO error is deliberately kept — it
   is a real measurement of the converter, not session state. */
extern void qo100_beacon_reset(void);

/* Drop the tracked-receiver pointer if it is this one, and stop the lock.
   Called from receiver_destroy() before the receiver is freed; a no-op for any
   other receiver. */
extern void qo100_beacon_forget_receiver(RECEIVER *rx);

/* Which beacon radio->qo100_beacon_sel selects, as an absolute downlink Hz, and
   whether that beacon has a carrier a frequency lock can be run against. The
   wideband beacon has NOT: it is DVB-S2, a suppressed-carrier signal. It is
   still offered, because the panadapter's level reference line wants it (the
   wideband transponder's power rule is written against that beacon), and it is
   the only beacon in the span while the operator is on the wideband transponder. */
extern long long qo100_beacon_frequency(int sel);
extern gboolean  qo100_beacon_has_carrier(int sel);

/* ...and the middle beacon, which has no line either but whose carrier a
   squaring loop recovers exactly (400 bd BPSK is +/-1 on one carrier, so z^2 is
   that carrier at twice the offset with the modulation gone). It is the source
   that cannot be one shift out, a BPSK spectrum carrying no tone convention —
   which is what the rest of the QO-100 world calibrates against. */
extern gboolean  qo100_beacon_is_bpsk(int sel);
extern gboolean  qo100_beacon_lockable(int sel);

/* An F1A beacon is TWO lines 400 Hz apart and the published figure names only
   one of them; which one is the whole difference between a truthful dial and a
   dial 400 Hz off, and it has now been settled by measurement rather than by
   reading a standard. The beacon RESTS on its published frequency and its keyed
   elements sit one shift ABOVE it (QO100_KEYED_FROM_REST_HZ in qo100.c), so the
   tone that is on air when the other is not — the only one a loop can track
   without gaps — IS the one the dial is trued to, and this offset is zero.
   Two independent instruments on the operator's dish, 2026-09-02, both against
   a dial that assumed the opposite: the middle BPSK beacon read a median +485 Hz
   (it is symmetric about 10489.750 and knows nothing of tone conventions), and
   the lock's own tone counters reported lines one shift ABOVE the anchor in
   every window and never one below — i.e. the loop was sitting on the lower of
   the two and the lower is the one that is nearly always there.
   It is kept as a named constant, not deleted, because it is the seam the
   question lives on: qo100_beacon_track_frequency() is where the loop looks and
   qo100_beacon_frequency() is what the operator tunes to. */
#define QO100_BEACON_REST_HZ 0LL
extern long long qo100_beacon_track_frequency(int sel);

/* The UTC clock the FT8/FT4 slot gate is judged against, replaceable so
   tools/qo100_offline.c can run the loop against STREAM time: the cadence cases
   cover minutes of drift in a run that lasts under a second. NULL restores the
   real clock. Nothing in the application calls it. */
extern void qo100_beacon_set_clock(double (*fn)(void));

/* How often the lock is allowed to move the radio, in TENTHS of a second, and
   the bounds the operator's setting is clamped to (0.1 s to a minute).
   It replaces a cadence the loop worked out for itself from the error, the
   measured drift and a warm-up window -- three interacting rules with nothing
   in the UI that could overrule any of them. This is one number and it means
   exactly what it says: a trim goes out when the interval has elapsed and the
   loop has a reading it believes.
   Two things it deliberately does NOT govern, because neither is a trim:
   a COARSE step (an offset above QO100_COARSE_HZ on a lock that has not
   settled) is acquisition and goes out at once, and the FT8/FT4 slot gate may
   still hold a step back for the quiet tail of a slot -- delaying it, never
   cancelling it (QO100_MAX_HOLD_S).
   The floor of what is USEFUL is one measurement plus the post-retune hold --
   QO100_AVG_MS of stream and QO100_HOLD_DIV of a second, about 1.5 s, measured
   in tools/qo100_offline.c. Below that the interval simply means "every
   measurement"; it is offered anyway because the alternative is a minimum the
   operator has to discover by experiment. */
#define QO100_CORRECT_DS_MIN     1   /* 0.1 s */
#define QO100_CORRECT_DS_MAX   600   /* 60 s  */
#define QO100_CORRECT_DS_DEFAULT 20  /* 2.0 s -- the floor the old adaptive
                                        limiter used, so an existing station's
                                        behaviour is unchanged until it is
                                        changed on purpose */

#define QO100_BEACON_SEL_LOWER  0
#define QO100_BEACON_SEL_UPPER  1
#define QO100_BEACON_SEL_WB     2
#define QO100_BEACON_SEL_MIDDLE 3   /* appended, not inserted: the value is
                                       persisted, so an existing props file has
                                       to keep meaning what it meant */
#define QO100_BEACON_SEL_MAX    3

/* What a radio with no props file starts on, and what an out-of-range value in
   one falls back to.  It is the MIDDLE beacon and not the lower one for the
   reason qo100_beacon_frequency() gives at length: a 400 bd BPSK spectrum is
   symmetric about its own published frequency, so locking to it cannot be one
   400 Hz key-shift out, whereas which of an F1A beacon's two lines the
   published figure names is a convention this tree has read out of a document
   and got wrong twice.  A default is exactly the setting nobody checks, so it
   is the one place the ambiguous source must not be. */
#define QO100_BEACON_SEL_DEFAULT QO100_BEACON_SEL_MIDDLE

#endif
