/* agc_offline.c -- self-test for the AGC block's run flag, and the FM AGC it
 * gates (src/core/agc.h, set_agc() in src/core/receiver.c).
 *
 * Why this exists.  WDSP's SetRXAMode() clears the AGC block's run flag for FM and WFM, and
 * nothing else in WDSP ever writes it -- so for the whole life of this fork the
 * AGC menu and the AGC-G slider worked perfectly in FMN and changed nothing,
 * because the block was not in the chain. set_agc() puts the flag back through
 * SetRXAAGCRun(), which only works while it runs AFTER set_mode(); a caller
 * that ever reverses that order loses FM AGC again, silently and with every
 * control still moving.
 *
 * That is what this harness pins, and it is the one thing no other test here
 * can see: `make check` never opens a WDSP channel, and the fault is invisible
 * from the UI because nothing about it looks broken.
 *
 * Two halves:
 *   - the FLAG, read back with GetRXAAGCRun(), against agc_run_for() -- the same
 *     inline receiver.c and subrx.c call, not a copy of the rule;
 *   - the EFFECT, measured: a 1 kHz tone FM'd at 3000 Hz and at 300 Hz
 *     deviation is a 10:1 (20 dB) spread at the discriminator, and levelling it
 *     is the whole point of the feature. The flag-clear run is the negative
 *     control -- it must show the full spread, or the measurement is not
 *     looking at anything.
 *
 * No WDSPwisdom(): the exhaustive FFT sweep costs minutes and buys nothing at
 * one channel and 1024 samples. Runs in about a second.
 *
 *   make agc-offline && ./agc_offline --selftest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "wdsp.h"
#include "agc.h"
#include "mode.h"

#define CH   0
#define BLK  1024
#define FS   48000.0

static int failures = 0;

static void check(const char *name, int ok, const char *fmt, ...) {
  va_list ap;
  printf("%-46s %s   ", name, ok ? "PASS" : "FAIL");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  if(!ok) failures++;
}

// The app's own order: receiver_mode_changed() calls set_mode() (SetRXAMode,
// which owns the flag) and only then set_agc(). Everything below goes through
// this, so a harness pass cannot come from a luckier order than the app's.
// mode.h's enum and WDSP's RXA mode enum are the same list in the same order,
// which is why the app hands rx->mode_a straight to SetRXAMode(); RXA.h cannot
// be included beside wdsp.h (duplicate enumerators), so this does the same.
static void app_set_mode_then_agc(int mode, int agc) {
  // SetRXAMode is a no-op when the mode is unchanged, so arm it from the other
  // family first -- exactly what a real mode change does.
  SetRXAMode(CH, mode == FMN ? LSB : FMN);
  SetRXAMode(CH, mode);
  SetRXAFMDeviation(CH, 8000.0);

  // ---- this block is set_agc(), receiver.c ----
  SetRXAAGCRun(CH, agc_run_for(mode, agc));
  SetRXAAGCMode(CH, agc);
  SetRXAAGCSlope(CH, 35);
  SetRXAAGCTop(CH, 80.0);
  if(agc != AGC_OFF) {
    SetRXAAGCAttack(CH, 2);
    SetRXAAGCHang(CH, 0);
    SetRXAAGCDecay(CH, 50);
    SetRXAAGCHangThreshold(CH, 100);
  }
}

// RMS of the demodulated audio for a tone FM'd at the given deviation.
// Blocks fexchange0 refuses (-2, its input ring full) are skipped rather than
// counted: a run that measured them would be averaging stale output, which is
// how an earlier version of this harness read 11.7 dB where the arithmetic says
// 20.
static double audio_rms(double dev_hz) {
  double *in  = malloc(sizeof(double) * 2 * BLK);
  double *out = malloc(sizeof(double) * 2 * BLK);
  double phi = 0.0, t = 0.0, sum = 0.0;
  long n = 0;
  int good = 0;

  for(int b = 0; b < 4000 && n < 200L * BLK; b++) {
    for(int i = 0; i < BLK; i++) {
      phi += 2.0 * M_PI * (dev_hz * sin(2.0 * M_PI * 1000.0 * t)) / FS;
      // The wire pair is (Q, I) -- see the I/Q buffer order rule in CLAUDE.md.
      in[2 * i + 0] = sin(phi);
      in[2 * i + 1] = cos(phi);
      t += 1.0 / FS;
    }
    int err = 0;
    fexchange0(CH, in, out, &err);
    if(err != 0) continue;
    // Let the channel's filters and the AGC settle before believing anything.
    if(++good > 300) {
      for(int i = 0; i < BLK; i++) { sum += out[2 * i] * out[2 * i]; n++; }
    }
  }
  free(in); free(out);
  return n ? sqrt(sum / (double)n) : 0.0;
}

static double spread_db(int agc) {
  app_set_mode_then_agc(FMN, agc);
  double loud  = audio_rms(3000.0);
  app_set_mode_then_agc(FMN, agc);
  double quiet = audio_rms(300.0);
  if(quiet <= 0.0 || loud <= 0.0) return NAN;   // nothing came out: not a spread
  return 20.0 * log10(loud / quiet);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;   // --selftest is the only mode; the argument is for
                            // `make check`, which passes it to every harness.

  SetDSPMult(2);
  OpenChannel(CH, BLK, BLK, (int)FS, (int)FS, (int)FS, 0 /*receive*/, 1 /*run*/,
              0.010, 0.025, 0.0, 0.010, 0);
  RXASetNC(CH, 2048);
  RXASetMP(CH, 0);
  RXASetPassband(CH, -8000.0, 8000.0);
  SetRXAPanelGain1(CH, 1.0);
  SetRXAPanelSelect(CH, 3);
  SetRXAPanelPan(CH, 0.5);
  SetRXAPanelCopy(CH, 0);
  SetRXAPanelBinaural(CH, 0);
  SetRXAPanelRun(CH, 1);
  SetChannelState(CH, 1, 0);

  int run;

  printf("\n-- what WDSP does on its own (the reason set_agc has to act) --\n");
  SetRXAMode(CH, FMN);
  SetRXAMode(CH, LSB);
  GetRXAAGCRun(CH, &run);
  check("SetRXAMode(LSB) leaves the AGC in the chain", run == 1, "run=%d", run);
  SetRXAMode(CH, FMN);
  GetRXAAGCRun(CH, &run);
  check("SetRXAMode(FM) takes it OUT", run == 0, "run=%d", run);

  printf("\n-- agc_run_for(), pushed after the mode like set_agc() --\n");
  app_set_mode_then_agc(LSB, AGC_OFF);
  GetRXAAGCRun(CH, &run);
  check("LSB + AGC OFF still runs (fixed-gain path)", run == 1, "run=%d", run);
  app_set_mode_then_agc(LSB, AGC_FAST);
  GetRXAAGCRun(CH, &run);
  check("LSB + AGC FAST runs", run == 1, "run=%d", run);
  app_set_mode_then_agc(FMN, AGC_OFF);
  GetRXAAGCRun(CH, &run);
  check("FM + AGC OFF stays out (no +60 dB fixed gain)", run == 0, "run=%d", run);
  app_set_mode_then_agc(FMN, AGC_FAST);
  GetRXAAGCRun(CH, &run);
  check("FM + AGC FAST is back in the chain", run == 1, "run=%d", run);

  // The order is the whole discipline: set_agc() has to FOLLOW set_mode(),
  // because SetRXAMode() is what clears the flag. Reversed, every control still
  // moves and FM AGC is gone -- which is exactly how this shipped for years.
  SetRXAMode(CH, LSB);
  SetRXAAGCRun(CH, agc_run_for(FMN, AGC_FAST));
  SetRXAMode(CH, FMN);
  GetRXAAGCRun(CH, &run);
  check("set_agc BEFORE set_mode loses it (order matters)", run == 0, "run=%d", run);

  printf("\n-- what it does to the audio (1 kHz tone, 3000 vs 300 Hz deviation) --\n");
  double off = spread_db(AGC_OFF);
  double on  = spread_db(AGC_FAST);
  // The negative control: without the AGC the two stations MUST come out 20 dB
  // apart (the deviation ratio), or this measurement is blind and the pass
  // below would mean nothing.
  check("AGC off: the full deviation spread survives", off > 15.0,
        "%.1f dB apart (arithmetic says 20.0)", off);
  // Signed, because a levelled pair can land either way round -- what is being
  // asserted is that the 20 dB is gone, not which one ends up on top.
  check("AGC fast: the two are levelled", fabs(on) < 4.0, "%.1f dB apart", on);

  printf("\n%s\n", failures ? "FAILED" : "all cases passed");
  return failures ? 1 : 0;
}
