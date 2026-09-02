/* keybind_offline.c -- self-test for the keyboard-shortcut store (src/core/keybind.c).
 *
 * Why this exists.  A shortcut is the same shape of bug as a setting that will
 * not stick: nothing crashes, nothing is logged, and the operator finds out by
 * pressing a key and getting nothing.  Every part of that is reachable without
 * a window, so it is tested here rather than by clicking:
 *
 *   - the props round trip.  Bindings are stored as GTK accelerator strings and
 *     read back with gtk_accelerator_parse(); a file is also a hand-edited
 *     input, and a line that will not parse must leave the row UNBOUND -- the
 *     alternative is binding keyval 0, which matches key events GTK cannot name;
 *   - one combination, one action.  Assigning a taken combination has to take
 *     it away from whoever held it, or dispatch runs whichever row it meets
 *     first and the settings page shows two rows claiming the same key;
 *   - the dispatch routing: the right action for the right combination, and
 *     nothing at all for the same key under different modifiers;
 *   - the hold (press-to-talk) half.  Its release is compared on the KEY ALONE,
 *     because an operator lets go of Ctrl before the letter as often as not and
 *     a missed release strands the transmitter keyed -- which is the one bug in
 *     here that costs more than a dead key.
 *
 * keybind_run() is stubbed below: this links the store, not the radio.  What it
 * therefore does NOT cover is what each action does once dispatched (that is
 * keybind_run.c, i.e. the receiver) and the layout-independent matching, which
 * needs a real GdkDisplay to map a keycode across layout groups.
 *
 *   make keybind-offline && ./keybind_offline --selftest
 */

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "property.h"
#include "keybind.h"

static int fails = 0;
static int checks = 0;

static void expect(int cond, const char *what) {
  checks++;
  if (cond) {
    printf("  ok    %s\n", what);
  } else {
    printf("  FAIL  %s\n", what);
    fails++;
  }
}

/* ---- the stub ------------------------------------------------------------ */

static int last_action = -1;
static int last_pressed = -1;
static int run_count = 0;

void keybind_run(int action, gboolean pressed) {
  last_action = action;
  last_pressed = pressed ? 1 : 0;
  run_count++;
}

static void arm(void) {
  last_action = -1;
  last_pressed = -1;
  run_count = 0;
}

/* Row index of an action id, so the tests name what the operator names. */
static int row(const char *id) {
  for (int i = 0; i < keybind_action_count; i++) {
    if (strcmp(keybind_actions[i].id, id) == 0) return i;
  }
  g_error("no such action id: %s", id);
}

/* ---- the table ----------------------------------------------------------- */

static void test_table(void) {
  printf("\n-- the action table --\n");
  int bad_id = 0, bad_label = 0, bad_group = 0, dup = 0;

  for (int i = 0; i < keybind_action_count; i++) {
    if (keybind_actions[i].id == NULL || keybind_actions[i].id[0] == '\0') bad_id++;
    if (keybind_actions[i].label == NULL || keybind_actions[i].label[0] == '\0') bad_label++;
    if (keybind_actions[i].group == NULL || keybind_actions[i].group[0] == '\0') bad_group++;
    for (int j = i + 1; j < keybind_action_count; j++) {
      /* The id is the props key AND the identity, so a duplicate silently makes
         two rows share one saved shortcut. */
      if (strcmp(keybind_actions[i].id, keybind_actions[j].id) == 0) dup++;
      if (keybind_actions[i].action == keybind_actions[j].action) dup++;
    }
  }
  expect(bad_id == 0 && bad_label == 0 && bad_group == 0, "every row carries an id, a label and a group");
  expect(dup == 0, "no id and no action appears twice");
  expect(keybind_action_count == KB_ACTIONS - 1, "one row per action (the static assert, restated)");

  /* The four the operator asked for by name. */
  expect(row("zoom_in") >= 0 && row("zoom_out") >= 0, "zoom in/out are bindable");
  expect(row("sideband") >= 0, "swap sideband is bindable");
  expect(row("ptt") >= 0 && row("mox") >= 0, "transmit is bindable, hold and toggle");
  expect(row("mode_digu") >= 0 && row("mode_usb") >= 0, "a named modulation is bindable");
  expect(keybind_actions[row("ptt")].hold && !keybind_actions[row("mox")].hold,
         "hold-to-talk is a hold action and the toggle is not");
}

/* ---- the store ----------------------------------------------------------- */

static void test_store(void) {
  printf("\n-- assigning --\n");
  int zoom_in = row("zoom_in"), zoom_out = row("zoom_out");
  GdkModifierType mods = 0;

  keybind_clear_all();
  keybind_set(zoom_in, GDK_KEY_z, GDK_CONTROL_MASK);
  expect(keybind_get(zoom_in, &mods) == GDK_KEY_z && mods == GDK_CONTROL_MASK,
         "a combination is stored as given");
  expect(keybind_find(GDK_KEY_z, GDK_CONTROL_MASK) == zoom_in, "and is found by it");
  expect(keybind_find(GDK_KEY_z, 0) == -1, "the same key without the modifier is a different shortcut");

  /* Lock and button bits ride in the raw state; a shortcut captured with Caps
     Lock on would never match again if they were not masked off. */
  keybind_set(zoom_out, GDK_KEY_x, GDK_CONTROL_MASK | GDK_LOCK_MASK);
  expect(keybind_find(GDK_KEY_x, GDK_CONTROL_MASK) == zoom_out,
         "Caps Lock is not part of a shortcut");

  /* Caps Lock turns the same press into the shifted keyval with the lock bit
     masked away, so a stored shortcut must not remember the case -- or one key
     ends up bound twice and dispatch runs whichever row it meets first. */
  keybind_clear(zoom_out);
  keybind_set(zoom_out, GDK_KEY_M, 0);
  expect(keybind_find(GDK_KEY_m, 0) == zoom_out, "case is not part of a shortcut");
  keybind_set(zoom_in, GDK_KEY_m, 0);
  expect(keybind_get(zoom_out, NULL) == 0,
         "and the other case is the SAME shortcut, so it is taken away");
  keybind_clear(zoom_in);

  keybind_set(zoom_in, GDK_KEY_z, GDK_CONTROL_MASK);
  keybind_set(zoom_out, GDK_KEY_z, GDK_CONTROL_MASK);
  expect(keybind_get(zoom_in, NULL) == 0, "assigning a taken combination frees the row that had it");
  expect(keybind_find(GDK_KEY_z, GDK_CONTROL_MASK) == zoom_out, "and gives it to the new one");

  keybind_clear(zoom_out);
  expect(keybind_find(GDK_KEY_z, GDK_CONTROL_MASK) == -1, "clearing removes it");

  /* An index off the end must do nothing at all: the settings page is not the
     only caller a future edit could give this. */
  keybind_set(-1, GDK_KEY_a, 0);
  keybind_set(keybind_action_count, GDK_KEY_a, 0);
  keybind_clear(keybind_action_count + 99);
  expect(keybind_find(GDK_KEY_a, 0) == -1, "an out-of-range index binds nothing");
  expect(keybind_get(-1, &mods) == 0 && mods == 0, "and reads back as unbound");
}

/* ---- dispatch ------------------------------------------------------------ */

static void test_dispatch(void) {
  printf("\n-- dispatch --\n");
  int mute = row("mute"), digu = row("mode_digu"), ptt = row("ptt");

  keybind_clear_all();
  keybind_set(mute, GDK_KEY_m, 0);
  keybind_set(digu, GDK_KEY_d, GDK_CONTROL_MASK);

  arm();
  expect(keybind_key_pressed(GDK_KEY_m, 0, 0) && run_count == 1 &&
         last_action == KB_MUTE && last_pressed == 1,
         "a bound key runs its action, once");

  arm();
  expect(!keybind_key_pressed(GDK_KEY_m, 0, GDK_CONTROL_MASK) && run_count == 0,
         "the same key with a modifier is not that shortcut (negative control)");

  arm();
  expect(keybind_key_pressed(GDK_KEY_d, 0, GDK_CONTROL_MASK) && last_action == KB_MODE_BASE + DIGU,
         "a modulation row dispatches that mode");

  arm();
  expect(!keybind_key_pressed(GDK_KEY_F7, 0, 0) && run_count == 0,
         "an unbound key is not consumed -- the fixed keys below it still work");

  /* Case: GTK reports the shifted keyval, so a shortcut assigned as 'm' must
     still be recognised when it arrives as 'M' under Shift-lock spelling. */
  keybind_clear_all();
  keybind_set(mute, GDK_KEY_M, 0);
  arm();
  expect(keybind_key_pressed(GDK_KEY_m, 0, 0) && last_action == KB_MUTE,
         "upper and lower case are the same key");

  printf("\n-- hold to talk --\n");
  keybind_clear_all();
  keybind_set(ptt, GDK_KEY_t, GDK_CONTROL_MASK);

  arm();
  keybind_key_pressed(GDK_KEY_t, 0, GDK_CONTROL_MASK);
  expect(last_action == KB_PTT && last_pressed == 1, "press keys the transmitter");

  arm();
  /* Ctrl released first -- the release arrives with no modifiers at all. */
  expect(keybind_key_released(GDK_KEY_t, 0, 0) && last_action == KB_PTT && last_pressed == 0,
         "release drops it even though the modifier went first");

  arm();
  expect(!keybind_key_released(GDK_KEY_t, 0, 0) && run_count == 0,
         "a second release does nothing (the hold is over)");

  arm();
  keybind_key_pressed(GDK_KEY_t, 0, GDK_CONTROL_MASK);
  arm();
  expect(!keybind_key_released(GDK_KEY_y, 0, 0) && run_count == 0,
         "another key's release does not drop the transmitter (negative control)");
  keybind_key_released(GDK_KEY_t, 0, 0);

  keybind_clear_all();
  keybind_set(mute, GDK_KEY_space, 0);
  expect(keybind_key_bound(GDK_KEY_space, 0),
         "a shortcut on the space bar is reported bound (the fixed MOX key asks)");
  expect(!keybind_key_bound(GDK_KEY_bracketleft, 0), "and an unbound key is not");
}

/* ---- persistence --------------------------------------------------------- */

static void test_persistence(void) {
  printf("\n-- the props round trip --\n");
  char *path = g_build_filename(g_get_tmp_dir(), "machpsdr_keybind_selftest.props", NULL);
  int zoom_in = row("zoom_in"), sideband = row("sideband"), usb = row("mode_usb");

  keybind_clear_all();
  keybind_set(zoom_in, GDK_KEY_plus, GDK_CONTROL_MASK);
  keybind_set(sideband, GDK_KEY_F8, 0);
  keybind_set(usb, GDK_KEY_u, GDK_ALT_MASK | GDK_SHIFT_MASK);

  initProperties();
  keybind_save_state();
  saveProperties(path);

  keybind_clear_all();
  initProperties();
  loadProperties(path);
  keybind_restore_state();

  GdkModifierType mods = 0;
  expect(keybind_get(zoom_in, &mods) == GDK_KEY_plus && mods == GDK_CONTROL_MASK,
         "a modified key survives the file");
  expect(keybind_get(sideband, &mods) == GDK_KEY_F8 && mods == 0,
         "a bare function key survives the file");
  expect(keybind_get(usb, &mods) == GDK_KEY_u && mods == (GDK_ALT_MASK | GDK_SHIFT_MASK),
         "two modifiers survive the file");
  expect(keybind_get(row("mode_digu"), NULL) == 0, "a row that was never bound stays unbound");

  /* Hand-edited/older file: a line that will not parse must leave the row
     unbound, NOT bind keyval 0 -- which would fire on any key GTK cannot name. */
  setProperty("keybind.zoom_in", "not-an-accelerator");
  setProperty("keybind.sideband", "");
  keybind_restore_state();
  expect(keybind_get(zoom_in, NULL) == 0, "an unparsable accelerator leaves the row unbound");
  expect(keybind_get(sideband, NULL) == 0, "an empty value leaves the row unbound");
  arm();
  expect(!keybind_key_pressed(0, 0, 0) && run_count == 0,
         "and nothing answers keyval 0 (negative control)");

  g_remove(path);
  g_free(path);
}

int main(int argc, char *argv[]) {
  int selftest = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--selftest") == 0) selftest = 1;
  }
  if (!selftest) {
    printf("usage: %s --selftest\n", argv[0]);
    return 1;
  }

  printf("keybind_offline: keyboard shortcut self-test\n");
  test_table();
  test_store();
  test_dispatch();
  test_persistence();

  printf("\n%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
