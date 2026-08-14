/* props_offline.c -- self-test for the property store (src/core/property.c).
 *
 * Why this exists.  Everything the operator sets goes through that file, and
 * the whole of it is invisible to every other harness: the settings are written
 * by the GUI at exit and read by it at start-up, so a defect there is silent
 * until someone notices a setting that will not stick.  Five did, and all five
 * were found by reading rather than by running (2026-08-14):
 *
 *   - loadProperties() and initProperties() wipe the ONE global store, so a
 *     second file (bookmarks, a MIDI export) read or written through it
 *     destroyed the running radio's settings;
 *   - radio_save_state() re-serialises everything, so anything that will NOT be
 *     written has to be retained first -- and the retain rule missed hidden
 *     receivers, the wideband window and the MIDI table;
 *   - floats went through sprintf("%f")/atof(), i.e. through the operator's
 *     LOCALE, so a props file survived neither a locale change nor an
 *     LC_NUMERIC=C run;
 *   - a value out of the file was copied into a fixed-size field unbounded.
 *
 * This harness covers the primitives those bugs were in: push/pop isolation,
 * retain/release, the file round trip, and the locale rules -- each with the
 * negative control that makes it mean something (the comma spelling that a
 * C-locale atof() gets wrong, and a store that is NOT parked, which must show
 * the wipe rather than hide it).
 *
 *   make props-offline && ./props_offline --selftest
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "property.h"

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

static void expect_str(const char *got, const char *want, const char *what) {
  checks++;
  if (got != NULL && strcmp(got, want) == 0) {
    printf("  ok    %s\n", what);
  } else {
    printf("  FAIL  %s (got \"%s\", want \"%s\")\n", what, got ? got : "(null)", want);
    fails++;
  }
}

static void expect_near(double got, double want, const char *what) {
  checks++;
  if (fabs(got - want) < 1e-9) {
    printf("  ok    %s (%f)\n", what, got);
  } else {
    printf("  FAIL  %s (got %f, want %f)\n", what, got, want);
    fails++;
  }
}

/* ---- the file round trip ------------------------------------------------- */

static char *tmp_path(const char *name) {
  return g_build_filename(g_get_tmp_dir(), name, NULL);
}

static void test_round_trip(void) {
  printf("\n-- save/load round trip --\n");
  char *path = tmp_path("machpsdr_props_selftest.props");

  initProperties();
  setProperty("radio.model", "3");
  setProperty("receiver[0].audio_name", "System Default");
  setPropertyDouble("radio.ppm_correction_value", 1.5);
  saveProperties(path);

  initProperties();
  expect(getProperty("radio.model") == NULL, "initProperties empties the store");

  loadProperties(path);
  expect_str(getProperty("radio.model"), "3", "an integer survives the file");
  expect_str(getProperty("receiver[0].audio_name"), "System Default",
             "a value with a space survives (strtok splits on = and newline only)");
  expect_near(propToDouble(getProperty("radio.ppm_correction_value")), 1.5,
              "a double survives the file");

  /* The version gate: loadProperties DISCARDS a file without the current
     property_version line, which is the trap a hand-written props for a test
     run falls into.  Assert it, so nobody "fixes" it into silence. */
  FILE *f = fopen(path, "w");
  g_assert(f != NULL);
  fputs("radio.model=7\n", f);
  fclose(f);
  loadProperties(path);
  expect(getProperty("radio.model") == NULL,
         "a file with no property_version is discarded ENTIRELY (negative control)");

  g_unlink(path);
  g_free(path);
}

/* ---- push/pop: the bookmarks bug ----------------------------------------- */

static void test_push_pop(void) {
  printf("\n-- a second file must not touch the live store --\n");
  char *path = tmp_path("machpsdr_props_selftest2.props");

  initProperties();
  setProperty("radio.model", "3");
  setProperty("receiver[1].frequency_a", "14074000");

  /* What a bookmarks save/restore does, parked. */
  pushPropertyStore();
  initProperties();
  setProperty("bookmark[0].name", "FT8");
  saveProperties(path);
  loadProperties(path);
  expect_str(getProperty("bookmark[0].name"), "FT8", "the parked store works normally");
  expect(getProperty("radio.model") == NULL,
         "and cannot see the radio's properties");
  popPropertyStore();

  expect_str(getProperty("radio.model"), "3", "the radio's store is back");
  expect_str(getProperty("receiver[1].frequency_a"), "14074000",
             "with every key intact");
  expect(getProperty("bookmark[0].name") == NULL,
         "and the second file's keys did not leak into it");

  /* Negative control: the same sequence WITHOUT parking is the bug -- the
     radio's settings are gone, which is what the operator saw as "every
     receiver I add comes up at factory defaults". */
  loadProperties(path);
  expect(getProperty("radio.model") == NULL,
         "unparked, loading a second file wipes the live store (negative control)");

  g_unlink(path);
  g_free(path);
}

/* ---- retain/release: the save-state rule --------------------------------- */

static void test_retain(void) {
  printf("\n-- retain across a full re-serialisation --\n");

  initProperties();
  setProperty("receiver[0].frequency_a", "7074000");   /* live receiver */
  setProperty("receiver[1].frequency_a", "14074000");  /* closed receiver */
  setProperty("wideband.panadapter_low", "-170");      /* window never opened */
  setProperty("midi[10].channels", "1");               /* controller unplugged */

  /* radio_save_state's shape: retain what will not be written, wipe, re-write
     the live state, merge the retained back. */
  retainProperties("receiver[1].");
  retainProperties("wideband.");
  retainProperties("midi");
  initProperties();
  setProperty("receiver[0].frequency_a", "7075000");   /* the live one moved */
  releaseRetainedProperties();

  expect_str(getProperty("receiver[0].frequency_a"), "7075000",
             "the live receiver's NEW value is the one kept");
  expect_str(getProperty("receiver[1].frequency_a"), "14074000",
             "the closed receiver's settings survive the wipe");
  expect_str(getProperty("wideband.panadapter_low"), "-170",
             "so do the wideband window's");
  expect_str(getProperty("midi[10].channels"), "1",
             "so does the MIDI table");

  /* Negative control: without the retain, the same sequence loses them -- which
     is precisely how a session that never opened the wideband window, or ran
     with the MIDI controller unplugged, erased those settings. */
  initProperties();
  setProperty("receiver[1].frequency_a", "14074000");
  initProperties();
  expect(getProperty("receiver[1].frequency_a") == NULL,
         "unretained, a re-serialisation drops them (negative control)");
}

/* ---- the locale rules ---------------------------------------------------- */

static void test_locale(void) {
  printf("\n-- floats do not depend on the locale --\n");

  /* Ask for a comma-decimal locale.  If the machine does not have one the
     WRITE half is untestable, but the READ half (which is what protects an
     existing file) still is, so the test says which it ran. */
  const char *got = setlocale(LC_NUMERIC, "ru_RU.UTF-8");
  if (got == NULL) got = setlocale(LC_NUMERIC, "de_DE.UTF-8");
  int comma_locale = (got != NULL);
  printf("  ..    LC_NUMERIC = %s\n", comma_locale ? got : "C (no comma locale installed)");

  initProperties();
  setPropertyDouble("radio.test", 1.5);
  expect_str(getProperty("radio.test"), "1.500000",
             "written with a DOT whatever the locale says");

  /* Both spellings must read back, so an existing comma file is understood
     once and rewritten in ASCII on the next save. */
  expect_near(propToDouble("1.500000"), 1.5, "a dot value reads back");
  expect_near(propToDouble("1,500000"), 1.5, "a comma value reads back too");
  expect_near(propToDouble("-0,250000"), -0.25, "including a negative one");
  expect_near(propToDouble(NULL), 0.0, "and NULL is 0, not a crash");

  if (comma_locale) {
    /* The negative control, and the whole reason for the rule: the C library's
       own atof() gets exactly one of these right, whichever way round the
       locale happens to be. */
    expect(atof("1,500000") == 1.5 && atof("1.500000") == 1.0,
           "atof() under a comma locale reads the DOT spelling as 1 (negative control)");
  }
  setlocale(LC_NUMERIC, "C");
  expect(atof("1,500000") == 1.0,
         "and under C it reads the COMMA spelling as 1 (negative control)");
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

  printf("props_offline: property store self-test\n");
  test_round_trip();
  test_push_pop();
  test_retain();
  test_locale();

  printf("\n%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
