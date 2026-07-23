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
static void dxcc_reload_cb(GtkWidget *w, gpointer data) {
  (void)w;
  ft8_dxcc_reload();
  char buf[1200];
  dxcc_status_text(buf, sizeof(buf));
  gtk_label_set_text(GTK_LABEL(data), buf);
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
  g_signal_connect(dreload,"clicked",G_CALLBACK(dxcc_reload_cb),dstatus);

  // ---- Network logging (WSJT-X/JTDX UDP) ----
  GtkWidget *lframe=gtk_frame_new("Network Logging");
  GtkWidget *lgrid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(lgrid),5);
  gtk_grid_set_row_spacing(GTK_GRID(lgrid),5);
  sui_style_group(lgrid);
  gtk_frame_set_child(GTK_FRAME(lframe),lgrid);

  GtkWidget *linfo=gtk_label_new("Send completed QSOs to a logger over the network, using the\n"
                                 "WSJT-X UDP protocol (as JTDX does). Point a logger such as\n"
                                 "Log4OM, N1MM+, JTAlert or GridTracker at the host/port below.");
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

  GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(vbox),frame);
  gtk_box_append(GTK_BOX(vbox),dframe);
  gtk_box_append(GTK_BOX(vbox),lframe);
  gtk_box_append(GTK_BOX(vbox),pframe);
  return vbox;
}
