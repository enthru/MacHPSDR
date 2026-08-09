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

/* Transponder edges and the three beacons (AMSAT-DL). The lower and upper
   beacons ARE the transponder edges; the middle beacon sits in the centre and is
   the reference the "keep your signal below the beacon" rule refers to. */
#define QO100_NB_DOWN_LOW    10489550000LL   /* = lower beacon, CW */
#define QO100_NB_DOWN_HIGH   10489800000LL   /* = upper beacon, CW */
#define QO100_BEACON_LOWER   10489550000LL   /* CW */
#define QO100_BEACON_MIDDLE  10489675000LL   /* 400 bd BPSK */
#define QO100_BEACON_UPPER   10489800000LL   /* CW */

/* Uplink 2400.050…2400.300 MHz against downlink 10489.550…10489.800 MHz, so the
   transponder is non-inverting with a constant translation of exactly this. */
#define QO100_TP_OFFSET       8089500000LL   /* downlink - uplink */

/* ---------------- band plan ---------------- */

typedef struct _QO100_SEGMENT {
  long long low;        /* downlink Hz, inclusive */
  long long high;
  const char *label;
  double r, g, b;       /* overlay tint */
  gboolean beacon;      /* a single-frequency marker rather than a span */
} QO100_SEGMENT;

extern int  qo100_segment_count(void);
extern const QO100_SEGMENT *qo100_segment(int index);

/* TRUE when f (absolute downlink Hz) is inside the narrow-band transponder. */
extern gboolean qo100_in_transponder(long long f);

/* ---------------- transponder mode ---------------- */

/* Put VFO B on the uplink that matches VFO A's downlink and link the two with the
   non-inverting SAT split, so tuning the receiver drags the transmitter along.
   Returns FALSE (and changes nothing) if VFO A is not on the downlink. */
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

/* Forget the lock state (e.g. when the operator switches beacon or band). The
   correction already written into the band's LO error is deliberately kept — it
   is a real measurement of the converter, not session state. */
extern void qo100_beacon_reset(void);

/* Which beacon radio->qo100_beacon_sel selects, as an absolute downlink Hz. */
extern long long qo100_beacon_frequency(int sel);

#endif
