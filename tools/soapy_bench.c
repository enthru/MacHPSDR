/* soapy_bench -- does the LINK carry the stream?  A receiver, with no receiver.
 *
 * A networked PlutoSDR reaches the application through libiio's network
 * backend, which is strictly request/response: every iio_buffer_refill() writes
 * "READBUF <dev> <len>" and then waits for the whole reply.  Nothing is draining
 * the device while that request is in flight, so a stall on the link is not late
 * data -- it is samples the Pluto's DMA ring dropped, spliced into the stream
 * with no error anywhere.  SoapyPlutoSDR's readStreamStatus() is
 * SOAPY_SDR_NOT_SUPPORTED, so the application is never told; the operator sees a
 * stuttering waterfall and a chopped signal and blames the receiver.
 *
 * This reads the device the way soapy_protocol.c's receive thread does and
 * reports what arrived, so the link can be indicted or cleared before anything
 * in the app is touched.  Compare the same device over `usb:` and over `ip:`;
 * compare rates; compare bufflen.  It opens the radio, so it must not run while
 * the app has it.
 *
 *   make soapy-bench
 *   ./soapy_bench --uri ip:192.168.100.5 --rate 768000 --secs 10
 *   ./soapy_bench --uri usb: --rate 2304000
 *   ./soapy_bench --uri ip:192.168.100.5 --rate 768000 --bufflen 65536
 */
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Logger.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define MAX_GAPS 200000

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double now_ms(void) {
  struct timeval t;
  gettimeofday(&t, NULL);
  return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

static void usage(void) {
  printf("usage: soapy_bench [--driver plutosdr] [--uri ip:192.168.100.5] [--rate 768000]\n"
         "                   [--freq 100000000] [--secs 10] [--bufflen N] [--read N] [--verbose]\n");
}

int main(int argc, char **argv) {
  const char *driver = "plutosdr";
  const char *uri = NULL;
  double rate = 768000.0;
  double freq = 100000000.0;
  double secs = 10.0;
  long bufflen = 0;     /* 0 = let the driver size it from the rate */
  int readsize = 0;     /* 0 = the stream MTU, which is what the app asks for */
  int verbose = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
    else if (!strcmp(argv[i], "--uri") && i + 1 < argc) uri = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--freq") && i + 1 < argc) freq = atof(argv[++i]);
    else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atof(argv[++i]);
    else if (!strcmp(argv[i], "--bufflen") && i + 1 < argc) bufflen = atol(argv[++i]);
    else if (!strcmp(argv[i], "--read") && i + 1 < argc) readsize = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    else { usage(); return 1; }
  }
  if (secs <= 0.0 || rate <= 0.0) { usage(); return 1; }

  SoapySDR_setLogLevel(verbose ? SOAPY_SDR_INFO : SOAPY_SDR_WARNING);

  SoapySDRKwargs args = {0};
  SoapySDRKwargs_set(&args, "driver", driver);
  if (uri != NULL) SoapySDRKwargs_set(&args, "uri", uri);
  SoapySDRDevice *dev = SoapySDRDevice_make(&args);
  SoapySDRKwargs_clear(&args);
  if (dev == NULL) {
    printf("open failed (driver=%s uri=%s): %s\n", driver, uri ? uri : "(none)",
           SoapySDRDevice_lastError());
    return 1;
  }

  SoapySDRDevice_setSampleRate(dev, SOAPY_SDR_RX, 0, rate);
  SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, freq, NULL);
  /* The rate the DEVICE ended up at, never the one that was asked for: a driver
     that substitutes owes no error, and every figure below is a fraction of
     this one.  Same rule as soapy_set_rx_rate() in the app. */
  const double actual = SoapySDRDevice_getSampleRate(dev, SOAPY_SDR_RX, 0);

  SoapySDRKwargs sargs = {0};
  if (bufflen > 0) {
    char v[32];
    snprintf(v, sizeof(v), "%ld", bufflen);
    SoapySDRKwargs_set(&sargs, "bufflen", v);
  }
  size_t chan = 0;
  SoapySDRStream *st = NULL;
#ifdef SOAPY_SDR_API_HAS_STREAM_HANDLE_ARG
  if (SoapySDRDevice_setupStream(dev, &st, SOAPY_SDR_RX, SOAPY_SDR_CF32, &chan, 1, &sargs) != 0) st = NULL;
#else
  st = SoapySDRDevice_setupStream(dev, SOAPY_SDR_RX, SOAPY_SDR_CF32, &chan, 1, &sargs);
#endif
  SoapySDRKwargs_clear(&sargs);
  if (st == NULL) {
    printf("setupStream failed: %s\n", SoapySDRDevice_lastError());
    SoapySDRDevice_unmake(dev);
    return 1;
  }

  const int mtu = (int)SoapySDRDevice_getStreamMTU(dev, st);
  int block = readsize > 0 ? readsize : (mtu > 0 ? mtu : 16384);
  float *buf = malloc((size_t)block * 2 * sizeof(float));
  void *buffs[1] = {buf};
  double *gaps = malloc(MAX_GAPS * sizeof(double));
  if (buf == NULL || gaps == NULL) { printf("out of memory\n"); return 1; }

  printf("driver=%s uri=%s\n", driver, uri ? uri : "(auto)");
  printf("rate: asked %.0f, device runs %.0f%s\n", rate, actual,
         fabs(actual - rate) > 1.0 ? "   <-- SUBSTITUTED" : "");
  printf("stream MTU %d, reading %d samples per call, %.0f s\n\n", mtu, block, secs);

  SoapySDRDevice_activateStream(dev, st, 0, 0, 0);

  long long total = 0;
  int overruns = 0, timeouts = 0, errors = 0, ngaps = 0;
  int flags;
  long long timeNs;
  const double t0 = now_ms();
  double tprev = t0;
  /* Same timeout the app's receive thread uses, so a stall shows up here the
     way it shows up there. */
  const long timeoutUs = 100000L;

  for (;;) {
    flags = 0;
    int n = SoapySDRDevice_readStream(dev, st, buffs, block, &flags, &timeNs, timeoutUs);
    const double t = now_ms();
    if (ngaps < MAX_GAPS) gaps[ngaps++] = t - tprev;
    tprev = t;
    if (n > 0) total += n;
    else if (n == SOAPY_SDR_TIMEOUT) timeouts++;
    else if (n == SOAPY_SDR_OVERFLOW) overruns++;
    else if (n < 0) errors++;
    if (flags & SOAPY_SDR_END_ABRUPT) overruns++;
    if ((t - t0) / 1000.0 >= secs) break;
  }

  const double elapsed = (now_ms() - t0) / 1000.0;
  SoapySDRDevice_deactivateStream(dev, st, 0, 0);
  SoapySDRDevice_closeStream(dev, st);
  SoapySDRDevice_unmake(dev);

  const double got = total / elapsed;
  /* A read that returns a full block back-to-back has a ~0 ms gap; what matters
     is the tail, because one 100 ms hole is a hole in the I/Q whether or not the
     average looks healthy. */
  qsort(gaps, ngaps, sizeof(double), cmp_double);
  const double nominal_ms = 1000.0 * (double)block / (actual > 0 ? actual : rate);
  int long_gaps = 0;
  for (int i = 0; i < ngaps; i++) if (gaps[i] > 2.0 * nominal_ms) long_gaps++;

  printf("delivered %lld samples in %.1f s = %.0f/s, %.1f%% of %.0f\n",
         total, elapsed, got, actual > 0 ? 100.0 * got / actual : 0.0, actual);
  printf("missing   %.0f samples/s (%.1f ms of signal per second)\n",
         actual - got, actual > 0 ? 1000.0 * (actual - got) / actual : 0.0);
  printf("reads     %d, nominal %.1f ms apart\n", ngaps, nominal_ms);
  if (ngaps > 0) {
    printf("gaps      p50 %.1f ms, p95 %.1f ms, max %.1f ms; %d over 2x nominal (%.1f%%)\n",
           gaps[ngaps / 2], gaps[(int)(ngaps * 0.95)], gaps[ngaps - 1],
           long_gaps, 100.0 * long_gaps / ngaps);
  }
  printf("overruns %d, timeouts %d, errors %d\n", overruns, timeouts, errors);
  printf("\nwire traffic at this rate: %.1f Mbit/s (complex 16-bit)\n",
         actual * 4.0 * 8.0 / 1e6);
  if (got < 0.99 * actual) {
    printf("VERDICT: the stream is NOT arriving whole.  Missing samples are not\n"
           "         silence -- the samples either side are spliced, which smears\n"
           "         every signal on the band.\n");
  } else {
    printf("VERDICT: the link carries this stream.\n");
  }
  free(buf);
  free(gaps);
  return 0;
}
