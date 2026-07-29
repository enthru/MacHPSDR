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

#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "settings_ui.h"

#ifdef CWDAEMON
#include "cwdaemon.h"
#endif

#include "cw_dialog.h"

static GtkWidget *cw_keyer_sidetone_frequency_b;
static GtkWidget *cw_keyer_speed_b;
static GtkWidget *cw_keyer_weight_b;
static GtkWidget *cw_keyer_sidetone_level_b;
static GtkWidget *cw_cwd_sidetone_b;

#ifdef CWDAEMON
static GtkWidget *cwport;
#endif

static void cw_keyer_speed_value_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cw_keyer_speed=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void cw_breakin_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cw_breakin=radio->cw_breakin==1?0:1;
}

#ifdef CWDAEMON
static void cw_gen_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  radio->cw_generation_mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  radio_change_cwgeneration(radio);
}

static void cw_cwd_sidetone_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cwd_sidetone=radio->cwd_sidetone==1?0:1;
}
#endif

static void cw_keyer_hang_time_value_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cw_keyer_hang_time=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void cw_keyer_weight_value_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cw_keyer_weight=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void cw_keys_reversed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cw_keys_reversed=radio->cw_keys_reversed==1?0:1;
}

static void cw_memory_changed_cb(GtkEditable *editable, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int idx=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(editable),"cw-mem-index"));
  if(idx<0 || idx>=CW_N_MEMORIES) return;
  g_strlcpy(radio->cw_memory[idx],gtk_editable_get_text(GTK_EDITABLE(editable)),sizeof(radio->cw_memory[idx]));
}

static void cw_keyer_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cw_keyer_mode=(int)gtk_drop_down_get_selected(widget);
}

static void cw_keyer_sidetone_level_value_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cw_keyer_sidetone_volume=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void cw_keyer_sidetone_frequency_value_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int i;

  radio->cw_keyer_sidetone_frequency=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
  for(i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]!=NULL) {
      receiver_filter_changed(radio->receiver[i],radio->receiver[i]->filter_a);
    }
  }
}

#ifdef CWDAEMON
static void cwdaemon_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cwdaemon=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  if(radio->cwdaemon) {
    log_info("Starting CWdaemon\n");
    radio->cwdaemon = cwdaemon_start();
    gtk_widget_set_sensitive(cw_keyer_speed_b, FALSE);
    gtk_widget_set_sensitive(cw_keyer_sidetone_frequency_b, FALSE);
    gtk_widget_set_sensitive(cw_keyer_weight_b, FALSE);
    gtk_widget_set_sensitive(cw_keyer_sidetone_level_b, FALSE);
    gtk_widget_set_sensitive(cw_cwd_sidetone_b, FALSE);
    gtk_widget_set_sensitive(cwport, FALSE);
  }
  else {
    cwdaemon_stop();
    gtk_widget_set_sensitive(cw_keyer_speed_b, TRUE);
    gtk_widget_set_sensitive(cw_keyer_sidetone_frequency_b, TRUE);
    gtk_widget_set_sensitive(cw_keyer_weight_b, TRUE);
    gtk_widget_set_sensitive(cw_keyer_sidetone_level_b, TRUE);
    gtk_widget_set_sensitive(cw_cwd_sidetone_b, TRUE);
    gtk_widget_set_sensitive(cwport, TRUE);
  }
}

static void cwport_value_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->cwd_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}
#endif

// Audio Peak Filter (CW) — moved here from the RX page (it only does anything in
// CWL/CWU). The apf_* fields are per-receiver, so these act on the active RX;
// set_apf() re-reads all three fields and gates on mode.
static void apf_enable_cb(GtkCheckButton *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  RECEIVER *rx=radio->active_receiver;
  if(rx==NULL) return;
  rx->apf_enable=gtk_check_button_get_active(widget);
  set_apf(rx);
}
static void apf_bw_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  RECEIVER *rx=radio->active_receiver;
  if(rx==NULL) return;
  rx->apf_bw=gtk_range_get_value(GTK_RANGE(widget));
  set_apf(rx);
}
static void apf_gain_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  RECEIVER *rx=radio->active_receiver;
  if(rx==NULL) return;
  rx->apf_gain=gtk_range_get_value(GTK_RANGE(widget));
  set_apf(rx);
}

GtkWidget *create_cw_dialog(RADIO *radio) {
  int x,y;

  // Match the canonical Configure-page layout (see radio_dialog.c): a
  // sui_style_page root grid holding a titled gtk_frame group, NOT a bare grid
  // as the page root (that lets the widgets stretch to fill the whole tab).
  GtkWidget *page=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(page),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(page),FALSE);
  sui_style_page(page);

  GtkWidget *cw_frame=gtk_frame_new("CW");
  GtkWidget *cw_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(cw_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(cw_grid),FALSE);
  sui_style_group(cw_grid);
  gtk_frame_set_child(GTK_FRAME(cw_frame),cw_grid);
  gtk_grid_attach(GTK_GRID(page),cw_frame,0,0,1,1);

  x=0;
  y=0;

  if (radio->discovered->device != DEVICE_HERMES_LITE2) {

    GtkWidget *cw_keyer_mode_label=gtk_label_new("Keyer Mode:");
    gtk_widget_set_visible(cw_keyer_mode_label, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_mode_label,x++,y,1,1);

    GtkWidget *cw_keyer_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(cw_keyer_combo_box))),"Straight Key");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(cw_keyer_combo_box))),"Mode A");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(cw_keyer_combo_box))),"Mode B");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(cw_keyer_combo_box),radio->cw_keyer_mode);
    g_signal_connect(cw_keyer_combo_box,"notify::selected",G_CALLBACK(cw_keyer_cb),radio);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_combo_box,x++,y,1,1);

    GtkWidget *cw_keyer_reversed_label=gtk_label_new("Keys Reversed:");
    gtk_widget_set_visible(cw_keyer_reversed_label, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_reversed_label,x++,y,1,1);

    GtkWidget *cw_keys_reversed_b=gtk_check_button_new();
    gtk_check_button_set_active (GTK_CHECK_BUTTON (cw_keys_reversed_b), radio->cw_keys_reversed);
    gtk_widget_set_visible(cw_keys_reversed_b, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_keys_reversed_b,x,y,1,1);
    g_signal_connect(cw_keys_reversed_b,"toggled",G_CALLBACK(cw_keys_reversed_cb),radio);
  }

  if (radio->discovered->device == DEVICE_HERMES_LITE2) {
    #ifdef CWDAEMON
    GtkWidget *cw_gen_label = gtk_label_new("CW generation:");
    gtk_widget_set_visible(cw_gen_label, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid), cw_gen_label, x++,y, 1, 1);

    GtkWidget *cw_gen_combo = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(cw_gen_combo))),"Radio");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(cw_gen_combo))),"MacHPSDR");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(cw_gen_combo),radio->cw_generation_mode);
    g_signal_connect(cw_gen_combo, "notify::selected", G_CALLBACK(cw_gen_cb), radio);
    gtk_grid_attach(GTK_GRID(cw_grid), cw_gen_combo, x, y, 1, 1);

    y++;
    x = 0;

    GtkWidget *cwdaemon_label=gtk_label_new("CWdaemon enabled:");
    gtk_widget_set_visible(cwdaemon_label, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cwdaemon_label,x++,y,1,1);

    GtkWidget *cwdaemon_tick=gtk_check_button_new();
    gtk_check_button_set_active (GTK_CHECK_BUTTON (cwdaemon_tick), radio->cwdaemon);
    gtk_widget_set_visible(cwdaemon_tick, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cwdaemon_tick,x++,y,1,1);
    g_signal_connect(cwdaemon_tick,"toggled",G_CALLBACK(cwdaemon_cb),radio);

    GtkWidget *cwport_label=gtk_label_new("Port:");
    gtk_widget_set_visible(cwport_label, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cwport_label,x++,y,1,1);

    cwport=gtk_spin_button_new_with_range(50000.0,52000.0,1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cwport), radio->cwd_port);
    gtk_grid_attach(GTK_GRID(cw_grid),cwport,x,y,1,1);
    g_signal_connect(cwport,"value_changed",G_CALLBACK(cwport_value_changed_cb),NULL);
    #endif
  }

  x=0;
  y++;

  GtkWidget *cw_speed_label=gtk_label_new("CW Speed (WPM)");
  gtk_widget_set_visible(cw_speed_label, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_speed_label,x++,y,1,1);

  cw_keyer_speed_b=gtk_spin_button_new_with_range(1.0,60.0,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(cw_keyer_speed_b),(double)radio->cw_keyer_speed);
  gtk_widget_set_visible(cw_keyer_speed_b, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_speed_b,x++,y,1,1);
  g_signal_connect(cw_keyer_speed_b,"value_changed",G_CALLBACK(cw_keyer_speed_value_changed_cb),radio);

  GtkWidget *cw_keyer_sidetone_level_label=gtk_label_new("Sidetone Level:");
  gtk_widget_set_visible(cw_keyer_sidetone_level_label, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_sidetone_level_label,x++,y,1,1);

  cw_keyer_sidetone_level_b=gtk_spin_button_new_with_range(1.0,radio->discovered->protocol==PROTOCOL_2?255.0:127.0,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(cw_keyer_sidetone_level_b),(double)radio->cw_keyer_sidetone_volume);
  gtk_widget_set_visible(cw_keyer_sidetone_level_b, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_sidetone_level_b,x++,y,1,1);
  g_signal_connect(cw_keyer_sidetone_level_b,"value_changed",G_CALLBACK(cw_keyer_sidetone_level_value_changed_cb),radio);

  x=0;
  y++;

  GtkWidget *cw_keyer_sidetone_frequency_label=gtk_label_new("Sidetone Freq:");
  gtk_widget_set_visible(cw_keyer_sidetone_frequency_label, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_sidetone_frequency_label,x++,y,1,1);

  cw_keyer_sidetone_frequency_b=gtk_spin_button_new_with_range(100.0,1000.0,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(cw_keyer_sidetone_frequency_b),(double)radio->cw_keyer_sidetone_frequency);
  gtk_widget_set_visible(cw_keyer_sidetone_frequency_b, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_sidetone_frequency_b,x++,y,1,1);
  g_signal_connect(cw_keyer_sidetone_frequency_b,"value_changed",G_CALLBACK(cw_keyer_sidetone_frequency_value_changed_cb),radio);

  GtkWidget *cw_keyer_weight_label=gtk_label_new("Weight:");
  gtk_widget_set_visible(cw_keyer_weight_label, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_weight_label,x++,y,1,1);

  cw_keyer_weight_b=gtk_spin_button_new_with_range(0.0,100.0,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(cw_keyer_weight_b),(double)radio->cw_keyer_weight);
  gtk_widget_set_visible(cw_keyer_weight_b, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_weight_b,x++,y,1,1);
  g_signal_connect(cw_keyer_weight_b,"value_changed",G_CALLBACK(cw_keyer_weight_value_changed_cb),radio);

  x=0;
  y++;

  if(radio->discovered->protocol==PROTOCOL_2) {
    GtkWidget *cw_keyer_breakin_label=gtk_label_new("CW Break In:");
    gtk_widget_set_visible(cw_keyer_breakin_label, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_breakin_label,x++,y,1,1);

    GtkWidget *cw_breakin_b=gtk_check_button_new();
    gtk_check_button_set_active (GTK_CHECK_BUTTON (cw_breakin_b), radio->cw_breakin);
    gtk_widget_set_visible(cw_breakin_b, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_breakin_b,x++,y,1,1);
    g_signal_connect(cw_breakin_b,"toggled",G_CALLBACK(cw_breakin_cb),radio);
  }
  #ifdef CWDAEMON
  else {
    if (radio->discovered->device == DEVICE_HERMES_LITE2) {
      GtkWidget *cw_cwd_sidetone_label = gtk_label_new("CWdaemon sidetone:");
      gtk_widget_set_visible(cw_cwd_sidetone_label, TRUE);
      gtk_grid_attach(GTK_GRID(cw_grid),cw_cwd_sidetone_label,x++,y,1,1);

      cw_cwd_sidetone_b = gtk_check_button_new();
      gtk_check_button_set_active (GTK_CHECK_BUTTON (cw_cwd_sidetone_b), radio->cwd_sidetone);
      gtk_widget_set_visible(cw_cwd_sidetone_b, TRUE);
      gtk_grid_attach(GTK_GRID(cw_grid),cw_cwd_sidetone_b,x++,y,1,1);
      g_signal_connect(cw_cwd_sidetone_b,"toggled",G_CALLBACK(cw_cwd_sidetone_cb),radio);
    }
  }

  if(radio->cwdaemon) {
    gtk_widget_set_sensitive(cw_keyer_speed_b, FALSE);
    gtk_widget_set_sensitive(cw_keyer_sidetone_frequency_b, FALSE);
    gtk_widget_set_sensitive(cw_keyer_weight_b, FALSE);
    gtk_widget_set_sensitive(cw_keyer_sidetone_level_b, FALSE);
    gtk_widget_set_sensitive(cw_cwd_sidetone_b, FALSE);
    gtk_widget_set_sensitive(cwport, FALSE);
  }
  #endif

  GtkWidget *cw_keyer_delay_label=gtk_label_new("Break In Delay (Ms):");
  gtk_widget_set_visible(cw_keyer_delay_label, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_delay_label,x++,y,1,1);

  GtkWidget *cw_keyer_hang_time_b=gtk_spin_button_new_with_range(0.0,1000.0,1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(cw_keyer_hang_time_b),(double)radio->cw_keyer_hang_time);
  gtk_widget_set_visible(cw_keyer_hang_time_b, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_keyer_hang_time_b,x++,y,1,1);
  g_signal_connect(cw_keyer_hang_time_b,"value_changed",G_CALLBACK(cw_keyer_hang_time_value_changed_cb),radio);

  // CW message memories (Phase 4.4a): M1..M8 free-text entries, 2 per row,
  // edited straight into radio->cw_memory[] (picked up live by cw_panel.c's
  // memory buttons the next time that panel is (re)built).
  x=0;
  y++;

  GtkWidget *cw_mem_title=gtk_label_new("CW Memories:");
  gtk_widget_set_halign(cw_mem_title, GTK_ALIGN_START);
  gtk_widget_set_visible(cw_mem_title, TRUE);
  gtk_grid_attach(GTK_GRID(cw_grid),cw_mem_title,0,y,4,1);
  y++;

  int cw_mem_row0=y;
  for(int i=0;i<CW_N_MEMORIES;i++) {
    int mrow=cw_mem_row0+i/2;
    int mcol=(i%2)*2;
    char mlbl[8];
    g_snprintf(mlbl,sizeof(mlbl),"M%d:",i+1);
    GtkWidget *cw_mem_label=gtk_label_new(mlbl);
    gtk_widget_set_visible(cw_mem_label, TRUE);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_mem_label,mcol,mrow,1,1);

    GtkWidget *cw_mem_entry=gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(cw_mem_entry),CW_MSG_LEN-1);
    gtk_editable_set_text(GTK_EDITABLE(cw_mem_entry),radio->cw_memory[i]);
    gtk_widget_set_hexpand(cw_mem_entry, TRUE);
    gtk_widget_set_visible(cw_mem_entry, TRUE);
    g_object_set_data(G_OBJECT(cw_mem_entry),"cw-mem-index",GINT_TO_POINTER(i));
    g_signal_connect(cw_mem_entry,"changed",G_CALLBACK(cw_memory_changed_cb),radio);
    gtk_grid_attach(GTK_GRID(cw_grid),cw_mem_entry,mcol+1,mrow,1,1);
  }
  y=cw_mem_row0+(CW_N_MEMORIES+1)/2;

  // Audio Peak Filter (CW): peaks at the CW sidetone; only active in CWL/CWU.
  // Per-RX settings — acts on the active receiver.
  {
    RECEIVER *arx=radio->active_receiver;
    GtkWidget *apf_frame=gtk_frame_new("Audio Peak Filter (CW)");
    gtk_widget_set_halign(apf_frame,GTK_ALIGN_START);
    GtkWidget *apf_grid=gtk_grid_new();
    sui_style_group(apf_grid);
    gtk_frame_set_child(GTK_FRAME(apf_frame),apf_grid);
    gtk_grid_attach(GTK_GRID(page),apf_frame,0,1,1,1);

    GtkWidget *l,*s;
    GtkWidget *apf_enable_b=gtk_check_button_new_with_label("Enable APF");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(apf_enable_b),arx!=NULL && arx->apf_enable);
    g_signal_connect(apf_enable_b,"toggled",G_CALLBACK(apf_enable_cb),radio);
    gtk_grid_attach(GTK_GRID(apf_grid),apf_enable_b,0,0,2,1);

    l=gtk_label_new("Bandwidth (Hz):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(apf_grid),l,0,1,1,1);
    s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(arx!=NULL?arx->apf_bw:100.0,25.0,300.0,5.0,25.0,0.0));
    gtk_widget_set_size_request(s,200,25);
    sui_scale_show_value(s,0);
    gtk_grid_attach(GTK_GRID(apf_grid),s,1,1,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(apf_bw_cb),radio);

    l=gtk_label_new("Gain:");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(apf_grid),l,0,2,1,1);
    s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(arx!=NULL?arx->apf_gain:1.0,0.5,4.0,0.1,0.5,0.0));
    gtk_widget_set_size_request(s,200,25);
    sui_scale_show_value(s,1);
    gtk_grid_attach(GTK_GRID(apf_grid),s,1,2,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(apf_gain_cb),radio);
  }

  return page;
}
