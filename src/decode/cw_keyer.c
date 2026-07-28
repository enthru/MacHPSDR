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

#include <gtk/gtk.h>

// Prerequisite types for radio.h (mirrors cw_encoder.c's include order).
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "mode.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"       // RADIO, KEYER_STRAIGHT/KEYER_MODE_A/KEYER_MODE_B
#include "main.h"        // global RADIO *radio
#include "protocol1.h"   // read_time_now()
#include "log.h"
#include "cw_encoder.h"  // cw_tx_key()
#include "cw_keyer.h"

// ===========================================================================
// Canonical Curtis A/B iambic keyer. All state is GTK-main-thread only (the
// paddle setters, the tick and cw_keyer_advance() all run there), so no locks.
//
// current_elem/state track the element currently sounding (MARK) or spacing
// (SPACE) after it. dit_latch/dah_latch record a squeeze of the *opposite*
// paddle seen at any point during the current element's mark+space -- the
// dot-dash memory that produces iambic alternation, and (Mode B only) the one
// extra trailing element after both paddles release mid-element.
// ===========================================================================

typedef enum { KEYER_ST_IDLE = 0, KEYER_ST_MARK, KEYER_ST_SPACE } keyer_state_t;

#define ELEM_DIT 0
#define ELEM_DAH 1

// PHYSICAL paddle contacts (pre-reverse). cw_keys_reversed is applied at read
// time via dot_down()/dash_down(), NOT when storing, so toggling the reverse
// setting while a paddle is held can never strand a logical flag (a press and
// its release always update the same physical bit).
static gboolean       phys_dot = FALSE;
static gboolean       phys_dash = FALSE;

static keyer_state_t  state = KEYER_ST_IDLE;
static int            current_elem = -1;   // ELEM_DIT/ELEM_DAH of the in-flight element
static double         elem_end_s = 0.0;    // scheduled absolute end of the MARK or SPACE

static gboolean       dit_latch = FALSE;   // DOT seen pressed during current element
static gboolean       dah_latch = FALSE;   // DASH seen pressed during current element

static guint          tick_id = 0;         // g_timeout_add id (0 = not running)
static double         last_change_s = 0.0; // time of the last paddle state change (stuck-paddle guard)

static void (*test_hook)(int type, double t_s) = NULL;

// Stuck-paddle safety timeout: if a paddle stays held with no state change for
// this long (a lost key-up: window focus loss with a key down, a dropped MIDI
// note-off), force it released so the transmitter can't be keyed forever. Far
// longer than any real continuous paddle hold.
#define CW_KEYER_STUCK_S   15.0

// Logical paddle state = physical, swapped when cw_keys_reversed.
static gboolean dot_down(void)  { return (radio != NULL && radio->cw_keys_reversed) ? phys_dash : phys_dot; }
static gboolean dash_down(void) { return (radio != NULL && radio->cw_keys_reversed) ? phys_dot  : phys_dash; }

// ---------------------------------------------------------------------------
// Timing (PARIS-standard unit, weight biases the mark/space ratio; matches
// cw_encoder.c's dot_ms formula but -- per spec -- the inter-element space
// stays a fixed 1 unit regardless of weight, it is not gap-compensated).
// ---------------------------------------------------------------------------
static double dot_unit_ms(void) {
  int wpm = (radio != NULL) ? radio->cw_keyer_speed : 20;
  if (wpm < 1) wpm = 1;
  return 1200.0 / (double)wpm;
}

static double weight_scale(void) {
  int weight = (radio != NULL) ? radio->cw_keyer_weight : 50;
  if (weight <= 0) weight = 50;
  return (double)weight / 50.0;
}

static void emit_mark_start(int elem, double t_s) {
  if (test_hook != NULL) {
    test_hook(elem, t_s);
  } else {
    cw_tx_key(TRUE);
  }
}

static void emit_mark_end(void) {
  if (test_hook == NULL) cw_tx_key(FALSE);
}

// Record a squeeze of the paddle opposite the currently-sounding element.
static void sample_latch(void) {
  if (current_elem == ELEM_DAH) {
    if (dot_down()) dit_latch = TRUE;
  } else if (current_elem == ELEM_DIT) {
    if (dash_down()) dah_latch = TRUE;
  }
}

static void start_mark(int elem, double start_s) {
  current_elem = elem;
  dit_latch = FALSE;
  dah_latch = FALSE;
  double dur_ms = (elem == ELEM_DIT ? dot_unit_ms() : 3.0 * dot_unit_ms()) * weight_scale();
  elem_end_s = start_s + dur_ms / 1000.0;
  state = KEYER_ST_MARK;
  emit_mark_start(elem, start_s);
  sample_latch();   // catch a squeeze already held at the very instant we start
}

static void begin_space(double start_s) {
  emit_mark_end();
  elem_end_s = start_s + dot_unit_ms() / 1000.0;
  state = KEYER_ST_SPACE;
}

// Curtis A/B decision at the end of an element's space: (1) the latched
// opposite element (Mode B honours it even if released; Mode A only if the
// opposite paddle is still actually held), else (2) the same paddle if still
// held, else (3) whichever paddle is held, else IDLE.
static int decide_next(void) {
  gboolean opp_is_dot = (current_elem == ELEM_DAH);
  gboolean latch = opp_is_dot ? dit_latch : dah_latch;
  int mode = (radio != NULL) ? radio->cw_keyer_mode : KEYER_MODE_A;
  int next = -1;

  if (latch) {
    gboolean opp_held_now = opp_is_dot ? dot_down() : dash_down();
    if (mode == KEYER_MODE_B || opp_held_now) {
      next = opp_is_dot ? ELEM_DIT : ELEM_DAH;
    }
  }
  if (next < 0) {
    gboolean same_held = opp_is_dot ? dash_down() : dot_down();
    if (same_held) next = current_elem;
  }
  if (next < 0) {
    if (dot_down()) next = ELEM_DIT;
    else if (dash_down()) next = ELEM_DAH;
  }
  dit_latch = FALSE;
  dah_latch = FALSE;
  return next;
}

void cw_keyer_advance(double now_s) {
  if (radio != NULL && radio->cw_keyer_mode == KEYER_STRAIGHT) {
    // Mode was switched to straight while an iambic element was in flight:
    // release the key and return to idle so the tick can stop and MOX can drop.
    if (state != KEYER_ST_IDLE) {
      if (state == KEYER_ST_MARK) emit_mark_end();
      state = KEYER_ST_IDLE;
      current_elem = -1;
    }
    return;
  }

  // Stuck-paddle safety: a held paddle with no state change for CW_KEYER_STUCK_S
  // is a lost key-up; force it released so we can't key the TX indefinitely.
  if ((phys_dot || phys_dash) && (now_s - last_change_s) > CW_KEYER_STUCK_S) {
    log_error("cw-keyer: paddle held >%.0fs with no change - forcing release (lost key-up?)\n",
              CW_KEYER_STUCK_S);
    phys_dot = FALSE;
    phys_dash = FALSE;
  }

  switch (state) {
    case KEYER_ST_IDLE:
      if (dot_down() || dash_down()) {
        // Squeezed from idle: dit takes priority (common keyer convention).
        start_mark(dot_down() ? ELEM_DIT : ELEM_DAH, now_s);
      }
      break;

    case KEYER_ST_MARK:
      sample_latch();
      if (now_s >= elem_end_s) begin_space(elem_end_s);   // schedule from the boundary, not now_s (no drift)
      break;

    case KEYER_ST_SPACE:
      sample_latch();
      if (now_s >= elem_end_s) {
        int next = decide_next();
        if (next < 0) {
          state = KEYER_ST_IDLE;
          current_elem = -1;
        } else {
          start_mark(next, elem_end_s);
        }
      }
      break;
  }
}

static gboolean keyer_tick(gpointer data) {
  (void)data;
  cw_keyer_advance(read_time_now());
  if (state == KEYER_ST_IDLE && !phys_dot && !phys_dash) {
    tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void ensure_timer(void) {
  if (tick_id == 0) tick_id = g_timeout_add(2, keyer_tick, NULL);
}

void cw_keyer_paddle(cw_paddle_t which, gboolean pressed) {
  if (radio == NULL) return;
  // Store the PHYSICAL paddle (reversal applied at read time). Always record it,
  // even in straight mode, so a later mode/reverse toggle stays consistent.
  if (which == CW_PADDLE_DOT) phys_dot = pressed; else phys_dash = pressed;
  last_change_s = read_time_now();

  if (radio->cw_keyer_mode == KEYER_STRAIGHT) {
    // Straight key: only the (logical) DOT paddle keys the TX directly; the DASH
    // paddle is ignored for keying. Act only when the DOT paddle itself changed
    // so a DASH event never emits a spurious key edge.
    cw_paddle_t key_paddle = radio->cw_keys_reversed ? CW_PADDLE_DASH : CW_PADDLE_DOT;
    if (which == key_paddle) cw_tx_key(pressed);
    return;
  }

  if (pressed) sample_latch();   // catch a squeeze that begins between ticks
  ensure_timer();
}

void cw_keyer_set_test_hook(void (*hook)(int type, double t_s)) {
  test_hook = hook;
}

void cw_keyer_reset(void) {
  if (tick_id != 0) {
    g_source_remove(tick_id);
    tick_id = 0;
  }
  if (state == KEYER_ST_MARK) emit_mark_end();   // release the key if reset mid-element
  phys_dot = FALSE;
  phys_dash = FALSE;
  dit_latch = FALSE;
  dah_latch = FALSE;
  state = KEYER_ST_IDLE;
  current_elem = -1;
  elem_end_s = 0.0;
}
