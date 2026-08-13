// Offline HFDL harness: feed a 16-bit stereo I/Q WAV straight into the decoder
// with an explicit receiver centre and tuned-channel (cursor) frequency, and
// print every decoded message. No GTK, no audio, no radio.
//
//   hfdl_offline <iq.wav> <centre_hz> <cursor_hz> [rotate_hz] [conj] [out_rate]
//   hfdl_offline --selftest
//
// --selftest needs no recording: it runs every layer's own test (demod, FEC,
// framer, PDU, message, ARINC-622, MIAM, OHMA, CPDLC) the way the other three
// harnesses do, so the decode chain can be checked on a machine that has no
// HFDL capture at all.
//
// out_rate resamples the file the way the I/Q Player does (cubic, looping),
// so the app's 192 kHz path can be reproduced from a 62.5 kHz recording.
//
// The WAV's own sample rate is the I/Q rate. Frequencies are absolute Hz; the
// cursor is where the operator would have the CTUN/freetune cursor.
//
// rotate_hz shifts the recording UP by that many Hz before feeding it, which is
// the I/Q Player's "Frequency offset" with the opposite sign — it is how you
// reproduce a capture whose signal was moved onto the receiver centre.

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hfdl_decoder.h"

static int rd32(FILE *f, unsigned *v) { unsigned char b[4]; if (fread(b,1,4,f)!=4) return 0;
  *v = b[0] | (b[1]<<8) | (b[2]<<16) | ((unsigned)b[3]<<24); return 1; }
static int rd16(FILE *f, unsigned *v) { unsigned char b[2]; if (fread(b,1,2,f)!=2) return 0;
  *v = b[0] | (b[1]<<8); return 1; }

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--selftest")) {
    gboolean ok = hfdl_decoder_selftest();
    printf("selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
  }
  if (argc < 4) {
    fprintf(stderr, "usage: %s <iq.wav> <centre_hz> <cursor_hz> [rotate_hz] [conj] [out_rate]\n"
                    "       %s --selftest\n", argv[0], argv[0]);
    return 2;
  }
  const char *path = argv[1];
  long long centre = atoll(argv[2]);
  long long cursor = atoll(argv[3]);
  double rot_hz = (argc > 4) ? atof(argv[4]) : 0.0;
  int conj = (argc > 5) ? atoi(argv[5]) : 0;   // 1 = mirror the spectrum (Q = -Q)
  unsigned out_rate = (argc > 6) ? (unsigned)atoi(argv[6]) : 0;

  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return 1; }
  unsigned v; char id[5] = {0};
  if (fread(id,1,4,f)!=4 || memcmp(id,"RIFF",4)) { fprintf(stderr,"not RIFF\n"); return 1; }
  rd32(f,&v); fread(id,1,4,f);
  unsigned rate = 0, chans = 0, bits = 0; long data_len = 0;
  for (;;) {
    if (fread(id,1,4,f)!=4) { fprintf(stderr,"no data chunk\n"); return 1; }
    unsigned len; if (!rd32(f,&len)) return 1;
    if (!memcmp(id,"fmt ",4)) {
      unsigned fmt, ch, sr, br, ba, bps;
      rd16(f,&fmt); rd16(f,&ch); rd32(f,&sr); rd32(f,&br); rd16(f,&ba); rd16(f,&bps);
      chans = ch; rate = sr; bits = bps;
      if (len > 16) fseek(f, len-16, SEEK_CUR);
    } else if (!memcmp(id,"data",4)) { data_len = len; break; }
    else fseek(f, len + (len&1), SEEK_CUR);
  }
  if (chans != 2 || bits != 16) { fprintf(stderr,"need 16-bit stereo I/Q\n"); return 1; }
  long nframes = data_len / 4;
  printf("file: %u Hz, %ld frames (%.1f s)\n", rate, nframes, (double)nframes/rate);
  printf("centre %lld Hz, cursor %lld Hz (%+lld Hz)\n", centre, cursor, cursor-centre);

  hfdl_decoder_set_enabled(TRUE);

  if (out_rate != 0 && out_rate != rate) {
    // Same shape as fake_protocol.c: cubic interpolation of the file at
    // step = file_rate / out_rate, fed in blocks at out_rate.
    short *all = g_new(short, (size_t)nframes*2);
    if ((long)fread(all, sizeof(short)*2, nframes, f) != nframes) { fprintf(stderr,"short read\n"); return 1; }
    double step = (double)rate / (double)out_rate;
    long outn = (long)((double)nframes / step);
    printf("resampled to %u Hz -> %ld frames\n", out_rate, outn);
    const int OB = 4096;
    double *ob = g_new(double, OB*2);
    char m2[65536];
    double pos = 0.0; long produced = 0; int k = 0;
    while (produced < outn) {
      int want = (int)MIN((long)OB, outn-produced);
      for (int i = 0; i < want; i++) {
        long i0 = (long)pos; double t = pos - (double)i0;
        long im1 = i0-1; if (im1 < 0) im1 += nframes;
        long ip1 = i0+1; if (ip1 >= nframes) ip1 -= nframes;
        long ip2 = i0+2; if (ip2 >= nframes) ip2 -= nframes;
        double a,b,c,d,re,im;
        a=all[im1*2]/32768.0; b=all[i0*2]/32768.0; c=all[ip1*2]/32768.0; d=all[ip2*2]/32768.0;
        re = b + 0.5*t*(c-a + t*(2.0*a-5.0*b+4.0*c-d + t*(3.0*(b-c)+d-a)));
        a=all[im1*2+1]/32768.0; b=all[i0*2+1]/32768.0; c=all[ip1*2+1]/32768.0; d=all[ip2*2+1]/32768.0;
        im = b + 0.5*t*(c-a + t*(2.0*a-5.0*b+4.0*c-d + t*(3.0*(b-c)+d-a)));
        if (conj) im = -im;
        ob[2*i] = re; ob[2*i+1] = im;
        pos += step; if (pos >= (double)nframes) pos -= (double)nframes;
      }
      hfdl_decoder_add_iq_at(ob, want, (int)out_rate, centre, cursor);
      produced += want; k++;
      int n2 = hfdl_decoder_get_messages(m2, sizeof(m2));
      if (n2 > 0) fputs(m2, stdout);
    }
    int n2 = hfdl_decoder_get_messages(m2, sizeof(m2));
    if (n2 > 0) fputs(m2, stdout);
    printf("--- fed %ld frames at %u Hz in %d blocks\n", produced, out_rate, k);
    return 0;
  }

  const int BLK = 4096;
  short *raw = g_new(short, BLK*2);
  double *iq = g_new(double, BLK*2);
  char msg[65536];
  long done = 0; int blocks = 0;
  double ph = 0.0, dph = 2.0*G_PI*rot_hz/(double)rate;
  while (done < nframes) {
    int want = (int)MIN((long)BLK, nframes-done);
    if ((int)fread(raw, sizeof(short)*2, want, f) != want) break;
    for (int i = 0; i < want; i++) {
      double re = (double)raw[2*i] / 32768.0, im = (double)raw[2*i+1] / 32768.0;
      if (conj) im = -im;
      if (rot_hz != 0.0) {
        double c = cos(ph), s = sin(ph);
        double r2 = re*c - im*s, i2 = re*s + im*c;
        re = r2; im = i2;
        ph += dph;
        if (ph > 2.0*G_PI) ph -= 2.0*G_PI; else if (ph < -2.0*G_PI) ph += 2.0*G_PI;
      }
      iq[2*i] = re; iq[2*i+1] = im;
    }
    hfdl_decoder_add_iq_at(iq, want, (int)rate, centre, cursor);
    done += want; blocks++;
    int n = hfdl_decoder_get_messages(msg, sizeof(msg));
    if (n > 0) fputs(msg, stdout);
  }
  int n = hfdl_decoder_get_messages(msg, sizeof(msg));
  if (n > 0) fputs(msg, stdout);
  printf("--- fed %ld frames in %d blocks\n", done, blocks);
  return 0;
}
