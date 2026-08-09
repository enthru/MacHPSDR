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
#include <time.h>
#include <math.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"

#include "apt_panel.h"
#include "apt_decoder.h"
#include "apt_geo.h"
#include "apt_map.h"
#include "gpu_image.h"
#include "log.h"

extern RADIO *radio;   // global application state (persisted APT settings)

#define REFRESH_MS 400          // ~2.5 fps (APT is 2 lines/s)

// Channel view: the whole 2080-word line, or one of the two 909-word images.
static const char *CHAN_LABELS[] = { "Both", "Channel A", "Channel B" };
#define N_CHAN 3

typedef struct {
  GtkWidget *area;          // image drawing area
  GtkWidget *geo_lbl;       // satellite / TLE age, or why there is no map
  GtkWidget *pos_lbl;       // lat/lon under the pointer
  GtkWidget *trim_spin;
  gboolean   map_on;
  int        map_lines;     // rows the map cache was last given
  GtkWidget *status;        // status label
  GtkWidget *slant_lbl;
  GtkWidget *contrast;      // manual exposure trim
  GtkWidget *bright;
  GtkWidget *folder_btn;    // auto-save destination (tooltip carries the path)
  GdkPixbuf *pb;            // latest decoded image (owned)
  guint      timer;
  char       last_status[64];
  int        last_line;
} AptPanel;

static GdkPixbuf *on_source(gpointer data) {
  AptPanel *p = data;
  return p->pb;
}

// Full-line word of the displayed picture's leftmost column: the View crop is
// what the decoder handed us, and the projection works in full-line words.
static int chan_x0(void) {
  int ch = radio ? radio->apt_channel : 0;
  return (ch == 1) ? 86 : (ch == 2) ? 1126 : 0;
}

// The map is cached in image coordinates; this is the only thing that knows how
// the view is currently scaled and panned.
static gboolean map_xform(double ix, double iy, double *ox, double *oy, gpointer user) {
  return gpu_image_image_to_widget(GPU_IMAGE(user), ix, iy, ox, oy);
}

static void on_overlay(cairo_t *cr, int width, int height, gpointer data) {
  AptPanel *p = data;
  if (!p->map_on || p->pb == NULL || !apt_geo_ready()) return;
  apt_map_draw(cr, width, height, map_xform, p->area);
}

// Where is the pointer on the ground?  This is the readout the whole feature
// exists for — a picture you can ask a question of, rather than look at.
static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer data) {
  AptPanel *p = data;
  if (!p->map_on || p->pb == NULL) return;
  double ix, iy, lat, lon;
  if (apt_geo_ready() &&
      gpu_image_widget_to_image(GPU_IMAGE(p->area), x, y, &ix, &iy) &&
      apt_geo_pixel_to_latlon(iy, chan_x0() + ix, &lat, &lon)) {
    char buf[64];
    g_snprintf(buf, sizeof(buf), "%.2f\u00b0%c  %.2f\u00b0%c",
               fabs(lat), lat >= 0 ? 'N' : 'S', fabs(lon), lon >= 0 ? 'E' : 'W');
    gtk_label_set_text(GTK_LABEL(p->pos_lbl), buf);
  } else {
    gtk_label_set_text(GTK_LABEL(p->pos_lbl), "");
  }
}

// ---- georeferencing --------------------------------------------------------
// Everything the map needs, refreshed on the same tick as the picture: the
// satellite (chosen by where the decoder is listening, never typed twice), the
// decoder's per-row time stamps, and the operator's time trim.
static void geo_tick(AptPanel *p, const apt_status_t *st) {
  if (!p->map_on) return;

  if (apt_geo_satellite() == NULL && st->tuned_hz > 0) apt_geo_select_freq(st->tuned_hz);

  static double rt[2048];
  int n = apt_decoder_get_row_times(rt, (int)G_N_ELEMENTS(rt));
  if (n >= 2) apt_geo_set_row_times(rt, n);

  char buf[160];
  if (apt_geo_satellite() == NULL) {
    g_snprintf(buf, sizeof(buf), "no element set for %.3f MHz",
               st->tuned_hz > 0 ? (double)st->tuned_hz / 1e6 : 0.0);
  } else if (!apt_geo_ready()) {
    g_snprintf(buf, sizeof(buf), "%s — waiting for lines", apt_geo_satellite());
  } else {
    double age = apt_geo_tle_age_days();
    g_snprintf(buf, sizeof(buf), "%s   TLE %+.1f d%s", apt_geo_satellite(), age,
               fabs(age) > 7.0 ? "  (stale)" : "");
  }
  gtk_label_set_text(GTK_LABEL(p->geo_lbl), buf);

  if (p->pb != NULL && apt_geo_ready()) {
    int lines = gdk_pixbuf_get_height(p->pb);
    int width = gdk_pixbuf_get_width(p->pb);
    apt_map_update(lines, chan_x0(), width);
    if (lines != p->map_lines) { p->map_lines = lines; gtk_widget_queue_draw(p->area); }
  }
}

static gboolean tick(gpointer data) {
  AptPanel *p = data;
  apt_status_t st;
  apt_decoder_get_status(&st);

  if (strcmp(st.status, p->last_status) != 0 || st.lines != p->last_line) {
    char buf[192];
    // The tuned frequency belongs in the readout: a decoder listening somewhere
    // other than the operator believes looks exactly like a dead pass.
    if (st.tuned_hz > 0)
      g_snprintf(buf, sizeof(buf), "%s   %d lines   %.4f MHz",
                 st.status, st.lines, (double)st.tuned_hz / 1e6);
    else
      g_snprintf(buf, sizeof(buf), "%s   %d lines", st.status, st.lines);
    gtk_label_set_text(GTK_LABEL(p->status), buf);
    g_strlcpy(p->last_status, st.status, sizeof(p->last_status));
    p->last_line = st.lines;
  }

  GdkPixbuf *np = apt_decoder_get_image();
  if (np != NULL) {
    if (p->pb != NULL) g_object_unref(p->pb);
    p->pb = np;
    gtk_widget_queue_draw(p->area);
  }
  geo_tick(p, &st);
  return G_SOURCE_CONTINUE;
}

static void chan_changed(GtkDropDown *c, GParamSpec *ps, gpointer data) {
  int idx = (int)gtk_drop_down_get_selected(c);
  if (idx >= 0 && idx < N_CHAN) {
    apt_decoder_set_channel(idx);
    if (radio) radio->apt_channel = idx;
  }
}

static void update_slant(AptPanel *p) {
  char b[32];
  g_snprintf(b, sizeof(b), "slant %+.0f ppm", apt_decoder_get_slant());
  gtk_label_set_text(GTK_LABEL(p->slant_lbl), b);
}
static void slant_minus(GtkButton *b, gpointer data) {
  AptPanel *p = data; apt_decoder_adjust_slant(-20.0); update_slant(p);
}
static void slant_plus(GtkButton *b, gpointer data) {
  AptPanel *p = data; apt_decoder_adjust_slant(+20.0); update_slant(p);
}
static void clear_clicked(GtkButton *b, gpointer data) { apt_decoder_reset(); }

// Where auto-saved passes go (and where the Save dialog starts).
static void save_folder(char *out, gsize len) {
  if (radio != NULL && radio->apt_save_dir[0] != '\0')
    g_strlcpy(out, radio->apt_save_dir, len);
  else
    g_snprintf(out, len, "%s/.local/share/machpsdr/apt", g_get_home_dir());
}

// GTK4: GtkFileDialog is async — the chosen path arrives here (mirrors SSTV).
static void save_done(GObject *src, GAsyncResult *res, gpointer data) {
  GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (gf == NULL) return;                       // cancelled / error
  // Take the picture fresh rather than reusing the panel's: the panel shows the
  // View crop, and a saved pass should keep the sync bars and telemetry wedges.
  GdkPixbuf *pb = apt_decoder_get_full_image();
  char *path = g_file_get_path(gf);
  g_object_unref(gf);
  if (pb != NULL) {
    GError *err = NULL;
    if (gdk_pixbuf_save(pb, path, "png", &err, NULL)) {
      log_info("APT: saved %s\n", path);
    } else {
      log_error("APT: save failed: %s\n", err ? err->message : "?");
      if (err) g_error_free(err);
    }
    g_object_unref(pb);
  }
  g_free(path);
}

static void save_clicked(GtkButton *b, gpointer data) {
  AptPanel *p = data;
  if (p->pb == NULL) return;
  GtkWidget *top = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
  char dir[512];
  save_folder(dir, sizeof(dir));
  g_mkdir_with_parents(dir, 0755);
  time_t now = time(NULL);
  struct tm tmv; gmtime_r(&now, &tmv);
  char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  char name[64]; g_snprintf(name, sizeof(name), "apt_%s.png", ts);

  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Save APT image");
  gtk_file_dialog_set_initial_name(dlg, name);
  GFile *folder = g_file_new_for_path(dir);
  gtk_file_dialog_set_initial_folder(dlg, folder);
  g_object_unref(folder);
  GtkFileFilter *filt = gtk_file_filter_new();
  gtk_file_filter_set_name(filt, "PNG image");
  gtk_file_filter_add_mime_type(filt, "image/png");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filt);
  g_object_unref(filt);
  gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
  g_object_unref(filters);
  gtk_file_dialog_save(dlg, GTK_WINDOW(top), NULL, save_done, p);
  g_object_unref(dlg);
}

// --- exposure + auto-save ---------------------------------------------------
static void levels_changed(GtkRange *r, gpointer data) {
  AptPanel *p = data;
  if (radio == NULL) return;
  radio->apt_contrast = gtk_range_get_value(GTK_RANGE(p->contrast));
  radio->apt_brightness = gtk_range_get_value(GTK_RANGE(p->bright));
  apt_decoder_set_levels(radio->apt_contrast, radio->apt_brightness);
  // The trim is applied where the picture is handed out, so the whole image
  // re-maps on the next poll — no need to wait for new lines.
  gtk_widget_queue_draw(p->area);
}

static void autosave_toggled(GtkCheckButton *b, gpointer data) {
  gboolean on = gtk_check_button_get_active(b);
  if (radio) radio->apt_autosave = on;
  char dir[512];
  save_folder(dir, sizeof(dir));
  apt_decoder_set_autosave(on, dir);
}

// The dialog outlives the click, and the panel can be closed while it is open,
// so this holds a ref on the button rather than the panel struct.
static void folder_done(GObject *src, GAsyncResult *res, gpointer data) {
  GtkWidget *btn = data;
  GFile *gf = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (gf != NULL) {
    char *path = g_file_get_path(gf);
    g_object_unref(gf);
    if (path != NULL && radio != NULL) {
      g_strlcpy(radio->apt_save_dir, path, sizeof(radio->apt_save_dir));
      apt_decoder_set_autosave(radio->apt_autosave, radio->apt_save_dir);
      gtk_widget_set_tooltip_text(btn, radio->apt_save_dir);
      log_info("APT: save folder %s\n", radio->apt_save_dir);
    }
    g_free(path);
  }
  g_object_unref(btn);
}

static void folder_clicked(GtkButton *b, gpointer data) {
  GtkWidget *top = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
  char dir[512];
  save_folder(dir, sizeof(dir));
  g_mkdir_with_parents(dir, 0755);
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Folder for saved APT passes");
  GFile *folder = g_file_new_for_path(dir);
  gtk_file_dialog_set_initial_folder(dlg, folder);
  g_object_unref(folder);
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(top), NULL, folder_done,
                                g_object_ref(b));
  g_object_unref(dlg);
}

// ---- map controls ----------------------------------------------------------
static void map_toggled(GtkCheckButton *b, gpointer data) {
  AptPanel *p = data;
  p->map_on = gtk_check_button_get_active(b);
  if (radio) radio->apt_map = p->map_on;
  if (p->map_on && radio) {
    char *path = (radio->apt_tle_path[0] != '\0') ? g_strdup(radio->apt_tle_path)
                                                  : apt_geo_default_tle_path();
    char *err = NULL;
    if (!apt_geo_load_tle(path, &err))
      gtk_label_set_text(GTK_LABEL(p->geo_lbl), err ? err : "no element sets");
    g_free(err);
    g_free(path);
  } else {
    apt_map_invalidate();
    gtk_label_set_text(GTK_LABEL(p->pos_lbl), "");
  }
  p->map_lines = -1;
  gtk_widget_queue_draw(p->area);
}

static void trim_changed(GtkSpinButton *sb, gpointer data) {
  AptPanel *p = data;
  double v = gtk_spin_button_get_value(sb);
  if (radio) radio->apt_time_trim = v;
  apt_geo_set_time_offset(v);
  p->map_lines = -1;
  gtk_widget_queue_draw(p->area);
}

static void tle_done(GObject *src, GAsyncResult *res, gpointer data) {
  AptPanel *p = data;
  GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (f == NULL) return;                       // cancelled
  char *path = g_file_get_path(f);
  if (path != NULL) {
    char *err = NULL;
    if (apt_geo_load_tle(path, &err)) {
      if (radio) g_strlcpy(radio->apt_tle_path, path, sizeof(radio->apt_tle_path));
      p->map_lines = -1;
      gtk_widget_queue_draw(p->area);
    } else {
      gtk_label_set_text(GTK_LABEL(p->geo_lbl), err ? err : "no element sets");
    }
    g_free(err);
    g_free(path);
  }
  g_object_unref(f);
}

static void tle_clicked(GtkButton *b, gpointer data) {
  AptPanel *p = data;
  GtkFileDialog *d = gtk_file_dialog_new();
  gtk_file_dialog_set_title(d, "Two-line element sets");
  if (radio && radio->apt_tle_path[0] != '\0') {
    GFile *f = g_file_new_for_path(radio->apt_tle_path);
    gtk_file_dialog_set_initial_file(d, f);
    g_object_unref(f);
  }
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(b));
  gtk_file_dialog_open(d, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, tle_done, p);
  g_object_unref(d);
}

static void on_destroy(GtkWidget *w, gpointer data) {
  AptPanel *p = data;
  if (p->timer) g_source_remove(p->timer);
  if (p->pb) g_object_unref(p->pb);
  g_free(p);
}

GtkWidget *apt_panel_create(void) {
  AptPanel *p = g_new0(AptPanel, 1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  // Initialise from the persisted setting and push it into the decoder, so the
  // combo and the decode agree before the operator touches anything.
  int chan0 = radio ? radio->apt_channel : 0;
  if (chan0 < 0 || chan0 >= N_CHAN) chan0 = 0;
  apt_decoder_set_channel(chan0);

  gtk_box_append(GTK_BOX(bar), gtk_label_new("View:"));
  GtkStringList *ch_sl = gtk_string_list_new(NULL);
  for (int i = 0; i < N_CHAN; i++) gtk_string_list_append(ch_sl, CHAN_LABELS[i]);
  GtkWidget *ch = gtk_drop_down_new(G_LIST_MODEL(ch_sl), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(ch), chan0);
  g_signal_connect(ch, "notify::selected", G_CALLBACK(chan_changed), p);
  gtk_box_append(GTK_BOX(bar), ch);

  GtkWidget *sm = gtk_button_new_with_label("Slant −");
  GtkWidget *sp = gtk_button_new_with_label("Slant +");
  p->slant_lbl = gtk_label_new("slant +0 ppm");
  g_signal_connect(sm, "clicked", G_CALLBACK(slant_minus), p);
  g_signal_connect(sp, "clicked", G_CALLBACK(slant_plus), p);
  gtk_box_append(GTK_BOX(bar), sm);
  gtk_box_append(GTK_BOX(bar), p->slant_lbl);
  gtk_box_append(GTK_BOX(bar), sp);

  GtkWidget *clr = gtk_button_new_with_label("Clear");
  GtkWidget *save = gtk_button_new_with_label("Save");
  g_signal_connect(clr, "clicked", G_CALLBACK(clear_clicked), p);
  g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), p);
  gtk_box_append(GTK_BOX(bar), clr);
  gtk_box_append(GTK_BOX(bar), save);
  gtk_box_append(GTK_BOX(box), bar);

  // Exposure + auto-save row.  The automatic per-line levels track the picture,
  // but a hazy pass or a bright cloud deck still wants a manual nudge, and there
  // was no way to give it one short of a rebuild.
  GtkWidget *ebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  double c0 = (radio && radio->apt_contrast > 0.0) ? radio->apt_contrast : 1.0;
  double b0 = radio ? radio->apt_brightness : 0.0;
  apt_decoder_set_levels(c0, b0);

  gtk_box_append(GTK_BOX(ebar), gtk_label_new("Contrast:"));
  p->contrast = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.3, 3.0, 0.05);
  gtk_range_set_value(GTK_RANGE(p->contrast), c0);
  gtk_scale_set_draw_value(GTK_SCALE(p->contrast), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(p->contrast), GTK_POS_RIGHT);
  gtk_scale_set_digits(GTK_SCALE(p->contrast), 2);
  gtk_widget_set_size_request(p->contrast, 130, -1);
  g_signal_connect(p->contrast, "value-changed", G_CALLBACK(levels_changed), p);
  gtk_box_append(GTK_BOX(ebar), p->contrast);

  gtk_box_append(GTK_BOX(ebar), gtk_label_new("Brightness:"));
  p->bright = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -100.0, 100.0, 5.0);
  gtk_range_set_value(GTK_RANGE(p->bright), b0);
  gtk_scale_set_draw_value(GTK_SCALE(p->bright), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(p->bright), GTK_POS_RIGHT);
  gtk_scale_set_digits(GTK_SCALE(p->bright), 0);
  gtk_widget_set_size_request(p->bright, 130, -1);
  g_signal_connect(p->bright, "value-changed", G_CALLBACK(levels_changed), p);
  gtk_box_append(GTK_BOX(ebar), p->bright);

  gboolean as0 = radio ? radio->apt_autosave : TRUE;
  GtkWidget *asb = gtk_check_button_new_with_label("Auto-save pass");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(asb), as0);
  gtk_widget_set_tooltip_text(asb,
      "Write the picture to disk when the pass ends (retune, 30 s without sync, "
      "or the decoder switched off). Clear does not save.");
  g_signal_connect(asb, "toggled", G_CALLBACK(autosave_toggled), p);
  gtk_box_append(GTK_BOX(ebar), asb);

  p->folder_btn = gtk_button_new_with_label("Folder…");
  char dir0[512];
  save_folder(dir0, sizeof(dir0));
  gtk_widget_set_tooltip_text(p->folder_btn, dir0);
  g_signal_connect(p->folder_btn, "clicked", G_CALLBACK(folder_clicked), p);
  gtk_box_append(GTK_BOX(ebar), p->folder_btn);
  apt_decoder_set_autosave(as0, dir0);
  gtk_box_append(GTK_BOX(box), ebar);

  // Map row.  The satellite is never typed — it comes from where the decoder is
  // listening — so what is left for the operator is the element sets, whether to
  // draw at all, and the time trim, which is the control that actually slides
  // the coastline onto the coast (see apt_geo.h on why the clock is the weak
  // link and the orbit is not).
  GtkWidget *mbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  p->map_on = radio ? radio->apt_map : FALSE;
  p->map_lines = -1;
  GtkWidget *mapb = gtk_check_button_new_with_label("Map");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(mapb), p->map_on);
  gtk_widget_set_tooltip_text(mapb,
      "Draw the coastline, a 10\u00b0 graticule and the ground track over the "
      "picture, and report the position under the pointer. Needs element sets "
      "for the satellite.");
  g_signal_connect(mapb, "toggled", G_CALLBACK(map_toggled), p);
  gtk_box_append(GTK_BOX(mbar), mapb);

  GtkWidget *tleb = gtk_button_new_with_label("TLE\u2026");
  gtk_widget_set_tooltip_text(tleb,
      "Two-line element sets (a celestrak weather.txt will do). Without a file "
      "the default is ~/.local/share/machpsdr/tle.txt.");
  g_signal_connect(tleb, "clicked", G_CALLBACK(tle_clicked), p);
  gtk_box_append(GTK_BOX(mbar), tleb);

  gtk_box_append(GTK_BOX(mbar), gtk_label_new("Time trim:"));
  p->trim_spin = gtk_spin_button_new_with_range(-600.0, 600.0, 0.5);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(p->trim_spin), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->trim_spin), radio ? radio->apt_time_trim : 0.0);
  gtk_widget_set_tooltip_text(p->trim_spin,
      "Seconds added to the capture time. One second is about 7 km along track, "
      "and it absorbs a stale element set, a wrong clock and the audio latency "
      "at once \u2014 nudge it until the coast sits on the coast.");
  g_signal_connect(p->trim_spin, "value-changed", G_CALLBACK(trim_changed), p);
  gtk_box_append(GTK_BOX(mbar), p->trim_spin);

  p->geo_lbl = gtk_label_new("");
  gtk_box_append(GTK_BOX(mbar), p->geo_lbl);
  p->pos_lbl = gtk_label_new("");
  gtk_widget_set_hexpand(p->pos_lbl, TRUE);
  gtk_widget_set_halign(p->pos_lbl, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(mbar), p->pos_lbl);
  gtk_box_append(GTK_BOX(box), mbar);

  if (p->map_on && radio) apt_geo_set_time_offset(radio->apt_time_trim);

  // Image area.  Same treatment as WEFAX: a tall scrolling image fit to the
  // panel width and bottom-anchored (newest lines visible), mip-mapped down so
  // the 2080-px line keeps its detail instead of dropping every other column.
  p->area = gpu_image_new(on_source, p);
  gpu_image_set_fit(GPU_IMAGE(p->area), GPU_FIT_WIDTH_BOTTOM);
  gpu_image_set_filter(GPU_IMAGE(p->area), GSK_SCALING_FILTER_TRILINEAR);
  // A pass is 2080 px wide and grows past a thousand lines; fit-to-width alone
  // shows a thumbnail of it.  Wheel scrolls back through the pass, Ctrl+wheel
  // zooms to full resolution, drag pans, double-click returns to fit.
  gpu_image_set_zoomable(GPU_IMAGE(p->area), TRUE, TRUE);
  gpu_image_set_overlay(GPU_IMAGE(p->area), on_overlay);
  {
    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(on_motion), p);
    gtk_widget_add_controller(p->area, mc);
  }
  gtk_widget_set_size_request(p->area, 400, 80);
  gtk_widget_set_hexpand(p->area, TRUE);
  gtk_widget_set_vexpand(p->area, TRUE);
  gtk_box_append(GTK_BOX(box), p->area);

  GtkWidget *sline = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  p->status = gtk_label_new("Waiting for APT…");
  gtk_widget_set_halign(p->status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(sline), p->status);
  GtkWidget *hint = gtk_label_new("(wheel: scroll · Ctrl+wheel: zoom · drag: pan · double-click: fit)");
  gtk_widget_set_hexpand(hint, TRUE);
  gtk_widget_set_halign(hint, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(sline), hint);
  gtk_box_append(GTK_BOX(box), sline);

  p->last_status[0] = '\0';
  p->last_line = -1;
  p->timer = g_timeout_add(REFRESH_MS, tick, p);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), p);
  return box;
}
