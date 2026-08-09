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

// Orientation: as received / always turned / turned when the orbit says so.
static const char *ROT_LABELS[] = { "As received", "180°", "North up" };
#define N_ROT 3

typedef struct {
  GtkWidget *area;          // image drawing area
  GtkWidget *geo_lbl;       // satellite / TLE age, or why there is no map
  GtkWidget *pos_lbl;       // lat/lon under the pointer
  GtkWidget *trim_spin;
  gboolean   map_on;
  gboolean   flip;          // the decoder is handing the picture over rotated 180°
  int        img_w, img_h;  // ...and its size, which the rotation is about
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

// The map is cached in the picture's OWN (unrotated) coordinates — that is what
// the projection knows about — so a rotated picture is a half turn away from it,
// and this is the one place that has to say so.  It is also the only thing that
// knows how the view is currently scaled and panned.
// Note `w - x`, not `(w-1) - x`: these are continuous coordinates, where pixel i
// occupies [i, i+1) and its centre is i+0.5, so the mirror is about the edge of
// the image and not about the centre of its last pixel.  The index form puts the
// whole overlay one pixel out — small, systematic, and exactly the sort of thing
// that is invisible by eye and obvious in a numeric test.
static gboolean map_xform(double ix, double iy, double *ox, double *oy, gpointer user) {
  AptPanel *p = user;
  if (p->flip) { ix = p->img_w - ix; iy = p->img_h - iy; }
  return gpu_image_image_to_widget(GPU_IMAGE(p->area), ix, iy, ox, oy);
}

static void on_overlay(cairo_t *cr, int width, int height, gpointer data) {
  AptPanel *p = data;
  if (!p->map_on || p->pb == NULL || !apt_geo_ready()) return;
  apt_map_draw(cr, width, height, map_xform, p);
}

// Where is the pointer on the ground?  This is the readout the whole feature
// exists for — a picture you can ask a question of, rather than look at.
static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer data) {
  AptPanel *p = data;
  if (!p->map_on || p->pb == NULL) return;
  double ix, iy, lat, lon;
  gboolean got = apt_geo_ready() &&
                 gpu_image_widget_to_image(GPU_IMAGE(p->area), x, y, &ix, &iy);
  if (got && p->flip) { ix = p->img_w - ix; iy = p->img_h - iy; }
  if (got && apt_geo_pixel_to_latlon(iy, chan_x0() + ix, &lat, &lon)) {
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
  // The feeding itself belongs to radio.c and happens whether or not this panel
  // is open — the north-up rotation has to reach the unattended auto-save too —
  // so all that is left here is what only a panel can do: say what is going on
  // and keep the projection cache in step with the picture.
  radio_apt_geo_pump(radio);
  if (!p->map_on) return;

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
    // Read the rotation back from the decoder rather than deriving it again:
    // it is what actually produced this picture, so the overlay cannot end up a
    // half turn out of step with it.
    p->flip  = apt_decoder_get_flip();
    p->img_w = gdk_pixbuf_get_width(np);
    p->img_h = gdk_pixbuf_get_height(np);
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

// ---- element sets over the network -----------------------------------------
// Hand-feeding a TLE file is the only part of the map the operator has to do
// outside the app, and it is also the part that goes off: element sets age, and
// a week-old set is worth kilometres of error in the overlay.  Celestrak's
// weather group is the public source everyone else uses and carries all three
// APT birds.
//
// Fetched by running curl (or wget), not by linking an HTTP client: celestrak is
// HTTPS-only, so the alternative is a TLS dependency — libsoup or libcurl — for
// one button that downloads 600 bytes.  Both tools are on every macOS and
// virtually every Linux, and if neither is there the operator still has TLE… and
// a file.  Both accept several URLs in one invocation and concatenate the
// replies, which is exactly the multi-satellite TLE file the loader wants.
//
// By CATALOGUE NUMBER rather than by group, though a group would be one request:
// celestrak's `weather` group no longer lists NOAA 15/18/19 at all (checked
// 2026-08-09 — 74 sets, none of them ours), and there is no `noaa` group to fall
// back on.  A group is a curated list that can drop the three satellites this
// decoder exists for; a catalogue number cannot.
#define CELESTRAK_GP "https://celestrak.org/NORAD/elements/gp.php?FORMAT=tle&CATNR="
static const char *TLE_URLS[] = {
  CELESTRAK_GP "25338",   // NOAA 15 — 137.620 MHz
  CELESTRAK_GP "28654",   // NOAA 18 — 137.9125 MHz
  CELESTRAK_GP "33591",   // NOAA 19 — 137.100 MHz
};

// The download is async and the panel can be closed while it is in flight, so
// this holds its own refs on the two widgets it touches (the same shape as
// folder_done above).  A destroyed-but-referenced GtkWidget is still a valid
// object; setting a label on one is a no-op the user never sees.
typedef struct { GtkWidget *lbl; GtkWidget *area; } TleFetch;

static void tle_fetch_free(TleFetch *f) {
  g_object_unref(f->lbl);
  g_object_unref(f->area);
  g_free(f);
}

// Enough of a check that a captive portal's login page, or a celestrak error
// message, cannot overwrite a working element-set file.
static gboolean looks_like_tle(const char *s) {
  gboolean l1 = FALSE, l2 = FALSE;
  for (const char *p = s; p != NULL && *p != '\0'; ) {
    if (p[0] == '1' && p[1] == ' ')      l1 = TRUE;
    else if (p[0] == '2' && p[1] == ' ') l2 = TRUE;
    const char *nl = strchr(p, '\n');
    p = nl ? nl + 1 : NULL;
  }
  return l1 && l2;
}

static void fetch_done(GObject *src, GAsyncResult *res, gpointer data) {
  TleFetch *f = data;
  char *out = NULL;
  GError *err = NULL;

  if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err) ||
      !g_subprocess_get_successful(G_SUBPROCESS(src)) ||
      out == NULL || !looks_like_tle(out)) {
    gtk_label_set_text(GTK_LABEL(f->lbl),
                       err ? err->message : "element-set download failed");
    log_error("APT: TLE download failed: %s\n",
              err ? err->message : "no element sets in the reply");
    if (err) g_error_free(err);
    g_free(out);
    tle_fetch_free(f);
    return;
  }

  // Always to the default path, never over a file the operator chose: this
  // button is "get me the current sets", not "replace whatever I curated".
  char *path = apt_geo_default_tle_path();
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);

  if (!g_file_set_contents(path, out, -1, &err)) {
    gtk_label_set_text(GTK_LABEL(f->lbl), err ? err->message : "cannot write TLE file");
    log_error("APT: %s\n", err ? err->message : "cannot write TLE file");
    if (err) g_error_free(err);
  } else {
    char *lerr = NULL;
    int n = apt_geo_load_tle(path, &lerr);
    if (n > 0) {
      if (radio) g_strlcpy(radio->apt_tle_path, path, sizeof(radio->apt_tle_path));
      char msg[128];
      g_snprintf(msg, sizeof(msg), "%d element sets downloaded", n);
      gtk_label_set_text(GTK_LABEL(f->lbl), msg);
      log_info("APT: %d element set(s) downloaded to %s\n", n, path);
      gtk_widget_queue_draw(f->area);
    } else {
      gtk_label_set_text(GTK_LABEL(f->lbl), lerr ? lerr : "no element sets");
    }
    g_free(lerr);
  }
  g_free(path);
  g_free(out);
  tle_fetch_free(f);
}

static void update_clicked(GtkButton *b, gpointer data) {
  AptPanel *p = data;
  // One URL from the environment replaces the lot — a mirror, a local file
  // server, or a group query if celestrak ever carries these three again.
  const char *env = g_getenv("MACHPSDR_TLE_URL");
  const char *urls[G_N_ELEMENTS(TLE_URLS)];
  unsigned nurl = 0;
  if (env != NULL && env[0] != '\0') urls[nurl++] = env;
  else for (unsigned i = 0; i < G_N_ELEMENTS(TLE_URLS); i++) urls[nurl++] = TLE_URLS[i];

  GPtrArray *av = g_ptr_array_new();
  g_ptr_array_add(av, (char *)"curl");
  g_ptr_array_add(av, (char *)"-fsSL");
  g_ptr_array_add(av, (char *)"--max-time");
  g_ptr_array_add(av, (char *)"20");
  for (unsigned i = 0; i < nurl; i++) g_ptr_array_add(av, (char *)urls[i]);
  g_ptr_array_add(av, NULL);

  GError *err = NULL;
  const GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE;
  GSubprocess *proc = g_subprocess_newv((const gchar * const *)av->pdata, flags, &err);
  if (proc == NULL) {
    g_clear_error(&err);
    g_ptr_array_set_size(av, 0);
    g_ptr_array_add(av, (char *)"wget");
    g_ptr_array_add(av, (char *)"-qO-");
    g_ptr_array_add(av, (char *)"--timeout=20");
    for (unsigned i = 0; i < nurl; i++) g_ptr_array_add(av, (char *)urls[i]);
    g_ptr_array_add(av, NULL);
    proc = g_subprocess_newv((const gchar * const *)av->pdata, flags, &err);
  }
  g_ptr_array_free(av, TRUE);
  if (proc == NULL) {
    gtk_label_set_text(GTK_LABEL(p->geo_lbl), "no curl or wget to download with");
    log_error("APT: cannot download element sets: %s\n", err ? err->message : "?");
    g_clear_error(&err);
    return;
  }

  gtk_label_set_text(GTK_LABEL(p->geo_lbl), "downloading element sets…");
  TleFetch *f = g_new0(TleFetch, 1);
  f->lbl  = g_object_ref(p->geo_lbl);
  f->area = g_object_ref(p->area);
  g_subprocess_communicate_utf8_async(proc, NULL, NULL, fetch_done, f);
  g_object_unref(proc);
}

// ---- map controls ----------------------------------------------------------

// Orientation.  An APT picture is north-up only because the satellite happened
// to be going south; on a northbound pass the same scan geometry writes it
// upside down, and the cure — in wxtoimg and noaa-apt alike — is a half turn.
// "North up" asks the orbit which it is (and does nothing without element sets,
// rather than guessing); "180°" is the manual override for when there is no TLE.
static void rotate_changed(GtkDropDown *c, GParamSpec *ps, gpointer data) {
  AptPanel *p = data;
  if (radio == NULL) return;
  radio->apt_rotate = (int)gtk_drop_down_get_selected(c);
  radio_apt_settings_sync(radio);      // may need element sets it has not loaded
  p->map_lines = -1;
  gtk_widget_queue_draw(p->area);
}
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

  gtk_box_append(GTK_BOX(bar), gtk_label_new("Rotate:"));
  GtkStringList *rot_sl = gtk_string_list_new(NULL);
  for (int i = 0; i < N_ROT; i++) gtk_string_list_append(rot_sl, ROT_LABELS[i]);
  GtkWidget *rot = gtk_drop_down_new(G_LIST_MODEL(rot_sl), NULL);
  int rot0 = radio ? radio->apt_rotate : 0;
  if (rot0 < 0 || rot0 >= N_ROT) rot0 = 0;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(rot), rot0);
  gtk_widget_set_tooltip_text(rot,
      "A pass flown south→north writes the picture upside down. \"North up\" "
      "asks the orbit which way this one went (element sets required) and turns "
      "it if needed; \"180°\" always turns it. The rotation follows the "
      "picture into Save and auto-save — but note that while a rotated pass "
      "is still being decoded the newest lines arrive at the TOP.");
  g_signal_connect(rot, "notify::selected", G_CALLBACK(rotate_changed), p);
  gtk_box_append(GTK_BOX(bar), rot);

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

  GtkWidget *updb = gtk_button_new_with_label("Update");
  gtk_widget_set_tooltip_text(updb,
      "Download the current weather-satellite element sets from celestrak.org "
      "to ~/.local/share/machpsdr/tle.txt. Sets more than about a week old cost "
      "the overlay kilometres of accuracy.");
  g_signal_connect(updb, "clicked", G_CALLBACK(update_clicked), p);
  gtk_box_append(GTK_BOX(mbar), updb);

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
