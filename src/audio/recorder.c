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
 */
#include <stdio.h>
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
static RECEIVER *rec_rx = NULL;      /* receiver being recorded, NULL when idle */
static FILE *rec_iq = NULL;
static FILE *rec_af = NULL;
static guint32 iq_bytes = 0;         /* PCM data bytes in the current segment */
static guint32 af_bytes = 0;
static char    rec_base[600] = "";   /* "<dir>/rec_<stamp>"; segment names built from this */
static guint32 rec_iq_rate = 0;      /* I/Q sample rate, remembered for continuation segments */
static int     iq_seg = 1;           /* current segment number (1 = first, unnumbered file) */
static int     af_seg = 1;

/* Split each WAV before its 32-bit RIFF/data size field can overflow (2^32).
 * ~3.75 GiB leaves a wide margin over any single transfer block and keeps every
 * segment a standard, --faker-replayable WAV. Multiple of the 4-byte block. */
#define REC_SPLIT_BYTES 0xF0000000u

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

/* Patch RIFF size (offset 4) and data size (offset 40), then close. */
static void close_wav(FILE *f, guint32 data_bytes) {
  unsigned char v[4];
  if(!f) return;
  put_u32(v, 36+data_bytes);
  fseek(f, 4, SEEK_SET);  fwrite(v, 1, 4, f);
  put_u32(v, data_bytes);
  fseek(f, 40, SEEK_SET); fwrite(v, 1, 4, f);
  fclose(f);
}

/* Close the current segment (its size is < 4 GiB, so the patched header is
 * valid) and open the next numbered continuation segment. On open failure the
 * stream is left NULL and callers stop writing it. Called under rec_mutex. */
static void roll_segment(FILE **fp, guint32 *bytes, const char *suffix,
                         guint32 rate, int *seg) {
  close_wav(*fp, *bytes);
  *seg += 1;
  char path[700];
  snprintf(path, sizeof(path), "%s_%s_%03d.wav", rec_base, suffix, *seg);
  *fp = fopen(path, "wb");
  if(*fp) {
    write_wav_header(*fp, rate);
    log_info("recorder: %s continued -> %s\n", suffix, path);
  } else {
    log_error("recorder: cannot open continuation file %s\n", path);
  }
  *bytes = 0;
}

static short clamp16(double x) {
  if(x >  1.0) x =  1.0;
  if(x < -1.0) x = -1.0;
  return (short)(x*32767.0);
}

gboolean recorder_active(void) {
  return rec_rx != NULL;
}

gboolean recorder_toggle(RECEIVER *rx) {
  g_mutex_lock(&rec_mutex);
  if(rec_rx) {
    /* stop */
    close_wav(rec_iq, iq_bytes);
    close_wav(rec_af, af_bytes);
    rec_iq=NULL; rec_af=NULL; rec_rx=NULL;
    log_info("recorder: stopped\n");
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
  rec_rx=rx;
  if(rec_iq) log_info("recorder: I/Q -> %s (%d Hz)\n", iqp, rx->sample_rate);
  if(rec_af) log_info("recorder: AF  -> %s (48000 Hz)\n", afp);
  g_mutex_unlock(&rec_mutex);
  return TRUE;
}

void recorder_iq(RECEIVER *rx, double *iq, int nsamples) {
  if(rec_rx != rx) return;               /* fast unlocked reject */
  g_mutex_lock(&rec_mutex);
  if(rec_rx==rx && rec_iq) {
    if(iq_bytes >= REC_SPLIT_BYTES)
      roll_segment(&rec_iq, &iq_bytes, "iq", rec_iq_rate, &iq_seg);
    if(rec_iq) {
      for(int i=0;i<nsamples;i++) {
        short s[2] = { clamp16(iq[i*2]), clamp16(iq[i*2+1]) };
        fwrite(s, sizeof(short), 2, rec_iq);
      }
      iq_bytes += (guint32)nsamples*4;
    }
  }
  g_mutex_unlock(&rec_mutex);
}

void recorder_audio(RECEIVER *rx, double *audio, int nstereo) {
  if(rec_rx != rx) return;
  g_mutex_lock(&rec_mutex);
  if(rec_rx==rx && rec_af) {
    if(af_bytes >= REC_SPLIT_BYTES)
      roll_segment(&rec_af, &af_bytes, "af", 48000, &af_seg);
    if(rec_af) {
      for(int i=0;i<nstereo;i++) {
        short s[2] = { clamp16(audio[i*2]), clamp16(audio[i*2+1]) };
        fwrite(s, sizeof(short), 2, rec_af);
      }
      af_bytes += (guint32)nstereo*4;
    }
  }
  g_mutex_unlock(&rec_mutex);
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
