/* Copyright (C)
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
#include "css.h"
#include "ppm_cal.h"

// Apply an entry's text to a stored label and update every widget that carries
// it live: the bottom-bar toolbar button and the check button on the Radio
// config page. Falls back to a default when the field is empty so nothing is
// ever left blank.
static void apply_label(const gchar *text, char *store, gsize store_size,
                        const char *fallback, GtkWidget *button, GtkWidget *check) {
  if(text==NULL || text[0]=='\0') text=fallback;
  g_strlcpy(store,text,store_size);
  if(button!=NULL) gtk_button_set_label(GTK_BUTTON(button),store);
  if(check!=NULL)  gtk_button_set_label(GTK_BUTTON(check),store);
}

static void att10_label_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  apply_label(gtk_entry_get_text(GTK_ENTRY(widget)),
              radio->att10_label,sizeof(radio->att10_label),"Att10",
              radio->att10_button,radio->att10_check);
}

static void att20_label_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  apply_label(gtk_entry_get_text(GTK_ENTRY(widget)),
              radio->att20_label,sizeof(radio->att20_label),"Att20",
              radio->att20_button,radio->att20_check);
}

static void wfm_deemph_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int sel=gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  if(sel<0) sel=0;
  radio_set_wfm_deemphasis(radio,sel);
}

static void rds_rbds_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int sel=gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  radio->rds_rbds=(sel>0)?1:0;   // 0 = RDS (Europe), 1 = RBDS (N. America)
}

static void theme_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int sel=gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  if(sel<0) sel=0;
  radio->theme=sel;
  css_set_theme(sel);         // restyle the CSS chrome live
  radio_refresh_skin(radio);  // repaint idle Cairo surfaces (e.g. TX monitor)
}

// ---- Frequency calibration (PPM) ----
// Widget refs kept static so the callbacks can cross-update the readout. Only
// one Configure dialog exists at a time; the page's "destroy" handler nulls
// these so the measurement poll never touches freed widgets.
static GtkWidget *ppm_status_label;   // summary: current correction + reference
static GtkWidget *ppm_result_label;   // auto-measurement progress / result
static GtkWidget *ppm_station_combo;
static GtkWidget *ppm_spin;
static GtkWidget *ppm_calib_btn;
static guint      ppm_poll_id;

static void ppm_status_refresh(RADIO *r) {
  if(ppm_status_label==NULL) return;
  const PPM_STATION *st=ppm_station(r->ppm_ref_station);
  char buf[256];
  snprintf(buf,sizeof(buf),"Correction: %+.2f ppm    Reference: %s",
           r->ppm_correction_value, st?st->name:"(none)");
  gtk_label_set_text(GTK_LABEL(ppm_status_label),buf);
}

static void ppm_value_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->ppm_correction_value=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  // Protocol 1 reads ppm live in its output thread; Protocol 2 / SoapySDR must
  // be re-tuned to pick up the new correction.
  if(r->active_receiver!=NULL) frequency_changed(r->active_receiver);
  ppm_status_refresh(r);
}

static void ppm_station_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  int sel=gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  if(sel<0) sel=0;
  r->ppm_ref_station=sel;
  ppm_status_refresh(r);
}

static void ppm_tune_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  int sel=r->ppm_ref_station;
  if(ppm_station_combo!=NULL) sel=gtk_combo_box_get_active(GTK_COMBO_BOX(ppm_station_combo));
  if(sel<0) sel=0;
  ppm_cal_tune_to_station(r,sel);
  ppm_status_refresh(r);
}

// Poll the background measurement (GTK thread). Refreshes the result label; on
// completion re-enables the button and, if a carrier was found, writes the
// suggested ppm into the spinner (which applies it via ppm_value_cb).
static gboolean ppm_poll(gpointer data) {
  RADIO *r=(RADIO *)data;
  gboolean done=FALSE, ok=FALSE;
  double off=0.0, sugg=0.0;
  char st[160];
  ppm_cal_measure_poll(&done,st,sizeof(st),&off,&sugg,&ok);
  if(ppm_result_label!=NULL) gtk_label_set_text(GTK_LABEL(ppm_result_label),st);
  if(done || !ppm_cal_measuring()) {   // finished normally, or stopped/cancelled
    ppm_poll_id=0;
    if(ppm_calib_btn!=NULL) {
      gtk_button_set_label(GTK_BUTTON(ppm_calib_btn),"Calibrate");
      gtk_widget_set_sensitive(ppm_calib_btn,TRUE);
    }
    if(done && ok && ppm_spin!=NULL)
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(ppm_spin),sugg);  // auto-apply new ppm
    ppm_cal_restore_rx();              // put the radio back where the operator was
    return FALSE;
  }
  return TRUE;
}

static void ppm_calib_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  if(ppm_cal_measuring()) {            // button is acting as "Stop"
    ppm_cal_measure_cancel();
    return;                            // the poll relabels the button and restores the RX
  }
  if(ppm_station_combo!=NULL) {
    int sel=gtk_combo_box_get_active(GTK_COMBO_BOX(ppm_station_combo));
    if(sel>=0) r->ppm_ref_station=sel;
  }
  if(!ppm_cal_measure_start(r)) {
    if(ppm_result_label!=NULL)
      gtk_label_set_text(GTK_LABEL(ppm_result_label),"Cannot start (no active receiver)");
    return;
  }
  gtk_button_set_label(GTK_BUTTON(widget),"Stop");
  if(ppm_poll_id!=0) g_source_remove(ppm_poll_id);
  ppm_poll_id=g_timeout_add(150,ppm_poll,r);
}

static void ppm_dialog_destroy(GtkWidget *widget, gpointer data) {
  if(ppm_poll_id!=0) { g_source_remove(ppm_poll_id); ppm_poll_id=0; }
  ppm_cal_measure_cancel();
  ppm_status_label=NULL; ppm_result_label=NULL;
  ppm_station_combo=NULL; ppm_spin=NULL; ppm_calib_btn=NULL;
}

GtkWidget *create_labels_dialog(RADIO *r) {
  GtkWidget *frame=gtk_frame_new("Button Labels");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_container_add(GTK_CONTAINER(frame),grid);

  GtkWidget *info=gtk_label_new("Custom labels for the RX front-end attenuator buttons.\n"
                                "Leave a field empty to restore the default.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12); // gap between the description and the fields below
  gtk_grid_attach(GTK_GRID(grid),info,0,0,2,1);

  GtkWidget *att10_lbl=gtk_label_new("Att10 button:");
  gtk_widget_set_halign(att10_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),att10_lbl,0,1,1,1);
  GtkWidget *att10_entry=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(att10_entry),sizeof(r->att10_label)-1);
  gtk_entry_set_text(GTK_ENTRY(att10_entry),r->att10_label);
  gtk_grid_attach(GTK_GRID(grid),att10_entry,1,1,1,1);
  g_signal_connect(att10_entry,"changed",G_CALLBACK(att10_label_cb),r);

  GtkWidget *att20_lbl=gtk_label_new("Att20 button:");
  gtk_widget_set_halign(att20_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),att20_lbl,0,2,1,1);
  GtkWidget *att20_entry=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(att20_entry),sizeof(r->att20_label)-1);
  gtk_entry_set_text(GTK_ENTRY(att20_entry),r->att20_label);
  gtk_grid_attach(GTK_GRID(grid),att20_entry,1,2,1,1);
  g_signal_connect(att20_entry,"changed",G_CALLBACK(att20_label_cb),r);

  // ---- Broadcast FM (WFM) options ----
  GtkWidget *fm_frame=gtk_frame_new("Broadcast FM (WFM)");
  GtkWidget *fm_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(fm_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(fm_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(fm_grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(fm_grid),5);
  sui_style_group(fm_grid);
  gtk_container_add(GTK_CONTAINER(fm_frame),fm_grid);

  GtkWidget *fm_info=gtk_label_new("Audio de-emphasis time constant. Use 50 µs in Europe and\n"
                                   "most of the world, 75 µs in the Americas and South Korea.");
  gtk_widget_set_halign(fm_info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(fm_info,12);
  gtk_grid_attach(GTK_GRID(fm_grid),fm_info,0,0,2,1);

  GtkWidget *de_lbl=gtk_label_new("De-emphasis:");
  gtk_widget_set_halign(de_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(fm_grid),de_lbl,0,1,1,1);
  GtkWidget *de_combo=gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(de_combo),NULL,"50 µs");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(de_combo),NULL,"75 µs");
  gtk_combo_box_set_active(GTK_COMBO_BOX(de_combo),r->wfm_deemphasis?1:0);
  gtk_grid_attach(GTK_GRID(fm_grid),de_combo,1,1,1,1);
  g_signal_connect(de_combo,"changed",G_CALLBACK(wfm_deemph_cb),r);

  GtkWidget *pty_lbl=gtk_label_new("RDS PTY names:");
  gtk_widget_set_halign(pty_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(fm_grid),pty_lbl,0,2,1,1);
  GtkWidget *pty_combo=gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(pty_combo),NULL,"RDS (Europe)");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(pty_combo),NULL,"RBDS (North America)");
  gtk_combo_box_set_active(GTK_COMBO_BOX(pty_combo),r->rds_rbds?1:0);
  gtk_grid_attach(GTK_GRID(fm_grid),pty_combo,1,2,1,1);
  g_signal_connect(pty_combo,"changed",G_CALLBACK(rds_rbds_cb),r);

  // ---- Appearance (main-window skin) ----
  GtkWidget *skin_frame=gtk_frame_new("Appearance");
  GtkWidget *skin_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(skin_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(skin_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(skin_grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(skin_grid),5);
  sui_style_group(skin_grid);
  gtk_container_add(GTK_CONTAINER(skin_frame),skin_grid);

  GtkWidget *skin_info=gtk_label_new("Color skin for the main window and this dialog.\n"
                                     "Applied immediately and remembered per radio.");
  gtk_widget_set_halign(skin_info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(skin_info,12);
  gtk_grid_attach(GTK_GRID(skin_grid),skin_info,0,0,2,1);

  GtkWidget *skin_lbl=gtk_label_new("Skin:");
  gtk_widget_set_halign(skin_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(skin_grid),skin_lbl,0,1,1,1);
  GtkWidget *skin_combo=gtk_combo_box_text_new();
  for(int t=0;t<css_theme_count();t++) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(skin_combo),NULL,css_theme_name(t));
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(skin_combo),r->theme);
  gtk_grid_attach(GTK_GRID(skin_grid),skin_combo,1,1,1,1);
  g_signal_connect(skin_combo,"changed",G_CALLBACK(theme_cb),r);

  // ---- Frequency Calibration (PPM) ----
  GtkWidget *ppm_frame=gtk_frame_new("Frequency Calibration (PPM)");
  GtkWidget *ppm_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(ppm_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(ppm_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(ppm_grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(ppm_grid),5);
  sui_style_group(ppm_grid);
  gtk_container_add(GTK_CONTAINER(ppm_frame),ppm_grid);

  GtkWidget *ppm_info=gtk_label_new(
      "Correct the radio's reference-oscillator error in parts-per-million.\n"
      "Pick a time/frequency-standard station and press Calibrate to measure the\n"
      "carrier and set the correction automatically, or Tune to zero-beat it by ear.");
  gtk_widget_set_halign(ppm_info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(ppm_info,12);
  gtk_grid_attach(GTK_GRID(ppm_grid),ppm_info,0,0,3,1);

  GtkWidget *ppm_lbl=gtk_label_new("Correction (ppm):");
  gtk_widget_set_halign(ppm_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(ppm_grid),ppm_lbl,0,1,1,1);
  ppm_spin=gtk_spin_button_new_with_range(-500,500,0.01);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ppm_spin),2);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ppm_spin),r->ppm_correction_value);
  gtk_grid_attach(GTK_GRID(ppm_grid),ppm_spin,1,1,1,1);
  g_signal_connect(ppm_spin,"value_changed",G_CALLBACK(ppm_value_cb),r);

  GtkWidget *st_lbl=gtk_label_new("Reference station:");
  gtk_widget_set_halign(st_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(ppm_grid),st_lbl,0,2,1,1);
  ppm_station_combo=gtk_combo_box_text_new();
  for(int s=0;s<ppm_station_count();s++) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ppm_station_combo),NULL,ppm_station(s)->name);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(ppm_station_combo),r->ppm_ref_station);
  gtk_grid_attach(GTK_GRID(ppm_grid),ppm_station_combo,1,2,1,1);
  g_signal_connect(ppm_station_combo,"changed",G_CALLBACK(ppm_station_cb),r);

  GtkWidget *btn_box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
  ppm_calib_btn=gtk_button_new_with_label("Calibrate");
  gtk_box_pack_start(GTK_BOX(btn_box),ppm_calib_btn,FALSE,FALSE,0);
  g_signal_connect(ppm_calib_btn,"clicked",G_CALLBACK(ppm_calib_cb),r);
  GtkWidget *tune_b=gtk_button_new_with_label("Tune");
  gtk_box_pack_start(GTK_BOX(btn_box),tune_b,FALSE,FALSE,0);
  g_signal_connect(tune_b,"clicked",G_CALLBACK(ppm_tune_cb),r);
  gtk_grid_attach(GTK_GRID(ppm_grid),btn_box,2,1,1,2);

  ppm_status_label=gtk_label_new("");
  gtk_widget_set_halign(ppm_status_label,GTK_ALIGN_START);
  gtk_widget_set_margin_top(ppm_status_label,6);
  gtk_grid_attach(GTK_GRID(ppm_grid),ppm_status_label,0,3,3,1);
  ppm_status_refresh(r);

  ppm_result_label=gtk_label_new("");
  gtk_widget_set_halign(ppm_result_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(ppm_grid),ppm_result_label,0,4,3,1);

  GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
  gtk_box_pack_start(GTK_BOX(vbox),skin_frame,FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(vbox),ppm_frame,FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(vbox),frame,FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(vbox),fm_frame,FALSE,FALSE,0);
  g_signal_connect(vbox,"destroy",G_CALLBACK(ppm_dialog_destroy),NULL);
  return vbox;
}
