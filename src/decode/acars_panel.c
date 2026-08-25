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

/*
 * Embedded VHF ACARS panel — the same shape as hfdl_panel.c (Messages tab plus
 * a live table, Clear / Log / Scan / Tune toolbar), because it is the same job
 * on a different band and an operator who knows one should not have to learn
 * the other.  Differences: there are no ground stations to list (VHF ACARS has
 * no equivalent of the HFDL system table), and the channel drop-down is the
 * fixed published channel list rather than one learned over the air.
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

#include "acars_panel.h"
#include "acars_decoder.h"
#include "log.h"

#define REFRESH_MS      300
#define DRAIN_CHUNK     4096
#define MAX_LOG_LINES   4000
#define TRIM_LINES      1000
#define MAX_AC          64

typedef struct {
  GtkWidget     *status;
  GtkWidget     *view;
  GtkTextBuffer *buf;
  GtkTextMark   *end_mark;
  GtkWidget     *ac_view;
  GtkTextBuffer *ac_buf;
  GtkWidget     *presets;
  FILE          *logf;
  guint          timer;
} AcarsPanel;

// --- message log file -------------------------------------------------------

static void log_path(char *out, size_t len) {
  g_snprintf(out, len, "%s/.local/share/machpsdr/acars_log.txt", g_get_home_dir());
}

static void log_close(AcarsPanel *p) {
  if (p->logf) { fclose(p->logf); p->logf = NULL; }
}

static gboolean log_open(AcarsPanel *p) {
  if (p->logf) return TRUE;
  char path[512], dir[512];
  g_snprintf(dir, sizeof(dir), "%s/.local/share/machpsdr", g_get_home_dir());
  g_mkdir_with_parents(dir, 0755);
  log_path(path, sizeof(path));
  p->logf = fopen(path, "a");
  if (p->logf == NULL) { log_error("acars: cannot open %s for logging\n", path); return FALSE; }
  GDateTime *now = g_date_time_new_now_utc();
  char *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%SZ");
  fprintf(p->logf, "\n===== ACARS session started %s =====\n", ts);
  fflush(p->logf);
  g_free(ts);
  g_date_time_unref(now);
  log_info("acars: logging messages to %s\n", path);
  return TRUE;
}

static void log_toggled(GtkCheckButton *b, gpointer data) {
  AcarsPanel *p = data;
  gboolean on = gtk_check_button_get_active(b);
  if (on) {
    if (!log_open(p)) { gtk_check_button_set_active(b, FALSE); return; }
  } else {
    log_close(p);
  }
  if (radio) radio->acars_log = on;
}

static void scan_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  acars_decoder_set_scan(on);
  if (radio) radio->acars_scan = on;
}

// --- channel presets --------------------------------------------------------

static void tune_clicked(GtkButton *b, gpointer data) {
  AcarsPanel *p = data;
  if (radio == NULL || radio->active_receiver == NULL) return;
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->presets));
  if (sel == GTK_INVALID_LIST_POSITION) return;
  const ACARS_CHANNEL_INFO *ch = acars_channel_at((int)sel);
  if (ch == NULL) return;

  RECEIVER *rx = radio->active_receiver;
  long long f = (long long)ch->khz * 1000LL;
  // As in the HFDL panel: the decoder follows the CTUN/freetune cursor, so with
  // either of those on it is the cursor that moves and the operator keeps the
  // view they had — the receiver itself only moves when the channel is outside
  // the passband.
  if (rx->ctun || rx->freetune) {
    if (llabs(f - (long long)rx->frequency_a) < (long long)(rx->sample_rate / 2)) {
      rx->ctun_frequency = f;
    } else {
      rx->frequency_a    = f;
      rx->ctun_frequency = f;
    }
  } else {
    rx->frequency_a    = f;
    rx->ctun_frequency = f;
  }
  // AM is what an ACARS channel is listened to in.  The decode itself does not
  // care (it takes raw I/Q and runs its own AM detector), but leaving the
  // receiver in some unrelated mode would only confuse the ear.
  if (rx->mode_a != AM) receiver_mode_changed(rx, AM);
  frequency_changed(rx);
  log_info("acars: tuned to %u kHz\n", ch->khz);
}

// --- aircraft table ----------------------------------------------------------

static void age_str(char *out, size_t len, gint64 t_us, gint64 now_us) {
  if (t_us == 0) { g_strlcpy(out, "-", len); return; }
  gint64 s = (now_us - t_us) / G_USEC_PER_SEC;
  if (s < 60)        g_snprintf(out, len, "%llds", (long long)s);
  else if (s < 3600) g_snprintf(out, len, "%lldm", (long long)(s / 60));
  else               g_snprintf(out, len, "%lldh", (long long)(s / 3600));
}

static void refresh_aircraft(AcarsPanel *p) {
  ACARS_AC_INFO ac[MAX_AC];
  int n = acars_decoder_ac_list(ac, MAX_AC);
  gint64 now = g_get_monotonic_time();

  GString *s = g_string_new(NULL);
  g_string_append(s, "Reg      Flight   Lbl  Channel      Heard  Msgs\n");
  for (int i = 0; i < n; i++) {
    char age[16], chan[16];
    age_str(age, sizeof(age), ac[i].last_heard_us, now);
    if (ac[i].khz) g_snprintf(chan, sizeof(chan), "%u.%03u MHz", ac[i].khz / 1000,
                              ac[i].khz % 1000);
    else           g_strlcpy(chan, "-", sizeof(chan));
    g_string_append_printf(s, "%-8s %-8s %-3s  %-12s %5s %5d\n",
                           ac[i].reg,
                           ac[i].flight[0] ? ac[i].flight : "-",
                           ac[i].label[0] ? ac[i].label : "-",
                           chan, age, ac[i].msgs);
  }
  if (n == 0) g_string_append(s, "(no aircraft heard yet)\n");
  gtk_text_buffer_set_text(p->ac_buf, s->str, -1);
  g_string_free(s, TRUE);
}

static void trim_scrollback(AcarsPanel *p) {
  int lines = gtk_text_buffer_get_line_count(p->buf);
  if (lines <= MAX_LOG_LINES) return;
  GtkTextIter start, cut;
  gtk_text_buffer_get_start_iter(p->buf, &start);
  gtk_text_buffer_get_iter_at_line(p->buf, &cut, TRIM_LINES);
  gtk_text_buffer_delete(p->buf, &start, &cut);
}

static gboolean tick(gpointer data) {
  AcarsPanel *p = data;
  char chunk[DRAIN_CHUNK];
  int n = acars_decoder_get_messages(chunk, sizeof(chunk));
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

  refresh_aircraft(p);

  gboolean listening; int rate; glong msgs;
  acars_decoder_get_status(&listening, &rate, &msgs);
  double level = acars_decoder_get_level_db();
  glong bad = acars_decoder_get_bad_count();
  int nch = acars_decoder_channel_count();
  char buf[200];
  if (listening && nch > 1)
    g_snprintf(buf, sizeof(buf),
               "signal %.0f dB \xC2\xB7 %d kHz \xC2\xB7 %ld message(s) \xC2\xB7 %ld bad \xC2\xB7 %d channels",
               level, rate / 1000, msgs, bad, nch);
  else if (listening)
    g_snprintf(buf, sizeof(buf),
               "signal %.0f dB \xC2\xB7 %d kHz \xC2\xB7 %ld message(s) \xC2\xB7 %ld bad",
               level, rate / 1000, msgs, bad);
  else
    g_snprintf(buf, sizeof(buf), "idle");
  gtk_label_set_text(GTK_LABEL(p->status), buf);
  return G_SOURCE_CONTINUE;
}

static void clear_clicked(GtkButton *b, gpointer data) {
  AcarsPanel *p = data;
  gtk_text_buffer_set_text(p->buf, "", 0);
  acars_decoder_reset();
}

static void on_destroy(GtkWidget *w, gpointer data) {
  AcarsPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  log_close(p);
  g_free(p);
}

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
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);

  *view_out = view;
  *buf_out  = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  return scroll;
}

GtkWidget *acars_panel_create(void) {
  AcarsPanel *p = g_new0(AcarsPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  // The panel is stock GTK widgets, which paint themselves from the platform
  // theme rather than the skin -- see the .decode-panel block in css.c.
  gtk_widget_add_css_class(box, "decode-panel");
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append(GTK_BOX(bar), gtk_label_new("ACARS"));
  GtkWidget *clr = gtk_button_new_with_label("Clear");
  g_signal_connect(clr, "clicked", G_CALLBACK(clear_clicked), p);
  gtk_box_append(GTK_BOX(bar), clr);

  GtkWidget *logcb = gtk_check_button_new_with_label("Log");
  {
    char path[512], tip[600];
    log_path(path, sizeof(path));
    g_snprintf(tip, sizeof(tip), "Append every decoded message to %s", path);
    gtk_widget_set_tooltip_text(logcb, tip);
  }
  g_signal_connect(logcb, "toggled", G_CALLBACK(log_toggled), p);
  gtk_box_append(GTK_BOX(bar), logcb);

  GtkWidget *scancb = gtk_check_button_new_with_label("Scan band");
  gtk_widget_set_tooltip_text(scancb,
    "Decode every known ACARS channel inside the receiver passband, not just the "
    "one under the cursor \xE2\x80\x94 the channels are 25 kHz apart, so a wide "
    "receiver holds several at once.");
  g_signal_connect(scancb, "toggled", G_CALLBACK(scan_toggled), p);
  gtk_box_append(GTK_BOX(bar), scancb);

  GtkStringList *chans = gtk_string_list_new(NULL);
  for (int i = 0; i < acars_channel_count(); i++) {
    const ACARS_CHANNEL_INFO *c = acars_channel_at(i);
    char label[64];
    g_snprintf(label, sizeof(label), "%u.%03u MHz \xE2\x80\x93 %s",
               c->khz / 1000, c->khz % 1000, c->region);
    gtk_string_list_append(chans, label);
  }
  p->presets = gtk_drop_down_new(G_LIST_MODEL(chans), NULL);
  gtk_widget_set_tooltip_text(p->presets,
    "Published VHF ACARS channels. Tune puts the receiver (or, with CTUN, just "
    "the cursor) on the channel in AM.");
  gtk_box_append(GTK_BOX(bar), p->presets);
  GtkWidget *tune = gtk_button_new_with_label("Tune");
  g_signal_connect(tune, "clicked", G_CALLBACK(tune_clicked), p);
  gtk_box_append(GTK_BOX(bar), tune);

  p->status = gtk_label_new("idle");
  gtk_widget_set_hexpand(p->status, TRUE);
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(bar), p->status);
  gtk_box_append(GTK_BOX(box), bar);

  GtkWidget *nb = gtk_notebook_new();
  gtk_widget_set_vexpand(nb, TRUE);

  GtkWidget *msg_scroll = make_view(&p->view, &p->buf, TRUE);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(p->buf, &end);
  p->end_mark = gtk_text_buffer_create_mark(p->buf, "acars-end", &end, FALSE);
  gtk_notebook_append_page(GTK_NOTEBOOK(nb), msg_scroll, gtk_label_new("Messages"));

  GtkWidget *ac_scroll = make_view(&p->ac_view, &p->ac_buf, FALSE);
  gtk_notebook_append_page(GTK_NOTEBOOK(nb), ac_scroll, gtk_label_new("Aircraft"));

  gtk_box_append(GTK_BOX(box), nb);

  if (radio && radio->acars_log)  gtk_check_button_set_active(GTK_CHECK_BUTTON(logcb), TRUE);
  if (radio && radio->acars_scan) gtk_check_button_set_active(GTK_CHECK_BUTTON(scancb), TRUE);

  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);

  return box;
}
