/* nr_offline.c -- self-test for the NR3 (RNNoise) and NR4 (libspecbleach)
 * receive noise-reduction blocks, wdsp/rnnr.c and wdsp/sbnr.c.
 *
 * Why this exists.  Every other RXA block carries a setSize_*() and a
 * setSamplerate_*() that setDSPBuffsize_rxa()/setSamplerate_rxa() push the
 * channel's CURRENT geometry into.  rnnr and sbnr shipped without either, so
 * they kept the block size the channel was OPENED with, and a span change --
 * which re-runs SetDSPBuffsize on a live channel -- left them mismatched:
 *
 *   - the block SHRINKS (384000 -> 1920000, dsp_size 5120 -> 1024): they read
 *     and wrote 5120 complex samples out of a midbuff that now holds 2048.
 *   - the block GROWS (1920000 -> 384000): only the first fifth of every block
 *     was processed and the other four fifths passed through raw, spliced
 *     against the delayed processed part twice per 106.7 ms block.
 *
 * Measured here before the setters existed, against the same run with no span
 * change: NR3's noise reduction went from 19.6 dB to 8.9 dB one way and to
 * 0.1 dB -- i.e. the feature did nothing at all -- the other.  NR (ANR) and NR2
 * (EMNR) were untouched, which is exactly the shape of the operator's report:
 * "NR3 and NR4 sound bad".
 *
 * The three things pinned:
 *   - that each block is IN the chain and doing something: it must move the
 *     noise floor by more than 5 dB.  The negative control is the same
 *     measurement with the NR off, which must move nothing -- or every other
 *     PASS here is about the chain and not about the block;
 *   - the SPAN CHANGE, both directions, against a channel OPENED at the block
 *     it ends up on.  Both the noise reduction and the voice level have to
 *     match it, which is the assertion the missing setters fail;
 *   - that NR3 stands down, WITHOUT bp1's 2.0 gain being left behind, on a
 *     channel that is not at 48 kHz.
 *
 * One fix here is deliberately NOT asserted, because every way of measuring it
 * through the chain turned out flakier than the thing it measures: RNNoise's
 * 480-sample frame divides no dsp_size this tree uses, so the first blocks
 * after a flush drain fewer samples than they must emit, and that shortfall was
 * filled with silence spliced into the audio -- deterministically seven blocks
 * of 64 samples at dsp_size 1024, five of up to 128 at 2048, i.e. a chop over
 * the first ~150 ms every time NR3 is switched on.  flush_rnnr primes a frame
 * of output instead, which makes the deficit unreachable at every block size
 * from 1024 to 25600.  It is ring arithmetic, provable on paper and by
 * simulation; through bp1 the zeros are no longer zeros and correlating the
 * creep out of RNNoise's own gating gave a 150-sample spread on code that is
 * correct.  A flaky assertion is worse than none.
 *
 * The signal is synthetic but deliberately NON-STATIONARY -- swept formants, a
 * gliding F0, a syllabic envelope and fricative bursts.  A stationary harmonic
 * complex is useless here: every spectral noise reducer learns it AS noise and
 * removes it, so a harness built on one measures the reducer destroying the
 * "speech" it was given and calls that a failure.
 *
 * What this harness deliberately does NOT assert is that the voice comes out
 * intact, because a synthesised voice does not exercise RNNoise's VAD: it gates
 * this signal by 12..27 dB where on real speech (macOS `say`, 13.5 s, 11 dB of
 * in-band SNR) it leaves the level within 1 dB and lifts the SNR by 20.  That
 * measurement needs a recording and belongs on the bench, not in `make check`;
 * what belongs here is that the same block behaves the same either side of a
 * span change, which is measurable on any signal at all.
 *
 * Two harness rules that cost real time to find, both about fexchange0:
 *   - it ALWAYS consumes its input.  There is no "input ring full" refusal (the
 *     check is a TODO comment in iobuffs.c).  *error = -2 says only that the
 *     OUTPUT was not ready, and it hands back a block of zeros having eaten the
 *     input anyway -- so a feeder that skips or re-offers on -2 measures its own
 *     splices.  The channel is opened with bfo=1, which makes fexchange0 wait
 *     for the DSP thread, and every call is then one block in, one block out.
 *   - RXASetNC() is the FILTER length and must be at least the block, or the
 *     NBP's fircore runs an FFT plan it never built.  The app passes
 *     rx->fft_size (RX_BLOCK_WIDE = 5120 on every wide-buffer device).
 *
 * No WDSPwisdom(): the exhaustive FFT sweep costs minutes and buys nothing.
 *
 *   make nr-offline && ./nr_offline --selftest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "wdsp.h"

#define CH        0
#define FS        48000
#define NC        5120          /* the app's fft_size for a wide-buffer device */
#define SECS      12
#define NS        (FS*SECS)
#define SETTLE    (FS*2)        /* skip the AGC/filter settling */

/* the two block sizes a Pluto/HackRF span change moves between */
#define BLK_NARROW  1024        /* 1920000 span: 5120/5 */
#define BLK_WIDE    5120        /* 192000 and 384000 spans */

enum { NR_OFF = 0, NR_ANR = 1, NR_EMNR = 2, NR_RNN = 3, NR_SB = 4 };

static int failures = 0;

static void check(const char *name, int ok, const char *fmt, ...) {
  va_list ap;
  printf("%-52s %s   ", name, ok ? "PASS" : "FAIL");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  if(!ok) failures++;
}

/* ---------------------------------------------------------------- signal --- */

static double *sigI, *sigQ;     /* USB analytic pair, (I, Q) in natural order */
static double *outbuf;

static unsigned int rs = 1234567u;
static double urand(void) { rs = rs*1103515245u + 12345u; return (double)((rs>>16)&0x7fff)/32768.0; }

/* A voice that MOVES: F0 glides, the three formants walk between /a/, /i/ and
 * /u/ targets every 150 ms, the syllable is 400 ms on and 400 ms off, and every
 * other syllable ends in an unvoiced burst.  Built directly as an analytic pair
 * (each harmonic contributes cos to I and sin to Q), so no Hilbert transform is
 * needed and the USB image is exact. */
static void make_signal(double level, double nlev) {
  static const double vowF[3][3] = {{ 730,1090,2440},   /* a */
                                    { 270,2290,3010},   /* i */
                                    { 300, 870,2240}};  /* u */
  sigI = malloc(sizeof(double)*NS);
  sigQ = malloc(sizeof(double)*NS);
  outbuf = calloc(NS, sizeof(double));

  double phase[64] = {0};
  for(int i=0;i<64;i++) phase[i] = 2.0*M_PI*urand();

  for(long n=0;n<NS;n++) {
    double t = (double)n/FS;
    double cyc = fmod(t, 0.8);
    double sh = 0.0;
    if(cyc < 0.4) {                                   /* 20 ms raised-cosine edges */
      double e = cyc < 0.02 ? cyc/0.02 : (cyc > 0.38 ? (0.4-cyc)/0.02 : 1.0);
      sh = 0.5 - 0.5*cos(M_PI*(e<0?0:(e>1?1:e)));
    }
    /* which vowel, and how far into the glide towards the next one */
    double seg = t/0.15;
    int v0 = (int)seg % 3, v1 = (v0+1)%3;
    double mu = seg - floor(seg);
    double F[3];
    for(int k=0;k<3;k++) F[k] = vowF[v0][k]*(1.0-mu) + vowF[v1][k]*mu;

    double f0 = 105.0 + 25.0*sin(2.0*M_PI*t/1.7);     /* gliding pitch */

    double si=0.0, sq=0.0;
    int nh = 0;
    for(int k=2;k<=28;k++) {
      double fk = k*f0;
      if(fk < 180.0 || fk > 2900.0) continue;
      double a = 0.0;
      for(int j=0;j<3;j++) {
        double d = (fk - F[j]) / (100.0 + 20.0*j);
        a += (j==0 ? 1.0 : (j==1 ? 0.6 : 0.3)) / sqrt(1.0 + d*d);
      }
      a *= 6.0/(double)k;                             /* glottal tilt */
      double th = 2.0*M_PI*fk*t + phase[nh];
      si += a*cos(th); sq += a*sin(th);
      nh++;
    }
    /* an unvoiced burst on every other syllable: shaped noise, no harmonics */
    double fric = 0.0;
    if(cyc > 0.30 && cyc < 0.38 && fmod(t,1.6) < 0.8) fric = 1.2;

    double vi = sh*si*0.30 + fric*sh*(urand()-0.5);
    double vq = sh*sq*0.30 + fric*sh*(urand()-0.5);

    sigI[n] = level*(vi + nlev*(urand()+urand()+urand()+urand()-2.0));
    sigQ[n] = level*(vq + nlev*(urand()+urand()+urand()+urand()-2.0));
  }
}

/* ----------------------------------------------------------------- channel - */

static void channel_open(int block) {
  SetDSPMult(4);
  /* bfo=1: fexchange0 waits for the DSP thread, one block in, one block out */
  OpenChannel(CH, block, block, FS, FS, FS, 0 /*receive*/, 1 /*run*/,
              0.010, 0.025, 0.0, 0.010, 1);
  RXASetNC(CH, NC);
  RXASetMP(CH, 0);
  SetRXAPanelGain1(CH, 1.0);
  SetRXAPanelSelect(CH, 3);
  SetRXAPanelPan(CH, 0.5);
  SetRXAPanelCopy(CH, 0);
  SetRXAPanelBinaural(CH, 0);
  SetRXAPanelRun(CH, 1);
  SetRXAMode(CH, 1);                                  /* USB */
  SetRXAAGCRun(CH, 0);                                /* the AGC would hide the level */
  RXASetPassband(CH, 150.0, 3000.0);
  SetChannelState(CH, 1, 0);
}

/* A span change, exactly as receiver_change_sample_rate does it: the channel is
 * STOPPED, both block sizes are re-set, the rates are re-asserted, and it is
 * started again. */
static void channel_resize(int block) {
  SetChannelState(CH, 0, 1);
  SetInputBuffsize(CH, block);
  SetDSPBuffsize(CH, block);
  SetAllRates(CH, FS, FS, FS);
  SetChannelState(CH, 1, 0);
}

static void select_nr(int nr) {
  SetRXAANRRun (CH, nr == NR_ANR);
  SetRXAEMNRRun(CH, nr == NR_EMNR);
  SetRXARNNRRun(CH, nr == NR_RNN);
  SetRXASBNRRun(CH, nr == NR_SB);
}

/* Runs the whole signal through and captures the demodulated audio. */
static void run_chain(int nr, int block) {
  /* stop FIRST, then select, then start: with the channel running the DSP
   * thread is already chewing on the primed silence, and how many blocks it
   * gets through before the stop is a race -- one that leaves the block's
   * adaptive state different between two otherwise identical runs. */
  SetChannelState(CH, 0, 1);
  select_nr(nr);
  SetChannelState(CH, 0, 1);                          /* flush every block's state */
  SetChannelState(CH, 1, 0);
  double *in  = malloc(sizeof(double)*2*block);
  double *out = malloc(sizeof(double)*2*block);
  long n = 0;
  for(long pos = 0; pos + block <= NS; pos += block) {
    for(int i=0;i<block;i++) { in[2*i] = sigQ[pos+i]; in[2*i+1] = sigI[pos+i]; }
    int err = 0;
    fexchange0(CH, in, out, &err);
    for(int i=0;i<block && n<NS;i++) outbuf[n++] = out[2*i];
  }
  free(in); free(out);
}

static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* Level of the syllables and of the gaps between them, from the sorted
 * short-term RMS -- alignment-free, so the latency each block adds (RNNoise a
 * frame, libspecbleach an STFT hop, the FIRs their own) cannot skew it. */
static void levels(double *voice, double *floor_) {
  const int w = 960;                                  /* 20 ms */
  static double v[NS/960 + 2];
  long m = 0;
  for(long n=SETTLE; n+w<NS; n+=w) {
    double s = 0.0;
    for(int i=0;i<w;i++) s += outbuf[n+i]*outbuf[n+i];
    v[m++] = sqrt(s/w);
  }
  qsort(v, m, sizeof(double), cmpd);
  *floor_ = v[(long)(0.10*m)];
  *voice  = v[(long)(0.90*m)];
}

typedef struct { double voice, floor_, snr; } RESULT;

/* The span-change assertion is made on the WAVEFORM, not on a statistic: after
 * the same flush and the same input, a block whose geometry followed the
 * channel must produce the same audio as one on a channel opened at that block.
 * A level/SNR comparison is too loose for RNNoise -- its per-band gating on a
 * synthetic voice moves a couple of dB between runs for reasons that have
 * nothing to do with the wiring, which is exactly the kind of threshold that
 * ends up widened until it proves nothing.
 *
 * Not bit-exactness, because neither block reproduces itself bit for bit: on a
 * FIXED input this residual was measured at -45.9, -50.0, -56.6, -63.5 and
 * -2980.7 dB across five runs of one binary on one machine, and at -19.5 dB on
 * a CI runner -- so it carries the numerical spread of the rebuilt filters as
 * well as the geometry, and a threshold inside that spread fails at random. The
 * defect it exists to catch is nowhere near it: a mismatched geometry reads
 * +1.2 dB and +11.0 dB, i.e. a residual LOUDER than the reference -- a
 * different signal, not a drifting one. -12 dB keeps 12 dB of clearance from
 * the worst run observed and 13 dB from the fault, which is what a threshold
 * has to do; widening it further would start to mean nothing. */
static double *saved;
static void save_out(void) {
  if(!saved) saved = malloc(sizeof(double)*NS);
  memcpy(saved, outbuf, sizeof(double)*NS);
}
/* residual of outbuf against the saved run, in dB below the saved run */
static double residual_db(void) {
  double sd = 0.0, sr = 0.0;
  for(long n=SETTLE;n<NS;n++) {
    double d = outbuf[n] - saved[n];
    sd += d*d; sr += saved[n]*saved[n];
  }
  if(sr <= 0.0) return 0.0;
  return 10.0*log10((sd > 0.0 ? sd : 1e-300)/sr);
}

static RESULT measure(int nr, int block) {
  RESULT r;
  run_chain(nr, block);
  levels(&r.voice, &r.floor_);
  r.snr = 20.0*log10(r.voice / (r.floor_ > 0.0 ? r.floor_ : 1e-30));
  return r;
}

static const char *nrname(int nr) {
  switch(nr) { case NR_RNN: return "NR3"; case NR_SB: return "NR4";
               case NR_EMNR: return "NR2"; case NR_ANR: return "NR"; }
  return "off";
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;   /* --selftest is the only mode; `make check` passes it */

  /* 0.001 of full scale with as much noise again: about 13 dB of in-band SNR at
   * the demodulator -- a signal worth switching NR on for, and weak enough that
   * a block doing nothing is visible.  NR_NOISE overrides it by hand. */
  { const char *e = getenv("NR_NOISE");
    make_signal(0.001, e ? atof(e) : 1.0); }

  RESULT ctlN[5];

  printf("\n-- in the chain and doing something (USB, dsp_size %d) --\n", BLK_NARROW);
  channel_open(BLK_NARROW);
  ctlN[NR_OFF] = measure(NR_OFF, BLK_NARROW);
  printf("   NR off: voice %.4g, floor %.4g, %.1f dB apart\n",
         ctlN[NR_OFF].voice, ctlN[NR_OFF].floor_, ctlN[NR_OFF].snr);
  for(int nr = NR_RNN; nr <= NR_SB; nr++) {
    ctlN[nr] = measure(nr, BLK_NARROW);
    char name[80];
    snprintf(name, sizeof name, "%s moves the noise floor", nrname(nr));
    check(name, ctlN[nr].snr - ctlN[NR_OFF].snr > 5.0,
          "%+.1f dB of SNR", ctlN[nr].snr - ctlN[NR_OFF].snr);

  }
  {
    RESULT again = measure(NR_OFF, BLK_NARROW);
    check("negative control: NR off moves nothing",
          fabs(again.snr - ctlN[NR_OFF].snr) < 1.0,
          "%+.1f dB", again.snr - ctlN[NR_OFF].snr);
  }
  CloseChannel(CH);

  /* ---- the span change, which is what was broken.  A block whose geometry did
   * not follow the channel reads a midbuff that has shrunk under it, or
   * processes the first fifth of each block and passes the rest through raw. ---- */
  printf("\n-- after a span change (SetDSPBuffsize on a live channel) --\n");

  for(int nr = NR_RNN; nr <= NR_SB; nr++) {
    char name[80];

    channel_open(BLK_NARROW);                 /* the reference: opened narrow */
    measure(nr, BLK_NARROW);
    save_out();
    CloseChannel(CH);
    channel_open(BLK_WIDE);                   /* 384000 -> 1920000 */
    channel_resize(BLK_NARROW);
    measure(nr, BLK_NARROW);
    double d = residual_db();
    snprintf(name, sizeof name, "%s: 5120 -> 1024 is the same audio", nrname(nr));
    check(name, d < -12.0, "residual %.1f dB", d);
    CloseChannel(CH);

    channel_open(BLK_WIDE);                   /* the reference: opened wide */
    measure(nr, BLK_WIDE);
    save_out();
    CloseChannel(CH);
    channel_open(BLK_NARROW);                 /* 1920000 -> 384000 */
    channel_resize(BLK_WIDE);
    measure(nr, BLK_WIDE);
    d = residual_db();
    snprintf(name, sizeof name, "%s: 1024 -> 5120 is the same audio", nrname(nr));
    check(name, d < -12.0, "residual %.1f dB", d);
    CloseChannel(CH);
  }

  /* ---- the sample rate.  RNNoise's 480-sample frame IS 10 ms at 48 kHz and
   * nothing else, so away from 48 kHz (WFM runs the chain at the span) the
   * block stands down.  It has to stand down through its RUN flag, because bp1
   * is switched on at a gain of 2.0 whenever an NR block runs -- that gain is
   * what makes the real signal these blocks emit analytic again, and left over
   * a buffer that is still complex it is +6 dB.  libspecbleach takes the rate,
   * so it is rebuilt instead and keeps working. ---- */
  printf("\n-- a channel that is not at 48 kHz (WFM runs the DSP at the span) --\n");
  channel_open(BLK_NARROW);
  SetChannelState(CH, 0, 1);
  SetAllRates(CH, FS, 192000, FS);
  SetChannelState(CH, 1, 0);
  {
    RESULT o = measure(NR_OFF, BLK_NARROW);
    RESULT r = measure(NR_RNN, BLK_NARROW);
    double dv = 20.0*log10(r.voice/o.voice);
    check("NR3 stands down at a non-48 kHz DSP rate",
          fabs(dv) < 0.5 && fabs(r.snr - o.snr) < 0.5,
          "%+.2f dB level, %+.2f dB SNR", dv, r.snr - o.snr);
    RESULT sb = measure(NR_SB, BLK_NARROW);
    check("NR4 is rebuilt at the new rate and stays finite",
          isfinite(sb.voice) && sb.voice > 0.0, "voice %.4g", sb.voice);
  }
  CloseChannel(CH);

  printf("\n%s\n", failures ? "FAILED" : "all cases passed");
  return failures ? 1 : 0;
}
