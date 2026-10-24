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
#include "web_tls.h"

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

static void rtc_port_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=data;
  radio->web_rtc_port=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
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

static gboolean view_login_set(const RADIO *radio) {
  return radio->web_view_salt[0]!='\0' &&
         strlen(radio->web_view_hash)==WEB_AUTH_HASH_HEX_SIZE-1;
}

static const char *view_note_text(const RADIO *radio) {
  if(!view_login_set(radio)) return "No viewer login: every login can take control.";
  return radio->web_auth_enable ?
      "Viewer login is on: it can watch and listen, never control." :
      "Viewer login is saved but idle until password protection is on.";
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
  GtkWidget *view_note=g_object_get_data(G_OBJECT(widget),"view-note");
  if(view_note!=NULL) gtk_label_set_text(GTK_LABEL(view_note),view_note_text(radio));
  refresh_status();
}

static void set_password_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  GtkWidget *entry=g_object_get_data(G_OBJECT(widget),"password-entry");
  GtkWidget *user_entry=g_object_get_data(G_OBJECT(widget),"username-entry");
  GtkWidget *check=g_object_get_data(G_OBJECT(widget),"auth-check");
  GtkWidget *note=g_object_get_data(G_OBJECT(widget),"auth-note");
  const char *password=gtk_editable_get_text(GTK_EDITABLE(entry));
  const char *username=gtk_editable_get_text(GTK_EDITABLE(user_entry));
  if(*username=='\0' || strchr(username,':')!=NULL) {
    gtk_label_set_text(GTK_LABEL(note),"Username must not be empty or contain a colon.");
    return;
  }
  if(view_login_set(radio) && strcmp(username,radio->web_view_user)==0) {
    gtk_label_set_text(GTK_LABEL(note),"Username must differ from the viewer username.");
    return;
  }
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
  g_strlcpy(radio->web_auth_user,username,sizeof(radio->web_auth_user));
  radio->web_auth_enable=TRUE;
  memset(hash,0,sizeof(hash));
  gtk_editable_set_text(GTK_EDITABLE(entry),"");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(check),TRUE);
  if(radio->web_enable) web_server_start();
  char saved[160];
  g_snprintf(saved,sizeof(saved),"Password saved; protection is on (username: %s).",
             radio->web_auth_user);
  gtk_label_set_text(GTK_LABEL(note),saved);
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
  GtkWidget *view_note=g_object_get_data(G_OBJECT(check),"view-note");
  if(view_note!=NULL) gtk_label_set_text(GTK_LABEL(view_note),view_note_text(radio));
  refresh_status();
}

/* The viewer login is a second Basic-auth account with the same salted-hash
   storage.  Its username must differ from the controller's, or one header
   would match both and the role would depend on the check order. */
static void set_view_password_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  GtkWidget *entry=g_object_get_data(G_OBJECT(widget),"password-entry");
  GtkWidget *user_entry=g_object_get_data(G_OBJECT(widget),"username-entry");
  GtkWidget *note=g_object_get_data(G_OBJECT(widget),"view-note");
  const char *password=gtk_editable_get_text(GTK_EDITABLE(entry));
  const char *username=gtk_editable_get_text(GTK_EDITABLE(user_entry));
  if(*username=='\0' || strchr(username,':')!=NULL) {
    gtk_label_set_text(GTK_LABEL(note),"Viewer username must not be empty or contain a colon.");
    return;
  }
  if(strcmp(username,radio->web_auth_user)==0) {
    gtk_label_set_text(GTK_LABEL(note),"Viewer username must differ from the main username.");
    return;
  }
  if(g_utf8_strlen(password,-1)<8) {
    gtk_label_set_text(GTK_LABEL(note),"Viewer password must contain at least 8 characters.");
    return;
  }
  gchar *salt=g_uuid_string_random();
  char hash[WEB_AUTH_HASH_HEX_SIZE];
  if(salt==NULL || !web_auth_password_hash(password,salt,
                                            WEB_AUTH_PBKDF2_ITERATIONS,hash)) {
    gtk_label_set_text(GTK_LABEL(note),"Could not store the viewer password.");
    g_free(salt);
    return;
  }
  if(radio->web_enable) web_server_stop();
  g_strlcpy(radio->web_view_salt,salt,sizeof(radio->web_view_salt));
  g_strlcpy(radio->web_view_hash,hash,sizeof(radio->web_view_hash));
  g_strlcpy(radio->web_view_user,username,sizeof(radio->web_view_user));
  memset(hash,0,sizeof(hash));
  gtk_editable_set_text(GTK_EDITABLE(entry),"");
  if(radio->web_enable) web_server_start();
  gtk_label_set_text(GTK_LABEL(note),view_note_text(radio));
  refresh_status();
  g_free(salt);
}

static void clear_view_password_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  GtkWidget *note=g_object_get_data(G_OBJECT(widget),"view-note");
  if(radio->web_enable) web_server_stop();
  memset(radio->web_view_salt,0,sizeof(radio->web_view_salt));
  memset(radio->web_view_hash,0,sizeof(radio->web_view_hash));
  if(radio->web_enable) web_server_start();
  gtk_label_set_text(GTK_LABEL(note),view_note_text(radio));
  refresh_status();
}

static void username_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=data;
  const char *user=gtk_editable_get_text(GTK_EDITABLE(widget));
  if(*user=='\0' || strchr(user,':')!=NULL) return;
  if(view_login_set(radio) && strcmp(user,radio->web_view_user)==0) return;
  gboolean restart=radio->web_enable;
  if(restart) web_server_stop();
  g_strlcpy(radio->web_auth_user,user,sizeof(radio->web_auth_user));
  if(restart) web_server_start();
  refresh_status();
}

static void apply_bundle_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=data;
  GtkWidget *entry=g_object_get_data(G_OBJECT(widget),"bundle-entry");
  GtkWidget *note=g_object_get_data(G_OBJECT(widget),"bundle-note");
  const char *path=gtk_editable_get_text(GTK_EDITABLE(entry));
  char *error=NULL;
  WEB_TLS_CONTEXT *test=web_tls_context_new(path,&error);
  if(test==NULL) {
    gtk_label_set_text(GTK_LABEL(note),error!=NULL?error:"Invalid TLS bundle");
    g_free(error); return;
  }
  const char *fingerprint=web_tls_context_fingerprint(test);
  char message[256];
  g_snprintf(message,sizeof(message),"SHA-256: %s",fingerprint);
  web_tls_context_free(test);
  gboolean restart=radio->web_enable;
  if(restart) web_server_stop();
  g_strlcpy(radio->web_tls_bundle,path,sizeof(radio->web_tls_bundle));
  if(restart) web_server_start();
  gtk_label_set_text(GTK_LABEL(note),message);
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
      "Built-in HTTPS/WSS supports direct LAN or forwarded-port Internet access.");
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

  GtkWidget *user_label=gtk_label_new("Username:");
  gtk_widget_set_halign(user_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),user_label,0,4,1,1);
  GtkWidget *username=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(username),64);
  gtk_editable_set_text(GTK_EDITABLE(username),radio->web_auth_user);
  gtk_grid_attach(GTK_GRID(grid),username,1,4,1,1);
  g_signal_connect(username,"activate",G_CALLBACK(username_cb),radio);

  GtkWidget *password_label=gtk_label_new("New password:");
  gtk_widget_set_halign(password_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),password_label,0,5,1,1);
  GtkWidget *password=gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(password),FALSE);
  gtk_entry_set_max_length(GTK_ENTRY(password),128);
  gtk_entry_set_placeholder_text(GTK_ENTRY(password),"minimum 8 characters");
  gtk_grid_attach(GTK_GRID(grid),password,1,5,1,1);

  GtkWidget *auth_buttons=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
  GtkWidget *set_password=gtk_button_new_with_label("Set password");
  GtkWidget *clear_password=gtk_button_new_with_label("Clear password");
  gtk_box_append(GTK_BOX(auth_buttons),set_password);
  gtk_box_append(GTK_BOX(auth_buttons),clear_password);
  gtk_grid_attach(GTK_GRID(grid),auth_buttons,1,6,1,1);

  GtkWidget *auth_note=gtk_label_new(radio->web_auth_enable?
      "Password protection is on.":
      "Password protection is off: anyone on the network can connect.");
  gtk_widget_set_halign(auth_note,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),auth_note,0,7,2,1);
  g_object_set_data(G_OBJECT(auth),"auth-note",auth_note);
  g_signal_connect(auth,"toggled",G_CALLBACK(auth_enable_cb),radio);
  g_object_set_data(G_OBJECT(set_password),"password-entry",password);
  g_object_set_data(G_OBJECT(set_password),"username-entry",username);
  g_object_set_data(G_OBJECT(set_password),"auth-check",auth);
  g_object_set_data(G_OBJECT(set_password),"auth-note",auth_note);
  g_signal_connect(set_password,"clicked",G_CALLBACK(set_password_cb),radio);
  g_object_set_data(G_OBJECT(clear_password),"auth-check",auth);
  g_object_set_data(G_OBJECT(clear_password),"auth-note",auth_note);
  g_signal_connect(clear_password,"clicked",G_CALLBACK(clear_password_cb),radio);

  /* Optional view-only login (rows 8-12): watches and listens, never controls. */
  GtkWidget *view_user_label=gtk_label_new("Viewer username:");
  gtk_widget_set_halign(view_user_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),view_user_label,0,8,1,1);
  GtkWidget *view_user=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(view_user),64);
  gtk_editable_set_text(GTK_EDITABLE(view_user),radio->web_view_user);
  gtk_grid_attach(GTK_GRID(grid),view_user,1,8,1,1);

  GtkWidget *view_password_label=gtk_label_new("Viewer password:");
  gtk_widget_set_halign(view_password_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),view_password_label,0,9,1,1);
  GtkWidget *view_password=gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(view_password),FALSE);
  gtk_entry_set_max_length(GTK_ENTRY(view_password),128);
  gtk_entry_set_placeholder_text(GTK_ENTRY(view_password),"minimum 8 characters");
  gtk_grid_attach(GTK_GRID(grid),view_password,1,9,1,1);

  GtkWidget *view_buttons=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
  GtkWidget *set_view=gtk_button_new_with_label("Set viewer password");
  GtkWidget *clear_view=gtk_button_new_with_label("Clear viewer password");
  gtk_box_append(GTK_BOX(view_buttons),set_view);
  gtk_box_append(GTK_BOX(view_buttons),clear_view);
  gtk_grid_attach(GTK_GRID(grid),view_buttons,1,10,1,1);

  GtkWidget *view_note=gtk_label_new(view_note_text(radio));
  gtk_widget_set_halign(view_note,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),view_note,0,11,2,1);
  GtkWidget *control_note=gtk_label_new(
      "One browser at a time has control; the others watch until they take over.\n"
      "Five wrong logins lock that address out for 30 s, doubling to 15 min.");
  sui_label_desc(control_note);
  gtk_widget_set_halign(control_note,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),control_note,0,12,2,1);
  g_object_set_data(G_OBJECT(auth),"view-note",view_note);
  g_object_set_data(G_OBJECT(set_view),"password-entry",view_password);
  g_object_set_data(G_OBJECT(set_view),"username-entry",view_user);
  g_object_set_data(G_OBJECT(set_view),"view-note",view_note);
  g_signal_connect(set_view,"clicked",G_CALLBACK(set_view_password_cb),radio);
  g_object_set_data(G_OBJECT(clear_view),"view-note",view_note);
  g_signal_connect(clear_view,"clicked",G_CALLBACK(clear_view_password_cb),radio);

  GtkWidget *bundle_label=gtk_label_new("TLS PEM bundle:");
  gtk_widget_set_halign(bundle_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),bundle_label,0,13,1,1);
  GtkWidget *bundle=gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(bundle),"empty = generated self-signed certificate");
  gtk_editable_set_text(GTK_EDITABLE(bundle),radio->web_tls_bundle);
  gtk_grid_attach(GTK_GRID(grid),bundle,1,13,1,1);
  GtkWidget *apply_bundle=gtk_button_new_with_label("Apply certificate");
  gtk_grid_attach(GTK_GRID(grid),apply_bundle,1,14,1,1);
  char tls_note[256];
  const char *fingerprint=web_server_tls_fingerprint();
  if(fingerprint!=NULL && *fingerprint!='\0')
    g_snprintf(tls_note,sizeof(tls_note),"SHA-256: %s",fingerprint);
  else
    g_strlcpy(tls_note,"HTTPS/WSS; first visit warns for a self-signed certificate.",sizeof(tls_note));
  GtkWidget *bundle_note=gtk_label_new(tls_note);
  gtk_widget_set_halign(bundle_note,GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(bundle_note),TRUE);
  gtk_grid_attach(GTK_GRID(grid),bundle_note,0,15,2,1);
  g_object_set_data(G_OBJECT(apply_bundle),"bundle-entry",bundle);
  g_object_set_data(G_OBJECT(apply_bundle),"bundle-note",bundle_note);
  g_signal_connect(apply_bundle,"clicked",G_CALLBACK(apply_bundle_cb),radio);

  GtkWidget *port_label=gtk_label_new("Port:");
  gtk_widget_set_halign(port_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port_label,0,16,1,1);
  GtkWidget *port=gtk_spin_button_new_with_range(1,65535,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port),
                            radio->web_port>0?radio->web_port:WEB_DEFAULT_PORT);
  gtk_widget_set_halign(port,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),port,1,16,1,1);
  g_signal_connect(port,"value-changed",G_CALLBACK(port_cb),radio);

  GtkWidget *rtc_port_label=gtk_label_new("WebRTC audio UDP port:");
  gtk_widget_set_halign(rtc_port_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),rtc_port_label,0,17,1,1);
  GtkWidget *rtc_port=gtk_spin_button_new_with_range(1,65535,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(rtc_port),radio->web_rtc_port);
  gtk_widget_set_halign(rtc_port,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),rtc_port,1,17,1,1);
  g_signal_connect(rtc_port,"value-changed",G_CALLBACK(rtc_port_cb),radio);

  GtkWidget *hint=gtk_label_new("Internet: forward the HTTPS TCP port and WebRTC UDP port to this computer.");
  gtk_widget_set_halign(hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),hint,0,18,2,1);

  status_label=gtk_label_new("Status: stopped");
  gtk_widget_set_halign(status_label,GTK_ALIGN_START);
  gtk_widget_set_margin_top(status_label,8);
  gtk_grid_attach(GTK_GRID(grid),status_label,0,19,2,1);
  refresh_status();

  if(poll_id!=0) g_source_remove(poll_id);
  poll_id=g_timeout_add(1000,poll_status,NULL);

  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_append(GTK_BOX(box),frame);
  g_signal_connect(box,"destroy",G_CALLBACK(destroy_cb),NULL);
  return box;
}
