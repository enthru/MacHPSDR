/* Configure -> Network controls for the embedded browser remote. */
#include <gtk/gtk.h>
#include <stdio.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "web_dialog.h"
#include "web_server.h"

static GtkWidget *status_label;
static guint poll_id;

static void refresh_status(void) {
  if(status_label==NULL) return;
  char text[256];
  g_snprintf(text,sizeof(text),"Status: %s",web_server_status());
  gtk_label_set_text(GTK_LABEL(status_label),text);
}

static gboolean poll_status(gpointer data) {
  (void)data;
  refresh_status();
  return TRUE;
}

static void destroy_cb(GtkWidget *widget, gpointer data) {
  (void)widget; (void)data;
  if(poll_id!=0) { g_source_remove(poll_id); poll_id=0; }
  status_label=NULL;
}

static void enable_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->web_enable=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  web_server_stop();
  if(radio->web_enable) web_server_start();
  refresh_status();
}

static void port_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->web_port=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
  if(radio->web_enable) { web_server_stop(); web_server_start(); }
  refresh_status();
}

static void tx_enable_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->web_tx_enable=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

GtkWidget *create_web_dialog(RADIO *radio) {
  GtkWidget *frame=gtk_frame_new("Browser Remote");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);

  GtkWidget *info=gtk_label_new(
      "Built-in browser control for VFO, mode, meter and PTT. Phase 1 listens\n"
      "on this computer only; spectrum, waterfall and audio come next.");
  sui_label_desc(info);
  gtk_widget_set_margin_bottom(info,12);
  gtk_grid_attach(GTK_GRID(grid),info,0,0,2,1);

  GtkWidget *enable=gtk_check_button_new_with_label("Enable browser remote");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(enable),radio->web_enable);
  gtk_grid_attach(GTK_GRID(grid),enable,0,1,2,1);
  g_signal_connect(enable,"toggled",G_CALLBACK(enable_cb),radio);

  GtkWidget *tx_enable=gtk_check_button_new_with_label("Allow browser PTT (this session only)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(tx_enable),radio->web_tx_enable);
  gtk_widget_set_sensitive(tx_enable,radio->can_transmit);
  gtk_grid_attach(GTK_GRID(grid),tx_enable,0,2,2,1);
  g_signal_connect(tx_enable,"toggled",G_CALLBACK(tx_enable_cb),radio);

  GtkWidget *port_label=gtk_label_new("Port:");
  gtk_widget_set_halign(port_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port_label,0,3,1,1);
  GtkWidget *port=gtk_spin_button_new_with_range(1,65535,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port),
                            radio->web_port>0?radio->web_port:WEB_DEFAULT_PORT);
  gtk_widget_set_halign(port,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port,1,3,1,1);
  g_signal_connect(port,"value-changed",G_CALLBACK(port_cb),radio);

  GtkWidget *hint=gtk_label_new("Open http://127.0.0.1:8080 after enabling (default port 8080)");
  gtk_widget_set_halign(hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),hint,0,4,2,1);

  status_label=gtk_label_new("Status: stopped");
  gtk_widget_set_halign(status_label,GTK_ALIGN_START);
  gtk_widget_set_margin_top(status_label,8);
  gtk_grid_attach(GTK_GRID(grid),status_label,0,5,2,1);
  refresh_status();

  if(poll_id!=0) g_source_remove(poll_id);
  poll_id=g_timeout_add(1000,poll_status,NULL);

  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(box),frame);
  g_signal_connect(box,"destroy",G_CALLBACK(destroy_cb),NULL);
  return box;
}
