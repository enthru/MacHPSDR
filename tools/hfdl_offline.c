// Offline HFDL harness: feed a 16-bit stereo I/Q WAV straight into the decoder
// with an explicit receiver centre and tuned-channel (cursor) frequency, and
// print every decoded message. No GTK, no audio, no radio.
//
//   hfdl_offline <iq.wav> <centre_hz> <cursor_hz> [rotate_hz]
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
  if (argc < 4) { fprintf(stderr, "usage: %s <iq.wav> <centre_hz> <cursor_hz>\n", argv[0]); return 2; }
  const char *path = argv[1];
  long long centre = atoll(argv[2]);
  long long cursor = atoll(argv[3]);
  double rot_hz = (argc > 4) ? atof(argv[4]) : 0.0;

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
