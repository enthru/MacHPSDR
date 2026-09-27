/* ui_layout_offline.c -- headless geometry self-test for the settings pages.
 *
 * Why this exists.  A Configure page is reachable only by clicking, so a layout
 * bug in one is invisible to every build and every other harness here:
 * create_keybind_dialog() shipped a bus error (a column index left at one past
 * the end of a two-element array, read as a GtkBox*) that nothing could see until
 * the operator opened Configure -> Hotkeys.  It need not be that way.
 *
 * gtk_init_check() opens a display connection WITHOUT showing a window, and a
 * page builder that returns a widget tree can then be forced through a full
 * layout with gtk_widget_measure()/gtk_widget_allocate() and read back with
 * gtk_widget_compute_bounds().  Building the page at all catches the crash; the
 * measure/allocate catches an allocation-time fault; and the geometry it then
 * asserts is stated as a RULE, never as a pixel number that a font change moves:
 *
 *   - the builder returns a tree (it did not crash while assembling it);
 *   - no child overflows its parent's allocation (a widget drawn outside the
 *     box it lives in is the visible half of an index-out-of-bounds);
 *   - every group frame on a page is the same width (the GtkSizeGroup invariant
 *     the keybind page is built around -- the groups are meant to be one size
 *     whatever their longest label is, and a frame left out of the group is a
 *     ragged page).
 *
 * TWO RULES this harness lives by, both from the note that asked for it:
 *
 *   - a run with NO DISPLAY skips LOUDLY and exits 0.  It proves nothing on a
 *     headless CI box (the Linux `make check` runs without an X server -- only
 *     the churn steps get Xvfb), and a silent pass there is worse than no test:
 *     it would read as coverage that is not happening.  macOS (the operator's
 *     machine and the macOS CI runner) has a window server, so that is where
 *     this actually runs.
 *   - it covers only pages whose builder takes NO live RADIO state.
 *     create_keybind_dialog() ignores its RADIO* (it drives the global keybind
 *     store), so it is built with NULL.  A page that reads radio->... cannot be
 *     built this way and does not belong here.
 *
 * keybind_run() is stubbed: this links the page and the store, not the radio --
 * the same split keybind_offline.c makes.
 *
 *   make ui-layout-offline && ./ui_layout_offline --selftest
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "property.h"
#include "keybind.h"
/* keybind_dialog.h pulls in radio.h, which needs these types declared first --
   the same include order keybind_dialog.c uses. */
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "keybind_dialog.h"

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

/* ---- the stub (see keybind_offline.c) ------------------------------------ */

void keybind_run(int action, gboolean pressed) {
  (void)action;
  (void)pressed;
}

/* ---- forcing a layout with no window ------------------------------------- */

/* Measure the page at its natural size and allocate it there, the way a parent
   would.  Nothing is realized and nothing is shown; this only runs the size
   negotiation so the child transforms are filled in for compute_bounds(). */
static void force_layout(GtkWidget *page) {
  int ignore, w_nat, h_nat;
  gtk_widget_measure(page, GTK_ORIENTATION_HORIZONTAL, -1, &ignore, &w_nat, NULL, NULL);
  gtk_widget_measure(page, GTK_ORIENTATION_VERTICAL, w_nat, &ignore, &h_nat, NULL, NULL);
  if (w_nat < 1) w_nat = 1;
  if (h_nat < 1) h_nat = 1;
  gtk_widget_allocate(page, w_nat, h_nat, -1, NULL);
}

/* A child drawn outside its parent's box is the visible symptom of a layout
   walked off the end of an array.  One pixel of slack absorbs the sub-pixel
   rounding GTK does in its allocation transforms. */
static int overflow_count(GtkWidget *w) {
  int bad = 0;
  double pw = gtk_widget_get_width(w);
  double ph = gtk_widget_get_height(w);
  for (GtkWidget *c = gtk_widget_get_first_child(w);
       c != NULL;
       c = gtk_widget_get_next_sibling(c)) {
    graphene_rect_t r;
    if (gtk_widget_compute_bounds(c, w, &r)) {
      if (r.origin.x < -1.0 || r.origin.y < -1.0 ||
          r.origin.x + r.size.width  > pw + 1.0 ||
          r.origin.y + r.size.height > ph + 1.0) {
        bad++;
      }
    }
    bad += overflow_count(c);
  }
  return bad;
}

/* Collect the widths of every GtkFrame in the tree -- the group boxes. */
static void frame_widths(GtkWidget *w, int *widths, int *n, int cap) {
  if (GTK_IS_FRAME(w) && *n < cap) widths[(*n)++] = gtk_widget_get_width(w);
  for (GtkWidget *c = gtk_widget_get_first_child(w);
       c != NULL;
       c = gtk_widget_get_next_sibling(c)) {
    frame_widths(c, widths, n, cap);
  }
}

/* ---- the pages ----------------------------------------------------------- */

/* Every RADIO-free page checked the same way: build it, force a layout, and
   assert the rules that hold for any page.  Per-page specifics follow. */
static void check_page(const char *name, GtkWidget *page) {
  printf("\n-- %s --\n", name);
  expect(page != NULL, "the builder returns a widget tree (it did not crash)");
  if (page == NULL) return;

  /* Own the floating page so it (and its tree) is freed at the end, not leaked. */
  g_object_ref_sink(page);
  force_layout(page);

  expect(gtk_widget_get_width(page) > 0 && gtk_widget_get_height(page) > 0,
         "the page allocates to a non-empty size");
  expect(overflow_count(page) == 0, "no child overflows its parent's allocation");

  int widths[64], n = 0;
  frame_widths(page, widths, &n, 64);
  expect(n >= 2, "the page has at least two group frames to compare");
  int equal = 1;
  for (int i = 1; i < n; i++) if (widths[i] != widths[0]) equal = 0;
  expect(equal, "every group frame is the same width (the size-group invariant)");

  g_object_unref(page);
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

  printf("ui_layout_offline: settings-page layout self-test\n");

  /* No window is shown; this only opens the display connection so the widgets
     can be measured.  If there is no display, say so LOUDLY and pass -- a silent
     pass here would read as coverage that never ran. */
  if (!gtk_init_check()) {
    printf("\n*** SKIP: no display -- gtk_init_check() failed.\n");
    printf("*** This harness measures real widget trees and needs a display "
           "connection\n");
    printf("*** (macOS window server, or an X/Wayland server under Xvfb). "
           "NOTHING was tested.\n");
    return 0;
  }

  initProperties();

  check_page("Hotkeys (create_keybind_dialog)", create_keybind_dialog(NULL));

  printf("\n%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
