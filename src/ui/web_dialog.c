/* Configure -> Network controls for the embedded browser remote. */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "web_dialog.h"
#include "web_protocol.h"
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

static gboolean auth_credentials_set(const RADIO *radio) {
  return radio->web_auth_salt[0]!='\0' &&
         strlen(radio->web_auth_hash)==WEB_AUTH_HASH_HEX_SIZE-1;
}

static void auth_enable_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  gboolean enabled=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  GtkWidget *note=g_object_get_data(G_OBJECT(widget),"auth-note");
  if(enabled && !auth_credentials_set(radio)) {
    if(note!=NULL) gtk_label_set_text(GTK_LABEL(note),"Set a password first (minimum 8 characters).");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widget),FALSE);
    return;
  }
  if(radio->web_auth_enable==enabled) return;
  if(radio->web_enable) web_server_stop();
  radio->web_auth_enable=enabled;
  if(radio->web_enable) web_server_start();
  if(note!=NULL) gtk_label_set_text(GTK_LABEL(note),
      enabled?"Password protection is on.":"Password protection is off: anyone on the network can connect.");
  refresh_status();
}

static void set_password_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  GtkWidget *entry=g_object_get_data(G_OBJECT(widget),"password-entry");
  GtkWidget *check=g_object_get_data(G_OBJECT(widget),"auth-check");
  GtkWidget *note=g_object_get_data(G_OBJECT(widget),"auth-note");
  const char *password=gtk_editable_get_text(GTK_EDITABLE(entry));
  if(g_utf8_strlen(password,-1)<8) {
    gtk_label_set_text(GTK_LABEL(note),"Password must contain at least 8 characters.");
    return;
  }
  gchar *salt=g_uuid_string_random();
  char hash[WEB_AUTH_HASH_HEX_SIZE];
  if(salt==NULL || !web_auth_password_hash(password,salt,
                                            WEB_AUTH_PBKDF2_ITERATIONS,hash)) {
    gtk_label_set_text(GTK_LABEL(note),"Could not store the password.");
    g_free(salt);
    return;
  }
  if(radio->web_enable) web_server_stop();
  g_strlcpy(radio->web_auth_salt,salt,sizeof(radio->web_auth_salt));
  g_strlcpy(radio->web_auth_hash,hash,sizeof(radio->web_auth_hash));
  radio->web_auth_enable=TRUE;
  memset(hash,0,sizeof(hash));
  gtk_editable_set_text(GTK_EDITABLE(entry),"");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(check),TRUE);
  if(radio->web_enable) web_server_start();
  gtk_label_set_text(GTK_LABEL(note),"Password saved; protection is on (username: machpsdr).");
  refresh_status();
  g_free(salt);
}

static void clear_password_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  GtkWidget *check=g_object_get_data(G_OBJECT(widget),"auth-check");
  GtkWidget *note=g_object_get_data(G_OBJECT(widget),"auth-note");
  if(radio->web_enable) web_server_stop();
  radio->web_auth_enable=FALSE;
  memset(radio->web_auth_salt,0,sizeof(radio->web_auth_salt));
  memset(radio->web_auth_hash,0,sizeof(radio->web_auth_hash));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(check),FALSE);
  if(radio->web_enable) web_server_start();
  gtk_label_set_text(GTK_LABEL(note),"Password cleared: anyone on the network can connect.");
  refresh_status();
}

GtkWidget *create_web_dialog(RADIO *radio) {
  GtkWidget *frame=gtk_frame_new("Browser Remote");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);

  GtkWidget *info=gtk_label_new(
      "Browser VFO, mode, meter, panadapter, waterfall and session-armed PTT.\n"
      "Available on this computer and the local network; do not expose this HTTP port to the Internet.");
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

  GtkWidget *auth=gtk_check_button_new_with_label("Require password on browser remote");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(auth),radio->web_auth_enable);
  gtk_grid_attach(GTK_GRID(grid),auth,0,3,2,1);

  GtkWidget *password_label=gtk_label_new("New password:");
  gtk_widget_set_halign(password_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),password_label,0,4,1,1);
  GtkWidget *password=gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(password),FALSE);
  gtk_entry_set_max_length(GTK_ENTRY(password),128);
  gtk_entry_set_placeholder_text(GTK_ENTRY(password),"minimum 8 characters");
  gtk_grid_attach(GTK_GRID(grid),password,1,4,1,1);

  GtkWidget *auth_buttons=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
  GtkWidget *set_password=gtk_button_new_with_label("Set password");
  GtkWidget *clear_password=gtk_button_new_with_label("Clear password");
  gtk_box_append(GTK_BOX(auth_buttons),set_password);
  gtk_box_append(GTK_BOX(auth_buttons),clear_password);
  gtk_grid_attach(GTK_GRID(grid),auth_buttons,1,5,1,1);

  GtkWidget *auth_note=gtk_label_new(radio->web_auth_enable?
      "Password protection is on (username: machpsdr).":
      "Password protection is off: anyone on the network can connect.");
  gtk_widget_set_halign(auth_note,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),auth_note,0,6,2,1);
  g_object_set_data(G_OBJECT(auth),"auth-note",auth_note);
  g_signal_connect(auth,"toggled",G_CALLBACK(auth_enable_cb),radio);
  g_object_set_data(G_OBJECT(set_password),"password-entry",password);
  g_object_set_data(G_OBJECT(set_password),"auth-check",auth);
  g_object_set_data(G_OBJECT(set_password),"auth-note",auth_note);
  g_signal_connect(set_password,"clicked",G_CALLBACK(set_password_cb),radio);
  g_object_set_data(G_OBJECT(clear_password),"auth-check",auth);
  g_object_set_data(G_OBJECT(clear_password),"auth-note",auth_note);
  g_signal_connect(clear_password,"clicked",G_CALLBACK(clear_password_cb),radio);

  GtkWidget *port_label=gtk_label_new("Port:");
  gtk_widget_set_halign(port_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port_label,0,7,1,1);
  GtkWidget *port=gtk_spin_button_new_with_range(1,65535,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port),
                            radio->web_port>0?radio->web_port:WEB_DEFAULT_PORT);
  gtk_widget_set_halign(port,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port,1,7,1,1);
  g_signal_connect(port,"value-changed",G_CALLBACK(port_cb),radio);

  GtkWidget *hint=gtk_label_new("Open http://<this-computer-IP>:8080 on the LAN (default port 8080)");
  gtk_widget_set_halign(hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),hint,0,8,2,1);

  status_label=gtk_label_new("Status: stopped");
  gtk_widget_set_halign(status_label,GTK_ALIGN_START);
  gtk_widget_set_margin_top(status_label,8);
  gtk_grid_attach(GTK_GRID(grid),status_label,0,9,2,1);
  refresh_status();

  if(poll_id!=0) g_source_remove(poll_id);
  poll_id=g_timeout_add(1000,poll_status,NULL);

  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(box),frame);
  g_signal_connect(box,"destroy",G_CALLBACK(destroy_cb),NULL);
  return box;
}
