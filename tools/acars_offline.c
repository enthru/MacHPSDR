// Offline VHF ACARS harness.  No GTK, no audio, no radio — this is how the
// decoder is verified, never by starting the app (which raises its window over
// whatever the operator is doing and rewrites their saved settings on exit).
//
//   acars_offline --selftest
//       Synthesised frames only: the physical layer at several rates, channel
//       offsets, noise, inverted polarity and conjugated I/Q, plus a full-stack
//       pass that reads a real message back out of modulated I/Q.
//
//   acars_offline <iq.wav> <centre_hz> <cursor_hz> [conj]
//       16-bit stereo I/Q, the WAV's own rate.  Frequencies are absolute Hz;
//       the cursor is where the operator would have the CTUN/freetune cursor.
//
//   acars_offline --audio <file.wav> [channel]
//       Already AM-demodulated audio (mono or multi-channel), the form ACARS
//       recordings circulate in.  `channel` picks one of a multi-channel file.
//
//   acars_offline --am <file.wav> <channel> <rate> <offset_hz>
//       Take one channel of an AM-demodulated recording, AM-modulate it back
//       onto a carrier `offset_hz` from centre at `rate`, and run it through the
//       whole I/Q path.  This is the only way to exercise the front end against
//       REAL modulation content rather than our own modulator's.

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "acars_decoder.h"

typedef struct {
  unsigned rate, chans, bits;
  long     frames;
  short   *data;         // interleaved
} WAVE;

static int rd32(FILE *f, unsigned *v) { unsigned char b[4]; if (fread(b,1,4,f)!=4) return 0;
  *v = b[0] | (b[1]<<8) | (b[2]<<16) | ((unsigned)b[3]<<24); return 1; }
static int rd16(FILE *f, unsigned *v) { unsigned char b[2]; if (fread(b,1,2,f)!=2) return 0;
  *v = b[0] | (b[1]<<8); return 1; }

static int wav_read(const char *path, WAVE *w) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return 0; }
  char id[5] = {0}; unsigned v;
  if (fread(id,1,4,f)!=4 || memcmp(id,"RIFF",4)) { fprintf(stderr,"not RIFF\n"); fclose(f); return 0; }
  rd32(f,&v); if (fread(id,1,4,f)!=4) { fclose(f); return 0; }
  unsigned long data_len = 0;
  memset(w, 0, sizeof(*w));
  for (;;) {
    if (fread(id,1,4,f)!=4) { fprintf(stderr,"no data chunk\n"); fclose(f); return 0; }
    unsigned len; if (!rd32(f,&len)) { fclose(f); return 0; }
    if (!memcmp(id,"fmt ",4)) {
      unsigned fmt, ch, sr, br, ba, bps;
      rd16(f,&fmt); rd16(f,&ch); rd32(f,&sr); rd32(f,&br); rd16(f,&ba); rd16(f,&bps);
      w->chans = ch; w->rate = sr; w->bits = bps;
      if (len > 16) fseek(f, len-16, SEEK_CUR);
    } else if (!memcmp(id,"data",4)) { data_len = len; break; }
    else fseek(f, len + (len&1), SEEK_CUR);
  }
  if (w->bits != 16 || w->chans == 0) {
    fprintf(stderr, "need 16-bit PCM (got %u-bit, %u channels)\n", w->bits, w->chans);
    fclose(f); return 0;
  }
  w->frames = (long)(data_len / (2 * w->chans));
  w->data = g_new(short, (size_t)w->frames * w->chans);
  if ((long)fread(w->data, 2 * w->chans, (size_t)w->frames, f) != w->frames)
    fprintf(stderr, "warning: short read\n");
  fclose(f);
  return 1;
}

static void drain(void) {
  char msg[65536];
  int n = acars_decoder_get_messages(msg, sizeof(msg));
  if (n > 0) fputs(msg, stdout);
}

static void report(void) {
  drain();
  printf("--- %ld message(s), %ld failed CRC\n",
         acars_decoder_get_messages_count(), acars_decoder_get_bad_count());
  ACARS_AC_INFO ac[64];
  int n = acars_decoder_ac_list(ac, 64);
  if (n > 0) {
    printf("--- aircraft heard\n");
    for (int i = 0; i < n; i++) {
      char chan[16];
      if (ac[i].khz) g_snprintf(chan, sizeof(chan), "%u.%03u MHz",
                                ac[i].khz / 1000, ac[i].khz % 1000);
      else           g_strlcpy(chan, "-", sizeof(chan));   // the audio path has no channel
      printf("    %-8s %-8s %-3s %-11s %d msg\n", ac[i].reg, ac[i].flight,
             ac[i].label, chan, ac[i].msgs);
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage: %s --selftest\n"
      "       %s <iq.wav> <centre_hz> <cursor_hz> [conj]\n"
      "       %s --audio <file.wav> [channel]\n"
      "       %s --am <file.wav> <channel> <rate> <offset_hz>\n",
      argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }

  if (!strcmp(argv[1], "--selftest")) {
    gboolean ok = acars_decoder_selftest();
    printf("selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
  }

  acars_decoder_set_enabled(TRUE);

  if (!strcmp(argv[1], "--audio")) {
    if (argc < 3) { fprintf(stderr, "--audio needs a file\n"); return 2; }
    WAVE w;
    if (!wav_read(argv[2], &w)) return 1;
    int first = (argc > 3) ? atoi(argv[3]) : -1;      // -1 = every channel
    printf("file: %u Hz, %u channel(s), %ld frames (%.1f s)\n",
           w.rate, w.chans, w.frames, (double)w.frames / w.rate);
    double *x = g_new(double, (size_t)w.frames);
    for (unsigned c = 0; c < w.chans; c++) {
      if (first >= 0 && (int)c != first) continue;
      for (long i = 0; i < w.frames; i++) x[i] = w.data[i * w.chans + c] / 32768.0;
      if (w.chans > 1) printf("--- channel %u\n", c);
      // A fresh decoder state per channel: they are different radios, and the
      // counters/aircraft table should not accumulate across them.
      acars_decoder_set_enabled(FALSE);
      acars_decoder_set_enabled(TRUE);
      acars_decoder_reset();
      const long BLK = 4096;
      for (long o = 0; o < w.frames; o += BLK) {
        long want = MIN(BLK, w.frames - o);
        acars_decoder_add_audio(x + o, (int)want, 1, (double)w.rate);
        drain();
      }
      report();
    }
    return 0;
  }

  if (!strcmp(argv[1], "--am")) {
    if (argc < 6) { fprintf(stderr, "--am needs <file> <channel> <rate> <offset>\n"); return 2; }
    WAVE w;
    if (!wav_read(argv[2], &w)) return 1;
    unsigned ch  = (unsigned)atoi(argv[3]);
    double   rate = atof(argv[4]);
    double   off  = atof(argv[5]);
    if (ch >= w.chans) { fprintf(stderr, "no channel %u\n", ch); return 1; }
    // Resample the envelope to the target I/Q rate (linear is plenty: the
    // envelope's content stops at 2400 Hz and the target rate is far above it).
    double step = (double)w.rate / rate;
    long   ns   = (long)(w.frames / step);
    printf("AM-remodulating channel %u of %s: %u Hz -> %.0f Hz I/Q, carrier %+.0f Hz\n",
           ch, argv[2], w.rate, rate, off);
    const long BLK = 4096;
    double *iq = g_new(double, (size_t)BLK * 2);
    double ph = 0.0, dph = 2.0 * M_PI * off / rate;
    long done = 0;
    while (done < ns) {
      long want = MIN(BLK, ns - done);
      for (long i = 0; i < want; i++) {
        double pos = (double)(done + i) * step;
        long   i0  = (long)pos;
        double t   = pos - i0;
        long   i1  = (i0 + 1 < w.frames) ? i0 + 1 : i0;
        double e   = (w.data[i0 * w.chans + ch] * (1.0 - t) +
                      w.data[i1 * w.chans + ch] * t) / 32768.0;
        double a = 0.5 + 0.4 * e;
        iq[2 * i]     = a * sin(ph);     // (Q, I) — the receiver's buffer order
        iq[2 * i + 1] = a * cos(ph);
        ph += dph; if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
      }
      acars_decoder_add_iq(iq, (int)want, (int)rate, 131550000LL,
                           131550000LL + (long long)off);
      drain();
      done += want;
    }
    report();
    return 0;
  }

  if (argc < 4) { fprintf(stderr, "need <iq.wav> <centre_hz> <cursor_hz>\n"); return 2; }
  WAVE w;
  if (!wav_read(argv[1], &w)) return 1;
  if (w.chans != 2) { fprintf(stderr, "need 16-bit stereo I/Q\n"); return 1; }
  long long centre = atoll(argv[2]);
  long long cursor = atoll(argv[3]);
  int conj = (argc > 4) ? atoi(argv[4]) : 0;
  printf("file: %u Hz, %ld frames (%.1f s)\n", w.rate, w.frames, (double)w.frames / w.rate);
  printf("centre %lld Hz, cursor %lld Hz (%+lld Hz)%s\n", centre, cursor, cursor - centre,
         conj ? ", conjugated" : "");
  const long BLK = 4096;
  double *iq = g_new(double, (size_t)BLK * 2);
  long done = 0;
  while (done < w.frames) {
    long want = MIN(BLK, w.frames - done);
    for (long i = 0; i < want; i++) {
      double q = w.data[(done + i) * 2]     / 32768.0;
      double I = w.data[(done + i) * 2 + 1] / 32768.0;
      if (conj) q = -q;
      iq[2 * i]     = q;
      iq[2 * i + 1] = I;
    }
    acars_decoder_add_iq(iq, (int)want, (int)w.rate, centre, cursor);
    drain();
    done += want;
  }
  report();
  return 0;
}
