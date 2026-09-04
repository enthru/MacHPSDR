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
#include <string.h>

#include "log.h"
#include "mode.h"
#include "property.h"
#include "keybind.h"

#define GRP_DISPLAY  "Display"
#define GRP_TX       "Transmit"
#define GRP_MODE     "Mode"
#define GRP_TUNING   "Tuning"
#define GRP_VFO      "VFO"
#define GRP_AUDIO    "Audio"
#define GRP_DSP      "DSP"
#define GRP_MODULATION "Modulation"

/* The `id` is what lands in the props file, so it is never derived from the
   enum value and never renamed -- reordering this table must not silently drop
   an operator's shortcuts. */
const KEYBIND_ACTION keybind_actions[] = {
  { "zoom_in",     "Zoom in",              GRP_DISPLAY, "Zoom the panadapter/waterfall in one step", KB_ZOOM_IN,    FALSE },
  { "zoom_out",    "Zoom out",             GRP_DISPLAY, "Zoom the panadapter/waterfall out one step", KB_ZOOM_OUT,  FALSE },
  { "zoom_reset",  "Zoom x1",              GRP_DISPLAY, "Back to the whole span", KB_ZOOM_RESET, FALSE },
  { "pan_left",    "Pan left",             GRP_DISPLAY, "Move the zoomed view down-band", KB_PAN_LEFT,  FALSE },
  { "pan_right",   "Pan right",            GRP_DISPLAY, "Move the zoomed view up-band", KB_PAN_RIGHT, FALSE },

  { "ptt",         "Transmit (hold)",      GRP_TX, "Transmits while the key is held, like the space bar", KB_PTT,  TRUE },
  { "mox",         "Transmit (toggle)",    GRP_TX, "MOX on/off", KB_MOX,  FALSE },
  { "tune",        "Tune",                 GRP_TX, "Key the tune carrier on/off", KB_TUNE, FALSE },

  { "sideband",    "Swap sideband",        GRP_MODE, "LSB<>USB, CWL<>CWU, DIGL<>DIGU", KB_SIDEBAND,  FALSE },
  { "mode_next",   "Next mode",            GRP_MODE, NULL, KB_MODE_NEXT, FALSE },
  { "mode_prev",   "Previous mode",        GRP_MODE, NULL, KB_MODE_PREV, FALSE },

  { "band_up",     "Band +",               GRP_TUNING, NULL, KB_BAND_UP,     FALSE },
  { "band_down",   "Band -",               GRP_TUNING, NULL, KB_BAND_DOWN,   FALSE },
  { "filter_up",   "Filter +",             GRP_TUNING, "Next filter of the current mode", KB_FILTER_UP,   FALSE },
  { "filter_down", "Filter -",             GRP_TUNING, "Previous filter of the current mode", KB_FILTER_DOWN, FALSE },
  { "freq_up",     "Tune up one step",     GRP_TUNING, "Same step the VFO wheel uses", KB_FREQ_UP,     FALSE },
  { "freq_down",   "Tune down one step",   GRP_TUNING, "Same step the VFO wheel uses", KB_FREQ_DOWN,   FALSE },
  { "lock",        "Lock VFO",             GRP_TUNING, NULL, KB_LOCK,        FALSE },

  { "a_to_b",      "A > B",                GRP_VFO, NULL, KB_A_TO_B,    FALSE },
  { "b_to_a",      "B > A",                GRP_VFO, NULL, KB_B_TO_A,    FALSE },
  { "a_swap_b",    "A <> B",               GRP_VFO, NULL, KB_A_SWAP_B,  FALSE },
  { "split",       "Split",                GRP_VFO, NULL, KB_SPLIT,     FALSE },
  { "ctun",        "CTUN",                 GRP_VFO, NULL, KB_CTUN,      FALSE },
  { "rit",         "RIT on/off",           GRP_VFO, NULL, KB_RIT,       FALSE },
  { "rit_clear",   "RIT clear",            GRP_VFO, NULL, KB_RIT_CLEAR, FALSE },
  { "xit",         "XIT on/off",           GRP_VFO, NULL, KB_XIT,       FALSE },
  { "xit_clear",   "XIT clear",            GRP_VFO, NULL, KB_XIT_CLEAR, FALSE },

  /* The gain rows move by ONE NOTCH of the control they mirror -- the same
     0.01 of AF/squelch and 1 dB of AGC-G a scroll wheel over the VFO row gives
     -- so a held key with the keyboard's own repeat behaves like a wheel and a
     single press is the smallest step the operator can already make by hand. */
  { "mute",         "Mute",             GRP_AUDIO, "Mute this receiver's audio", KB_MUTE, FALSE },
  { "volume_up",    "Volume +",         GRP_AUDIO, "AF gain, one step (hold to run it up)", KB_VOLUME_UP,   FALSE },
  { "volume_down",  "Volume -",         GRP_AUDIO, "AF gain, one step down", KB_VOLUME_DOWN, FALSE },
  { "agc_gain_up",  "AGC gain +",       GRP_AUDIO, "AGC-G, 1 dB", KB_AGC_GAIN_UP,   FALSE },
  { "agc_gain_down","AGC gain -",       GRP_AUDIO, "AGC-G, 1 dB down", KB_AGC_GAIN_DOWN, FALSE },
  { "squelch_up",   "Squelch +",        GRP_AUDIO, "Squelch threshold up one step (a threshold above zero IS squelch on)", KB_SQUELCH_UP,   FALSE },
  { "squelch_down", "Squelch -",        GRP_AUDIO, "Squelch threshold down one step; zero is squelch off", KB_SQUELCH_DOWN, FALSE },

  { "agc",         "AGC speed",            GRP_DSP, "Cycle off/long/slow/med/fast", KB_AGC,  FALSE },
  { "nb",          "Noise blanker",        GRP_DSP, "Cycle off/NB/NB2", KB_NB,   FALSE },
  { "nr",          "Noise reduction",      GRP_DSP, "Cycle off/NR/NR2/NR3/NR4", KB_NR,   FALSE },
  { "anf",         "Auto notch",           GRP_DSP, NULL, KB_ANF,  FALSE },
  { "snb",         "Spectral NB",          GRP_DSP, NULL, KB_SNB,  FALSE },

  /* One row per mode. mode_string[] is the label everywhere else in the app, so
     it is the label here too; the id is fixed text, not that array, because the
     props file must not move if a mode is ever renamed. */
  { "mode_lsb",  "LSB",  GRP_MODULATION, NULL, KB_MODE_BASE+LSB,  FALSE },
  { "mode_usb",  "USB",  GRP_MODULATION, NULL, KB_MODE_BASE+USB,  FALSE },
  { "mode_dsb",  "DSB",  GRP_MODULATION, NULL, KB_MODE_BASE+DSB,  FALSE },
  { "mode_cwl",  "CWL",  GRP_MODULATION, NULL, KB_MODE_BASE+CWL,  FALSE },
  { "mode_cwu",  "CWU",  GRP_MODULATION, NULL, KB_MODE_BASE+CWU,  FALSE },
  { "mode_fmn",  "FMN",  GRP_MODULATION, NULL, KB_MODE_BASE+FMN,  FALSE },
  { "mode_am",   "AM",   GRP_MODULATION, NULL, KB_MODE_BASE+AM,   FALSE },
  { "mode_digu", "DIGU", GRP_MODULATION, NULL, KB_MODE_BASE+DIGU, FALSE },
  { "mode_spec", "SPEC", GRP_MODULATION, NULL, KB_MODE_BASE+SPEC, FALSE },
  { "mode_digl", "DIGL", GRP_MODULATION, NULL, KB_MODE_BASE+DIGL, FALSE },
  { "mode_sam",  "SAM",  GRP_MODULATION, NULL, KB_MODE_BASE+SAM,  FALSE },
  { "mode_drm",  "DRM",  GRP_MODULATION, NULL, KB_MODE_BASE+DRM,  FALSE },
  { "mode_wfm",  "WFM",  GRP_MODULATION, NULL, KB_MODE_BASE+WFM,  FALSE },
};

const int keybind_action_count=(int)(sizeof(keybind_actions)/sizeof(keybind_actions[0]));

/* Every action bar KB_NONE has exactly one row: a new enum value with no table
   entry is a shortcut the settings page cannot offer and nothing would notice. */
_Static_assert(sizeof(keybind_actions)/sizeof(keybind_actions[0])==KB_ACTIONS-1,
               "keybind_actions[] must carry one row per KB_* action");

typedef struct {
  guint keyval;
  GdkModifierType mods;
  gboolean held;         /* a hold action whose key-press we consumed */
} BINDING;

static BINDING bindings[KB_ACTIONS];   /* indexed like keybind_actions[] */

/* Only the modifiers GTK itself considers part of an accelerator: the raw state
   also carries lock and button bits, and a shortcut captured with Caps Lock on
   would then never match again. */
static GdkModifierType mod_mask(GdkModifierType state) {
  return state & gtk_accelerator_get_default_mod_mask();
}

guint keybind_get(int index, GdkModifierType *mods) {
  if(index<0 || index>=keybind_action_count) {
    if(mods) *mods=0;
    return 0;
  }
  if(mods) *mods=bindings[index].mods;
  return bindings[index].keyval;
}

int keybind_find(guint keyval, GdkModifierType mods) {
  int i;
  mods=mod_mask(mods);
  keyval=gdk_keyval_to_lower(keyval);
  for(i=0;i<keybind_action_count;i++) {
    if(bindings[i].keyval!=0 && bindings[i].keyval==keyval && bindings[i].mods==mods) return i;
  }
  return -1;
}

void keybind_clear(int index) {
  if(index<0 || index>=keybind_action_count) return;
  bindings[index].keyval=0;
  bindings[index].mods=0;
  bindings[index].held=FALSE;
}

void keybind_set(int index, guint keyval, GdkModifierType mods) {
  int other;
  if(index<0 || index>=keybind_action_count) return;
  if(keyval==0) { keybind_clear(index); return; }
  mods=mod_mask(mods);
  /* Case is not part of a shortcut, and storing it as it arrived would let one
     key be bound twice: Caps Lock turns the very same press from ('m',none) into
     ('M',none) -- LOCK is masked off above -- so the two rows would both pass
     the conflict test below and dispatch, which compares case-insensitively,
     would run whichever it met first. */
  keyval=gdk_keyval_to_lower(keyval);
  /* One combination, one action: whoever held it before loses it, or the
     dispatch below would run whichever row it met first and the settings page
     would show two rows claiming the same key. */
  while((other=keybind_find(keyval,mods))>=0 && other!=index) keybind_clear(other);
  bindings[index].keyval=keyval;
  bindings[index].mods=mods;
  bindings[index].held=FALSE;
}

void keybind_clear_all(void) {
  int i;
  for(i=0;i<keybind_action_count;i++) keybind_clear(i);
}

gchar *keybind_accel_label(int index) {
  if(index<0 || index>=keybind_action_count) return NULL;
  if(bindings[index].keyval==0) return NULL;
  return gtk_accelerator_get_label(bindings[index].keyval,bindings[index].mods);
}

/* ---- matching ------------------------------------------------------------ */

/* Keyboard layouts: GTK reports the keyval the ACTIVE layout produces, so a
   shortcut captured as Ctrl+Z on a Latin layout arrives as Ctrl+я on a Russian
   one and a plain keyval compare misses it. Every keyval the pressed hardware
   keycode produces across all layout groups is collected once per event (there
   is always a Latin group), and the bindings are matched against that set --
   the same trick key_is_q() uses in receiver.c for Cmd-Q, done for the whole
   table at once so a keypress costs one display lookup rather than one per row.
*/
#define KB_MAX_CANDIDATES 8

typedef struct {
  guint vals[KB_MAX_CANDIDATES];
  int n;
} CANDIDATES;

static void candidates_add(CANDIDATES *c, guint keyval) {
  int i;
  keyval=gdk_keyval_to_lower(keyval);
  if(keyval==0) return;
  for(i=0;i<c->n;i++) if(c->vals[i]==keyval) return;
  if(c->n<KB_MAX_CANDIDATES) c->vals[c->n++]=keyval;
}

static void candidates_build(CANDIDATES *c, guint keyval, guint keycode) {
  GdkDisplay *display;
  GdkKeymapKey *keys=NULL;
  guint *keyvals=NULL;
  int n=0,i;
  c->n=0;
  candidates_add(c,keyval);
  display=gdk_display_get_default();
  if(display==NULL || keycode==0) return;
  if(gdk_display_map_keycode(display,keycode,&keys,&keyvals,&n)) {
    for(i=0;i<n;i++) candidates_add(c,keyvals[i]);
  }
  g_free(keys);
  g_free(keyvals);
}

static gboolean candidates_have(const CANDIDATES *c, guint keyval) {
  int i;
  keyval=gdk_keyval_to_lower(keyval);
  for(i=0;i<c->n;i++) if(c->vals[i]==keyval) return TRUE;
  return FALSE;
}

gboolean keybind_key_pressed(guint keyval, guint keycode, GdkModifierType state) {
  CANDIDATES c;
  GdkModifierType mods=mod_mask(state);
  int i;
  gboolean built=FALSE;

  for(i=0;i<keybind_action_count;i++) {
    if(bindings[i].keyval==0 || bindings[i].mods!=mods) continue;
    if(gdk_keyval_to_lower(bindings[i].keyval)!=gdk_keyval_to_lower(keyval)) {
      if(!built) { candidates_build(&c,keyval,keycode); built=TRUE; }
      if(!candidates_have(&c,bindings[i].keyval)) continue;
    }
    log_debug_area(LOG_UI, "keybind: %s\n",keybind_actions[i].label);
    if(keybind_actions[i].hold) bindings[i].held=TRUE;
    keybind_run(keybind_actions[i].action,TRUE);
    return TRUE;
  }
  return FALSE;
}

gboolean keybind_key_released(guint keyval, guint keycode, GdkModifierType state) {
  CANDIDATES c;
  gboolean handled=FALSE;
  gboolean built=FALSE;
  int i;

  /* The modifiers are NOT compared here: an operator lets go of Ctrl before the
     letter as often as not, and a hold action whose release is missed leaves
     the transmitter keyed with nothing left to drop it. The key alone ends it. */
  for(i=0;i<keybind_action_count;i++) {
    if(!bindings[i].held) continue;
    if(gdk_keyval_to_lower(bindings[i].keyval)!=gdk_keyval_to_lower(keyval)) {
      if(!built) { candidates_build(&c,keyval,keycode); built=TRUE; }
      if(!candidates_have(&c,bindings[i].keyval)) continue;
    }
    bindings[i].held=FALSE;
    keybind_run(keybind_actions[i].action,FALSE);
    handled=TRUE;
  }
  return handled;
}

gboolean keybind_key_bound(guint keyval, guint keycode) {
  CANDIDATES c;
  gboolean built=FALSE;
  int i;
  for(i=0;i<keybind_action_count;i++) {
    if(bindings[i].keyval==0) continue;
    if(gdk_keyval_to_lower(bindings[i].keyval)==gdk_keyval_to_lower(keyval)) return TRUE;
    if(!built) { candidates_build(&c,keyval,keycode); built=TRUE; }
    if(candidates_have(&c,bindings[i].keyval)) return TRUE;
  }
  return FALSE;
}

/* ---- persistence --------------------------------------------------------- */

/* Stored as a GTK accelerator string ("<Control>z"), which is ASCII whatever
   the locale -- the same reason the rest of this tree keeps numbers off
   sprintf("%f") -- and round-trips through gtk_accelerator_parse(). */
void keybind_save_state(void) {
  char name[64];
  int i;
  for(i=0;i<keybind_action_count;i++) {
    gchar *accel;
    g_snprintf(name,sizeof(name),"keybind.%s",keybind_actions[i].id);
    if(bindings[i].keyval==0) {
      setProperty(name,"");
      continue;
    }
    accel=gtk_accelerator_name(bindings[i].keyval,bindings[i].mods);
    setProperty(name,accel!=NULL?accel:"");
    g_free(accel);
  }
}

void keybind_restore_state(void) {
  char name[64];
  int i;
  keybind_clear_all();
  for(i=0;i<keybind_action_count;i++) {
    char *value;
    guint keyval=0;
    GdkModifierType mods=0;
    g_snprintf(name,sizeof(name),"keybind.%s",keybind_actions[i].id);
    value=getProperty(name);
    if(value==NULL || value[0]=='\0') continue;
    /* A props file is a supported hand-edited input: an unparsable accelerator
       leaves the row unbound rather than binding keyval 0, which would match
       every key event GTK cannot name. */
    if(!gtk_accelerator_parse(value,&keyval,&mods) || keyval==0) {
      log_error("keybind: %s: cannot parse accelerator \"%s\"\n",keybind_actions[i].id,value);
      continue;
    }
    keybind_set(i,keyval,mods);
  }
}
