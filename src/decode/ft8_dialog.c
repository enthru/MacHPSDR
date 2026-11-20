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
#include <string.h>
#include <ctype.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "ft8_dialog.h"
#include "ft8_dxcc.h"
#include "ft8_panel.h"

// Copy src into dst uppercased (callsigns and grids are conventionally upper).
static void upper_copy(char *dst, size_t dstsz, const char *src) {
  size_t i=0;
  for(; src[i] && i<dstsz-1; i++) dst[i]=(char)toupper((unsigned char)src[i]);
  dst[i]='\0';
}

// Reflect the uppercased value back into the entry so the field always displays
// upper case as the user types.  Blocks `cb` around the set to avoid recursion
// and preserves the caret position.
static void reflect_upper(GtkWidget *w, const char *up, GCallback cb, gpointer data) {
  if (strcmp(gtk_editable_get_text(GTK_EDITABLE(w)), up) == 0) return;
  gint pos = gtk_editable_get_position(GTK_EDITABLE(w));
  g_signal_handlers_block_by_func(w, (gpointer)cb, data);
  gtk_editable_set_text(GTK_EDITABLE(w), up);
  gtk_editable_set_position(GTK_EDITABLE(w), pos);
  g_signal_handlers_unblock_by_func(w, (gpointer)cb, data);
}
static void call_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  upper_copy(r->station_call,sizeof(r->station_call),gtk_editable_get_text(GTK_EDITABLE(w)));
  reflect_upper(w, r->station_call, G_CALLBACK(call_cb), data);
}
static void grid_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  upper_copy(r->station_grid,sizeof(r->station_grid),gtk_editable_get_text(GTK_EDITABLE(w)));
  reflect_upper(w, r->station_grid, G_CALLBACK(grid_cb), data);
}
static void udp_enable_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->ft8_log_udp=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
}
static void udp_host_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  g_strlcpy(r->ft8_log_udp_host,gtk_editable_get_text(GTK_EDITABLE(w)),sizeof(r->ft8_log_udp_host));
}
static void udp_port_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->ft8_log_udp_port=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
}
static void pskr_enable_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->ft8_pskr=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
}

// ---- Decode-list colours ----
// Each button writes "#rrggbb" into its radio->ft8_col_* field (the field is the
// signal's user data) and the open panel repaints at once.
#define FT8_NCOLORS 4
typedef struct { GtkWidget *btn[FT8_NCOLORS]; const char *def[FT8_NCOLORS]; } ft8_colors_ctx;

static void color_cb(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  const GdkRGBA *c=gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(obj));
  char *s=gdk_rgba_to_string(c);
  ft8_color_set((char *)data,s,"#000000");
  g_free(s);
  ft8_panel_colors_changed();
}
static GtkWidget *color_button(char *field) {
  GdkRGBA c;
  if(!gdk_rgba_parse(&c,field)) gdk_rgba_parse(&c,"#000000");
  GtkWidget *b=gtk_color_dialog_button_new(gtk_color_dialog_new());  // takes ownership
  gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(b),&c);
  gtk_widget_set_halign(b,GTK_ALIGN_START);
  g_signal_connect(b,"notify::rgba",G_CALLBACK(color_cb),field);
  return b;
}
// Reset goes through the buttons, so each one's notify handler stores the value.
static void colors_reset_cb(GtkButton *b, gpointer data) {
  ft8_colors_ctx *c=data;
  for(int i=0;i<FT8_NCOLORS;i++) {
    GdkRGBA rgba;
    gdk_rgba_parse(&rgba,c->def[i]);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(c->btn[i]),&rgba);
  }
}

// One-line-ish status of the loaded DXCC country file.
static void dxcc_status_text(char *buf, size_t n) {
  int c = ft8_dxcc_count();
  const char *p = ft8_dxcc_path();
  if (c > 0)
    snprintf(buf, n, "Loaded %d DXCC entities from:\n%s", c, p ? p : "?");
  else
    snprintf(buf, n, "cty.dat not found. Put it next to the app, in\n"
                     "~/.local/share/machpsdr/, or set $MACHPSDR_CTY, then Reload.");
}
// Reload cty.dat with a bit of visible progress feedback. The parse itself is
// synchronous and near-instant, so there is nothing real to measure; instead we
// disable the button, show a pulsing progress bar, and run the reload one idle
// tick later (so the "Reloading…" state actually paints before the GTK thread
// blocks on the parse). When done the bar is hidden and replaced, in the same
// spot, by a coloured result label (green "✓ Loaded N entities" on success,
// red "✗ cty.dat not found" on failure) that stays until the next reload.
typedef struct { GtkWidget *button, *bar, *result, *status; guint pulse_id; } dxcc_reload_ctx;

// GClosureNotify-signature wrapper for freeing the closure data: casting g_free
// (void(*)(void*)) straight to GClosureNotify (void(*)(void*,GClosure*)) is an
// incompatible-function-type cast that clang flags; this matches the signature.
static void free_closure_data(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

// The progress bar is going away: disarm the pulse timer that draws it, and
// clear the stored id so nothing removes a source GLib has already dropped.
static void dxcc_reload_stop(GtkWidget *w, gpointer p) {
  dxcc_reload_ctx *c = p;
  if (c->pulse_id) { g_source_remove(c->pulse_id); c->pulse_id = 0; }
}

static gboolean dxcc_reload_pulse(gpointer p) {
  gtk_progress_bar_pulse(GTK_PROGRESS_BAR(((dxcc_reload_ctx *)p)->bar));
  return G_SOURCE_CONTINUE;
}

static gboolean dxcc_reload_run(gpointer p) {
  dxcc_reload_ctx *c = p;
  int n = ft8_dxcc_reload();
  if (c->pulse_id) { g_source_remove(c->pulse_id); c->pulse_id = 0; }
  gtk_widget_set_visible(c->bar, FALSE);

  // Inline result in place of the progress bar: green tick / red cross, using
  // the same Pango-markup colouring style the rest of the app uses.
  char m[128];
  if (n > 0)
    snprintf(m, sizeof(m),
             "<span foreground=\"#33aa33\">✓ Loaded %d entities</span>", n);
  else
    snprintf(m, sizeof(m),
             "<span foreground=\"#D94545\">✗ cty.dat not found</span>");
  gtk_label_set_markup(GTK_LABEL(c->result), m);
  gtk_widget_set_visible(c->result, TRUE);

  char buf[1200];
  dxcc_status_text(buf, sizeof(buf));
  gtk_label_set_text(GTK_LABEL(c->status), buf);
  gtk_widget_set_sensitive(c->button, TRUE);
  return G_SOURCE_REMOVE;
}

static void dxcc_reload_cb(GtkWidget *w, gpointer data) {
  dxcc_reload_ctx *c = data;
  c->button = w;
  gtk_widget_set_sensitive(w, FALSE);
  gtk_widget_set_visible(c->result, FALSE);
  gtk_widget_set_visible(c->bar, TRUE);
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(c->bar), 0.0);
  gtk_progress_bar_pulse(GTK_PROGRESS_BAR(c->bar));
  if (!c->pulse_id) c->pulse_id = g_timeout_add(80, dxcc_reload_pulse, c);
  g_idle_add(dxcc_reload_run, c);
}

GtkWidget *create_ft8_dialog(RADIO *r) {
  // ---- Station identity ----
  GtkWidget *frame=gtk_frame_new("Station");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);

  GtkWidget *info=gtk_label_new("Your callsign and Maidenhead grid, used for FT8 TX and\n"
                                "auto-QSO. Set these before transmitting.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12);
  gtk_grid_attach(GTK_GRID(grid),info,0,0,2,1);

  GtkWidget *call_lbl=gtk_label_new("Callsign:");
  gtk_widget_set_halign(call_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),call_lbl,0,1,1,1);
  GtkWidget *call=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(call),sizeof(r->station_call)-1);
  gtk_editable_set_width_chars(GTK_EDITABLE(call),12);
  gtk_editable_set_text(GTK_EDITABLE(call),r->station_call);
  gtk_grid_attach(GTK_GRID(grid),call,1,1,1,1);
  g_signal_connect(call,"changed",G_CALLBACK(call_cb),r);

  GtkWidget *grid_lbl=gtk_label_new("Grid:");
  gtk_widget_set_halign(grid_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),grid_lbl,0,2,1,1);
  GtkWidget *loc=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(loc),sizeof(r->station_grid)-1);
  gtk_editable_set_width_chars(GTK_EDITABLE(loc),8);
  gtk_editable_set_text(GTK_EDITABLE(loc),r->station_grid);
  gtk_grid_attach(GTK_GRID(grid),loc,1,2,1,1);
  g_signal_connect(loc,"changed",G_CALLBACK(grid_cb),r);

  // ---- DXCC country file (cty.dat) ----
  GtkWidget *dframe=gtk_frame_new("DXCC (new-one highlight)");
  GtkWidget *dgrid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(dgrid),5);
  gtk_grid_set_row_spacing(GTK_GRID(dgrid),5);
  sui_style_group(dgrid);
  gtk_frame_set_child(GTK_FRAME(dframe),dgrid);

  GtkWidget *dinfo=gtk_label_new("Highlights decoded stations whose DXCC entity you have not\n"
                                 "worked before, resolved from AD1C's cty.dat country file.");
  gtk_widget_set_halign(dinfo,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(dinfo,12);
  gtk_grid_attach(GTK_GRID(dgrid),dinfo,0,0,2,1);

  char dbuf[1200];
  dxcc_status_text(dbuf,sizeof(dbuf));
  GtkWidget *dstatus=gtk_label_new(dbuf);
  gtk_widget_set_halign(dstatus,GTK_ALIGN_START);
  gtk_label_set_selectable(GTK_LABEL(dstatus),TRUE);
  gtk_grid_attach(GTK_GRID(dgrid),dstatus,0,1,2,1);

  GtkWidget *dreload=gtk_button_new_with_label("Reload cty.dat");
  gtk_widget_set_halign(dreload,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(dgrid),dreload,0,2,1,1);

  // Progress bar and the post-reload result label share the cell next to the
  // button: the bar shows while reloading, then hides and the result appears.
  GtkWidget *dfeedback=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_hexpand(dfeedback,TRUE);
  gtk_widget_set_valign(dfeedback,GTK_ALIGN_CENTER);
  gtk_grid_attach(GTK_GRID(dgrid),dfeedback,1,2,1,1);

  GtkWidget *dbar=gtk_progress_bar_new();
  gtk_widget_set_valign(dbar,GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(dbar,TRUE);
  gtk_widget_set_visible(dbar,FALSE);
  gtk_box_append(GTK_BOX(dfeedback),dbar);

  GtkWidget *dresult=gtk_label_new(NULL);
  gtk_widget_set_halign(dresult,GTK_ALIGN_START);
  gtk_widget_set_valign(dresult,GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(dresult,4);
  gtk_widget_set_visible(dresult,FALSE);
  gtk_box_append(GTK_BOX(dfeedback),dresult);

  dxcc_reload_ctx *dctx=g_new0(dxcc_reload_ctx,1);
  dctx->bar=dbar; dctx->result=dresult; dctx->status=dstatus; dctx->button=dreload;
  g_signal_connect_data(dreload,"clicked",G_CALLBACK(dxcc_reload_cb),dctx,
                        free_closure_data,0);
  // The pulse timer redraws dbar, so the page going away has to stop it -- the
  // rule the decoder panels' on_destroy handlers follow.  The window between
  // arming it and dxcc_reload_run disarming it is one main-loop iteration, but
  // "narrow" is not "closed": five call sites destroy the Configure dialog
  // directly, and a timer left running on a finalised GtkProgressBar takes the
  // ctx it dereferences with it (free_closure_data runs on this same destroy).
  g_signal_connect(dbar,"destroy",G_CALLBACK(dxcc_reload_stop),dctx);

  // ---- Network logging (WSJT-X/JTDX UDP) ----
  GtkWidget *lframe=gtk_frame_new("Network Logging");
  GtkWidget *lgrid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(lgrid),5);
  gtk_grid_set_row_spacing(GTK_GRID(lgrid),5);
  sui_style_group(lgrid);
  gtk_frame_set_child(GTK_FRAME(lframe),lgrid);

  GtkWidget *linfo=gtk_label_new("Send completed QSOs to a logger over the network, using the\n"
                                 "WSJT-X UDP protocol (as JTDX does). Point a logger such as\n"
                                 "RUMlogNG, Log4OM, N1MM+, JTAlert or GridTracker at the host/port below.");
  gtk_widget_set_halign(linfo,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(linfo,12);
  gtk_grid_attach(GTK_GRID(lgrid),linfo,0,0,2,1);

  GtkWidget *en=gtk_check_button_new_with_label("Send QSOs over the network");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(en),r->ft8_log_udp);
  gtk_grid_attach(GTK_GRID(lgrid),en,0,1,2,1);
  g_signal_connect(en,"toggled",G_CALLBACK(udp_enable_cb),r);

  GtkWidget *host_lbl=gtk_label_new("Host / IP:");
  gtk_widget_set_halign(host_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(lgrid),host_lbl,0,2,1,1);
  GtkWidget *host=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(host),sizeof(r->ft8_log_udp_host)-1);
  gtk_editable_set_width_chars(GTK_EDITABLE(host),18);
  gtk_editable_set_text(GTK_EDITABLE(host),r->ft8_log_udp_host);
  gtk_grid_attach(GTK_GRID(lgrid),host,1,2,1,1);
  g_signal_connect(host,"changed",G_CALLBACK(udp_host_cb),r);

  GtkWidget *port_lbl=gtk_label_new("Port:");
  gtk_widget_set_halign(port_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(lgrid),port_lbl,0,3,1,1);
  GtkWidget *port=gtk_spin_button_new_with_range(1,65535,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port),r->ft8_log_udp_port);
  gtk_grid_attach(GTK_GRID(lgrid),port,1,3,1,1);
  g_signal_connect(port,"value-changed",G_CALLBACK(udp_port_cb),r);

  // ---- Spot reporting (PSK Reporter) ----
  GtkWidget *pframe=gtk_frame_new("Spot Reporting");
  GtkWidget *pgrid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(pgrid),5);
  gtk_grid_set_row_spacing(GTK_GRID(pgrid),5);
  sui_style_group(pgrid);
  gtk_frame_set_child(GTK_FRAME(pframe),pgrid);

  GtkWidget *pinfo=gtk_label_new("Report every decoded FT8 station to the PSK Reporter network\n"
                                 "(report.pskreporter.info), as WSJT-X/JTDX do, so your spots\n"
                                 "appear on the PSK Reporter map. Requires your callsign and grid\n"
                                 "above; nothing is sent until both are set.");
  gtk_widget_set_halign(pinfo,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(pinfo,12);
  gtk_grid_attach(GTK_GRID(pgrid),pinfo,0,0,2,1);

  GtkWidget *pen=gtk_check_button_new_with_label("Report spots to PSK Reporter");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(pen),r->ft8_pskr);
  gtk_grid_attach(GTK_GRID(pgrid),pen,0,1,2,1);
  g_signal_connect(pen,"toggled",G_CALLBACK(pskr_enable_cb),r);

  // ---- Decode list colours ----
  GtkWidget *cframe=gtk_frame_new("Decode List Colours");
  GtkWidget *cgrid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(cgrid),5);
  gtk_grid_set_row_spacing(GTK_GRID(cgrid),5);
  sui_style_group(cgrid);
  gtk_frame_set_child(GTK_FRAME(cframe),cgrid);

  GtkWidget *cinfo=gtk_label_new("Colours of the FT8 panel's band-activity list. A station from a DXCC\n"
                                 "entity never worked, or new on this band, gets a row background; its\n"
                                 "text then uses the highlighted-row text colour. The bottom-bar readout\n"
                                 "shows the two highlight colours as text.");
  gtk_widget_set_halign(cinfo,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(cinfo,12);
  gtk_grid_attach(GTK_GRID(cgrid),cinfo,0,0,2,1);

  ft8_colors_ctx *cctx=g_new0(ft8_colors_ctx,1);
  struct { const char *label; char *field; const char *def; } crow[FT8_NCOLORS]={
    { "CQ message text:",          r->ft8_col_cq,       FT8_COL_CQ_DEFAULT },
    { "New DXCC (never worked):",  r->ft8_col_new_dxcc, FT8_COL_NEW_DXCC_DEFAULT },
    { "New DXCC on this band:",    r->ft8_col_new_band, FT8_COL_NEW_BAND_DEFAULT },
    { "Highlighted-row text:",     r->ft8_col_hl_text,  FT8_COL_HL_TEXT_DEFAULT },
  };
  for(int i=0;i<FT8_NCOLORS;i++) {
    GtkWidget *l=gtk_label_new(crow[i].label);
    gtk_widget_set_halign(l,GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(cgrid),l,0,i+1,1,1);
    cctx->btn[i]=color_button(crow[i].field);
    cctx->def[i]=crow[i].def;
    gtk_grid_attach(GTK_GRID(cgrid),cctx->btn[i],1,i+1,1,1);
  }
  GtkWidget *creset=gtk_button_new_with_label("Reset to defaults");
  gtk_widget_set_halign(creset,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(cgrid),creset,1,FT8_NCOLORS+1,1,1);
  g_signal_connect_data(creset,"clicked",G_CALLBACK(colors_reset_cb),cctx,
                        free_closure_data,0);

  GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(vbox),frame);
  gtk_box_append(GTK_BOX(vbox),cframe);
  gtk_box_append(GTK_BOX(vbox),dframe);
  gtk_box_append(GTK_BOX(vbox),lframe);
  gtk_box_append(GTK_BOX(vbox),pframe);
  return vbox;
}
