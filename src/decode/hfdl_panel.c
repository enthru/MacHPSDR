/* Copyright (C)
* 2026 - MacHPSDR fork
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <gtk/gtk.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"

#include "hfdl_panel.h"
#include "hfdl_decoder.h"
#include "hfdl_msg.h"
#include "log.h"

#define REFRESH_MS      300     // ~3 fps (HFDL is a slow data link)
#define DRAIN_CHUNK     4096
#define MAX_LOG_LINES   4000    // trim the on-screen scrollback beyond this
#define TRIM_LINES      1000
#define MAX_GS          64
#define MAX_AC          64
#define MAX_PRESETS     512

typedef struct {
  GtkWidget     *status;      // "signal -57 dB · 192 kHz · 13 frames" readout
  GtkWidget     *view;        // scrolling decoded-message view
  GtkTextBuffer *buf;
  GtkTextMark   *end_mark;    // kept at the end so new text auto-scrolls into view
  GtkWidget     *gs_view;     // Stations tab
  GtkTextBuffer *gs_buf;
  GtkWidget     *ac_view;     // Aircraft tab
  GtkTextBuffer *ac_buf;
  GtkWidget     *presets;     // frequency drop-down
  GtkStringList *preset_list;
  uint32_t       preset_khz[MAX_PRESETS];
  int            preset_cnt;
  int            preset_stamp;  // systable version the list was built from
  int            preset_rows;   // station count the list was built from
  FILE          *logf;
  guint          timer;
} HfdlPanel;

// --- message log file -------------------------------------------------------
//
// Everything decoded scrolls out of the panel eventually, and HFDL traffic is
// slow enough that an operator leaves it running for hours. Appending to a file
// is what makes an unattended session worth anything.

static void log_path(char *out, size_t len) {
  g_snprintf(out, len, "%s/.local/share/machpsdr/hfdl_log.txt", g_get_home_dir());
}

static void log_close(HfdlPanel *p) {
  if (p->logf) { fclose(p->logf); p->logf = NULL; }
}

static gboolean log_open(HfdlPanel *p) {
  if (p->logf) return TRUE;
  char path[512], dir[512];
  g_snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  log_path(path, sizeof(path));
  p->logf = fopen(path, "a");
  if (p->logf == NULL) { log_error("hfdl: cannot open %s for logging\n", path); return FALSE; }
  GDateTime *now = g_date_time_new_now_utc();
  char *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%SZ");
  fprintf(p->logf, "\n===== HFDL session started %s =====\n", ts);
  fflush(p->logf);
  g_free(ts);
  g_date_time_unref(now);
  log_info("hfdl: logging messages to %s\n", path);
  return TRUE;
}

static void log_toggled(GtkCheckButton *b, gpointer data) {
  HfdlPanel *p = data;
  gboolean on = gtk_check_button_get_active(b);
  if (on) {
    if (!log_open(p)) { gtk_check_button_set_active(b, FALSE); return; }
  } else {
    log_close(p);
  }
  if (radio) radio->hfdl_log = on;
}

// --- frequency presets ------------------------------------------------------
//
// Every HFDL channel is already in the station table (learned over the air, or
// the embedded snapshot), so the operator should never have to look one up:
// pick "11387 kHz - Riverhead, New York" and press Tune.

static int preset_cmp(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static void presets_rebuild(HfdlPanel *p) {
  HFDL_GS_INFO gs[MAX_GS];
  int ng = hfdl_msg_gs_list(gs, MAX_GS);
  int version = hfdl_msg_systable_version();
  // Only rebuild when the underlying table actually changed — otherwise the
  // drop-down would reset the operator's selection three times a second.
  if (version == p->preset_stamp && ng == p->preset_rows && p->preset_cnt > 0) return;
  p->preset_stamp = version;
  p->preset_rows  = ng;

  // Collect (frequency, station) pairs, sorted by frequency. A frequency shared
  // by two stations gets one row per station: they are different signals.
  typedef struct { uint32_t khz; int gs; } ENTRY;
  ENTRY ent[MAX_PRESETS];
  int n = 0;
  for (int i = 0; i < ng && n < MAX_PRESETS; i++)
    for (int f = 0; f < gs[i].freq_cnt && n < MAX_PRESETS; f++) {
      if (gs[i].freqs[f] == 0) continue;
      ent[n].khz = gs[i].freqs[f];
      ent[n].gs  = i;
      n++;
    }
  qsort(ent, (size_t)n, sizeof(ENTRY), preset_cmp);   // khz is the first member

  guint old = g_list_model_get_n_items(G_LIST_MODEL(p->preset_list));
  if (old > 0) gtk_string_list_splice(p->preset_list, 0, old, NULL);
  p->preset_cnt = 0;
  for (int i = 0; i < n; i++) {
    char label[96];
    const char *name = gs[ent[i].gs].name;
    g_snprintf(label, sizeof(label), "%u kHz \xE2\x80\x93 %s", ent[i].khz,
               name ? name : "unknown station");
    gtk_string_list_append(p->preset_list, label);
    p->preset_khz[p->preset_cnt++] = ent[i].khz;
  }
  if (p->preset_cnt > 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(p->presets), 0);
}

static void tune_clicked(GtkButton *b, gpointer data) {
  HfdlPanel *p = data;
  if (radio == NULL || radio->active_receiver == NULL) return;
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->presets));
  if (sel == GTK_INVALID_LIST_POSITION || (int)sel >= p->preset_cnt) return;

  RECEIVER *rx = radio->active_receiver;
  long long f = (long long)p->preset_khz[sel] * 1000LL;
  // The decoder takes its I/Q from the receiver centre and expects the PSK
  // carrier 1440 Hz up, which is exactly where it sits when the dial is on the
  // assigned channel frequency — so tune the channel, not an offset from it.
  // CTUN would move the demodulator without moving that centre, so clear it.
  rx->frequency_a    = f;
  rx->ctun_frequency = f;
  rx->ctun           = 0;
  if (rx->mode_a != DIGU) receiver_mode_changed(rx, DIGU);
  frequency_changed(rx);
  log_info("hfdl: tuned to %u kHz\n", p->preset_khz[sel]);
}

// --- station / aircraft views ----------------------------------------------

// "3s" / "12m" / "2h" since a monotonic timestamp, or "-" if never.
static void age_str(char *out, size_t len, gint64 t_us, gint64 now_us) {
  if (t_us == 0) { g_strlcpy(out, "-", len); return; }
  gint64 s = (now_us - t_us) / G_USEC_PER_SEC;
  if (s < 60)        g_snprintf(out, len, "%llds", (long long)s);
  else if (s < 3600) g_snprintf(out, len, "%lldm", (long long)(s / 60));
  else               g_snprintf(out, len, "%lldh", (long long)(s / 3600));
}

static void refresh_stations(HfdlPanel *p) {
  HFDL_GS_INFO gs[MAX_GS];
  int n = hfdl_msg_gs_list(gs, MAX_GS);
  gint64 now = g_get_monotonic_time();
  int version = hfdl_msg_systable_version();

  GString *s = g_string_new(NULL);
  if (version >= 0)
    g_string_append_printf(s, "System table: version %d (received on air)\n\n", version);
  else
    g_string_append(s, "System table: built-in snapshot (none received yet)\n\n");
  g_string_append(s, "ID  Station                       Heard  Frm  UTC  In use (kHz)\n");
  for (int i = 0; i < n; i++) {
    char age[16];
    age_str(age, sizeof(age), gs[i].last_heard_us, now);
    GString *fr = g_string_new(NULL);
    for (int f = 0; f < gs[i].freq_cnt; f++)
      if (gs[i].inuse_mask & (1u << f))
        g_string_append_printf(fr, "%s%u", fr->len ? " " : "", gs[i].freqs[f]);
    g_string_append_printf(s, "%-3u %-28s %5s %4d  %-3s  %s\n",
                           gs[i].id, gs[i].name ? gs[i].name : "(unknown)",
                           age, gs[i].frames,
                           gs[i].last_heard_us || gs[i].inuse_mask
                             ? (gs[i].utc_sync ? "yes" : "no") : "-",
                           fr->len ? fr->str : "");
    g_string_free(fr, TRUE);
  }
  if (n == 0) g_string_append(s, "(no stations)\n");
  gtk_text_buffer_set_text(p->gs_buf, s->str, -1);
  g_string_free(s, TRUE);
}

static void refresh_aircraft(HfdlPanel *p) {
  HFDL_AC_INFO ac[MAX_AC];
  int n = hfdl_msg_ac_list(ac, MAX_AC);
  gint64 now = g_get_monotonic_time();

  GString *s = g_string_new(NULL);
  g_string_append(s, "ID   ICAO    Flight   Heard  Frm  Position\n");
  for (int i = 0; i < n; i++) {
    char age[16], icao[16], pos[48];
    age_str(age, sizeof(age), ac[i].last_heard_us, now);
    if (ac[i].icao) g_snprintf(icao, sizeof(icao), "%06X", ac[i].icao);
    else            g_strlcpy(icao, "-", sizeof(icao));
    if (ac[i].have_pos) {
      char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(lat, sizeof(lat), "%.4f", ac[i].lat);
      g_ascii_formatd(lon, sizeof(lon), "%.4f", ac[i].lon);
      g_snprintf(pos, sizeof(pos), "%s, %s", lat, lon);
    } else {
      g_strlcpy(pos, "-", sizeof(pos));
    }
    g_string_append_printf(s, "%-4u %-7s %-8s %5s %4d  %s\n",
                           ac[i].ac_id, icao,
                           ac[i].flight[0] ? ac[i].flight : "-",
                           age, ac[i].frames, pos);
  }
  if (n == 0) g_string_append(s, "(no aircraft heard yet)\n");
  gtk_text_buffer_set_text(p->ac_buf, s->str, -1);
  g_string_free(s, TRUE);
}

// Keep the message scrollback bounded — an all-day session would otherwise grow
// the buffer without limit.
static void trim_scrollback(HfdlPanel *p) {
  int lines = gtk_text_buffer_get_line_count(p->buf);
  if (lines <= MAX_LOG_LINES) return;
  GtkTextIter start, cut;
  gtk_text_buffer_get_start_iter(p->buf, &start);
  gtk_text_buffer_get_iter_at_line(p->buf, &cut, TRIM_LINES);
  gtk_text_buffer_delete(p->buf, &start, &cut);
}

static gboolean tick(gpointer data) {
  HfdlPanel *p = data;
  char chunk[DRAIN_CHUNK];
  int n = hfdl_decoder_get_messages(chunk, sizeof(chunk));
  if (n > 0) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(p->buf, &end);
    gtk_text_buffer_insert(p->buf, &end, chunk, n);
    trim_scrollback(p);
    gtk_text_buffer_get_end_iter(p->buf, &end);
    gtk_text_buffer_move_mark(p->buf, p->end_mark, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(p->view), p->end_mark, 0.0, FALSE, 0.0, 0.0);
    if (p->logf) { fwrite(chunk, 1, (size_t)n, p->logf); fflush(p->logf); }
  }

  refresh_stations(p);
  refresh_aircraft(p);
  presets_rebuild(p);

  gboolean listening; int rate; glong syms;
  hfdl_decoder_get_status(&listening, &rate, &syms);
  double level = hfdl_decoder_get_level_db();
  glong frames = hfdl_decoder_get_frames();
  char buf[160];
  if (listening)
    g_snprintf(buf, sizeof(buf), "signal %.0f dB \xC2\xB7 %d kHz \xC2\xB7 %ld frames \xC2\xB7 %ld ksym",
               level, rate / 1000, frames, syms / 1000);
  else
    g_snprintf(buf, sizeof(buf), "idle");
  gtk_label_set_text(GTK_LABEL(p->status), buf);
  return G_SOURCE_CONTINUE;
}

static void clear_clicked(GtkButton *b, gpointer data) {
  HfdlPanel *p = data;
  gtk_text_buffer_set_text(p->buf, "", 0);
  hfdl_decoder_reset();
}

static void on_destroy(GtkWidget *w, gpointer data) {
  HfdlPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  log_close(p);
  g_free(p);
}

// A monospace, read-only text view inside a scroller — the shape all three tabs
// use (the two tables are fixed-width columns, so a text view IS the table).
static GtkWidget *make_view(GtkWidget **view_out, GtkTextBuffer **buf_out, gboolean wrap) {
  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),
                              wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_widget_set_margin_start(view, 4);
  gtk_widget_set_margin_end(view, 4);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  // Small min height (like the CW/SSTV panels) so the GTK4 paned can still shrink
  // the panel and leave the RF spectrum above it a usable height.
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);

  *view_out = view;
  *buf_out  = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  return scroll;
}

GtkWidget *hfdl_panel_create(void) {
  HfdlPanel *p = g_new0(HfdlPanel, 1);
  p->preset_stamp = -2;                   // -1 is a real value ("no table yet")

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  // Toolbar row: Clear, log toggle, channel presets, status readout.
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append(GTK_BOX(bar), gtk_label_new("HFDL"));
  GtkWidget *clr = gtk_button_new_with_label("Clear");
  g_signal_connect(clr, "clicked", G_CALLBACK(clear_clicked), p);
  gtk_box_append(GTK_BOX(bar), clr);

  GtkWidget *logcb = gtk_check_button_new_with_label("Log");
  {
    char path[512];
    log_path(path, sizeof(path));
    char tip[600];
    g_snprintf(tip, sizeof(tip), "Append every decoded message to %s", path);
    gtk_widget_set_tooltip_text(logcb, tip);
  }
  g_signal_connect(logcb, "toggled", G_CALLBACK(log_toggled), p);
  gtk_box_append(GTK_BOX(bar), logcb);

  p->preset_list = gtk_string_list_new(NULL);
  p->presets = gtk_drop_down_new(G_LIST_MODEL(p->preset_list), NULL);
  gtk_widget_set_tooltip_text(p->presets,
    "HFDL channels from the ground-station table. Tune puts the receiver on the "
    "assigned channel frequency in DIGU, which is where the decoder expects it.");
  gtk_box_append(GTK_BOX(bar), p->presets);
  GtkWidget *tune = gtk_button_new_with_label("Tune");
  g_signal_connect(tune, "clicked", G_CALLBACK(tune_clicked), p);
  gtk_box_append(GTK_BOX(bar), tune);

  p->status = gtk_label_new("idle");
  gtk_widget_set_hexpand(p->status, TRUE);
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(bar), p->status);
  gtk_box_append(GTK_BOX(box), bar);

  // Messages / Stations / Aircraft.
  GtkWidget *nb = gtk_notebook_new();
  gtk_widget_set_vexpand(nb, TRUE);

  GtkWidget *msg_scroll = make_view(&p->view, &p->buf, TRUE);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(p->buf, &end);
  p->end_mark = gtk_text_buffer_create_mark(p->buf, "hfdl-end", &end, FALSE);
  gtk_notebook_append_page(GTK_NOTEBOOK(nb), msg_scroll, gtk_label_new("Messages"));

  GtkWidget *gs_scroll = make_view(&p->gs_view, &p->gs_buf, FALSE);
  gtk_notebook_append_page(GTK_NOTEBOOK(nb), gs_scroll, gtk_label_new("Stations"));

  GtkWidget *ac_scroll = make_view(&p->ac_view, &p->ac_buf, FALSE);
  gtk_notebook_append_page(GTK_NOTEBOOK(nb), ac_scroll, gtk_label_new("Aircraft"));

  gtk_box_append(GTK_BOX(box), nb);

  presets_rebuild(p);
  if (radio && radio->hfdl_log) gtk_check_button_set_active(GTK_CHECK_BUTTON(logcb), TRUE);

  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);

  return box;
}
