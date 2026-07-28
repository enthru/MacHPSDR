/* Copyright (C)
* 2026 - MacHPSDR fork
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

/*
 * Software iambic keyer (Phase 4.4b): a canonical Curtis A/B paddle state
 * machine driving cw_encoder.c's real-time keyer tone (cw_tx_key()). Two
 * paddle inputs (DOT/DASH) come from the keyboard (receiver.c, `[`/`]`) or a
 * MIDI controller (midi3.c, CWLEFT/CWRIGHT); radio->cw_keyer_mode selects
 * KEYER_STRAIGHT (DOT paddle = straight key, no timing) or iambic Mode A/B
 * (radio->cw_keyer_speed/weight time the dit/dah/space elements).
 *
 * The state machine itself has no RADIO/GTK dependency beyond reading
 * radio->cw_keyer_* and calling cw_tx_key()/read_time_now(), so it can be
 * exercised by a small headless test harness with those two functions and a
 * minimal RADIO stubbed out (see cw_keyer_set_test_hook()).
 */

#ifndef _CW_KEYER_H
#define _CW_KEYER_H

#include <glib.h>

typedef enum { CW_PADDLE_DOT = 0, CW_PADDLE_DASH = 1 } cw_paddle_t;

// A paddle contact opened/closed. Called from the keyboard handler (receiver.c)
// and the MIDI handler (midi3.c). Applies radio->cw_keys_reversed. GTK thread.
extern void cw_keyer_paddle(cw_paddle_t which, gboolean pressed);

// --- test seam (headless unit test); NOT used in the app ---
// Advance the state machine to absolute time now_s (seconds). The app drives
// this from a g_timeout tick via read_time_now(); the test drives it with a
// synthetic clock. Emits key-down/up through the same path the app uses.
extern void cw_keyer_advance(double now_s);

// Install a test hook receiving every keyed MARK as it starts, with its type
// and start time; when non-NULL the keyer does NOT touch cw_encoder/MOX (pure
// logic for the unit test). type: 0=dit, 1=dah.
extern void cw_keyer_set_test_hook(void (*hook)(int type, double t_s));

// Reset all keyer state (paddles up, idle). For the test between scenarios.
extern void cw_keyer_reset(void);

#endif
