/* reconnect_offline.c -- self-test for the link watchdog's decision half
 * (the pure part of src/proto/reconnect.c).
 *
 * Why this exists.  The watchdog is the one piece of the application that only
 * ever runs when something has already gone wrong, so every defect in it is
 * discovered at the worst possible moment and by the operator.  It also cannot
 * be exercised by any of the emulators: they answer, and the whole subject here
 * is what happens when nothing does.
 *
 * The shape it replaces had three faults, and this harness is built around
 * them:
 *
 *   - it only armed itself AFTER the first block arrived, so a device that
 *     opened and never streamed was invisible for the rest of the session;
 *   - it asked, modally, and then waited to be answered -- so a radio that came
 *     back on its own came back to a dialog nobody had clicked;
 *   - it never retried by itself.
 *
 * The decision is therefore a pure function over a LINK struct, driven here on
 * a mock clock in whole seconds.  Nothing in this file touches a radio, a
 * socket or GTK; what it asserts is the sequence of states and the interval
 * between attempts, which is exactly the half that was wrong.
 *
 *   make reconnect-offline && ./reconnect_offline --selftest
 *
 * Exit status 0 = every case passed.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "reconnect.h"

static int failures;

static void check(const char *name, gboolean ok, const char *detail) {
  printf("%-56s %s   %s\n", name, ok ? "PASS" : "FAIL", detail ? detail : "");
  if (!ok) failures++;
}

static void check_state(const char *name, LINK *l, LINK_STATE want) {
  char d[80];
  g_snprintf(d, sizeof d, "state=%s", link_state_name(l->state));
  check(name, l->state == want, d);
}

/* Run the watchdog for `secs` seconds of mock time, answering every reconnect
 * request the way the live watchdog does (run it, then report it). `deliver`
 * is the second at which a block arrives, or -1 for a link that stays dead.
 * Returns the number of attempts made, and records the second of each. */
typedef struct {
  int  attempts;
  gint64 at[32];
} RUN;

static RUN run_for(LINK *l, gint64 *now, int secs, gint64 deliver,
                   gboolean transmitting) {
  RUN r = { 0 };
  for (int i = 0; i < secs; i++) {
    (*now)++;
    if (deliver >= 0 && *now >= deliver) {                 // a block arrived
      l->data_seq++;
      l->last_data = *now;      // what the receive threads publish, in effect
    }
    if (link_tick(l, *now, transmitting) == LINK_ACT_RECONNECT) {
      if (r.attempts < (int)(sizeof r.at / sizeof r.at[0])) r.at[r.attempts] = *now;
      r.attempts++;
      link_attempted(l, *now);                             // the attempt "ran"
    }
  }
  return r;
}

/* ---- 1. a device that opens and never streams ---------------------------- */
/* The fault the old watchdog could not see at all: it stayed disarmed until the
 * first block, so this case produced no log line, no dialog and no retry --
 * just an application sitting at a flat waterfall. */
static void test_never_starts(void) {
  printf("\n-- a device that opens and never streams --\n");
  LINK l; gint64 now = 1000;
  link_reset(&l, now);
  check_state("armed, it starts by WAITING for the first block", &l, LINK_STARTING);

  RUN r = run_for(&l, &now, FIRST_DATA_TIMEOUT_SEC - 1, -1, FALSE);
  check_state("and is still waiting a second before the timeout", &l, LINK_STARTING);
  check("with no attempt made yet", r.attempts == 0, NULL);

  r = run_for(&l, &now, 2, -1, FALSE);
  char d[80];
  g_snprintf(d, sizeof d, "%d attempt(s)", r.attempts);
  check("at the timeout the link is declared dead and retried", r.attempts == 1, d);
}

/* ---- 2. an established stream that stops --------------------------------- */
static void test_stream_stops(void) {
  printf("\n-- an established stream that stops --\n");
  LINK l; gint64 now = 2000;
  link_reset(&l, now);

  run_for(&l, &now, 3, now + 1, FALSE);        /* data arrives */
  check_state("data arriving puts the link in STREAMING", &l, LINK_STREAMING);

  /* Now nothing. last_data is not advanced, so the gap grows. */
  RUN r = run_for(&l, &now, DISCONNECT_TIMEOUT_SEC - 1, -1, FALSE);
  check_state("a gap shorter than the timeout is not a disconnect", &l, LINK_STREAMING);
  check("and provokes no attempt", r.attempts == 0, NULL);

  r = run_for(&l, &now, 2, -1, FALSE);
  check("the gap timeout retries at once, without asking", r.attempts >= 1, NULL);
}

/* ---- 3. the backoff ladder ----------------------------------------------- */
/* A radio that is off, or a cable that is out, can be that way for hours, and a
 * retry is not free -- it tears the device down and re-makes it. The interval
 * must therefore GROW, and cap. */
static void test_backoff(void) {
  printf("\n-- retries widen, and cap --\n");
  LINK l; gint64 now = 3000;
  link_reset(&l, now);

  RUN r = run_for(&l, &now, 400, -1, FALSE);   /* dead for 400 s */
  char d[160];
  g_snprintf(d, sizeof d, "%d attempts in 400 s", r.attempts);
  check("a link that stays dead is retried repeatedly", r.attempts > 5, d);

  /* Each gap must be at least the previous one, and never above the cap.
   * Attempts follow the first-data timeout too, so the observed gap is
   * backoff + FIRST_DATA_TIMEOUT_SEC; what is asserted is that it grows and
   * then stops growing, not the exact arithmetic. */
  gboolean grows = TRUE, capped = TRUE;
  gint64 prev_gap = 0;
  char gaps[160] = ""; gsize gl = 0;
  for (int i = 1; i < r.attempts; i++) {
    gint64 gap = r.at[i] - r.at[i-1];
    if (gap < prev_gap) grows = FALSE;      /* strictly non-decreasing */
    if (gap > RECONNECT_BACKOFF_MAX_SEC + FIRST_DATA_TIMEOUT_SEC + 1) capped = FALSE;
    prev_gap = gap;
    if (gl < sizeof gaps - 8)
      gl += g_snprintf(gaps + gl, sizeof gaps - gl, "%lld ", (long long)gap);
  }
  g_snprintf(d, sizeof d, "gaps %ss", gaps);
  check("the interval between attempts never shrinks", grows, d);
  check("and never exceeds the cap", capped, NULL);

  /* Negative control: without a widening interval, 400 s of a dead link at the
   * one-second watchdog tick would be ~40 attempts, each of which re-makes the
   * device. Anything near that is a ladder that is not climbing. */
  g_snprintf(d, sizeof d, "%d, against ~%d with no backoff at all",
             r.attempts, 400/(FIRST_DATA_TIMEOUT_SEC+1));
  check("so a long outage costs few attempts, not one per timeout",
        r.attempts < 400/(FIRST_DATA_TIMEOUT_SEC+1), d);
}

/* ---- 4. a link that heals by itself -------------------------------------- */
/* A radio power-cycled, a switch rebooted, a Wi-Fi drop that came back: nothing
 * was ever stopped, so data simply resumes. The old modal dialog was still on
 * screen at this point with the whole UI behind it. */
static void test_self_heal(void) {
  printf("\n-- a link that comes back on its own --\n");
  LINK l; gint64 now = 4000;
  link_reset(&l, now);
  run_for(&l, &now, 3, now + 1, FALSE);
  check_state("streaming", &l, LINK_STREAMING);

  run_for(&l, &now, DISCONNECT_TIMEOUT_SEC + 1, -1, FALSE);
  check("the link was declared lost", l.state != LINK_STREAMING, NULL);

  /* Data resumes with nothing having been reconnected. */
  run_for(&l, &now, 3, now + 1, FALSE);
  check_state("data resuming is enough on its own", &l, LINK_STREAMING);

  char d[80];
  g_snprintf(d, sizeof d, "backoff=%d attempt=%d", l.backoff, l.attempt);
  check("and the backoff ladder is reset for the next outage",
        l.backoff == RECONNECT_BACKOFF_MIN_SEC && l.attempt == 0, d);
}

/* ---- 5. transmitting is not a disconnect --------------------------------- */
/* A half-duplex SoapySDR device delivers no RX while keyed. Without this the
 * watchdog would tear the device down in the middle of every transmission
 * longer than three seconds. */
static void test_transmitting(void) {
  printf("\n-- a keyed transmitter is not a dead link --\n");
  LINK l; gint64 now = 5000;
  link_reset(&l, now);
  run_for(&l, &now, 3, now + 1, FALSE);
  check_state("streaming", &l, LINK_STREAMING);

  RUN r = run_for(&l, &now, 60, -1, TRUE);     /* keyed, no RX for a minute */
  check_state("a minute of transmit leaves the link alone", &l, LINK_STREAMING);
  check("and provokes no reconnect", r.attempts == 0, NULL);

  /* Negative control: the same silence with the transmitter DOWN is a
   * disconnect, so the guard above cannot be a blanket "never fire". */
  r = run_for(&l, &now, 60, -1, FALSE);
  check("the same silence unkeyed IS a disconnect (negative control)",
        r.attempts > 0, NULL);
}

/* ---- 6. Retry now -------------------------------------------------------- */
static void test_retry_now(void) {
  printf("\n-- the operator's 'Retry now' --\n");
  LINK l; gint64 now = 6000;
  link_reset(&l, now);
  run_for(&l, &now, 200, -1, FALSE);           /* climb the ladder a while */
  check("the link is waiting out a long backoff",
        l.next_attempt > now + 2, NULL);

  link_retry_now(&l, now);
  RUN r = run_for(&l, &now, 2, -1, FALSE);
  check("asking brings the next attempt forward immediately",
        r.attempts >= 1, NULL);
}

int main(int argc, char *argv[]) {
  int selftest = 0;
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "--selftest") == 0) selftest = 1;
  if (!selftest) { printf("usage: %s --selftest\n", argv[0]); return 1; }

  printf("reconnect_offline: link watchdog self-test\n");
  test_never_starts();
  test_stream_stops();
  test_backoff();
  test_self_heal();
  test_transmitting();
  test_retry_now();

  printf("\n%s\n", failures == 0 ? "all cases passed" : "SOME CASES FAILED");
  return failures == 0 ? 0 : 1;
}
