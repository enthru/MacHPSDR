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
#include "tci_dialog.h"
#include "tci.h"

// Status label + poll id, static so the 1s refresh can cross-update the
// readout (only one Configure dialog exists at a time). The "destroy" handler
// nulls the label and cancels the poll so it never touches freed widgets
// (mirrors cluster_dialog.c).
static GtkWidget *tci_status_label;
static guint      tci_poll_id;

static void tci_status_refresh(void) {
  if(tci_status_label==NULL) return;
  char buf[192];
  snprintf(buf,sizeof(buf),"Status: %s",tci_status());
  gtk_label_set_text(GTK_LABEL(tci_status_label),buf);
}

static gboolean tci_status_poll(gpointer data) {
  (void)data;
  tci_status_refresh();
  return TRUE;
}

static void tci_dialog_destroy(GtkWidget *widget, gpointer data) {
  (void)widget; (void)data;
  if(tci_poll_id!=0) { g_source_remove(tci_poll_id); tci_poll_id=0; }
  tci_status_label=NULL;
}

static void enable_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->tci_enable=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
  // Restart live so toggling the checkbox takes effect immediately.
  tci_stop();
  if(r->tci_enable) tci_start();
  tci_status_refresh();
}

static void port_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->tci_port=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
  // Rebind on the new port if we're currently enabled.
  if(r->tci_enable) { tci_stop(); tci_start(); }
  tci_status_refresh();
}

GtkWidget *create_tci_dialog(RADIO *r) {
  GtkWidget *frame=gtk_frame_new("TCI Server");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);

  GtkWidget *info=gtk_label_new(
      "TCI (Expert Electronics) control server over WebSocket. Lets loggers\n"
      "and skimmers (Log4OM, N1MM+, SkookumLogger, …) set and follow VFO,\n"
      "mode and PTT. Phase A is control only — spectrum/audio streaming is\n"
      "not yet implemented.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12);
  gtk_grid_attach(GTK_GRID(grid),info,0,0,2,1);

  GtkWidget *en=gtk_check_button_new_with_label("Enable TCI server");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(en),r->tci_enable);
  gtk_grid_attach(GTK_GRID(grid),en,0,1,2,1);
  g_signal_connect(en,"toggled",G_CALLBACK(enable_cb),r);

  GtkWidget *port_lbl=gtk_label_new("Port:");
  gtk_widget_set_halign(port_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port_lbl,0,2,1,1);
  GtkWidget *port=gtk_spin_button_new_with_range(1,65535,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port),r->tci_port>0?r->tci_port:TCI_DEFAULT_PORT);
  gtk_grid_attach(GTK_GRID(grid),port,1,2,1,1);
  g_signal_connect(port,"value-changed",G_CALLBACK(port_cb),r);

  GtkWidget *port_hint=gtk_label_new("(default 40001)");
  gtk_widget_set_halign(port_hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port_hint,0,3,2,1);

  tci_status_label=gtk_label_new("Status: stopped");
  gtk_widget_set_halign(tci_status_label,GTK_ALIGN_START);
  gtk_widget_set_margin_top(tci_status_label,8);
  gtk_grid_attach(GTK_GRID(grid),tci_status_label,0,4,2,1);
  tci_status_refresh();

  if(tci_poll_id!=0) g_source_remove(tci_poll_id);
  tci_poll_id=g_timeout_add(1000,tci_status_poll,NULL);

  GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(vbox),frame);
  g_signal_connect(vbox,"destroy",G_CALLBACK(tci_dialog_destroy),NULL);
  return vbox;
}
