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
 *     channel that is not at 48 kHz;
 *   - NR3's depth control, both that it goes monotonically from no reduction to
 *     all of it and that its dry side is FRAME-ALIGNED with its wet side.
 *     rnnoise_process_frame lags its input by one whole 480-sample frame
 *     (measured: an impulse fed at sample 9700 came out at 10180), so the dry
 *     copy has to be held back by one frame or the mix comb-filters.  A
 *     1050 Hz tone is 10.5 cycles per frame, i.e. exactly antiphase one frame
 *     out, which turns that mistake into a collapse at depth 0.5 instead of
 *     something subtle: aligned it reads +0.01 dB against depth 0, and with the
 *     dry deliberately taken from the current frame it reads -36.7 dB.
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
#ifdef _WIN32
#include <windows.h>
#define tap_nap() Sleep(1)
#else
#include <time.h>
static void tap_nap(void) { struct timespec ts = { 0, 500000 }; nanosleep(&ts, NULL); }
#endif

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
  if(!sigI) {
    sigI = malloc(sizeof(double)*NS);
    sigQ = malloc(sizeof(double)*NS);
    outbuf = calloc(NS, sizeof(double));
  }

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

/* A steady tone, for the wet/dry alignment case.  1050 Hz because 480 samples
 * of it is 10.5 cycles: one frame of misalignment is a sign flip. */
static void make_tone(double level, double f) {
  for(long n=0;n<NS;n++) {
    double th = 2.0*M_PI*f*(double)n/FS;
    sigI[n] = level*cos(th);
    sigQ[n] = level*sin(th);
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

/* ------------------------------------------------------------- the tap ---
 *
 * The stream tap (SetRXAPreAgcTap): a ring WDSP fills inside xrxa with the
 * signal as it is the moment it has been demodulated, filtered by a bandpass of
 * the tap's own.  Installed here exactly as receiver_pretap_alloc does it,
 * including on a span change, because that is what broke: the tap's filter is
 * sized from dsp_size and its COEFFICIENT COUNT was not, and fircore answers
 * nc < size with nfor = 0, a zero-length plan array and a NULL fftw plan -- a
 * SIGSEGV on the DSP thread on the next block, not bad audio.  The ring is
 * installed for every receiver whether or not anything is subscribed, so that
 * was a crash for any operator changing span. */
static double *tap_ring;        /* what WDSP writes into */
static int     tap_cap;
static long    tap_r;           /* the app's own read cursor */
static int     tap_blk;         /* the DSP block the ring was sized for */
static double *tapbuf;          /* what this harness reads out, I only */
static long    tap_n;

static void tap_install(int block) {
  int cap = block*6;                                  /* as receiver_pretap_alloc */
  if(cap < 4096) cap = 4096;
  SetRXAPreAgcTap(CH, NULL, 0);
  free(tap_ring);
  tap_ring = calloc((size_t)cap*2, sizeof(double));
  tap_cap  = cap;
  tap_blk  = block;
  if(!tapbuf) tapbuf = malloc(sizeof(double)*NS);
  SetRXAPreAgcTap(CH, tap_ring, cap);
  tap_r = GetRXAPreAgcTapPos(CH);
}

static void tap_remove(void) {
  SetRXAPreAgcTap(CH, NULL, 0);
  free(tap_ring); tap_ring = NULL; tap_cap = 0;
}

/* Drain whatever the last DSP pass wrote.  Same walk as rx_tci_audio_publish:
 * a monotonic write count, the reader dropped forward if it fell behind. */
static void tap_drain(void) {
  if(!tap_ring) return;
  long w = GetRXAPreAgcTapPos(CH);
  long avail = w - tap_r;
  if(avail <= 0) return;
  /* Two blocks of the ring stay clear of the reader, as in the app: the writer
   * is not waiting for this loop, so "the reader may hold the whole ring" means
   * the writer's next block lands on what is being copied. */
  long room = tap_cap - 2L*tap_blk;
  if(room < tap_blk) room = tap_cap;
  if(avail > room) { tap_r = w - room; avail = room; }
  while(avail > 0) {
    int idx = (int)(tap_r % tap_cap);
    int n   = tap_cap - idx;
    if(n > avail) n = (int)avail;
    for(int i=0;i<n && tap_n<NS;i++) tapbuf[tap_n++] = tap_ring[2*(idx+i)];
    tap_r  += n;
    avail  -= n;
  }
}

/* Collect the rest of what this run owes us, instead of guessing when the DSP
 * thread is done.
 *
 * fexchange0 processes nothing.  It copies the block into r1, releases
 * Sem_BuffReady and returns; WDSP's own thread does the work and is what writes
 * this tap.  bfo=1 does not pace them in lock-step either, because
 * flush_iobuffs primes Sem_OutReady with (dsp_mult-1) blocks.  So the harness
 * runs ahead, and the blocks still in flight when the input loop ends have not
 * reached the tap.  The assertion below used to be written around the four
 * blocks an IDLE machine leaves behind; a loaded one leaves more (5 and 10
 * short in 2 local runs of 20, 15 short on the macos-15-intel runner, which
 * failed the 4.4 release build).
 *
 * The first cure here waited for the tap position to stop moving -- three
 * unchanged reads 0.5 ms apart -- and that is the SAME MISTAKE one level down:
 * on a loaded runner a DSP thread that is merely descheduled for two
 * milliseconds is indistinguishable from one that has finished, so it went
 * green locally and on one CI run and then failed the tag build 15 blocks
 * short.  An absence of change is not a completion.
 *
 * What the harness actually knows is how many samples this run owes it -- every
 * whole pass of the input -- so that is what it waits for, draining as it goes
 * so the ring cannot wrap under the reader.  A tap that genuinely does not
 * carry the audio never reaches the count and fails the assertion on the
 * timeout, which is the answer that case deserves. */
static void tap_collect(long want) {
  if(!tap_ring) return;
  /* 5 s at the outside: ~500x the worst tail measured, and a timeout here is a
   * failed assertion rather than a hung harness. */
  for(int i=0;i<10000 && tap_n<want;i++) {
    tap_drain();
    if(tap_n >= want) break;
    tap_nap();
  }
  tap_drain();
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
  tap_n = 0;
  if(tap_ring) tap_r = GetRXAPreAgcTapPos(CH);
  double *in  = malloc(sizeof(double)*2*block);
  double *out = malloc(sizeof(double)*2*block);
  long n = 0;
  for(long pos = 0; pos + block <= NS; pos += block) {
    for(int i=0;i<block;i++) { in[2*i] = sigQ[pos+i]; in[2*i+1] = sigI[pos+i]; }
    int err = 0;
    fexchange0(CH, in, out, &err);
    tap_drain();                                      /* the pass is done: bfo=1 */
    for(int i=0;i<block && n<NS;i++) outbuf[n++] = out[2*i];
  }
  tap_collect((NS/block)*(long)block);                 /* the blocks still in flight */
  free(in); free(out);
}

static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* Level of the syllables and of the gaps between them, from the sorted
 * short-term RMS -- alignment-free, so the latency each block adds (RNNoise a
 * frame, libspecbleach an STFT hop, the FIRs their own) cannot skew it. */
static void buf_levels(const double *b, long len, double *voice, double *floor_) {
  const int w = 960;                                  /* 20 ms */
  static double v[NS/960 + 2];
  long m = 0;
  for(long n=SETTLE; n+w<len; n+=w) {
    double s = 0.0;
    for(int i=0;i<w;i++) s += b[n+i]*b[n+i];
    v[m++] = sqrt(s/w);
  }
  if(m < 3) { *voice = *floor_ = 0.0; return; }
  qsort(v, m, sizeof(double), cmpd);
  *floor_ = v[(long)(0.10*m)];
  *voice  = v[(long)(0.90*m)];
}

static void levels(double *voice, double *floor_) {
  buf_levels(outbuf, NS, voice, floor_);
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
 * different signal, not a drifting one.
 *
 * That spread is not spread, though -- it is one localised wobble scored over
 * the whole ten seconds, and scoring it that way was still costing CI runs:
 * "NR4: 1024 -> 5120 is the same audio" reached -11.8 dB against the -12.0 dB
 * threshold here, which is the same failure the tap assertion below was having
 * and for the same reason (see median_second_residual).  Both are scored on the
 * MEDIAN one-second residual now.  The threshold stays at -12 dB, because what
 * it has to clear is the fault at +1.2 dB and not the wobble, and the wobble no
 * longer reaches it: the median second reads -2980 dB on code that is right. */
static double *saved;
static void save_out(void) {
  if(!saved) saved = malloc(sizeof(double)*NS);
  memcpy(saved, outbuf, sizeof(double)*NS);
}
/* The MEDIAN of the one-second residuals, which is the only honest way to score
 * a comparison that has to tell "bit-exact" from "not there at all".
 *
 * The chain is not reproducible run to run.  Two identical runs of it -- same
 * binary, same input, same open channel, nothing rebuilt in between -- come out
 * bit-identical about five times in six here, and the sixth differs inside ONE
 * window of three DSP blocks somewhere in the ten seconds, by about 1e-6
 * relative, with every sample either side of that window bit-exact.  Measured
 * with an off-versus-off control, which is how it was established that this is
 * the chain wobbling and not NR3 leaking; it shows on macOS and never on the
 * x86-64 CI runner, which is what an FFTW_PATIENT plan chosen by timing looks
 * like.  Three blocks of error against ten seconds of signal is a TOTAL
 * residual of about -22 dB -- so the total cannot be scored tightly, and this
 * assertion was written at -100 dB and duly failed about one CI run in three.
 *
 * The median can be scored tightly, because the two things it has to separate
 * sit at opposite ends of it: a block that reaches the tap reaches EVERY second
 * of it, and the wobble reaches one.  The negative control below is the same
 * measurement on the audio, which NR3 really does reach. */
static double median_second_residual(const double *a, const double *ref, long len) {
  double sec[SECS + 2];
  int m = 0;
  for(long n0 = SETTLE; n0 + FS <= len && m < SECS + 2; n0 += FS) {
    double sd = 0.0, sr = 0.0;
    for(long n = n0; n < n0 + FS; n++) {
      double d = a[n] - ref[n];
      sd += d*d; sr += ref[n]*ref[n];
    }
    sec[m++] = (sr <= 0.0) ? 0.0
             : 10.0*log10((sd > 0.0 ? sd : 1e-300)/sr);
  }
  if(m == 0) return 0.0;
  qsort(sec, (size_t)m, sizeof(double), cmpd);
  return sec[m/2];
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
  static double noise = 1.0;
  { const char *e = getenv("NR_NOISE"); if(e) noise = atof(e); }
  make_signal(0.001, noise);

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
    double d = median_second_residual(outbuf, saved, NS);
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
    d = median_second_residual(outbuf, saved, NS);
    snprintf(name, sizeof name, "%s: 1024 -> 5120 is the same audio", nrname(nr));
    check(name, d < -12.0, "residual %.1f dB", d);
    CloseChannel(CH);
  }

  /* ---- the depth control ---- */
  printf("\n-- NR3 depth (the wet/dry mix) --\n");
  channel_open(BLK_NARROW);
  {
    double snr[5], v[5];
    for(int k=0;k<=4;k++) {
      SetRXARNNRdepth(CH, k/4.0);
      RESULT r = measure(NR_RNN, BLK_NARROW);
      snr[k] = r.snr - ctlN[NR_OFF].snr;
      v[k] = 20.0*log10(r.voice / ctlN[NR_OFF].voice);
    }
    check("depth 0 reduces nothing", fabs(snr[0]) < 2.0, "%+.1f dB of SNR", snr[0]);
    check("depth 100 is the full reduction", snr[4] > 5.0, "%+.1f dB of SNR", snr[4]);
    /* Steeply nonlinear on purpose: a linear mix floors the noise at (1-depth),
     * so the reduction is 2.5 dB at a quarter and all of it only at the end.
     * Monotonic is the assertion; the shape is arithmetic. */
    check("and it is monotonic in between",
          snr[1] > snr[0] && snr[2] > snr[1] && snr[3] > snr[2] && snr[4] > snr[3],
          "%+.1f %+.1f %+.1f %+.1f %+.1f dB", snr[0], snr[1], snr[2], snr[3], snr[4]);
    (void)v;   /* the voice level across the sweep is not asserted here -- see
                * the note in the header on why a synthesised voice cannot test
                * that; on real speech it moved +0.9 dB from depth 0 to 100 */
    SetRXARNNRdepth(CH, 1.0);
  }
  CloseChannel(CH);

  /* the alignment, on the tone that makes a one-frame error a collapse */
  make_tone(0.01, 1050.0);
  channel_open(BLK_NARROW);
  {
    double lvl[5];
    for(int k=0;k<=4;k++) {
      SetRXARNNRdepth(CH, k/4.0);
      RESULT r = measure(NR_RNN, BLK_NARROW);
      lvl[k] = 20.0*log10(r.voice / 1e-30);
    }
    double worst = 0.0;
    for(int k=1;k<=4;k++) { double d = lvl[k]-lvl[0]; if(fabs(d) > fabs(worst)) worst = d; }
    check("dry and wet are frame-aligned (1050 Hz tone)", fabs(worst) < 1.0,
          "depth 0.25..1.00 within %+.2f dB of depth 0 (misaligned: -36.7)", worst);
    SetRXARNNRdepth(CH, 1.0);
  }
  CloseChannel(CH);
  make_signal(0.001, noise);   /* put the speech back for what follows */

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

  /* ---- the stream tap, across every block a span change produces.
   *
   * It is not gated on a subscriber: receiver_pretap_alloc installs the ring on
   * every receiver, so the tap's bandpass runs on every DSP pass at every span.
   * Its coefficient count did not follow dsp_size, and fircore answers nc < size
   * with nfor = 0 -- a zero-length plan array, then a NULL fftw plan inside
   * xfircore.  That is a segmentation fault on the DSP thread the moment the
   * operator selects a span whose block is larger than the tap's nc (2048), so
   * the assertion below is in part the harness surviving at all. ---- */
  printf("\n-- the stream tap, across every span's block --\n");
  {
    static const struct { int block; const char *span; } tier[] = {
      { 5120, "192/384 kHz" }, { 2560, "768 kHz" },
      { 1280, "1536 kHz"    }, { 1024, "1920 kHz" },
    };
    const int NT = (int)(sizeof tier / sizeof tier[0]);
    channel_open(tier[0].block);
    tap_install(tier[0].block);
    for(int t=0;t<NT;t++) {
      if(t) { channel_resize(tier[t].block); tap_install(tier[t].block); }
      run_chain(NR_OFF, tier[t].block);
      /* Every whole pass of the input, and not one sample short: run_chain
       * collects until this count arrives (tap_collect), so what the tap holds
       * is no longer a race against the scheduler.  A block that
       * does not reach the tap at all reads 0 here, and a geometry that stops
       * partway reads a count that is not a multiple of the block. */
      const long want = (NS/tier[t].block)*(long)tier[t].block;
      double v, f;
      buf_levels(tapbuf, tap_n, &v, &f);
      char name[80];
      snprintf(name, sizeof name, "tap carries the audio at a %s span", tier[t].span);
      check(name, tap_n == want && (tap_n % tier[t].block) == 0
                  && isfinite(v) && v > 0.0,
            "block %d, %ld of %ld samples (%ld passes behind), voice %.4g",
            tier[t].block, tap_n, want, (want-tap_n)/tier[t].block, v);
    }

    /* The fork is BEFORE the noise reduction, which is the whole point of the
     * tap having a filter of its own: the operator's NR is theirs, and the
     * stream must be the signal.  Asserted both ways round -- the tap must not
     * move, and the audio must, or the pair proves nothing. */
    const int blk = tier[NT-1].block;
    static double tapref[NS];
    run_chain(NR_OFF, blk);
    long n_ref = tap_n;
    memcpy(tapref, tapbuf, sizeof(double)*(size_t)n_ref);
    save_out();
    run_chain(NR_RNN, blk);
    long lim = tap_n < n_ref ? tap_n : n_ref;
    double dtap = median_second_residual(tapbuf, tapref, lim);
    double daud = median_second_residual(outbuf, saved, NS);
    check("NR3 does not reach the tap", dtap < -100.0,
          "median second %.1f dB", dtap);
    check("negative control: it did reach the audio", daud > -20.0,
          "median second %.1f dB", daud);
    tap_remove();
    CloseChannel(CH);
  }

  printf("\n%s\n", failures ? "FAILED" : "all cases passed");
  return failures ? 1 : 0;
}
