/* recorder.c -- see recorder.h.
 *
 * A minimal streaming RIFF/WAVE writer: the 44-byte header is written up front
 * with placeholder sizes and patched with the real byte counts on stop. Both
 * files are 16-bit little-endian PCM, 2 channels. The I/Q file mirrors the
 * format the --faker replay path expects (fake_protocol.c), so a capture can be
 * looped straight back through the RX/decoder chain.
 *
 * WAV size fields are 32-bit, so a single file caps at 4 GiB (~46 min of
 * 384 kHz I/Q). Rather than switch to RF64 (which the --faker replay path and
 * most players can't read), the writer auto-splits: when a stream nears the
 * limit its file is closed with a valid header and a numbered continuation
 * segment (rec_<stamp>_iq_002.wav, ...) is opened. Every segment is a standard,
 * replayable WAV.
 *
 * THE DISK IS NOT ON THE DSP THREAD. Every sample used to be converted and
 * fwrite()n from the RX audio/DSP thread, one two-byte pair per fwrite, under
 * the same mutex the GTK thread takes to start and stop -- so any hesitation in
 * the filesystem was time that thread did not spend draining the device, which
 * is not late audio but SPLICED audio and a stalled waterfall. A spinning disk,
 * a network home, an encrypted volume or another process' flush is enough. The
 * writes were also unchecked while the byte counter went up regardless, so a
 * full disk produced a WAV whose header promised bytes that were never written
 * -- a recording that looks fine until it is opened.
 *
 * So the taps only CONVERT (the one genuinely per-sample piece of work) and
 * hand a block to a bounded queue; a writer thread owns both FILE*s and does
 * every open, write, split and close. The queue is bounded because a producer
 * that waits for the disk is the bug this removes: when it is full the block is
 * DROPPED and counted, and the operator is told -- in the log every five
 * seconds, and once in a dialog -- rather than the recording quietly coming out
 * short. A write that actually fails stops the recording and says so.
 */
#include <stdio.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>          /* _chsize_s, _fileno */
#else
#include <unistd.h>      /* ftruncate, fileno */
#endif
#include "log.h"
#include <string.h>
#include "time_compat.h"   // <time.h> + gmtime_r() on Windows
#include <gtk/gtk.h>

#include "recorder.h"
/* radio.h needs these types declared first; main.h provides the global `radio`. */
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "ext.h"
#include "main.h"
#include "settings_ui.h"

static GMutex rec_mutex;
static GCond  rec_cond;              /* queue not empty, or the writer must stop */
static RECEIVER *rec_rx = NULL;      /* receiver being recorded, NULL when idle */

/* --- owned by the WRITER THREAD between start and join, by nobody else ----- */
static FILE *rec_iq = NULL;
static FILE *rec_af = NULL;
static guint32 iq_bytes = 0;         /* PCM data bytes in the current segment */
static guint32 af_bytes = 0;
static char    rec_base[600] = "";   /* "<dir>/rec_<stamp>"; segment names built from this */
static char    iq_path[700] = "";    /* the segment each stream is writing NOW */
static char    af_path[700] = "";
static guint32 rec_iq_rate = 0;      /* I/Q sample rate, remembered for continuation segments */
static int     iq_seg = 1;           /* current segment number (1 = first, unnumbered file) */
static int     af_seg = 1;

/* --- the queue between the taps and the writer, all under rec_mutex -------- */
static GQueue   *rec_q = NULL;
static gsize     rec_q_bytes = 0;    /* PCM bytes queued, against REC_QUEUE_MAX_BYTES */
static gsize     rec_q_peak  = 0;    /* high-water mark, reported at stop */
static GThread  *rec_writer = NULL;
static gboolean  rec_writer_run = FALSE;
static guint64   iq_dropped = 0;     /* frames the queue had no room for */
static guint64   af_dropped = 0;
static guint64   iq_written = 0;     /* frames that reached a file */
static guint64   af_written = 0;
static gint64    last_drop_report = 0;   /* monotonic us, for the 5 s log line */
static gboolean  drop_warned = FALSE;    /* the one-shot "gaps" dialog has been raised */

/* Test hook, read once per recording from MACHPSDR_REC_STALL_US: microseconds
 * the writer sleeps per block. Zero -- the default, and one getenv per Record
 * click -- is the ordinary path untouched. It exists because the drop path is
 * otherwise UNREACHABLE on any machine fast enough to run the application, and
 * a path that has never been executed is not a path that works. */
static guint32   rec_stall_us = 0;

/* Split each WAV before its 32-bit RIFF/data size field can overflow (2^32).
 * ~3.75 GiB leaves a wide margin over any single transfer block and keeps every
 * segment a standard, --faker-replayable WAV. Multiple of the 4-byte block. */
#define REC_SPLIT_BYTES 0xF0000000u

/* How much stream the queue may hold before it starts dropping. This is the
 * only number that decides how long a disk may stall without costing the
 * operator any recording, and it is spent in RAM, so it is stated in TIME at
 * the rates this is actually used at: 32 MiB is 21 s of 384 kHz I/Q, 10.6 s of
 * 768 kHz, 174 s of 48 kHz AF -- and 0.9 s at the widest 9.6 MHz span, where
 * the stream is 38 MB/s and no ordinary disk sustains it in the first place.
 * Bigger is not better: it only buys a longer stall before the same drop, at
 * the cost of that much more to write out at stop. */
#define REC_QUEUE_MAX_BYTES (32u*1024u*1024u)

/* One converted block on its way to the disk. The taps do the int16 conversion
 * (per sample, and the only part worth keeping off the writer) and the writer
 * does one fwrite of the lot. */
typedef enum { REC_STREAM_IQ = 0, REC_STREAM_AF = 1 } REC_STREAM;
typedef struct {
  REC_STREAM stream;
  guint32    frames;                 /* stereo frames; PCM bytes = frames*4 */
  short      pcm[];                  /* 2*frames interleaved int16 */
} REC_BLOCK;

static gpointer rec_writer_thread(gpointer data);

static void put_u32(unsigned char *p, guint32 v) {
  p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}
static void put_u16(unsigned char *p, guint16 v) {
  p[0]=v&0xff; p[1]=(v>>8)&0xff;
}

/* Write a 44-byte canonical WAV header with data size 0 (patched on close). */
static void write_wav_header(FILE *f, guint32 rate) {
  unsigned char h[44];
  const guint16 channels=2, bits=16;
  const guint32 byte_rate=rate*channels*(bits/8);
  memcpy(h, "RIFF", 4);
  put_u32(h+4, 36);                 /* RIFF chunk size (patched)   */
  memcpy(h+8, "WAVE", 4);
  memcpy(h+12, "fmt ", 4);
  put_u32(h+16, 16);                /* fmt chunk size              */
  put_u16(h+20, 1);                 /* PCM                         */
  put_u16(h+22, channels);
  put_u32(h+24, rate);
  put_u32(h+28, byte_rate);
  put_u16(h+32, channels*(bits/8)); /* block align                 */
  put_u16(h+34, bits);
  memcpy(h+36, "data", 4);
  put_u32(h+40, 0);                 /* data chunk size (patched)   */
  fwrite(h, 1, 44, f);
}

/* Finish a WAV: close the stream, then patch RIFF size (offset 4) and data
 * size (offset 40) from what the file actually holds.
 *
 * The patch is what turns the placeholder header written at open into a valid
 * one, so a failure here costs the WHOLE file rather than its tail -- and the
 * case it has to survive is precisely the one where writing is already failing.
 * Three things follow, all of them learned from rec_offline's RLIMIT_FSIZE
 * case, whose first two attempts left a 64 KiB file whose header said zero:
 *
 *   - a failed write LATCHES the stream's error flag AND leaves data stdio
 *     cannot flush, so every later call on that FILE* fails too -- fseek and
 *     the header write included, and clearerr() does not help because the
 *     unflushable buffer is still there. The stream is CLOSED first, which
 *     resolves it either way, and the header is fixed through a second handle.
 *   - the byte counter says what was asked for; the file says what landed, and
 *     after a short write those differ. The header must describe the file or
 *     nothing will play it, so the size comes from the file.
 *   - the tail is truncated to a whole number of frames, so the file is exactly
 *     what its header claims.
 */
static void close_wav(FILE *f, guint32 data_bytes, const char *path) {
  unsigned char v[4];
  if(!f) return;

  clearerr(f);
  if(fclose(f)!=0)
    log_error("recorder: %s did not close cleanly (%s) -- fixing its header up "
              "from what reached the disk\n", path, g_strerror(errno));

  if(path==NULL || path[0]=='\0') return;

  FILE *g = fopen(path, "r+b");
  if(g==NULL) {
    log_error("recorder: cannot reopen %s to finish its header (%s) -- that "
              "file will not play\n", path, g_strerror(errno));
    return;
  }

  gboolean ok = TRUE;
  if(fseek(g, 0, SEEK_END)==0) {
    long end = ftell(g);
    guint32 real = (end >= 44) ? (guint32)(((end - 44) / 4) * 4) : 0u;
    if(real != data_bytes) {
      log_error("recorder: %lu of %lu PCM bytes reached %s -- the header will "
                "describe what is really there\n",
                (unsigned long)real, (unsigned long)data_bytes, path);
      data_bytes = real;
    }
  }

  // Drop any partial frame past the size the header is about to claim.
#ifdef _WIN32
  _chsize_s(_fileno(g), (__int64)(44 + (gint64)data_bytes));
#else
  if(ftruncate(fileno(g), (off_t)(44 + (gint64)data_bytes))!=0) { /* best effort */ }
#endif

  put_u32(v, 36+data_bytes);
  if(fseek(g, 4, SEEK_SET)!=0 || fwrite(v, 1, 4, g)!=4) ok = FALSE;
  put_u32(v, data_bytes);
  if(fseek(g, 40, SEEK_SET)!=0 || fwrite(v, 1, 4, g)!=4) ok = FALSE;
  if(fclose(g)!=0) ok = FALSE;
  if(!ok) log_error("recorder: could not finish the WAV header of %s (%s) -- "
                    "that file will not play\n", path, g_strerror(errno));
}

/* Close the current segment (its size is < 4 GiB, so the patched header is
 * valid) and open the next numbered continuation segment. On open failure the
 * stream is left NULL and callers stop writing it. Writer thread only. */
static void roll_segment(FILE **fp, guint32 *bytes, const char *suffix,
                         guint32 rate, int *seg, char *path, size_t pathlen) {
  close_wav(*fp, *bytes, path);
  *seg += 1;
  snprintf(path, pathlen, "%s_%s_%03d.wav", rec_base, suffix, *seg);
  *fp = fopen(path, "wb");
  if(*fp) {
    write_wav_header(*fp, rate);
    log_info("recorder: %s continued -> %s\n", suffix, path);
  } else {
    log_error("recorder: cannot open continuation file %s\n", path);
    path[0] = '\0';
  }
  *bytes = 0;
}

/* ---- telling the operator ------------------------------------------------
 *
 * Both of these run on the WRITER thread, so the dialog is handed to the GTK
 * thread. Each is one-shot per recording: the condition lasts as long as the
 * disk does, and a dialog per block would be worse than the fault.
 */
typedef struct { char *title; char *detail; } REC_ALERT;

static gboolean rec_alert_idle(gpointer data) {
  REC_ALERT *a = (REC_ALERT *)data;
  if(main_window != NULL) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", a->title);
    gtk_alert_dialog_set_detail(dialog, a->detail);
    const char *buttons[] = { "OK", NULL };
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_show(dialog, GTK_WINDOW(main_window));
    g_object_unref(dialog);
  }
  g_free(a->title); g_free(a->detail); g_free(a);
  return G_SOURCE_REMOVE;
}

static void rec_alert(const char *title, const char *detail) {
  REC_ALERT *a = g_new0(REC_ALERT, 1);
  a->title  = g_strdup(title);
  a->detail = g_strdup(detail);
  g_idle_add(rec_alert_idle, a);
}

/* Stop the recording from the writer thread's own error path. It cannot join
 * itself, so it asks the GTK thread -- the same thread the Record button's
 * handler runs on, through the same call, which is also what puts the button's
 * label back. A button still reading "Stop" over a recording that died is how
 * an operator comes to believe a lost recording is still running. */
static gboolean rec_stop_idle(gpointer data) {
  recorder_stop();
  radio_record_button_sync();
  return G_SOURCE_REMOVE;
}

/* A write actually failed: the disk is full, the volume went away, the quota is
 * gone. Nothing further can be recorded, so say so and stop -- the old code
 * ignored the return and went on incrementing the byte counter, which is how a
 * WAV came out with a header promising bytes that were never written. */
static void rec_write_failed(const char *what, int err) {
  char *detail = g_strdup_printf(
      "MacHPSDR could not write the %s recording:\n\n    %s\n\n"
      "The recording has been stopped. The part already written is complete "
      "and playable; everything after the failure is lost.\n\n"
      "Most often this is a full disk, a volume that was unmounted, or a "
      "recording folder that is not writable (Configure \xe2\x86\x92 Recording).",
      what, g_strerror(err));
  log_error("recorder: %s write failed: %s -- stopping the recording\n",
            what, g_strerror(err));
  rec_alert("Recording stopped: cannot write to disk", detail);
  g_free(detail);
  g_idle_add(rec_stop_idle, NULL);
}

static short clamp16(double x) {
  if(x >  1.0) x =  1.0;
  if(x < -1.0) x = -1.0;
  return (short)(x*32767.0);
}

gboolean recorder_active(void) {
  return rec_rx != NULL;
}

/* What the last (or current) recording actually captured and actually lost.
 * Counted in FRAMES because that is time: dropped frames are a hole in the
 * middle of the recording, not a shorter tail. */
void recorder_stats(guint64 *iq_frames, guint64 *af_frames,
                    guint64 *iq_drop, guint64 *af_drop) {
  g_mutex_lock(&rec_mutex);
  if(iq_frames) *iq_frames = iq_written;
  if(af_frames) *af_frames = af_written;
  if(iq_drop)   *iq_drop   = iq_dropped;
  if(af_drop)   *af_drop   = af_dropped;
  g_mutex_unlock(&rec_mutex);
}

/* Stop the writer, drain what is queued and go idle. Entered and left with
 * rec_mutex HELD, but it drops the lock across the join -- the writer takes
 * that same mutex on every block, so holding it here would deadlock the join.
 *
 * The order matters. rec_rx and rec_writer_run are cleared FIRST, so a tap that
 * has already passed its unlocked fast check is rejected by rec_enqueue() and
 * frees its block rather than leaving one behind in a queue nobody will drain
 * again. Only the GTK thread ever stops a recording (the Record button,
 * delete_receiver, and the writer's own error path via an idle), so nothing can
 * enter here twice in the window where the lock is down; the rec_writer guard
 * makes that explicit rather than assumed. */
static void stop_locked(const char *why) {
  GThread *t = rec_writer;
  if(t == NULL) { rec_rx = NULL; return; }

  rec_rx = NULL;
  rec_writer_run = FALSE;
  rec_writer = NULL;
  g_cond_signal(&rec_cond);
  g_mutex_unlock(&rec_mutex);
  /* Drains the queue and closes both files. It can take as long as the last
   * few blocks take to write -- which is the recording the operator asked for,
   * so it is waited for rather than discarded. */
  g_thread_join(t);
  g_mutex_lock(&rec_mutex);

  guint64 iq_d = iq_dropped, af_d = af_dropped;
  log_info("recorder: stopped%s%s -- I/Q %llu frames (%.1f s), AF %llu frames "
           "(%.1f s), queue peak %.1f MiB\n",
           why?" ":"", why?why:"",
           (unsigned long long)iq_written, rec_iq_rate ? (double)iq_written/rec_iq_rate : 0.0,
           (unsigned long long)af_written, (double)af_written/48000.0,
           (double)rec_q_peak/(1024.0*1024.0));
  if(iq_d || af_d)
    log_error("recorder: %llu I/Q frames (%.1f s) and %llu AF frames (%.1f s) "
              "were DROPPED -- the recording is shorter than the time recorded\n",
              (unsigned long long)iq_d, rec_iq_rate ? (double)iq_d/rec_iq_rate : 0.0,
              (unsigned long long)af_d, (double)af_d/48000.0);

  /* Anything a tap slipped in during the window above. */
  if(rec_q != NULL) {
    REC_BLOCK *b;
    while((b = (REC_BLOCK *)g_queue_pop_head(rec_q)) != NULL) g_free(b);
  }
  rec_q_bytes = 0;
}

/* Stop a recording if one is running; no-op otherwise. The writer thread's
 * error path asks for this through an idle, since it cannot join itself. */
void recorder_stop(void) {
  g_mutex_lock(&rec_mutex);
  if(rec_rx != NULL || rec_writer != NULL) stop_locked("(disk error)");
  g_mutex_unlock(&rec_mutex);
}

/* Stop a recording that belongs to rx, no-op otherwise; TRUE if one was
 * stopped. The taps are keyed on the RECEIVER pointer, so a receiver closed
 * mid-recording used to leave rec_rx pointing at it with the in-flight files
 * never closed -- their headers still carrying the placeholder sizes written at
 * open, which is a lost recording rather than a truncated one. delete_receiver
 * calls this; the lock order there is delete_rx_mutex -> rec_mutex, the same
 * order the RX audio thread takes them in, so the two cannot deadlock. */
gboolean recorder_stop_for_receiver(RECEIVER *rx) {
  gboolean stopped=FALSE;
  g_mutex_lock(&rec_mutex);
  if(rec_rx!=NULL && rec_rx==rx) {
    stop_locked("(receiver closed)");
    stopped=TRUE;
  }
  g_mutex_unlock(&rec_mutex);
  return stopped;
}

gboolean recorder_toggle(RECEIVER *rx) {
  g_mutex_lock(&rec_mutex);
  if(rec_rx) {
    /* stop */
    stop_locked(NULL);
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }

  /* start -- honour the Configure -> Recording settings (which streams, where) */
  gboolean want_iq = radio->rec_iq;
  gboolean want_af = radio->rec_af;
  if(!want_iq && !want_af) {
    log_info("recorder: nothing selected (enable I/Q and/or AF in Configure -> Recording)\n");
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }

  char dir[512], iqp[600], afp[600], stamp[32];
  time_t t=time(NULL);
  struct tm tm; gmtime_r(&t, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
  if(radio->rec_dir[0])
    g_strlcpy(dir, radio->rec_dir, sizeof(dir));
  else
    snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  snprintf(rec_base, sizeof(rec_base), "%s/rec_%s", dir, stamp);
  snprintf(iqp, sizeof(iqp), "%s_iq.wav", rec_base);
  snprintf(afp, sizeof(afp), "%s_af.wav", rec_base);

  if(want_iq) rec_iq=fopen(iqp, "wb");
  if(want_af) rec_af=fopen(afp, "wb");
  g_strlcpy(iq_path, rec_iq?iqp:"", sizeof(iq_path));
  g_strlcpy(af_path, rec_af?afp:"", sizeof(af_path));
  if((want_iq && !rec_iq) || (want_af && !rec_af)) {
    if(rec_iq) { fclose(rec_iq); rec_iq=NULL; }
    if(rec_af) { fclose(rec_af); rec_af=NULL; }
    log_info("recorder: cannot open output files in %s\n", dir);
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }
  rec_iq_rate=(guint32)rx->sample_rate;
  if(rec_iq) write_wav_header(rec_iq, rec_iq_rate);
  if(rec_af) write_wav_header(rec_af, 48000);
  iq_bytes=0; af_bytes=0;
  iq_seg=1; af_seg=1;
  iq_dropped=0; af_dropped=0; iq_written=0; af_written=0;
  rec_q_bytes=0; rec_q_peak=0; drop_warned=FALSE;
  last_drop_report=0;
  { const char *e=g_getenv("MACHPSDR_REC_STALL_US");
    rec_stall_us = e ? (guint32)strtoul(e,NULL,10) : 0;
    if(rec_stall_us) log_info("recorder: TEST HOOK -- writer stalls %u us per block\n",
                              rec_stall_us); }
  if(rec_q==NULL) rec_q=g_queue_new();

  /* Publish rec_rx and start the writer under the same lock the taps enqueue
   * under, so the first block cannot arrive before there is a thread to take
   * it -- nor a thread before there is a file to write into. */
  rec_writer_run=TRUE;
  rec_writer=g_thread_new("recorder writer", rec_writer_thread, NULL);
  if(rec_writer==NULL) {
    log_error("recorder: cannot start the writer thread -- not recording\n");
    rec_writer_run=FALSE;
    close_wav(rec_iq, 0, iq_path); close_wav(rec_af, 0, af_path);
    rec_iq=NULL; rec_af=NULL;
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }
  rec_rx=rx;
  if(rec_iq) log_info("recorder: I/Q -> %s (%d Hz)\n", iqp, rx->sample_rate);
  if(rec_af) log_info("recorder: AF  -> %s (48000 Hz)\n", afp);
  g_mutex_unlock(&rec_mutex);
  return TRUE;
}

/* Write one block. Writer thread only, and OUTSIDE rec_mutex -- that is the
 * whole point of the split: the disk may take as long as it likes here without
 * any producer noticing. A stream whose write fails is closed and dropped, so
 * the queue keeps draining instead of the thread spinning on a dead file. */
static void rec_write_block(REC_BLOCK *b) {
  FILE **fp;  guint32 *bytes;  const char *suffix;  guint32 rate;  int *seg;
  guint64 *written;  char *path;  size_t pathlen;

  if(b->stream == REC_STREAM_IQ) {
    fp=&rec_iq; bytes=&iq_bytes; suffix="iq"; rate=rec_iq_rate; seg=&iq_seg;
    written=&iq_written; path=iq_path; pathlen=sizeof(iq_path);
  } else {
    fp=&rec_af; bytes=&af_bytes; suffix="af"; rate=48000;       seg=&af_seg;
    written=&af_written; path=af_path; pathlen=sizeof(af_path);
  }
  if(*fp == NULL) return;                     /* stream already given up on */

  /* The check sits BETWEEN blocks, so a segment always ends on a frame
   * boundary and every segment stays a standard, replayable WAV. */
  if(*bytes >= REC_SPLIT_BYTES)
    roll_segment(fp, bytes, suffix, rate, seg, path, pathlen);
  if(*fp == NULL) return;

  size_t items = (size_t)b->frames*2;
  errno = 0;
  size_t done = fwrite(b->pcm, sizeof(short), items, *fp);
  if(done != items) {
    int err = errno ? errno : EIO;
    // Count the whole frames that DID land, so the summary and the file agree
    // about how much of the recording survived.
    guint32 good = (guint32)(done/2);
    *bytes   += good*4;
    *written += good;
    close_wav(*fp, *bytes, path);
    *fp = NULL;
    rec_write_failed(suffix, err);
    return;
  }
  *bytes  += b->frames*4;
  *written += b->frames;
}

static gpointer rec_writer_thread(gpointer data) {
  for(;;) {
    g_mutex_lock(&rec_mutex);
    while(rec_writer_run && g_queue_is_empty(rec_q))
      g_cond_wait(&rec_cond, &rec_mutex);
    if(g_queue_is_empty(rec_q)) {            /* asked to stop, and drained */
      g_mutex_unlock(&rec_mutex);
      break;
    }
    REC_BLOCK *b = (REC_BLOCK *)g_queue_pop_head(rec_q);
    rec_q_bytes -= (gsize)b->frames*4;
    g_mutex_unlock(&rec_mutex);

    rec_write_block(b);
    g_free(b);
    if(rec_stall_us) g_usleep(rec_stall_us);
  }

  /* The writer owns both files for its whole life, so it is also the one that
   * closes them -- after the queue is empty, which is what makes "stop" mean
   * "everything captured is on the disk" rather than "everything not yet
   * written is lost". */
  close_wav(rec_iq, iq_bytes, iq_path);
  close_wav(rec_af, af_bytes, af_path);
  rec_iq = NULL; rec_af = NULL;
  return NULL;
}

/* Called from the RX audio/DSP thread. Never waits for the disk: a full queue
 * DROPS the block and counts it. The alternative is a producer blocked on the
 * filesystem, which is the fault this whole split exists to remove. */
static void rec_enqueue(REC_BLOCK *b) {
  gsize bytes = (gsize)b->frames*4;
  gboolean warn = FALSE;
  guint64 iq_d=0, af_d=0;
  gint64 now = 0;

  g_mutex_lock(&rec_mutex);
  if(!rec_writer_run) {                      /* stopped under us */
    g_mutex_unlock(&rec_mutex);
    g_free(b);
    return;
  }
  if(rec_q_bytes + bytes > REC_QUEUE_MAX_BYTES) {
    if(b->stream == REC_STREAM_IQ) iq_dropped += b->frames;
    else                           af_dropped += b->frames;
    /* Say it every five seconds while it is happening. A recording that comes
     * out short with nothing in the log is indistinguishable from one that was
     * stopped early, and the number of FRAMES is the number that matters --
     * it is how much time is missing out of the middle. */
    now = g_get_monotonic_time();
    if(now - last_drop_report >= 5*G_USEC_PER_SEC) {
      last_drop_report = now;
      iq_d = iq_dropped; af_d = af_dropped;
      warn = !drop_warned;
      drop_warned = TRUE;
    }
    g_mutex_unlock(&rec_mutex);
    g_free(b);
    if(iq_d || af_d) {
      log_error("recorder: the disk is not keeping up -- dropped %llu I/Q frames "
                "(%.1f s) and %llu AF frames (%.1f s) so far; the queue holds "
                "%u MiB\n",
                (unsigned long long)iq_d, rec_iq_rate ? (double)iq_d/rec_iq_rate : 0.0,
                (unsigned long long)af_d, (double)af_d/48000.0,
                REC_QUEUE_MAX_BYTES/(1024u*1024u));
    }
    if(warn)
      rec_alert("Recording is losing samples",
                "The disk is not accepting the recording as fast as the radio "
                "produces it, so parts of the stream are being discarded. The "
                "files stay valid and playable, but they will be shorter than "
                "the time you recorded, with the gaps in the middle rather "
                "than at the end.\n\n"
                "Recording is continuing. Worth trying: a faster or less busy "
                "disk (Configure \xe2\x86\x92 Recording chooses the folder), "
                "recording only AF rather than I/Q, or a narrower span -- the "
                "I/Q rate is the span, so 768 kHz costs four times the disk of "
                "192 kHz.");
    return;
  }
  g_queue_push_tail(rec_q, b);
  rec_q_bytes += bytes;
  if(rec_q_bytes > rec_q_peak) rec_q_peak = rec_q_bytes;
  g_cond_signal(&rec_cond);
  g_mutex_unlock(&rec_mutex);
}

void recorder_iq(RECEIVER *rx, double *iq, int nsamples) {
  if(rec_rx != rx || nsamples <= 0) return;   /* fast unlocked reject */
  REC_BLOCK *b = g_malloc(sizeof(REC_BLOCK) + (size_t)nsamples*2*sizeof(short));
  b->stream = REC_STREAM_IQ;
  b->frames = (guint32)nsamples;
  for(int i=0;i<nsamples;i++) {
    b->pcm[i*2]   = clamp16(iq[i*2]);
    b->pcm[i*2+1] = clamp16(iq[i*2+1]);
  }
  rec_enqueue(b);
}

void recorder_audio(RECEIVER *rx, double *audio, int nstereo) {
  if(rec_rx != rx || nstereo <= 0) return;
  REC_BLOCK *b = g_malloc(sizeof(REC_BLOCK) + (size_t)nstereo*2*sizeof(short));
  b->stream = REC_STREAM_AF;
  b->frames = (guint32)nstereo;
  for(int i=0;i<nstereo;i++) {
    b->pcm[i*2]   = clamp16(audio[i*2]);
    b->pcm[i*2+1] = clamp16(audio[i*2+1]);
  }
  rec_enqueue(b);
}

/* ---- Configure -> Recording settings page ------------------------------- */

static void rec_dir_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  g_strlcpy(r->rec_dir, gtk_editable_get_text(GTK_EDITABLE(w)), sizeof(r->rec_dir));
}

// GTK4: GtkFileChooserDialog is deprecated; GtkFileDialog is async — the
// chosen folder is applied in this finish callback.
static void rec_browse_done(GObject *src, GAsyncResult *res, gpointer data) {
  GtkWidget *entry=(GtkWidget *)data;
  GFile *gf=gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
  if(gf==NULL) return;   // cancelled / error
  char *dir=g_file_get_path(gf);
  g_object_unref(gf);
  if(dir) { gtk_editable_set_text(GTK_EDITABLE(entry), dir); g_free(dir); }
}

static void rec_browse_cb(GtkWidget *w, gpointer data) {
  GtkWidget *entry=(GtkWidget *)data;
  GtkRoot *root=gtk_widget_get_root(w);
  GtkFileDialog *d=gtk_file_dialog_new();
  gtk_file_dialog_set_title(d,"Choose recording folder");
  const char *cur=gtk_editable_get_text(GTK_EDITABLE(entry));
  if(cur && cur[0]) {
    GFile *cf=g_file_new_for_path(cur);
    gtk_file_dialog_set_initial_folder(d, cf);
    g_object_unref(cf);
  }
  gtk_file_dialog_select_folder(d, root?GTK_WINDOW(root):NULL, NULL, rec_browse_done, entry);
  g_object_unref(d);
}

static void rec_iq_cb(GtkWidget *w, gpointer data) {
  ((RADIO *)data)->rec_iq=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
}
static void rec_af_cb(GtkWidget *w, gpointer data) {
  ((RADIO *)data)->rec_af=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
}

GtkWidget *create_recording_dialog(struct _radio *rp) {
  RADIO *r=(RADIO *)rp;

  GtkWidget *frame=gtk_frame_new("Recording");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);

  GtkWidget *info=gtk_label_new(
      "The Record button (bottom bar, SETUP) writes the active receiver's\n"
      "off-air I/Q and/or demodulated audio to timestamped 16-bit WAV files.\n"
      "The I/Q file can be replayed with --faker. Changes apply to the next\n"
      "recording, not one already running.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12);
  gtk_grid_attach(GTK_GRID(grid),info,0,0,3,1);

  GtkWidget *dir_lbl=gtk_label_new("Folder:");
  gtk_widget_set_halign(dir_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),dir_lbl,0,1,1,1);
  GtkWidget *dir=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(dir),sizeof(r->rec_dir)-1);
  gtk_editable_set_width_chars(GTK_EDITABLE(dir),32);
  gtk_entry_set_placeholder_text(GTK_ENTRY(dir),"(default ~/.local/share/machpsdr)");
  gtk_editable_set_text(GTK_EDITABLE(dir),r->rec_dir);
  gtk_widget_set_hexpand(dir,TRUE);
  gtk_grid_attach(GTK_GRID(grid),dir,1,1,1,1);
  g_signal_connect(dir,"changed",G_CALLBACK(rec_dir_cb),r);
  GtkWidget *browse=gtk_button_new_with_label("Browse…");
  gtk_grid_attach(GTK_GRID(grid),browse,2,1,1,1);
  g_signal_connect(browse,"clicked",G_CALLBACK(rec_browse_cb),dir);

  GtkWidget *hint=gtk_label_new("Leave blank for the default folder.");
  gtk_widget_set_halign(hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),hint,1,2,2,1);

  GtkWidget *iq=gtk_check_button_new_with_label("Record I/Q (raw off-air, --faker-replayable)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(iq),r->rec_iq);
  gtk_grid_attach(GTK_GRID(grid),iq,0,3,3,1);
  g_signal_connect(iq,"toggled",G_CALLBACK(rec_iq_cb),r);

  GtkWidget *af=gtk_check_button_new_with_label("Record AF (demodulated audio, 48 kHz)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(af),r->rec_af);
  gtk_grid_attach(GTK_GRID(grid),af,0,4,3,1);
  g_signal_connect(af,"toggled",G_CALLBACK(rec_af_cb),r);

  GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(vbox),frame);
  return vbox;
}
