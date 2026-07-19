/* recorder.c -- see recorder.h.
 *
 * A minimal streaming RIFF/WAVE writer: the 44-byte header is written up front
 * with placeholder sizes and patched with the real byte counts on stop. Both
 * files are 16-bit little-endian PCM, 2 channels. The I/Q file mirrors the
 * format the --faker replay path expects (fake_protocol.c), so a capture can be
 * looped straight back through the RX/decoder chain.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <gtk/gtk.h>

#include "recorder.h"
/* radio.h needs these types declared first; main.h provides the global `radio`. */
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "settings_ui.h"

static GMutex rec_mutex;
static RECEIVER *rec_rx = NULL;      /* receiver being recorded, NULL when idle */
static FILE *rec_iq = NULL;
static FILE *rec_af = NULL;
static guint32 iq_bytes = 0;         /* PCM data bytes written to each file */
static guint32 af_bytes = 0;

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
    g_print("recorder: stopped\n");
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }

  /* start -- honour the Configure -> Recording settings (which streams, where) */
  gboolean want_iq = radio->rec_iq;
  gboolean want_af = radio->rec_af;
  if(!want_iq && !want_af) {
    g_print("recorder: nothing selected (enable I/Q and/or AF in Configure -> Recording)\n");
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
  snprintf(iqp, sizeof(iqp), "%s/rec_%s_iq.wav", dir, stamp);
  snprintf(afp, sizeof(afp), "%s/rec_%s_af.wav", dir, stamp);

  if(want_iq) rec_iq=fopen(iqp, "wb");
  if(want_af) rec_af=fopen(afp, "wb");
  if((want_iq && !rec_iq) || (want_af && !rec_af)) {
    if(rec_iq) { fclose(rec_iq); rec_iq=NULL; }
    if(rec_af) { fclose(rec_af); rec_af=NULL; }
    g_print("recorder: cannot open output files in %s\n", dir);
    g_mutex_unlock(&rec_mutex);
    return FALSE;
  }
  if(rec_iq) write_wav_header(rec_iq, (guint32)rx->sample_rate);
  if(rec_af) write_wav_header(rec_af, 48000);
  iq_bytes=0; af_bytes=0;
  rec_rx=rx;
  if(rec_iq) g_print("recorder: I/Q -> %s (%d Hz)\n", iqp, rx->sample_rate);
  if(rec_af) g_print("recorder: AF  -> %s (48000 Hz)\n", afp);
  g_mutex_unlock(&rec_mutex);
  return TRUE;
}

void recorder_iq(RECEIVER *rx, double *iq, int nsamples) {
  if(rec_rx != rx) return;               /* fast unlocked reject */
  g_mutex_lock(&rec_mutex);
  if(rec_rx==rx && rec_iq) {
    for(int i=0;i<nsamples;i++) {
      short s[2] = { clamp16(iq[i*2]), clamp16(iq[i*2+1]) };
      fwrite(s, sizeof(short), 2, rec_iq);
    }
    iq_bytes += (guint32)nsamples*4;
  }
  g_mutex_unlock(&rec_mutex);
}

void recorder_audio(RECEIVER *rx, double *audio, int nstereo) {
  if(rec_rx != rx) return;
  g_mutex_lock(&rec_mutex);
  if(rec_rx==rx && rec_af) {
    for(int i=0;i<nstereo;i++) {
      short s[2] = { clamp16(audio[i*2]), clamp16(audio[i*2+1]) };
      fwrite(s, sizeof(short), 2, rec_af);
    }
    af_bytes += (guint32)nstereo*4;
  }
  g_mutex_unlock(&rec_mutex);
}

/* ---- Configure -> Recording settings page ------------------------------- */

static void rec_dir_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  g_strlcpy(r->rec_dir, gtk_entry_get_text(GTK_ENTRY(w)), sizeof(r->rec_dir));
}

static void rec_browse_cb(GtkWidget *w, gpointer data) {
  GtkWidget *entry=(GtkWidget *)data;
  GtkWidget *top=gtk_widget_get_toplevel(w);
  GtkWidget *d=gtk_file_chooser_dialog_new(
      "Choose recording folder",
      GTK_IS_WINDOW(top)?GTK_WINDOW(top):NULL,
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
      "Cancel", GTK_RESPONSE_CANCEL, "Select", GTK_RESPONSE_ACCEPT, NULL);
  const char *cur=gtk_entry_get_text(GTK_ENTRY(entry));
  if(cur && cur[0]) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), cur);
  if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT) {
    char *dir=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    if(dir) { gtk_entry_set_text(GTK_ENTRY(entry), dir); g_free(dir); }
  }
  gtk_widget_destroy(d);
}

static void rec_iq_cb(GtkWidget *w, gpointer data) {
  ((RADIO *)data)->rec_iq=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
}
static void rec_af_cb(GtkWidget *w, gpointer data) {
  ((RADIO *)data)->rec_af=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
}

GtkWidget *create_recording_dialog(struct _radio *rp) {
  RADIO *r=(RADIO *)rp;

  GtkWidget *frame=gtk_frame_new("Recording");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_container_add(GTK_CONTAINER(frame),grid);

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
  gtk_entry_set_width_chars(GTK_ENTRY(dir),32);
  gtk_entry_set_placeholder_text(GTK_ENTRY(dir),"(default ~/.local/share/machpsdr)");
  gtk_entry_set_text(GTK_ENTRY(dir),r->rec_dir);
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
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(iq),r->rec_iq);
  gtk_grid_attach(GTK_GRID(grid),iq,0,3,3,1);
  g_signal_connect(iq,"toggled",G_CALLBACK(rec_iq_cb),r);

  GtkWidget *af=gtk_check_button_new_with_label("Record AF (demodulated audio, 48 kHz)");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(af),r->rec_af);
  gtk_grid_attach(GTK_GRID(grid),af,0,4,3,1);
  g_signal_connect(af,"toggled",G_CALLBACK(rec_af_cb),r);

  GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_pack_start(GTK_BOX(vbox),frame,FALSE,FALSE,0);
  return vbox;
}
