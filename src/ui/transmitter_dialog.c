/* Copyright (C)
* 2018 - John Melton, G0ORX/N6LYT
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
#include "log.h"
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <wdsp.h>

#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#include "audio.h"
#include "band.h"
#include "tx_panadapter.h"


static GtkWidget *microphone_frame;
static GtkWidget *tx_spin_low;
static GtkWidget *tx_spin_high;
static GtkWidget *tx_latency;

/*
static gboolean close_cb (GtkWidget *widget, GdkEventButton *event, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
log_info("tx->dialog: close_cb");
  tx->dialog=NULL;
  return TRUE;
}
*/

/*
static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
log_info("tx->dialog: delete_cb: %p\n",tx->dialog);
  tx->dialog=NULL;
  return FALSE;
}
*/

static void microphone_audio_cb(GtkWidget *widget,gpointer data) {
  RADIO *radio=(RADIO *)data;
  if(radio->local_microphone) {
    audio_close_input(radio);
  }
  radio->local_microphone=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  if(radio->local_microphone) {
    if(audio_open_input(radio)<0) {
      radio->local_microphone=FALSE;
      gtk_check_button_set_active(GTK_CHECK_BUTTON (radio->transmitter->local_microphone_b),FALSE);
    }
  }
}

static void microphone_choice_cb(GtkDropDown *widget,GParamSpec *ps,gpointer data) {
  RADIO *radio=(RADIO *)data;
  int i;
  if(radio->local_microphone) {
    audio_close_input(radio);
    i=(int)gtk_drop_down_get_selected(widget);
    if(radio->microphone_name!=NULL) {
      g_free(radio->microphone_name);
    }
    radio->microphone_name=g_new0(gchar,strlen(input_devices[i].name)+1);
    strcpy(radio->microphone_name,input_devices[i].name);
    if(audio_open_input(radio)<0) {
      radio->local_microphone=FALSE;
      gtk_check_button_set_active(GTK_CHECK_BUTTON (radio->transmitter->local_microphone_b),FALSE);
    }
  }  else {
      i=(int)gtk_drop_down_get_selected(widget);
      if(radio->microphone_name!=NULL) {
        g_free(radio->microphone_name);
        radio->microphone_name=NULL;
    }
    if(i>=0) {
      radio->microphone_name=g_new0(gchar,strlen(input_devices[i].name)+1);
      strcpy(radio->microphone_name,input_devices[i].name);
    }
  }
  if((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(radio->transmitter->microphone_choice_b))==-1) {
    gtk_widget_set_sensitive(radio->transmitter->local_microphone_b, FALSE);
  } else {
    gtk_widget_set_sensitive(radio->transmitter->local_microphone_b, TRUE);
  }
  log_info("Input device changed: %d: %s (%s)\n",i,input_devices[i].name,output_devices[i].description);
}

/*
static void mic_gain_value_changed_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->mic_gain=gtk_range_get_value(GTK_RANGE(widget));
  SetTXAPanelGain1(tx->channel,pow(10.0, tx->mic_gain / 20.0));
}
*/

static void tune_value_changed_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->tune_percent=gtk_range_get_value(GTK_RANGE(widget));
}

static void tune_use_drive_cb(GtkWidget *widget,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->tune_use_drive=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void use_rx_filter_cb(GtkWidget *widget,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->use_rx_filter=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  if(tx->use_rx_filter) {
    transmitter_set_filter(tx,tx->rx->filter_low_a,tx->rx->filter_high_a);
  } else {
    transmitter_set_filter(tx,tx->filter_low,tx->filter_high);
  }
  gtk_widget_set_sensitive(tx_spin_low,!tx->use_rx_filter);
  gtk_widget_set_sensitive(tx_spin_high,!tx->use_rx_filter);
  update_tx_panadapter(radio);
}

static void tx_spin_low_cb (GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->filter_low=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
  transmitter_set_filter(tx,tx->filter_low,tx->filter_high);
}

static void tx_spin_high_cb (GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->filter_high=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
  transmitter_set_filter(tx,tx->filter_low,tx->filter_high);
}

static void emp_cb (GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->pre_emphasize=gtk_check_button_get_active (GTK_CHECK_BUTTON (widget));
  transmitter_set_pre_emphasize(tx,tx->pre_emphasize);
}

static void am_carrier_level_value_changed_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->am_carrier_level=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  transmitter_set_am_carrier_level(tx);
}

static void enable_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->enable_equalizer=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetTXAEQRun(tx->channel, tx->enable_equalizer);
}

static void tx_eq_value_changed_cb (GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"eq_band"));
  tx->equalizer[idx]=(int)gtk_range_get_value(GTK_RANGE(widget));
  SetTXAGrphEQ10(tx->channel, tx->equalizer);
}

static void fps_value_changed_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->fps=gtk_range_get_value(GTK_RANGE(widget));
  transmitter_fps_changed(tx);
}

static void panadapter_high_value_changed_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->panadapter_high=gtk_range_get_value(GTK_RANGE(widget));
}

static void panadapter_low_value_changed_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->panadapter_low=gtk_range_get_value(GTK_RANGE(widget));
}

static void ctcss_enable_cb (GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int state=gtk_check_button_get_active (GTK_CHECK_BUTTON (widget));
  transmitter_set_ctcss(tx,state,tx->ctcss);
}

static void ctcss_frequency_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int i=(int)gtk_drop_down_get_selected(widget);
  transmitter_set_ctcss(tx,tx->ctcss_enabled,i);
}

static void tx_latency_cb(GtkWidget *widget, gpointer data) {
  RADIO *r = (RADIO*)data;
  r->hl2->hl2_tx_buffer_size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)); 
}

static void tx_leveler_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->leveler = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetTXALevelerSt(tx->channel, tx->leveler);
}

static void tx_cessb_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->cessb = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetTXAosctrlRun(tx->channel, tx->cessb);
}

static void comp_value_changed_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->compressor_level = gtk_range_get_value(GTK_RANGE(widget));
  SetTXACompressorGain(tx->channel, tx->compressor_level);
}

static void tx_compressor_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->compressor = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetTXACompressorRun(tx->channel, tx->compressor);
}

static void cfc_apply_profile(TRANSMITTER *tx) {
  SetTXACFCOMPprofile(tx->channel, 5, tx->cfc_freq, tx->cfc_comp, tx->cfc_eq);
}

static void cfc_run_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->cfc_run = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetTXACFCOMPRun(tx->channel, tx->cfc_run);
}

static void cfc_peq_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->cfc_peq_run = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetTXACFCOMPPeqRun(tx->channel, tx->cfc_peq_run);
}

static void cfc_precomp_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->cfc_precomp = gtk_range_get_value(GTK_RANGE(widget));
  SetTXACFCOMPPrecomp(tx->channel, tx->cfc_precomp);
}

static void cfc_prepeq_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->cfc_prepeq = gtk_range_get_value(GTK_RANGE(widget));
  SetTXACFCOMPPrePeq(tx->channel, tx->cfc_prepeq);
}

static void cfc_comp_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"cfc_band"));
  tx->cfc_comp[idx] = gtk_range_get_value(GTK_RANGE(widget));
  cfc_apply_profile(tx);
}

static void cfc_eq_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"cfc_band"));
  tx->cfc_eq[idx] = gtk_range_get_value(GTK_RANGE(widget));
  cfc_apply_profile(tx);
}

static void phrot_run_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->phrot_run = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetTXAPHROTRun(tx->channel, tx->phrot_run);
}
static void phrot_corner_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->phrot_corner = gtk_range_get_value(GTK_RANGE(widget));
  SetTXAPHROTCorner(tx->channel, tx->phrot_corner);
}
static void phrot_nstages_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->phrot_nstages = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  SetTXAPHROTNstages(tx->channel, tx->phrot_nstages);
}

void update_transmitter_dialog(TRANSMITTER *tx) {
  int i;

log_info("%s: tx=%d\n",__FUNCTION__,tx->channel);
  // re-scan audio devices so a mic connected after launch (e.g. a Bluetooth
  // headset) shows up in the list without restarting the app
  audio_refresh_devices();
  g_signal_handler_block(G_OBJECT(tx->microphone_choice_b),tx->microphone_choice_signal_id);
  g_signal_handler_block(G_OBJECT(tx->local_microphone_b),tx->local_microphone_signal_id);

  GtkStringList *mic_sl=GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(radio->transmitter->microphone_choice_b)));
  gtk_string_list_splice(mic_sl,0,g_list_model_get_n_items(G_LIST_MODEL(mic_sl)),NULL);
  for(i=0;i<n_input_devices;i++) {
log_info("adding: %s\n",input_devices[i].description);
    gtk_string_list_append(mic_sl,input_devices[i].description);
    if(radio->microphone_name!=NULL) {
      if(strcmp(input_devices[i].name,radio->microphone_name)==0) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(radio->transmitter->microphone_choice_b),i);
      }
    }
  }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (tx->local_microphone_b), radio->local_microphone);

  if((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(radio->transmitter->microphone_choice_b))==-1) {
    gtk_widget_set_sensitive(radio->transmitter->local_microphone_b, FALSE);
  } else {
    gtk_widget_set_sensitive(radio->transmitter->local_microphone_b, TRUE);
  }

  g_signal_handler_unblock(G_OBJECT(tx->local_microphone_b),tx->local_microphone_signal_id);
  g_signal_handler_unblock(G_OBJECT(tx->microphone_choice_b),tx->microphone_choice_signal_id);
}

GtkWidget *create_transmitter_dialog(TRANSMITTER *tx) {
  int i;
  char temp[32];

log_info("%s: tx=%d\n",__FUNCTION__,tx->channel);
  GtkWidget *grid=gtk_grid_new();
  sui_style_page(grid);
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);

  int row=0;
  int col=0;

  microphone_frame=gtk_frame_new("Microphone");
  GtkWidget *microphone_grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(microphone_grid),10);
  gtk_grid_set_row_homogeneous(GTK_GRID(microphone_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(microphone_grid),FALSE);
  sui_style_group(microphone_grid);
  gtk_frame_set_child(GTK_FRAME(microphone_frame),microphone_grid);
  gtk_grid_attach(GTK_GRID(grid),microphone_frame,col,row++,1,1);

  if(n_input_devices>=0) {
    radio->transmitter->local_microphone_b=gtk_check_button_new_with_label("Local Microphone");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (radio->transmitter->local_microphone_b), radio->local_microphone);
    gtk_grid_attach(GTK_GRID(microphone_grid),radio->transmitter->local_microphone_b,0,0,1,1);
    radio->transmitter->local_microphone_signal_id=g_signal_connect(radio->transmitter->local_microphone_b,"toggled",G_CALLBACK(microphone_audio_cb),radio);

    radio->transmitter->microphone_choice_b=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    //update_transmitter_audio_choices(tx);
    // TO REMOVE because the variable n_input_devices is always zero here
    // for(i=0;i<n_input_devices;i++) {
    //   g_print("adding: %s\n",input_devices[i].description);
    //   gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(radio->transmitter->microphone_choice_b),NULL,input_devices[i].description);
    //   if(radio->microphone_name!=NULL) {
    //     if(strcmp(input_devices[i].name,radio->microphone_name)==0) {
    //       gtk_drop_down_set_selected(GTK_DROP_DOWN(radio->transmitter->microphone_choice_b),i);
    //     }
    //   }
    // }
    // Moved to update_transmitter_dialog
    // if((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(radio->transmitter->microphone_choice_b))==-1) {
    //   gtk_widget_set_sensitive(radio->transmitter->local_microphone_b, FALSE);
    // } else {
    //   gtk_widget_set_sensitive(radio->transmitter->local_microphone_b, TRUE);
    // }

    gtk_grid_attach(GTK_GRID(microphone_grid),radio->transmitter->microphone_choice_b,1,0,1,1);
    radio->transmitter->microphone_choice_signal_id=g_signal_connect(radio->transmitter->microphone_choice_b,"notify::selected",G_CALLBACK(microphone_choice_cb),radio);

  }

  GtkWidget *tune_frame=gtk_frame_new("Tune");
  GtkWidget *tune_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(tune_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(tune_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(tune_grid),10);
  sui_style_group(tune_grid);
  gtk_frame_set_child(GTK_FRAME(tune_frame),tune_grid);
  gtk_grid_attach(GTK_GRID(grid),tune_frame,col,row++,1,1);

  GtkWidget *tune_label=gtk_label_new("Tune Percent:");
  gtk_widget_set_visible(tune_label, TRUE);
  gtk_grid_attach(GTK_GRID(tune_grid),tune_label,0,1,1,1);

  GtkWidget *tune_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0.0,100.0,1.00);
  gtk_widget_set_size_request (tune_scale, 300, 32);
  gtk_range_set_value (GTK_RANGE(tune_scale),tx->tune_percent);
  sui_scale_show_value(tune_scale,0);
  gtk_widget_set_visible(tune_scale, TRUE);
  g_signal_connect(G_OBJECT(tune_scale),"value_changed",G_CALLBACK(tune_value_changed_cb),tx);
  gtk_grid_attach(GTK_GRID(tune_grid),tune_scale,1,1,1,1);

  GtkWidget *tune_use_drive=gtk_check_button_new_with_label("Tune Use Drive");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (tune_use_drive), tx->tune_use_drive);
  gtk_grid_attach(GTK_GRID(tune_grid),tune_use_drive,0,2,1,1);
  g_signal_connect(tune_use_drive,"toggled",G_CALLBACK(tune_use_drive_cb),tx);

  GtkWidget *filter_frame=gtk_frame_new("TX Filter");
  GtkWidget *filter_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(filter_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(filter_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(filter_grid),10);
  sui_style_group(filter_grid);
  gtk_frame_set_child(GTK_FRAME(filter_frame),filter_grid);
  gtk_grid_attach(GTK_GRID(grid),filter_frame,col,row++,1,1);

  GtkWidget *use_rx_filter=gtk_check_button_new_with_label("Use Rx Filter");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (use_rx_filter), tx->use_rx_filter);
  gtk_grid_attach(GTK_GRID(filter_grid),use_rx_filter,0,0,2,1);
  g_signal_connect(use_rx_filter,"toggled",G_CALLBACK(use_rx_filter_cb),tx);

  GtkWidget *low_label=gtk_label_new("Low:");
  gtk_widget_set_visible(low_label, TRUE);
  gtk_grid_attach(GTK_GRID(filter_grid),low_label,0,1,1,1);

  tx_spin_low=gtk_spin_button_new_with_range(0.0,8000.0,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(tx_spin_low),(double)tx->filter_low);
  gtk_grid_attach(GTK_GRID(filter_grid),tx_spin_low,1,1,1,1);
  g_signal_connect(tx_spin_low,"value-changed",G_CALLBACK(tx_spin_low_cb),tx);

  GtkWidget *high_label=gtk_label_new("High:");
  gtk_widget_set_visible(high_label, TRUE);
  gtk_grid_attach(GTK_GRID(filter_grid),high_label,2,1,1,1);

  tx_spin_high=gtk_spin_button_new_with_range(0.0,8000.0,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(tx_spin_high),(double)tx->filter_high);
  gtk_grid_attach(GTK_GRID(filter_grid),tx_spin_high,3,1,1,1);
  g_signal_connect(tx_spin_high,"value-changed",G_CALLBACK(tx_spin_high_cb),tx);

  gtk_widget_set_sensitive(tx_spin_low,!tx->use_rx_filter);
  gtk_widget_set_sensitive(tx_spin_high,!tx->use_rx_filter);

  GtkWidget *fm_frame=gtk_frame_new("FM");
  GtkWidget *fm_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(fm_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(fm_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(fm_grid),10);
  sui_style_group(fm_grid);
  gtk_frame_set_child(GTK_FRAME(fm_frame),fm_grid);
  gtk_grid_attach(GTK_GRID(grid),fm_frame,col,row++,1,1);

  GtkWidget *emp_b=gtk_check_button_new_with_label("FM TX Pre-emphasize before limiting");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (emp_b), tx->pre_emphasize);
  gtk_widget_set_visible(emp_b, TRUE);
  gtk_grid_attach(GTK_GRID(fm_grid),emp_b,0,0,1,1);
  g_signal_connect(emp_b,"toggled",G_CALLBACK(emp_cb),tx);

  GtkWidget *am_frame=gtk_frame_new("AM");
  GtkWidget *am_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(am_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(am_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(am_grid),10);
  sui_style_group(am_grid);
  gtk_frame_set_child(GTK_FRAME(am_frame),am_grid);
  gtk_grid_attach(GTK_GRID(grid),am_frame,col,row++,1,1);

  GtkWidget *am_carrier_level_label=gtk_label_new("AM Carrier Level: ");
#ifdef GTK316
  gtk_label_set_xalign(GTK_LABEL(am_carrier_level_label),0);
#endif
  gtk_widget_set_visible(am_carrier_level_label, TRUE);
  gtk_grid_attach(GTK_GRID(am_grid),am_carrier_level_label,0,0,1,1);
  GtkWidget *am_carrier_level=gtk_spin_button_new_with_range(0.0,1.0,0.1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(am_carrier_level),(double)tx->am_carrier_level);
  gtk_widget_set_visible(am_carrier_level, TRUE);
  // The AM frame is a single-row group sharing an outer-grid row with the tall
  // "Speech Processing" frame (column 1), so that row is stretched tall. GTK4
  // widgets default to GTK_ALIGN_FILL, which made the spin button balloon to the
  // full row height; keep it at its natural height, vertically centred.
  gtk_widget_set_valign(am_carrier_level,GTK_ALIGN_CENTER);
  gtk_grid_attach(GTK_GRID(am_grid),am_carrier_level,1,0,1,1);
  g_signal_connect(am_carrier_level,"value_changed",G_CALLBACK(am_carrier_level_value_changed_cb),tx);

  GtkWidget *ctcss_frame=gtk_frame_new("CTCSS");
  GtkWidget *ctcss_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(ctcss_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(ctcss_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(ctcss_grid),10);
  sui_style_group(ctcss_grid);
  gtk_frame_set_child(GTK_FRAME(ctcss_frame),ctcss_grid);
  gtk_grid_attach(GTK_GRID(grid),ctcss_frame,col,row++,1,1);

  GtkWidget *ctcss_enable=gtk_check_button_new_with_label("Enable CTCSS");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (ctcss_enable), tx->ctcss_enabled);
  gtk_grid_attach(GTK_GRID(ctcss_grid),ctcss_enable,0,0,1,1);
  g_signal_connect(ctcss_enable,"toggled",G_CALLBACK(ctcss_enable_cb),tx);

  GtkStringList *ctcss_sl=gtk_string_list_new(NULL);
  for(i=0;i<CTCSS_FREQUENCIES;i++) {
    sprintf(temp,"%0.1f",ctcss_frequencies[i]);
    gtk_string_list_append(ctcss_sl,temp);
  }
  GtkWidget *ctcss_frequency_b=gtk_drop_down_new(G_LIST_MODEL(ctcss_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(ctcss_frequency_b),tx->ctcss);
  // The CTCSS frame shares a stretched outer-grid row with a tall neighbour, so
  // GTK4's default GTK_ALIGN_FILL ballooned this lone dropdown vertically (same
  // issue fixed for the AM Carrier spin button). Pin it to its natural height.
  gtk_widget_set_valign(ctcss_frequency_b,GTK_ALIGN_CENTER);
  gtk_grid_attach(GTK_GRID(ctcss_grid),ctcss_frequency_b,1,0,1,1);
  g_signal_connect(ctcss_frequency_b,"notify::selected",G_CALLBACK(ctcss_frequency_cb),tx);

  GtkWidget *panadapter_frame=gtk_frame_new("Panadapter");
  GtkWidget *panadapter_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(panadapter_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(panadapter_grid),FALSE);
  sui_style_group(panadapter_grid);
  gtk_frame_set_child(GTK_FRAME(panadapter_frame),panadapter_grid);
  gtk_grid_attach(GTK_GRID(grid),panadapter_frame,col,row++,1,1);

  GtkWidget *fps_label=gtk_label_new("FPS:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),fps_label,0,0,1,1);

  GtkWidget *fps_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,1.0, 50.0, 1.00);
  GtkAdjustment *adj=gtk_range_get_adjustment(GTK_RANGE(fps_scale));
  gtk_adjustment_set_page_increment(adj,1.0);
  gtk_widget_set_size_request(fps_scale,200,30);
  gtk_range_set_value (GTK_RANGE(fps_scale),tx->fps);
  sui_scale_show_value(fps_scale,0);
  gtk_widget_set_visible(fps_scale, TRUE);
  g_signal_connect(G_OBJECT(fps_scale),"value_changed",G_CALLBACK(fps_value_changed_cb),tx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),fps_scale,1,0,1,1);

  high_label=gtk_label_new("High:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),high_label,0,1,1,1);

  GtkWidget *panadapter_high_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-200.0, 20.0, 1.00);
  gtk_widget_set_size_request(panadapter_high_scale,200,30);
  gtk_range_set_value (GTK_RANGE(panadapter_high_scale),tx->panadapter_high);
  sui_scale_show_value(panadapter_high_scale,0);
  gtk_widget_set_visible(panadapter_high_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_high_scale),"value_changed",G_CALLBACK(panadapter_high_value_changed_cb),tx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_high_scale,1,1,1,1);

  GtkWidget *waterfall_low_label=gtk_label_new("Low:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_low_label,0,2,1,1);

  GtkWidget *panadapter_low_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-200.0, 20.0, 1.00);
  gtk_widget_set_size_request(panadapter_low_scale,200,30);
  gtk_range_set_value (GTK_RANGE(panadapter_low_scale),tx->panadapter_low);
  sui_scale_show_value(panadapter_low_scale,0);
  gtk_widget_set_visible(panadapter_low_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_low_scale),"value_changed",G_CALLBACK(panadapter_low_value_changed_cb),tx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_low_scale,1,2,1,1);

  col++;
  row=0;
  
  GtkWidget *equalizer_frame=gtk_frame_new("Equalizer");
  GtkWidget *equalizer_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(equalizer_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(equalizer_grid),TRUE);
  sui_style_group(equalizer_grid);
  gtk_frame_set_child(GTK_FRAME(equalizer_frame),equalizer_grid);
  gtk_grid_attach(GTK_GRID(grid),equalizer_frame,col,row++,1,4);

  GtkWidget *enable_b=gtk_check_button_new_with_label("Enable Equalizer");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (enable_b), tx->enable_equalizer);
  g_signal_connect(enable_b,"toggled",G_CALLBACK(enable_cb),tx);
  gtk_grid_attach(GTK_GRID(equalizer_grid),enable_b,0,0,11,1);

  const char *eq_band_labels[11]={"Pre","32","63","125","250","500","1k","2k","4k","8k","16k"};
  for(int i=0;i<11;i++) {
    GtkWidget *label=gtk_label_new(eq_band_labels[i]);
    gtk_grid_attach(GTK_GRID(equalizer_grid),label,i,1,1,1);

    GtkWidget *scale=gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,-12.0,15.0,1.0);
    adj=gtk_range_get_adjustment(GTK_RANGE(scale));
    gtk_adjustment_set_page_increment(adj,1.0);
    gtk_range_set_value(GTK_RANGE(scale),(double)tx->equalizer[i]);
    gtk_range_set_inverted(GTK_RANGE(scale),TRUE);
    g_object_set_data(G_OBJECT(scale),"eq_band",GINT_TO_POINTER(i));
    g_signal_connect(scale,"value-changed",G_CALLBACK(tx_eq_value_changed_cb),tx);
    gtk_grid_attach(GTK_GRID(equalizer_grid),scale,i,2,1,10);
    gtk_widget_set_size_request(scale,10,270);
    for(int m=-12;m<=15;m+=3) {
      gtk_scale_add_mark(GTK_SCALE(scale),(double)m,GTK_POS_LEFT, (m==-12)?"-12dB":(m==0)?"0dB":(m==15)?"15dB":NULL);
    }
  }

  if (radio->hl2 != NULL) {
  
  row += 3;

  GtkWidget *latency_frame=gtk_frame_new("TX Buffer Latency");
  GtkWidget *latency_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(latency_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(latency_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(latency_grid),10);
  sui_style_group(latency_grid);
  gtk_frame_set_child(GTK_FRAME(latency_frame),latency_grid);
  gtk_grid_attach(GTK_GRID(grid),latency_frame,col,row++,1,1);

  GtkWidget *fifo_label=gtk_label_new("Size (ms):");
  gtk_widget_set_visible(fifo_label, TRUE);
  gtk_grid_attach(GTK_GRID(latency_grid),fifo_label,1,1,1,1);

  tx_latency = gtk_spin_button_new_with_range(10, 60,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(tx_latency),(double)radio->hl2->hl2_tx_buffer_size);
  gtk_grid_attach(GTK_GRID(latency_grid),tx_latency,2,1,1,1);

  g_signal_connect(tx_latency, "value_changed", G_CALLBACK(tx_latency_cb), radio);

  } 

  row += 3;

  GtkWidget *compressor_frame=gtk_frame_new("Speech Processing");
  GtkWidget *compressor_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(compressor_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(compressor_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(compressor_grid),10);
  sui_style_group(compressor_grid);
  gtk_frame_set_child(GTK_FRAME(compressor_frame),compressor_grid);
  gtk_grid_attach(GTK_GRID(grid),compressor_frame,col,row++,1,1);

  GtkWidget *enable_cessb = gtk_check_button_new_with_label("Enable CESSB");
  gtk_widget_set_tooltip_text(enable_cessb, "Controlled-Envelope SSB overshoot control (SSB talk-power)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(enable_cessb), tx->cessb);
  gtk_grid_attach(GTK_GRID(compressor_grid), enable_cessb, 0, 0, 2, 1);
  g_signal_connect(enable_cessb,"toggled", G_CALLBACK(tx_cessb_cb), tx);

  GtkWidget *enable_comp = gtk_check_button_new_with_label("Enable compressor");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(enable_comp), tx->compressor);
  gtk_grid_attach(GTK_GRID(compressor_grid), enable_comp, 0, 2, 1, 1);
  g_signal_connect(enable_comp,"toggled", G_CALLBACK(tx_compressor_cb), tx);

  GtkWidget *enable_leveler = gtk_check_button_new_with_label("Enable leveler");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(enable_leveler), tx->leveler);
  gtk_grid_attach(GTK_GRID(compressor_grid), enable_leveler,1,2,1,1);
  g_signal_connect(enable_leveler, "toggled", G_CALLBACK(tx_leveler_cb), tx);
  
  GtkWidget *comp_label=gtk_label_new("Compression (dB):");
  gtk_widget_set_visible(comp_label, TRUE);
  gtk_grid_attach(GTK_GRID(compressor_grid), comp_label,0,1,1,1);

  GtkWidget *comp_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0 ,20 ,1);
  gtk_widget_set_size_request(comp_scale, 150, 32);
  gtk_range_set_value (GTK_RANGE(comp_scale), tx->compressor_level);
  sui_scale_show_value(comp_scale,0);
  gtk_widget_set_visible(comp_scale, TRUE);
  g_signal_connect(G_OBJECT(comp_scale),"value_changed",G_CALLBACK(comp_value_changed_cb),tx);
  gtk_grid_attach(GTK_GRID(compressor_grid),comp_scale,1,1,1,1);

  GtkWidget *cfc_frame=gtk_frame_new("CFC (Continuous Frequency Compressor)");
  GtkWidget *cfc_grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(cfc_grid),6);
  gtk_grid_set_row_spacing(GTK_GRID(cfc_grid),3);
  sui_style_group(cfc_grid);
  gtk_frame_set_child(GTK_FRAME(cfc_frame),cfc_grid);
  gtk_grid_attach(GTK_GRID(grid),cfc_frame,col,row++,1,1);

  GtkWidget *cfc_enable=gtk_check_button_new_with_label("Enable CFC");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(cfc_enable), tx->cfc_run);
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_enable, 0,0,2,1);
  g_signal_connect(cfc_enable,"toggled",G_CALLBACK(cfc_run_cb),tx);

  GtkWidget *cfc_peq=gtk_check_button_new_with_label("Post-EQ");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(cfc_peq), tx->cfc_peq_run);
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_peq, 2,0,2,1);
  g_signal_connect(cfc_peq,"toggled",G_CALLBACK(cfc_peq_cb),tx);

  GtkWidget *cfc_precomp_label=gtk_label_new("Precomp (dB):");
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_precomp_label,0,1,1,1);

  GtkWidget *cfc_precomp_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,20,1);
  gtk_widget_set_size_request(cfc_precomp_scale, 120, 28);
  gtk_range_set_value(GTK_RANGE(cfc_precomp_scale), tx->cfc_precomp);
  sui_scale_show_value(cfc_precomp_scale,0);
  gtk_widget_set_tooltip_text(cfc_precomp_scale, "CFC pre-compression gain (dB)");
  g_signal_connect(G_OBJECT(cfc_precomp_scale),"value_changed",G_CALLBACK(cfc_precomp_cb),tx);
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_precomp_scale,1,1,1,1);

  GtkWidget *cfc_prepeq_label=gtk_label_new("PrePeq (dB):");
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_prepeq_label,2,1,1,1);

  GtkWidget *cfc_prepeq_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,20,1);
  gtk_widget_set_size_request(cfc_prepeq_scale, 120, 28);
  gtk_range_set_value(GTK_RANGE(cfc_prepeq_scale), tx->cfc_prepeq);
  sui_scale_show_value(cfc_prepeq_scale,0);
  gtk_widget_set_tooltip_text(cfc_prepeq_scale, "CFC pre-post-eq gain (dB)");
  g_signal_connect(G_OBJECT(cfc_prepeq_scale),"value_changed",G_CALLBACK(cfc_prepeq_cb),tx);
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_prepeq_scale,3,1,1,1);

  GtkWidget *cfc_band_header=gtk_label_new("Band");
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_band_header,0,2,1,1);
  GtkWidget *cfc_comp_header=gtk_label_new("Comp");
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_comp_header,1,2,1,1);
  GtkWidget *cfc_eq_header=gtk_label_new("EQ");
  gtk_grid_attach(GTK_GRID(cfc_grid), cfc_eq_header,2,2,1,1);

  for(i=0;i<5;i++) {
    snprintf(temp,sizeof temp,"%.0f Hz",tx->cfc_freq[i]);
    GtkWidget *cfc_band_label=gtk_label_new(temp);
    gtk_grid_attach(GTK_GRID(cfc_grid), cfc_band_label,0,3+i,1,1);

    GtkWidget *cfc_comp_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-20,20,1);
    gtk_widget_set_size_request(cfc_comp_scale, 120, 28);
    gtk_range_set_value(GTK_RANGE(cfc_comp_scale), tx->cfc_comp[i]);
    sui_scale_show_value(cfc_comp_scale,0);
    gtk_widget_set_tooltip_text(cfc_comp_scale, "Per-band compression (dB)");
    g_object_set_data(G_OBJECT(cfc_comp_scale),"cfc_band",GINT_TO_POINTER(i));
    g_signal_connect(G_OBJECT(cfc_comp_scale),"value_changed",G_CALLBACK(cfc_comp_cb),tx);
    gtk_grid_attach(GTK_GRID(cfc_grid), cfc_comp_scale,1,3+i,1,1);

    GtkWidget *cfc_eq_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-20,20,1);
    gtk_widget_set_size_request(cfc_eq_scale, 120, 28);
    gtk_range_set_value(GTK_RANGE(cfc_eq_scale), tx->cfc_eq[i]);
    sui_scale_show_value(cfc_eq_scale,0);
    gtk_widget_set_tooltip_text(cfc_eq_scale, "Per-band post-eq (dB)");
    g_object_set_data(G_OBJECT(cfc_eq_scale),"cfc_band",GINT_TO_POINTER(i));
    g_signal_connect(G_OBJECT(cfc_eq_scale),"value_changed",G_CALLBACK(cfc_eq_cb),tx);
    gtk_grid_attach(GTK_GRID(cfc_grid), cfc_eq_scale,2,3+i,2,1);
  }

  GtkWidget *phrot_frame=gtk_frame_new("Phase Rotator");
  GtkWidget *phrot_grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(phrot_grid),6);
  gtk_grid_set_row_spacing(GTK_GRID(phrot_grid),3);
  sui_style_group(phrot_grid);
  gtk_frame_set_child(GTK_FRAME(phrot_frame),phrot_grid);
  gtk_grid_attach(GTK_GRID(grid),phrot_frame,col,row++,1,1);

  GtkWidget *phrot_enable=gtk_check_button_new_with_label("Enable Phase Rotator");
  gtk_widget_set_tooltip_text(phrot_enable, "All-pass phase rotator: reduces speech-waveform asymmetry for more average talk power");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(phrot_enable), tx->phrot_run);
  gtk_grid_attach(GTK_GRID(phrot_grid), phrot_enable, 0,0,2,1);
  g_signal_connect(phrot_enable,"toggled",G_CALLBACK(phrot_run_cb),tx);

  GtkWidget *phrot_corner_label=gtk_label_new("Corner (Hz):");
  gtk_grid_attach(GTK_GRID(phrot_grid), phrot_corner_label,0,1,1,1);
  GtkWidget *phrot_corner_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,100,800,1);
  gtk_widget_set_size_request(phrot_corner_scale, 120, 28);
  gtk_range_set_value(GTK_RANGE(phrot_corner_scale), tx->phrot_corner);
  sui_scale_show_value(phrot_corner_scale,0);
  g_signal_connect(G_OBJECT(phrot_corner_scale),"value_changed",G_CALLBACK(phrot_corner_cb),tx);
  gtk_grid_attach(GTK_GRID(phrot_grid), phrot_corner_scale,1,1,1,1);

  GtkWidget *phrot_nstages_label=gtk_label_new("Stages:");
  gtk_grid_attach(GTK_GRID(phrot_grid), phrot_nstages_label,0,2,1,1);
  GtkWidget *phrot_nstages_spin=gtk_spin_button_new_with_range(1,12,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(phrot_nstages_spin), tx->phrot_nstages);
  g_signal_connect(G_OBJECT(phrot_nstages_spin),"value_changed",G_CALLBACK(phrot_nstages_cb),tx);
  gtk_grid_attach(GTK_GRID(phrot_grid), phrot_nstages_spin,1,2,1,1);

  update_transmitter_dialog(tx);

  return grid;
}
