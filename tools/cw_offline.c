/* CW offline harness: encoder->decoder round trip, noise rejection, and the
 * iambic keyer state machine.
 *
 * Everything here runs headlessly against synthesised audio and a synthetic
 * clock, for the standing reason: starting the GUI raises a window over
 * whatever the operator is doing and rewrites their saved settings on exit,
 * and none of the three properties below can be seen from a GUI run anyway.
 *
 * What it proves, and why each case exists:
 *
 *  1. Round trip. cw_encode_to_audio() is a pure function (no RADIO/GTK/MOX),
 *     so its output can be fed straight into cw_decoder_add_audio() and the
 *     text read back. This is the only test that exercises the decoder's
 *     Goertzel bank, adaptive envelope, dot-length estimator and Morse table
 *     end to end, at several speeds and at a non-default weight.
 *
 *  2. Noise rejection. "A CW decoder must stay silent on band noise, not
 *     stream garbage" is the cardinal property of cw_decoder.c and the one a
 *     regression would break silently — a decoder that got noisier would still
 *     pass case 1. So band noise alone must produce NOTHING.
 *
 *  3. Keyer. cw_keyer.c exposes cw_keyer_set_test_hook()/cw_keyer_advance()
 *     precisely so a synthetic paddle timeline can be driven on a mock clock.
 *     Two kinds of assertion: the Curtis A/B element sequence on a squeeze
 *     (the whole point of the mode setting), and the three "never strand the
 *     transmitter keyed" invariants — reverse toggled mid-hold, a switch to
 *     Straight mid-element, and the stuck-paddle timeout.
 *
 *     The invariants are checked through the REAL cw_tx_key() path (test hook
 *     off) by listening to cw_tx_next_sample(): "the key is stranded down" is
 *     audible as a sidetone that never stops, which is exactly what a stuck
 *     key is. Each of those cases carries a positive control (the tone IS
 *     there while the element sounds), so a harness that silenced everything
 *     could not pass by accident.
 *
 *   make cw-offline && ./cw_offline --selftest
 *
 * Exit status 0 = every case passed.
 */

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Prerequisite types for radio.h (the include order cw_encoder.c uses).
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "mode.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "cw_decoder.h"
#include "cw_encoder.h"
#include "cw_keyer.h"

// ---- the application, reduced to what cw_encoder.c / cw_keyer.c reach for ---

RADIO *radio;                 // the global both files read

static double mock_now = 0.0; // the synthetic clock the keyer test drives

// protocol1.c's clock. The keyer calls it from cw_keyer_paddle() to stamp the
// stuck-paddle guard, so the test must own it or that guard cannot be timed.
double read_time_now(void) { return mock_now; }

// radio.c's PTT. cw_encoder.c raises/drops MOX around a keying session; here it
// only has to be observable enough not to lie to the code under test.
void set_mox(RADIO *r, gboolean state) { if (r != NULL) r->mox = state; }

// transmitter.c: cw_tx_key() refuses to key unless the TRANSMITTER's mode is CW.
static int stub_tx_mode = CWU;
int transmitter_get_mode(TRANSMITTER *tx) { (void)tx; return stub_tx_mode; }

// ---- test rig ---------------------------------------------------------------

static int failures;

static void check(const char *name, gboolean ok, const char *detail) {
  printf("%-46s %s   %s\n", name, ok ? "PASS" : "FAIL", detail ? detail : "");
  if (!ok) failures++;
}

#define SR 48000

// Deterministic LCG noise in [-1,1): reproducible runs matter more than
// spectral purity, and a fixed seed means a failure can be re-run identically.
static guint32 rng_state = 22222u;
static void   rng_seed(guint32 s) { rng_state = s; }
static double rng_uniform(void) {
  rng_state = rng_state * 1103515245u + 12345u;
  return ((double)((rng_state >> 8) & 0xFFFF) / 32768.0) - 1.0;
}

// ---- audio construction -----------------------------------------------------

// Build the decoder's input format: interleaved stereo doubles, left channel
// used (cw_decoder_add_audio reads samples[i*2]).
//
// Lead-in and tail are NOT padding for convenience, they are properties of the
// decoder and are documented as such:
//   - lead-in: cw_decoder.c has a WARMUP_BLOCKS (~160 ms) quiet calibration
//     window in which it averages the noise floor and forms no marks at all.
//     Audio that starts keying inside it is simply not looked at.
//   - tail: the last character is only flushed by a gap (>= 2 dot units) or by
//     the squelch closing, both of which need audio AFTER the final mark. A
//     real transmission always has that; a buffer that stops on the last dit
//     legitimately leaves the final symbol unresolved.
static double *build_audio(const char *text, int wpm, int weight, double tone_hz,
                           double amplitude, double noise,
                           double lead_s, double tail_s, int *nframes_out) {
  int n_wave = 0;
  float *wave = cw_encode_to_audio(text, wpm, weight, tone_hz, SR, amplitude, &n_wave);
  if (wave == NULL || n_wave <= 0) { g_free(wave); return NULL; }

  int lead = (int)(lead_s * SR);
  int tail = (int)(tail_s * SR);
  int n = lead + n_wave + tail;
  double *buf = g_new0(double, (gsize)n * 2);

  for (int i = 0; i < n; i++) {
    double s = 0.0;
    if (i >= lead && i < lead + n_wave) s = wave[i - lead];
    if (noise > 0.0) s += noise * rng_uniform();
    buf[i * 2]     = s;
    buf[i * 2 + 1] = s;
  }
  g_free(wave);
  if (nframes_out) *nframes_out = n;
  return buf;
}

// Feed the decoder the way the RX audio thread does — in buffer-sized chunks —
// draining decoded text as it appears (the panel polls; the pending ring is
// finite).
static void feed_and_collect(const double *buf, int nframes, char *out, int outlen) {
  int used = 0;
  out[0] = '\0';
  const int CHUNK = 1024;
  for (int off = 0; off < nframes; off += CHUNK) {
    int n = (nframes - off < CHUNK) ? (nframes - off) : CHUNK;
    cw_decoder_add_audio(buf + (gsize)off * 2, n);
    char part[256];
    int got = cw_decoder_get_text(part, sizeof(part));
    if (got > 0 && used + got < outlen) { memcpy(out + used, part, got); used += got; out[used] = '\0'; }
  }
  char part[256];
  int got = cw_decoder_get_text(part, sizeof(part));
  if (got > 0 && used + got < outlen) { memcpy(out + used, part, got); used += got; out[used] = '\0'; }
}

// Normalise for comparison the way the decoder actually emits: it has no case
// (Morse has none) and it emits ONE space per word gap however long the pause,
// so runs of spaces collapse and the ends are trimmed. Nothing else is touched
// — a wrong character stays a wrong character.
static void normalise(const char *in, char *out, int outlen) {
  int o = 0;
  gboolean last_space = TRUE;   // trims the leading run too
  for (int i = 0; in[i] != '\0' && o < outlen - 1; i++) {
    char c = (char)toupper((unsigned char)in[i]);
    if (c == ' ') { if (!last_space) { out[o++] = ' '; last_space = TRUE; } }
    else          { out[o++] = c; last_space = FALSE; }
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = '\0';
}

static void decoder_start_fresh(void) {
  cw_decoder_set_enabled(FALSE);
  cw_decoder_set_enabled(TRUE);   // an off->on edge is what resets the DSP state
  char junk[256];
  cw_decoder_get_text(junk, sizeof(junk));
}

// One round-trip case. Returns TRUE when the decode matches the text sent.
static gboolean round_trip(const char *text, int wpm, int weight, double tone_hz,
                           double noise, char *detail, int detail_len) {
  int n = 0;
  rng_seed(4242u);   // every case reproducible on its own, whatever ran before it
  double *buf = build_audio(text, wpm, weight, tone_hz, 0.5, noise, 0.5, 1.0, &n);
  if (buf == NULL) {
    g_snprintf(detail, detail_len, "encoder produced nothing");
    return FALSE;
  }
  decoder_start_fresh();
  char raw[512];
  feed_and_collect(buf, n, raw, sizeof(raw));
  g_free(buf);

  char got[512], want[512];
  normalise(raw, got, sizeof(got));
  normalise(text, want, sizeof(want));

  int rx_wpm = 0; double rx_tone = 0.0; gboolean locked = FALSE;
  cw_decoder_get_status(&rx_wpm, &rx_tone, &locked);
  g_snprintf(detail, detail_len, "\"%s\" (%d wpm/wt %d) -> \"%s\" [%d wpm, %.0f Hz]",
             want, wpm, weight, got, rx_wpm, rx_tone);
  return strcmp(got, want) == 0;
}

// ---- keyer rig --------------------------------------------------------------

#define MAX_MARKS 4096
static int    mark_type[MAX_MARKS];    // 0 = dit, 1 = dah
static double mark_time[MAX_MARKS];
static int    n_marks;

static void mark_hook(int type, double t_s) {
  if (n_marks < MAX_MARKS) { mark_type[n_marks] = type; mark_time[n_marks] = t_s; n_marks++; }
}

static void marks_to_string(char *out, int outlen) {
  int o = 0;
  for (int i = 0; i < n_marks && o < outlen - 1; i++) out[o++] = mark_type[i] ? '-' : '.';
  out[o] = '\0';
}

// Advance the keyer to `until` on the mock clock, in the app's own 2 ms tick.
static void keyer_run_to(double until) {
  while (mock_now < until) {
    mock_now += 0.002;
    cw_keyer_advance(mock_now);
  }
}

static void keyer_case_init(int mode, gboolean reversed, void (*hook)(int, double)) {
  cw_keyer_set_test_hook(hook);
  cw_keyer_reset();
  cw_tx_abort();                 // clear any keying session left by the previous case
  radio->cw_keyer_mode = mode;
  radio->cw_keys_reversed = reversed;
  radio->mox = FALSE;
  mock_now = 0.0;
  n_marks = 0;
}

// Peak level of the keyer sidetone over `ms` of audio pulled through the real
// TX sample path, ignoring the first `skip_ms`. This is how "the key is down"
// is observed when the test hook is off: cw_tx_next_sample() renders a gated
// tone from cw_tx_key()'s state.
//
// The skip is not fudge: cw_tx_next_sample() ramps the envelope down over
// CW_KEYER_RAMP_SAMPLES (~5 ms) after a key-up — anti-click shaping, not a key
// that is still down — so a window starting at the release always contains
// full-amplitude samples. Anything still sounding 50 ms later is a stuck key.
static double tone_peak(double skip_ms, double ms) {
  int skip = (int)(skip_ms * SR / 1000.0);
  int n    = (int)(ms * SR / 1000.0);
  double peak = 0.0;
  for (int i = 0; i < skip; i++) (void)cw_tx_next_sample();
  for (int i = 0; i < n; i++) {
    double s = cw_tx_next_sample();
    if (fabs(s) > peak) peak = fabs(s);
  }
  return peak;
}

// ---- cases ------------------------------------------------------------------

static void run_round_trip_cases(void) {
  char d[512];

  // The speeds bracket the usable range: the decoder seeds its dot-length
  // estimate at 20 WPM (DOT_MS_SEED 60 ms), so 12 and 30 make it converge from
  // both directions — a broken estimator passes at 20 and fails at the ends.
  //
  // MEASURED LIMIT, not a bug: at 10 WPM and below, a message whose first mark
  // is a DOT loses that first character — a 120 ms dot is not < 2*60 ms, so it
  // reads as a dash until the estimate has moved. Same at ~35 WPM and above,
  // from the other side. Real operating practice (a "VVV" or "CQ CQ" lead-in)
  // covers it, which is why the slow case below starts on a dash.
  check("round trip 12 wpm", round_trip("CQ CQ DE TEST K", 12, 50, 700.0, 0.0, d, sizeof(d)), d);
  check("round trip 20 wpm", round_trip("HELLO WORLD 73", 20, 50, 600.0, 0.0, d, sizeof(d)), d);
  check("round trip 25 wpm", round_trip("SOS TEST DE MM0ABC", 25, 50, 550.0, 0.0, d, sizeof(d)), d);
  check("round trip 30 wpm", round_trip("PARIS PARIS PARIS", 30, 50, 800.0, 0.0, d, sizeof(d)), d);

  // Non-default weight: the mark is stretched (or shortened) against the gaps,
  // which is precisely what an estimator built on "mark vs 2*dot" can get wrong.
  //
  // These cases are the regression test for the compensation bug they found:
  // cw_encoder.c used to accumulate a whole character's weight compensation onto
  // the ONE inter-character gap instead of each mark's own following gap. At
  // weight 65 that gap collapsed to the anti-click ramp and "M0X" came out as a
  // single unbroken run of elements; below 45 it grew past 5 dot units and every
  // letter gap read as a word space. So the wide weights below are the point —
  // narrowing them back to ~47..53 would make this test pass over the bug again.
  // The heavy case carries a "VVV" lead-in, as a real transmission does: the
  // decoder seeds its dot estimate at 20 WPM (cw_decoder.c DOT_MS_SEED) and at
  // weight 65 an 18 WPM dash is 260 ms, so the FIRST element can still be lost
  // while the estimate converges. That is a decoder acquisition property, not
  // the encoder's weighting — "TEST DE MACHPSDR" without the lead-in copies as
  // "EST DE MACHPSDR", i.e. everything after the first element is exact.
  check("round trip heavy weight (65)", round_trip("VVV TEST DE MACHPSDR", 18, 65, 650.0, 0.0, d, sizeof(d)), d);
  check("round trip light weight (40)", round_trip("BEST 73 GL", 18, 40, 650.0, 0.0, d, sizeof(d)), d);
  check("round trip heavy weight, long codes (65)", round_trip("M0X 599", 20, 65, 600.0, 0.0, d, sizeof(d)), d);

  // Digits and punctuation exercise the long codes (6 elements), where a
  // symbol-buffer or table error shows up and letters alone would not.
  check("round trip digits + punctuation",
        round_trip("RST 599 = QTH LONDON /P ?", 20, 50, 600.0, 0.0, d, sizeof(d)), d);

  // ...and the same thing on a noisy band, which is the only condition it is
  // ever actually used in.
  check("round trip 20 wpm with noise", round_trip("CQ TEST DE G0ORX K", 20, 50, 600.0, 0.02, d, sizeof(d)), d);

  // The tracked tone must follow the operator's pitch: the bank spans
  // 400-1000 Hz and there is no manual pitch setting, so a signal near either
  // end must still decode.
  check("round trip low tone (430 Hz)", round_trip("SOS TEST", 18, 50, 430.0, 0.0, d, sizeof(d)), d);
  check("round trip high tone (960 Hz)", round_trip("SOS TEST", 18, 50, 960.0, 0.0, d, sizeof(d)), d);
}

static void run_noise_case(void) {
  char d[256];

  // THE cardinal assertion. Band noise, no keying: the decoder must emit
  // nothing at all. A regression in the modulation-depth squelch or the
  // Morse-timing gate turns this into a stream of garbage characters, and
  // every round-trip case above would still pass.
  for (int trial = 0; trial < 3; trial++) {
    const double amp[3] = { 0.02, 0.10, 0.40 };
    rng_seed(7777u + trial);
    decoder_start_fresh();
    int n = SR * 20;                       // 20 s of band noise
    double *buf = g_new0(double, (gsize)n * 2);
    for (int i = 0; i < n; i++) {
      double s = amp[trial] * rng_uniform();
      buf[i * 2] = s; buf[i * 2 + 1] = s;
    }
    char raw[512];
    feed_and_collect(buf, n, raw, sizeof(raw));
    g_free(buf);
    g_snprintf(d, sizeof(d), "20 s @ amp %.2f -> %d chars \"%s\"",
               amp[trial], (int)strlen(raw), raw);
    char name[64];
    g_snprintf(name, sizeof(name), "silent on band noise (amp %.2f)", amp[trial]);
    check(name, raw[0] == '\0', d);
  }

  // A slowly swelling noise floor (a dead band with the AGC breathing) is still
  // not CW: it modulates, so it can get past the modulation-depth squelch, and
  // then the Morse-timing plausibility gate is the only thing between it and
  // printed garbage.
  //
  // MEASURED: silent at a 40 % swell (below). A very deep one — 5:1 at 0.3 Hz —
  // does leak the occasional single character (1-2 chars per 20 s in 2 of 4
  // seeds tried). That is not asserted here because it is the decoder's real
  // measured behaviour rather than a regression, but it is worth knowing: the
  // timing gate narrows the leak, it does not close it.
  for (int trial = 0; trial < 3; trial++) {
    rng_seed(31337u + trial);
    decoder_start_fresh();
    int n = SR * 20;
    double *buf = g_new0(double, (gsize)n * 2);
    for (int i = 0; i < n; i++) {
      double fade = 0.8 + 0.2 * sin(2.0 * M_PI * 0.3 * (double)i / SR);   // ~0.3 Hz, 40 % swell
      double s = 0.15 * fade * rng_uniform();
      buf[i * 2] = s; buf[i * 2 + 1] = s;
    }
    char raw[512];
    feed_and_collect(buf, n, raw, sizeof(raw));
    g_free(buf);
    g_snprintf(d, sizeof(d), "20 s of swelling noise -> %d chars \"%s\"", (int)strlen(raw), raw);
    char name[64];
    g_snprintf(name, sizeof(name), "silent on swelling band noise (%d)", trial + 1);
    check(name, raw[0] == '\0', d);
  }
}

static void run_keyer_cases(void) {
  char d[256];
  char seq[64];

  radio->cw_keyer_speed = 20;        // dot unit = 60 ms
  radio->cw_keyer_weight = 50;
  radio->cw_keyer_sidetone_frequency = 600;
  radio->cw_keyer_hang_time = 250;

  // ---- 1. iambic alternation on a squeeze, Curtis A.
  // Both paddles down at t=0; both released 200 ms in, i.e. during the DAH that
  // the dot-dash memory produced. Mode A stops there.
  {
    keyer_case_init(KEYER_MODE_A, FALSE, mark_hook);
    cw_keyer_paddle(CW_PADDLE_DOT, TRUE);
    cw_keyer_paddle(CW_PADDLE_DASH, TRUE);
    keyer_run_to(0.200);
    cw_keyer_paddle(CW_PADDLE_DOT, FALSE);
    cw_keyer_paddle(CW_PADDLE_DASH, FALSE);
    keyer_run_to(1.500);
    marks_to_string(seq, sizeof(seq));
    g_snprintf(d, sizeof(d), "elements \"%s\"", seq);
    check("Curtis A: squeeze gives dit-dah, then stops", strcmp(seq, ".-") == 0, d);
  }

  // ---- 2. ...and Curtis B, which honours the latched opposite element even
  //          though the paddle was released during the current one. Same
  //          timeline as case 1: the ONLY difference must be the extra dit.
  {
    keyer_case_init(KEYER_MODE_B, FALSE, mark_hook);
    cw_keyer_paddle(CW_PADDLE_DOT, TRUE);
    cw_keyer_paddle(CW_PADDLE_DASH, TRUE);
    keyer_run_to(0.200);
    cw_keyer_paddle(CW_PADDLE_DOT, FALSE);
    cw_keyer_paddle(CW_PADDLE_DASH, FALSE);
    keyer_run_to(1.500);
    marks_to_string(seq, sizeof(seq));
    g_snprintf(d, sizeof(d), "elements \"%s\"", seq);
    check("Curtis B: same squeeze adds the trailing dit", strcmp(seq, ".-.") == 0, d);
  }

  // ---- 3. a held single paddle repeats its own element at the right rate:
  //          1 dit + 1 space = 120 ms at 20 WPM, so 1.2 s of hold is 10 dits.
  {
    keyer_case_init(KEYER_MODE_B, FALSE, mark_hook);
    cw_keyer_paddle(CW_PADDLE_DASH, TRUE);
    keyer_run_to(1.220);
    cw_keyer_paddle(CW_PADDLE_DASH, FALSE);
    keyer_run_to(2.000);
    int all_dah = 1;
    for (int i = 0; i < n_marks; i++) if (mark_type[i] != 1) all_dah = 0;
    // a dah element period is 3+1 = 4 dot units = 240 ms
    g_snprintf(d, sizeof(d), "%d dahs in 1.22 s (expect 5-6), all dah=%d", n_marks, all_dah);
    check("held dash paddle repeats dahs at speed", all_dah && n_marks >= 5 && n_marks <= 6, d);
  }

  // ---- 4. reversed toggled MID-HOLD must not strand the key down. The keyer
  //          stores PHYSICAL contacts and applies the reversal at read time; if
  //          it stored the logical side instead, the release below would clear
  //          a different flag than the press set and the paddle would look held
  //          for ever — the transmitter keyed with nobody touching it.
  //          Checked through the real cw_tx_key() path, so "keyed" means the
  //          sidetone is really there.
  {
    keyer_case_init(KEYER_MODE_B, FALSE, NULL);   // no hook: exercise cw_tx_key()
    cw_keyer_paddle(CW_PADDLE_DOT, TRUE);
    keyer_run_to(0.020);
    double on_peak = tone_peak(0.0, 10.0);        // positive control: it IS keyed
    radio->cw_keys_reversed = TRUE;               // operator flips the setting mid-element
    keyer_run_to(0.030);
    cw_keyer_paddle(CW_PADDLE_DOT, FALSE);        // the same PHYSICAL contact opens
    keyer_run_to(2.000);
    double off_peak = tone_peak(50.0, 1000.0);    // 1 s: any further element would show
    g_snprintf(d, sizeof(d), "keyed peak %.3f, after release peak %.4f", on_peak, off_peak);
    check("reverse toggled mid-hold releases the key", on_peak > 0.1 && off_peak < 0.01, d);
    radio->cw_keys_reversed = FALSE;
  }

  // ---- 5. switching to Straight mid-element must release the key rather than
  //          leaving the iambic element sounding with no state machine left to
  //          end it.
  {
    keyer_case_init(KEYER_MODE_B, FALSE, NULL);
    cw_keyer_paddle(CW_PADDLE_DASH, TRUE);        // a dah: 180 ms, plenty of room
    keyer_run_to(0.020);
    double on_peak = tone_peak(0.0, 10.0);
    radio->cw_keyer_mode = KEYER_STRAIGHT;        // switched mid-element
    keyer_run_to(0.040);
    double off_peak = tone_peak(50.0, 1000.0);
    g_snprintf(d, sizeof(d), "keyed peak %.3f, after switch peak %.4f", on_peak, off_peak);
    check("switch to Straight mid-element releases key", on_peak > 0.1 && off_peak < 0.01, d);
  }

  // ---- 6. the stuck-paddle timeout: a paddle held with no state change for
  //          CW_KEYER_STUCK_S (15 s) is a lost key-up (focus loss, dropped MIDI
  //          note-off) and must be forced released. Without it the transmitter
  //          keys until someone notices.
  {
    keyer_case_init(KEYER_MODE_B, FALSE, mark_hook);
    cw_keyer_paddle(CW_PADDLE_DOT, TRUE);
    keyer_run_to(20.0);                           // never released
    int before = 0; double last_t = 0.0;
    for (int i = 0; i < n_marks; i++) {
      if (mark_time[i] < 14.0) before++;
      last_t = mark_time[i];
    }
    g_snprintf(d, sizeof(d), "%d marks, last at %.2f s (limit 15 s)", n_marks, last_t);
    check("stuck paddle is force-released after 15 s",
          before > 100 && last_t < 15.5, d);
  }

  // ---- 7. ...and the forced release must leave the KEY up too, not merely the
  //          state machine idle.
  {
    keyer_case_init(KEYER_MODE_B, FALSE, NULL);
    cw_keyer_paddle(CW_PADDLE_DOT, TRUE);
    keyer_run_to(0.020);
    double on_peak = tone_peak(0.0, 10.0);
    keyer_run_to(20.0);
    double off_peak = tone_peak(50.0, 1000.0);
    g_snprintf(d, sizeof(d), "keyed peak %.3f, after timeout peak %.4f", on_peak, off_peak);
    check("stuck-paddle timeout leaves the key up", on_peak > 0.1 && off_peak < 0.01, d);
  }

  // ---- 8. Straight-key mode keys directly off the DOT paddle and ignores the
  //          DASH one, and every press must be matched by its release.
  {
    keyer_case_init(KEYER_STRAIGHT, FALSE, NULL);
    cw_keyer_paddle(CW_PADDLE_DASH, TRUE);
    double dash_peak = tone_peak(10.0, 20.0);
    cw_keyer_paddle(CW_PADDLE_DASH, FALSE);
    cw_keyer_paddle(CW_PADDLE_DOT, TRUE);
    double dot_peak = tone_peak(0.0, 20.0);
    cw_keyer_paddle(CW_PADDLE_DOT, FALSE);
    double up_peak = tone_peak(50.0, 200.0);
    g_snprintf(d, sizeof(d), "dash %.4f, dot %.3f, released %.4f", dash_peak, dot_peak, up_peak);
    check("straight key: dot paddle keys, dash ignored",
          dash_peak < 0.01 && dot_peak > 0.1 && up_peak < 0.01, d);
    cw_tx_abort();
  }

  // ---- 9. ...and reversed, the OTHER contact is the key — the same read-time
  //          reversal, on the path that bypasses the state machine entirely.
  {
    keyer_case_init(KEYER_STRAIGHT, TRUE, NULL);
    cw_keyer_paddle(CW_PADDLE_DOT, TRUE);
    double dot_peak = tone_peak(10.0, 20.0);
    cw_keyer_paddle(CW_PADDLE_DOT, FALSE);
    cw_keyer_paddle(CW_PADDLE_DASH, TRUE);
    double dash_peak = tone_peak(0.0, 20.0);
    cw_keyer_paddle(CW_PADDLE_DASH, FALSE);
    double up_peak = tone_peak(50.0, 200.0);
    g_snprintf(d, sizeof(d), "dot %.4f, dash %.3f, released %.4f", dot_peak, dash_peak, up_peak);
    check("straight key reversed: dash paddle keys",
          dot_peak < 0.01 && dash_peak > 0.1 && up_peak < 0.01, d);
    radio->cw_keys_reversed = FALSE;
    cw_tx_abort();
  }

  cw_keyer_set_test_hook(NULL);
  cw_keyer_reset();
  cw_tx_abort();
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--selftest") == 0) continue;   // the only mode there is
    printf("usage: %s [--selftest]\n", argv[0]);
    printf("  Runs the CW encoder/decoder round trip, the noise-rejection\n");
    printf("  assertion and the iambic keyer unit tests. No recording, no radio.\n");
    return (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) ? 0 : 2;
  }

  radio = g_new0(RADIO, 1);
  radio->can_transmit = TRUE;
  radio->transmitter = g_new0(TRANSMITTER, 1);
  radio->cw_keyer_speed = 20;
  radio->cw_keyer_weight = 50;
  radio->cw_keyer_mode = KEYER_MODE_B;
  radio->cw_keyer_sidetone_frequency = 600;
  radio->cw_keyer_hang_time = 250;

  // Line-buffered: the keyer's own log_error() goes to unbuffered stderr, so
  // without this the two expected "paddle held >15s" lines land ahead of every
  // PASS line instead of next to the case that provoked them.
  setvbuf(stdout, NULL, _IOLBF, 0);

  printf("-- encoder -> decoder round trip --\n");
  run_round_trip_cases();
  printf("\n-- noise rejection --\n");
  run_noise_case();
  printf("\n-- iambic keyer --\n");
  printf("   (two 'paddle held >15s' log lines below are the stuck-paddle\n");
  printf("    cases doing their job, not failures)\n");
  run_keyer_cases();

  cw_decoder_set_enabled(FALSE);
  printf("\n%s\n", failures == 0 ? "all cases passed" : "FAILURES ABOVE");
  return failures == 0 ? 0 : 1;
}
