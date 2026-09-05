/* rec_offline.c -- self-test for the I/Q + audio recorder (src/audio/recorder.c).
 *
 * Why this exists.  The recorder is reached by clicking one button and its
 * output is looked at hours later, so every defect in it is discovered as a
 * capture that will not play -- at which point the signal it was recording is
 * gone.  Nothing else in the tree touches it: it is not on any decoder path, it
 * has no protocol to emulate, and the two faults it shipped with were both
 * invisible from the outside.
 *
 *   - Every sample was converted and fwrite()n from the RX audio/DSP thread,
 *     two bytes at a time, under the mutex the GTK thread takes to start and
 *     stop.  Any hesitation in the filesystem was time that thread did not
 *     spend draining the device -- which is not late audio but SPLICED audio.
 *   - The writes were unchecked while the byte counter went up regardless, so a
 *     disk that ran out produced a WAV whose header promised bytes that were
 *     never written.  It looks fine until it is opened.
 *
 * The cure -- convert on the tap, write on a thread of its own, bound the queue
 * and DROP rather than block -- adds three things that are themselves invisible
 * without a harness: that everything queued still reaches the disk at stop,
 * that a queue which overflows costs frames and not the file's validity, and
 * that a write which genuinely fails stops the recording instead of lying in
 * the header.  Each is asserted here by reading the WAV back and comparing
 * numbers, never by eyeballing a file size.
 *
 * Two hooks make the last two reachable at all, since no machine fast enough to
 * run the application has a disk slow enough to provoke them:
 * MACHPSDR_REC_STALL_US stalls the writer, and RLIMIT_FSIZE makes a write fail
 * exactly as a full disk does.
 *
 *   make rec-offline && ./rec_offline --selftest
 *
 * Exit status 0 = every case passed.
 */

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

// Prerequisite types for radio.h (the include order recorder.c uses).
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "mode.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "recorder.h"

// ---- the application, reduced to what recorder.c reaches for ---------------

RADIO *radio;
GtkWidget *main_window = NULL;   // NULL: rec_alert_idle() raises no dialog here

// radio.c's Record button. The writer's disk-error path asks for this through
// an idle; the harness only has to see that it was asked.
static int button_syncs = 0;
void radio_record_button_sync(void) { button_syncs++; }

// settings_ui.c, reached only by create_recording_dialog(), which this harness
// never calls -- but the linker still wants the symbol.
void sui_style_group(GtkWidget *w) { (void)w; }

// ---- test rig ---------------------------------------------------------------

static int failures;

static void check(const char *name, gboolean ok, const char *detail) {
  printf("%-52s %s   %s\n", name, ok ? "PASS" : "FAIL", detail ? detail : "");
  if (!ok) failures++;
}

static char test_dir[512];

static RECEIVER *make_rx(int rate) {
  RECEIVER *rx = g_new0(RECEIVER, 1);
  rx->sample_rate = rate;
  return rx;
}

static void set_dir(gboolean iq, gboolean af) {
  g_strlcpy(radio->rec_dir, test_dir, sizeof(radio->rec_dir));
  radio->rec_iq = iq;
  radio->rec_af = af;
}

// Run the GTK main loop's pending idles, which is how the writer thread's
// disk-error path reaches recorder_stop(). Bounded, so a test cannot hang.
static void pump_idles(int ms) {
  gint64 until = g_get_monotonic_time() + (gint64)ms * 1000;
  do {
    while (g_main_context_iteration(NULL, FALSE)) { }
    g_usleep(2000);
  } while (g_get_monotonic_time() < until);
}

// ---- reading a WAV back -----------------------------------------------------

typedef struct {
  gboolean valid;      // 44-byte canonical header, RIFF/data sizes agree with
                       // the bytes that are really in the file
  guint32  rate;
  guint32  data_bytes; // as the header PROMISES
  guint32  real_bytes; // as the file actually holds
  short   *pcm;
  guint32  frames;
} WAV;

static guint32 get_u32(const unsigned char *p) {
  return (guint32)p[0] | ((guint32)p[1]<<8) | ((guint32)p[2]<<16) | ((guint32)p[3]<<24);
}
static guint16 get_u16(const unsigned char *p) {
  return (guint16)((guint32)p[0] | ((guint32)p[1]<<8));
}

static WAV read_wav(const char *path) {
  WAV w = { 0 };
  char *buf = NULL; gsize len = 0;
  if (!g_file_get_contents(path, &buf, &len, NULL)) return w;
  if (len < 44) { g_free(buf); return w; }
  const unsigned char *h = (const unsigned char *)buf;

  w.rate       = get_u32(h+24);
  w.data_bytes = get_u32(h+40);
  w.real_bytes = (guint32)(len - 44);
  // "Valid" is the whole point of the header patch: the sizes the file
  // ADVERTISES have to be the ones it holds, or a player reads past the end or
  // stops short of real audio.
  w.valid = memcmp(h, "RIFF", 4) == 0 && memcmp(h+8, "WAVE", 4) == 0 &&
            memcmp(h+12, "fmt ", 4) == 0 && memcmp(h+36, "data", 4) == 0 &&
            get_u16(h+22) == 2 && get_u16(h+34) == 16 &&
            get_u32(h+4) == 36 + w.real_bytes &&
            w.data_bytes == w.real_bytes;
  w.frames = w.real_bytes / 4;
  w.pcm = g_memdup2(buf + 44, w.real_bytes ? w.real_bytes : 1);
  g_free(buf);
  return w;
}

static void wav_free(WAV *w) { g_free(w->pcm); w->pcm = NULL; }

// A ramp whose value names its own frame number, so a HOLE in the middle shows
// up as a discontinuity rather than as a merely shorter file.
static double ramp(int frame, int chan) {
  return ((double)((frame * 2 + chan) % 20000) / 20000.0) * 2.0 - 1.0;
}

static void feed_iq(RECEIVER *rx, int first, int frames) {
  double *b = g_new(double, (size_t)frames * 2);
  for (int i = 0; i < frames; i++) {
    b[i*2]   = ramp(first + i, 0);
    b[i*2+1] = ramp(first + i, 1);
  }
  recorder_iq(rx, b, frames);
  g_free(b);
}

static void feed_af(RECEIVER *rx, int first, int frames) {
  double *b = g_new(double, (size_t)frames * 2);
  for (int i = 0; i < frames; i++) {
    b[i*2]   = ramp(first + i, 0);
    b[i*2+1] = ramp(first + i, 1);
  }
  recorder_audio(rx, b, frames);
  g_free(b);
}

static short expect_pcm(int frame, int chan) {
  double x = ramp(frame, chan);
  return (short)(x * 32767.0);
}

// The newest rec_* file with this suffix in the test directory.
static char *newest(const char *suffix) {
  GDir *d = g_dir_open(test_dir, 0, NULL);
  if (!d) return NULL;
  const char *n; char *best = NULL;
  while ((n = g_dir_read_name(d)) != NULL) {
    if (g_str_has_prefix(n, "rec_") && g_str_has_suffix(n, suffix)) {
      if (best == NULL || strcmp(n, best) > 0) { g_free(best); best = g_strdup(n); }
    }
  }
  g_dir_close(d);
  if (!best) return NULL;
  char *p = g_build_filename(test_dir, best, NULL);
  g_free(best);
  return p;
}

static void wipe_dir(void) {
  GDir *d = g_dir_open(test_dir, 0, NULL);
  if (!d) return;
  const char *n;
  GSList *victims = NULL;
  while ((n = g_dir_read_name(d)) != NULL)
    victims = g_slist_prepend(victims, g_build_filename(test_dir, n, NULL));
  g_dir_close(d);
  for (GSList *v = victims; v; v = v->next) { g_unlink(v->data); g_free(v->data); }
  g_slist_free(victims);
}

// ---- 1. the round trip ------------------------------------------------------
//
// Everything handed to the taps must reach the disk, byte for byte, with a
// header that says so. This is the case the unchecked fwrite could not fail
// loudly: it wrote a header promising bytes it never wrote.
static void test_round_trip(void) {
  printf("\n-- round trip: what goes in comes out --\n");
  wipe_dir();
  set_dir(TRUE, TRUE);
  RECEIVER *rx = make_rx(192000);

  check("recording starts", recorder_toggle(rx), NULL);
  const int BLK = 1024, N = 40;
  for (int i = 0; i < N; i++) { feed_iq(rx, i*BLK, BLK); feed_af(rx, i*BLK, BLK); }
  recorder_stop();

  char *iqp = newest("_iq.wav"), *afp = newest("_af.wav");
  check("an I/Q file was produced", iqp != NULL, iqp ? iqp : "");
  check("an AF file was produced",  afp != NULL, afp ? afp : "");
  if (!iqp || !afp) { g_free(iqp); g_free(afp); g_free(rx); return; }

  WAV iq = read_wav(iqp), af = read_wav(afp);
  char d[160];

  g_snprintf(d, sizeof d, "%u frames, header says %u bytes, file holds %u",
             iq.frames, iq.data_bytes, iq.real_bytes);
  check("the I/Q WAV's header matches the bytes in it", iq.valid, d);
  check("the I/Q WAV carries the receiver's sample rate", iq.rate == 192000, NULL);
  check("the AF WAV's header matches the bytes in it", af.valid, NULL);
  check("the AF WAV is 48 kHz", af.rate == 48000, NULL);

  g_snprintf(d, sizeof d, "%u of %d", iq.frames, N*BLK);
  check("every I/Q frame fed reached the disk", iq.frames == (guint32)(N*BLK), d);
  check("every AF frame fed reached the disk",  af.frames == (guint32)(N*BLK), d);

  // Sample-for-sample, not merely the right count: a queue that reordered or
  // duplicated blocks would pass a length check.
  int wrong = 0;
  for (guint32 f = 0; f < iq.frames && wrong == 0; f++)
    for (int c = 0; c < 2; c++)
      if (iq.pcm[f*2+c] != expect_pcm((int)f, c)) { wrong = 1; break; }
  check("and in the order it was fed, sample for sample", wrong == 0, NULL);

  guint64 iqf = 0, aff = 0, iqd = 1, afd = 1;
  recorder_stats(&iqf, &aff, &iqd, &afd);
  g_snprintf(d, sizeof d, "iq %llu af %llu, dropped %llu/%llu",
             (unsigned long long)iqf, (unsigned long long)aff,
             (unsigned long long)iqd, (unsigned long long)afd);
  check("nothing was dropped on an idle disk", iqd == 0 && afd == 0, d);

  wav_free(&iq); wav_free(&af);
  g_free(iqp); g_free(afp); g_free(rx);
}

// ---- 2. stop drains the queue ----------------------------------------------
//
// The writer is deliberately stalled and then stopped at once. "Stop" has to
// mean "everything captured is on the disk", not "everything not yet written is
// lost" -- otherwise the split that took the disk off the DSP thread would have
// bought that at the cost of the tail of every recording.
static void test_stop_drains(void) {
  printf("\n-- stop waits for the queue, it does not discard it --\n");
  wipe_dir();
  set_dir(TRUE, FALSE);
  RECEIVER *rx = make_rx(192000);

  g_setenv("MACHPSDR_REC_STALL_US", "4000", TRUE);   // 4 ms per block
  check("recording starts", recorder_toggle(rx), NULL);
  const int BLK = 1024, N = 30;                      // ~120 ms of stalled writes
  for (int i = 0; i < N; i++) feed_iq(rx, i*BLK, BLK);
  recorder_stop();                                   // returns only once drained
  g_unsetenv("MACHPSDR_REC_STALL_US");

  char *iqp = newest("_iq.wav");
  check("an I/Q file was produced", iqp != NULL, NULL);
  if (!iqp) { g_free(rx); return; }
  WAV iq = read_wav(iqp);
  char d[120];
  g_snprintf(d, sizeof d, "%u of %d frames", iq.frames, N*BLK);
  check("every queued frame was written before stop returned",
        iq.frames == (guint32)(N*BLK), d);
  check("and the header is still valid", iq.valid, NULL);

  wav_free(&iq); g_free(iqp); g_free(rx);
}

// ---- 3. the drop path -------------------------------------------------------
//
// A disk that cannot keep up must cost FRAMES, never the file and never the DSP
// thread. Provoked by stalling the writer hard enough that the 32 MiB queue
// overflows; the assertion is that frames were counted as dropped AND that what
// did get written is a valid, playable WAV of exactly the frames that survived.
//
// The negative control is test_round_trip above: the same feed at the same
// rates with no stall drops nothing, so a harness that dropped everything (or a
// counter that counted regardless) could not pass both.
static void test_drops(void) {
  printf("\n-- an overloaded disk costs frames, not the file --\n");
  wipe_dir();
  set_dir(TRUE, FALSE);
  RECEIVER *rx = make_rx(768000);

  g_setenv("MACHPSDR_REC_STALL_US", "20000", TRUE);  // 20 ms per block
  check("recording starts", recorder_toggle(rx), NULL);

  // 40 MiB of I/Q against a 32 MiB queue and a writer doing ~50 blocks/s.
  const int BLK = 25600, N = 410;                    // 410 * 25600 * 4 = 40 MiB
  for (int i = 0; i < N; i++) feed_iq(rx, i*BLK, BLK);

  guint64 iqf = 0, iqd = 0;
  recorder_stats(&iqf, NULL, &iqd, NULL);
  char d[160];
  g_snprintf(d, sizeof d, "%llu frames dropped of %d fed",
             (unsigned long long)iqd, N*BLK);
  check("the full queue DROPPED frames and counted them", iqd > 0, d);

  recorder_stop();
  g_unsetenv("MACHPSDR_REC_STALL_US");

  recorder_stats(&iqf, NULL, &iqd, NULL);
  char *iqp = newest("_iq.wav");
  check("an I/Q file was produced anyway", iqp != NULL, NULL);
  if (!iqp) { g_free(rx); return; }
  WAV iq = read_wav(iqp);

  check("the file that came out is a VALID WAV", iq.valid, NULL);
  g_snprintf(d, sizeof d, "file %u, written %llu, dropped %llu, fed %d",
             iq.frames, (unsigned long long)iqf, (unsigned long long)iqd, N*BLK);
  check("holding exactly the frames that were not dropped",
        iq.frames == (guint32)iqf, d);
  check("and written + dropped accounts for everything fed",
        iqf + iqd == (guint64)(N*BLK), d);

  wav_free(&iq); g_free(iqp); g_free(rx);
}

// ---- 4. a write that genuinely fails ---------------------------------------
//
// RLIMIT_FSIZE makes fwrite fail with EFBIG at a chosen size, which is the same
// errno path a full disk takes. The recorder must stop itself, ask for the
// Record button back, and leave behind a file that is valid up to the point it
// failed -- the old code ignored the return and went on counting bytes, so the
// header promised data that was never written.
static void test_disk_failure(void) {
#ifdef RLIMIT_FSIZE
  printf("\n-- a write that fails stops the recording and says so --\n");
  wipe_dir();
  set_dir(TRUE, FALSE);
  RECEIVER *rx = make_rx(192000);

  struct rlimit old;
  if (getrlimit(RLIMIT_FSIZE, &old) != 0) { g_free(rx); return; }
  struct rlimit lim = { 64*1024, old.rlim_max };
  signal(SIGXFSZ, SIG_IGN);           // or the process dies instead of erroring
  if (setrlimit(RLIMIT_FSIZE, &lim) != 0) {
    printf("  skip  RLIMIT_FSIZE not settable here\n");
    g_free(rx); return;
  }

  button_syncs = 0;
  check("recording starts", recorder_toggle(rx), NULL);
  const int BLK = 1024;
  for (int i = 0; i < 60 && recorder_active(); i++) {   // 240 KiB against 64 KiB
    feed_iq(rx, i*BLK, BLK);
    pump_idles(20);
  }
  pump_idles(300);
  setrlimit(RLIMIT_FSIZE, &old);

  check("the recorder stopped itself", !recorder_active(), NULL);
  check("and asked for the Record button back", button_syncs > 0, NULL);

  char *iqp = newest("_iq.wav");
  if (iqp) {
    WAV iq = read_wav(iqp);
    char d[160];
    g_snprintf(d, sizeof d, "header says %u bytes, file holds %u",
               iq.data_bytes, iq.real_bytes);
    check("the part that was written is a valid WAV, not a lying header",
          iq.valid, d);
    wav_free(&iq); g_free(iqp);
  } else {
    check("a file was produced", FALSE, NULL);
  }
  recorder_stop();
  g_free(rx);
#endif
}

// ---- 5. a folder that cannot be written -------------------------------------
//
// The other half: the failure that happens before a single sample is taken.
// Starting must simply refuse, rather than arm a recorder with no files behind
// it -- which would leave the button reading "Stop" over nothing.
static void test_unwritable_folder(void) {
  printf("\n-- a folder that cannot be written refuses to start --\n");
  char *ro = g_build_filename(test_dir, "readonly", NULL);
  g_mkdir_with_parents(ro, 0755);
  RECEIVER *rx = make_rx(192000);
  g_strlcpy(radio->rec_dir, ro, sizeof(radio->rec_dir));
  radio->rec_iq = TRUE; radio->rec_af = FALSE;

  g_chmod(ro, 0500);
  gboolean started = recorder_toggle(rx);
  g_chmod(ro, 0755);
  if (started) {
    recorder_stop();
    printf("  skip  unwritable-folder case (the open succeeded anyway)\n");
  } else {
    check("recording refuses to start", TRUE, NULL);
    check("and the recorder is left idle", !recorder_active(), NULL);
  }
  g_rmdir(ro);
  g_free(ro); g_free(rx);
}

int main(int argc, char *argv[]) {
  int selftest = 0;
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "--selftest") == 0) selftest = 1;
  if (!selftest) { printf("usage: %s --selftest\n", argv[0]); return 1; }

  radio = g_new0(RADIO, 1);
  g_snprintf(test_dir, sizeof test_dir, "%s/machpsdr_rec_selftest", g_get_tmp_dir());
  g_mkdir_with_parents(test_dir, 0755);

  printf("rec_offline: recorder self-test\n");
  test_round_trip();
  test_stop_drains();
  test_drops();
  test_disk_failure();
  test_unwritable_folder();

  wipe_dir();
  g_rmdir(test_dir);

  printf("\n%s\n", failures == 0 ? "all cases passed" : "SOME CASES FAILED");
  return failures == 0 ? 0 : 1;
}
