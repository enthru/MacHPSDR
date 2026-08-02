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

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "cluster_dialog.h"
#include "dxcluster.h"

// Status label + poll id kept static so the 1s refresh can cross-update the
// readout; only one Configure dialog exists at a time. The page's "destroy"
// handler nulls the label and cancels the poll so it never touches freed
// widgets (mirrors labels_dialog.c's PPM status pattern).
static GtkWidget *cluster_status_label;
static guint      cluster_poll_id;

static void cluster_status_refresh(void) {
  if(cluster_status_label==NULL) return;
  char buf[160];
  snprintf(buf,sizeof(buf),"Status: %s",dxcluster_status());
  gtk_label_set_text(GTK_LABEL(cluster_status_label),buf);
}

static gboolean cluster_status_poll(gpointer data) {
  (void)data;
  cluster_status_refresh();
  return TRUE;
}

static void cluster_dialog_destroy(GtkWidget *widget, gpointer data) {
  (void)widget; (void)data;
  if(cluster_poll_id!=0) { g_source_remove(cluster_poll_id); cluster_poll_id=0; }
  cluster_status_label=NULL;
}

static void enable_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->cluster_enable=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
  // Reconnect live so toggling the checkbox takes effect immediately, rather
  // than only on the next restart.
  dxcluster_stop();
  if(r->cluster_enable) dxcluster_start();
  cluster_status_refresh();
}

static void spots_show_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->cluster_spots_show=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
}

static void spots_on_cb(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  RADIO *r=(RADIO *)data;
  r->cluster_spots_on=(gint)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
}

static void font_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->cluster_spots_font=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
}

static void bg_cb(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  RADIO *r=(RADIO *)data;
  const GdkRGBA *c=gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(obj));
  r->cluster_spots_bg_r=c->red;
  r->cluster_spots_bg_g=c->green;
  r->cluster_spots_bg_b=c->blue;
  r->cluster_spots_bg_a=c->alpha;
}

static void fg_dxcc_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->cluster_spots_fg_dxcc=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
}

static void fg_cb(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  RADIO *r=(RADIO *)data;
  const GdkRGBA *c=gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(obj));
  r->cluster_spots_fg_r=c->red;
  r->cluster_spots_fg_g=c->green;
  r->cluster_spots_fg_b=c->blue;
  r->cluster_spots_fg_a=c->alpha;
}

static void host_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  g_strlcpy(r->cluster_host,gtk_editable_get_text(GTK_EDITABLE(w)),sizeof(r->cluster_host));
}

static void port_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->cluster_port=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
}

static void login_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  g_strlcpy(r->cluster_login,gtk_editable_get_text(GTK_EDITABLE(w)),sizeof(r->cluster_login));
}

GtkWidget *create_cluster_dialog(RADIO *r) {
  GtkWidget *frame=gtk_frame_new("DX Cluster");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);

  // Rows are placed with a running counter so inserting/removing controls never
  // requires renumbering the ones below (which repeatedly went wrong by hand).
  int row=0;

  GtkWidget *info=gtk_label_new("Connect to a telnet DX cluster and overlay decoded spots on\n"
                                "the RX panadapter and/or waterfall, colour-keyed by DXCC entity.\n"
                                "Left-click a spot marker to tune the RX to its exact frequency.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12);
  gtk_grid_attach(GTK_GRID(grid),info,0,row++,2,1);

  GtkWidget *en=gtk_check_button_new_with_label("Connect to DX cluster");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(en),r->cluster_enable);
  gtk_grid_attach(GTK_GRID(grid),en,0,row++,2,1);
  g_signal_connect(en,"toggled",G_CALLBACK(enable_cb),r);

  GtkWidget *sh=gtk_check_button_new_with_label("Show spots");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(sh),r->cluster_spots_show);
  gtk_grid_attach(GTK_GRID(grid),sh,0,row++,2,1);
  g_signal_connect(sh,"toggled",G_CALLBACK(spots_show_cb),r);

  GtkWidget *on_lbl=gtk_label_new("Show spots on:");
  gtk_widget_set_halign(on_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),on_lbl,0,row,1,1);
  const char *on_opts[]={"Panadapter","Waterfall","Both",NULL};
  GtkWidget *on_dd=gtk_drop_down_new_from_strings(on_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(on_dd),
                             (r->cluster_spots_on>=0 && r->cluster_spots_on<=2)?r->cluster_spots_on:0);
  gtk_widget_set_halign(on_dd,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),on_dd,1,row++,1,1);
  g_signal_connect(on_dd,"notify::selected",G_CALLBACK(spots_on_cb),r);

  GtkWidget *font_lbl=gtk_label_new("Spot label font (px):");
  gtk_widget_set_halign(font_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),font_lbl,0,row,1,1);
  GtkWidget *font=gtk_spin_button_new_with_range(7,28,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(font),r->cluster_spots_font);
  gtk_grid_attach(GTK_GRID(grid),font,1,row++,1,1);
  g_signal_connect(font,"value-changed",G_CALLBACK(font_cb),r);

  GtkWidget *dxcc=gtk_check_button_new_with_label("Colour label by DXCC entity");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(dxcc),r->cluster_spots_fg_dxcc);
  gtk_grid_attach(GTK_GRID(grid),dxcc,0,row++,2,1);
  g_signal_connect(dxcc,"toggled",G_CALLBACK(fg_dxcc_cb),r);

  GtkWidget *fg_lbl=gtk_label_new("Spot label text colour:");
  gtk_widget_set_halign(fg_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),fg_lbl,0,row,1,1);
  GdkRGBA fg={ (float)r->cluster_spots_fg_r, (float)r->cluster_spots_fg_g,
               (float)r->cluster_spots_fg_b, (float)r->cluster_spots_fg_a };
  GtkColorDialog *fcd=gtk_color_dialog_new();
  gtk_color_dialog_set_with_alpha(fcd,TRUE);
  GtkWidget *fg_btn=gtk_color_dialog_button_new(fcd);   // takes ownership of fcd
  gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(fg_btn),&fg);
  gtk_widget_set_halign(fg_btn,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),fg_btn,1,row++,1,1);
  g_signal_connect(fg_btn,"notify::rgba",G_CALLBACK(fg_cb),r);

  GtkWidget *bg_lbl=gtk_label_new("Spot label background:");
  gtk_widget_set_halign(bg_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),bg_lbl,0,row,1,1);
  GdkRGBA bg={ (float)r->cluster_spots_bg_r, (float)r->cluster_spots_bg_g,
               (float)r->cluster_spots_bg_b, (float)r->cluster_spots_bg_a };
  GtkColorDialog *cd=gtk_color_dialog_new();
  gtk_color_dialog_set_with_alpha(cd,TRUE);
  GtkWidget *bg_btn=gtk_color_dialog_button_new(cd);   // takes ownership of cd
  gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(bg_btn),&bg);
  gtk_widget_set_halign(bg_btn,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),bg_btn,1,row++,1,1);
  g_signal_connect(bg_btn,"notify::rgba",G_CALLBACK(bg_cb),r);

  GtkWidget *host_lbl=gtk_label_new("Host / IP:");
  gtk_widget_set_halign(host_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),host_lbl,0,row,1,1);
  GtkWidget *host=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(host),sizeof(r->cluster_host)-1);
  gtk_editable_set_width_chars(GTK_EDITABLE(host),22);
  gtk_editable_set_text(GTK_EDITABLE(host),r->cluster_host);
  gtk_grid_attach(GTK_GRID(grid),host,1,row++,1,1);
  g_signal_connect(host,"changed",G_CALLBACK(host_cb),r);

  GtkWidget *port_lbl=gtk_label_new("Port:");
  gtk_widget_set_halign(port_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port_lbl,0,row,1,1);
  GtkWidget *port=gtk_spin_button_new_with_range(1,65535,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port),r->cluster_port);
  gtk_grid_attach(GTK_GRID(grid),port,1,row++,1,1);
  g_signal_connect(port,"value-changed",G_CALLBACK(port_cb),r);

  GtkWidget *login_lbl=gtk_label_new("Login call:");
  gtk_widget_set_halign(login_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),login_lbl,0,row,1,1);
  GtkWidget *login=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(login),sizeof(r->cluster_login)-1);
  gtk_editable_set_width_chars(GTK_EDITABLE(login),12);
  gtk_editable_set_text(GTK_EDITABLE(login),r->cluster_login);
  gtk_grid_attach(GTK_GRID(grid),login,1,row++,1,1);
  g_signal_connect(login,"changed",G_CALLBACK(login_cb),r);

  GtkWidget *login_hint=gtk_label_new("(blank = use station call, from the FT8 page)");
  gtk_widget_set_halign(login_hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),login_hint,0,row++,2,1);

  cluster_status_label=gtk_label_new("Status: disconnected");
  gtk_widget_set_halign(cluster_status_label,GTK_ALIGN_START);
  gtk_widget_set_margin_top(cluster_status_label,8);
  gtk_grid_attach(GTK_GRID(grid),cluster_status_label,0,row++,2,1);
  cluster_status_refresh();

  if(cluster_poll_id!=0) g_source_remove(cluster_poll_id);
  cluster_poll_id=g_timeout_add(1000,cluster_status_poll,NULL);

  GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(vbox),frame);
  g_signal_connect(vbox,"destroy",G_CALLBACK(cluster_dialog_destroy),NULL);
  return vbox;
}
