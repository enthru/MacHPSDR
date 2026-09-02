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

#ifndef KEYBIND_H
#define KEYBIND_H

#include <gtk/gtk.h>

#include "mode.h"

/*
 * Operator-assignable keyboard shortcuts.
 *
 * Every action here is dispatched by keybind_run(), which does exactly what the
 * matching control in the main window does -- mutate the state, push it to WDSP
 * with the same setter, then update_vfo() to resync the whole VFO row.  Nothing
 * in this file is a second implementation of a control: an action that cannot
 * be expressed that way does not belong in the table, because a shortcut that
 * half-applies a setting is the "setter that only writes the field" lie the CAT
 * rules already name.
 *
 * NOT built on actions.c's switch_action(): that enum arrived from LinHPSDR for
 * GPIO/i2c front panels, has no caller anywhere in this tree, and half its cases
 * are empty `break;`s -- binding a key to one would produce a shortcut that does
 * nothing at all.
 */

enum {
  KB_NONE=0,
  /* Display */
  KB_ZOOM_IN,
  KB_ZOOM_OUT,
  KB_ZOOM_RESET,
  KB_PAN_LEFT,
  KB_PAN_RIGHT,
  /* Transmit */
  KB_PTT,
  KB_MOX,
  KB_TUNE,
  /* Mode */
  KB_SIDEBAND,
  KB_MODE_NEXT,
  KB_MODE_PREV,
  /* Tuning */
  KB_BAND_UP,
  KB_BAND_DOWN,
  KB_FILTER_UP,
  KB_FILTER_DOWN,
  KB_FREQ_UP,
  KB_FREQ_DOWN,
  KB_LOCK,
  /* VFO */
  KB_A_TO_B,
  KB_B_TO_A,
  KB_A_SWAP_B,
  KB_SPLIT,
  KB_CTUN,
  KB_RIT,
  KB_RIT_CLEAR,
  KB_XIT,
  KB_XIT_CLEAR,
  /* Audio / DSP */
  KB_MUTE,
  KB_AGC,
  KB_NB,
  KB_NR,
  KB_ANF,
  KB_SNB,
  /* One row per demodulation mode: KB_MODE_BASE+LSB ... KB_MODE_BASE+WFM. */
  KB_MODE_BASE,
  KB_ACTIONS=KB_MODE_BASE+MODES
};

typedef struct __KEYBIND_ACTION {
  const char *id;        /* stable identity: the props key, never the enum value */
  const char *label;     /* what the operator reads in the settings page */
  const char *group;     /* section heading; rows are listed in table order */
  const char *tip;       /* hover text, NULL for none */
  int action;
  gboolean hold;         /* acts on press AND release (hold-to-talk) */
} KEYBIND_ACTION;

extern const KEYBIND_ACTION keybind_actions[];
extern const int keybind_action_count;

/* Store: index is into keybind_actions[], NOT the action enum. */
extern guint keybind_get(int index, GdkModifierType *mods);
extern void keybind_set(int index, guint keyval, GdkModifierType mods);
extern void keybind_clear(int index);
extern void keybind_clear_all(void);
/* Row currently holding this combination, or -1. */
extern int keybind_find(guint keyval, GdkModifierType mods);
/* "Ctrl+Z" for the settings page, NULL when the row is unbound (g_free it). */
extern gchar *keybind_accel_label(int index);

/* Carry out one action. Implemented in keybind_run.c -- the only part of this
   subsystem that touches the radio, which is what lets the harness link the
   store against a recording stub. `pressed` is FALSE only on the release half
   of a hold action. */
extern void keybind_run(int action, gboolean pressed);

/* Dispatch, from the main window's key controller. TRUE when consumed. */
extern gboolean keybind_key_pressed(guint keyval, guint keycode, GdkModifierType state);
extern gboolean keybind_key_released(guint keyval, guint keycode, GdkModifierType state);
/* Whether this physical key is bound at all, whatever the modifiers -- the
   hardcoded space/paddle handlers ask before acting on their own key. */
extern gboolean keybind_key_bound(guint keyval, guint keycode);

extern void keybind_save_state(void);
extern void keybind_restore_state(void);

#endif
