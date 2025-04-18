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
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#include "audio.h"
#include "band.h"
#include "hl2.h"

static GtkWidget *bias_label;
static GtkWidget* hl2mrf_scale;
static GtkWidget* hl2mrf_save;

static void pa_value_changed_cb(GtkWidget *widget, gpointer data) {
  BAND *band=(BAND *)data;

  band->pa_calibration=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void pa_disable_changed_cb(GtkWidget *widget, gpointer data) {
  BAND *band=(BAND *)data;

  band->disablePA = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  //if(radio->discovered->protocol==PROTOCOL_2) {
  //  protocol2_high_priority();
  //}
}

static void hl2mrf_bias_save_cb(GtkWidget *widget,gpointer data) { 
  HERMESLITE2 *hl2=(HERMESLITE2 *)data;  
  g_print("Saving bias value %d\n", radio->hl2->mrf101_bias_value);
  HL2mrf101StoreBias(hl2);
}

static void hl2mrf_bias_changed_cb(GtkWidget *widget, gpointer data) {
  HERMESLITE2 *hl2=(HERMESLITE2 *)data;
  hl2->mrf101_bias_value = (int)gtk_range_get_value(GTK_RANGE(widget));
  g_print("Set bias %i\n", hl2->mrf101_bias_value);
  HL2mrf101SetBias(hl2);
}

static void enable_bias_cb(GtkWidget *widget, gpointer data) {
  HERMESLITE2 *hl2=(HERMESLITE2 *)data;  

  hl2->mrf101_bias_enable = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
  if (hl2->mrf101_bias_enable == TRUE) {
    g_print("Bias enabled\n");
    gtk_widget_set_sensitive(hl2mrf_scale, TRUE);    
    gtk_widget_set_sensitive(hl2mrf_save, TRUE);
    g_print("Bias enabled_\n");
  }
  else {
    gtk_widget_set_sensitive(hl2mrf_scale, FALSE);
    gtk_widget_set_sensitive(hl2mrf_save, FALSE);
  }
}

GtkWidget *create_pa_dialog(RADIO *r) {
  int i;
  g_print("%s\n",__FUNCTION__);
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);

  int row=0;
  int col=0;
  
  GtkWidget *pa_frame=gtk_frame_new("PA Calibration");
  GtkWidget *pa_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(pa_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(pa_grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(pa_grid),15);
  gtk_container_add(GTK_CONTAINER(pa_frame),pa_grid);
  gtk_grid_attach(GTK_GRID(grid),pa_frame,col,row++,1,1);

  int stop_at = bandGen;
  #ifdef SOAPYSDR
    stop_at = bandAIR;
  #endif

  for(i=0; i <= stop_at; i++) {
    BAND *band=band_get_band(i);

    GtkWidget *band_label=gtk_label_new(band->title);
    gtk_widget_show(band_label);
    gtk_grid_attach(GTK_GRID(pa_grid),band_label,(i%3)*3,i/3,1,1);

    GtkWidget *pa_disable_b = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pa_disable_b), band->disablePA);
    gtk_widget_show(pa_disable_b);   
    gtk_grid_attach(GTK_GRID(pa_grid), pa_disable_b, ((i%3)*3)+1,i/3,1,1);
    g_signal_connect(pa_disable_b,"toggled",G_CALLBACK(pa_disable_changed_cb), band); 
    
    GtkWidget *pa_r=gtk_spin_button_new_with_range(38.8,100.0,0.1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pa_r),(double)band->pa_calibration);
    gtk_widget_show(pa_r);
    gtk_grid_attach(GTK_GRID(pa_grid),pa_r,((i%3)*3)+2,i/3,1,1);
    g_signal_connect(pa_r,"value_changed",G_CALLBACK(pa_value_changed_cb),band);
  }
  
  // Hermes-Lite 2 companion card 100 W PA hl2-mrf101
  // bias is set by digital pot (MCP4561) on the hl2-mrf101 PCB
  if ((radio->discovered->device==DEVICE_HERMES_LITE2) && (r->hl2 != NULL) && (radio->filter_board == HL2_MRF101)) {   
    
    // ****************** TEMP
    // Read the stored bias  
    HL2mrf101ReadBias(r->hl2);
    // ****************** TEMP    

    
    GtkWidget *hl2mrf_frame = gtk_frame_new("HL2-MRF101");
    GtkWidget *hl2mrf_grid = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(hl2mrf_grid), FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(hl2mrf_grid), FALSE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_container_add(GTK_CONTAINER(hl2mrf_frame), hl2mrf_grid);
    gtk_grid_attach(GTK_GRID(grid), hl2mrf_frame, col, row++, 1, 1);

    int x = 0;
    int y = 0;

    // Only allow bias to be set if user enables this tick box, this acts as
    // a caution to make sure user thinks about what they are doing
    GtkWidget *enable_bias_b = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_bias_b), r->hl2->mrf101_bias_enable);
    gtk_widget_show(enable_bias_b);
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), enable_bias_b, x, y, 1, 1);
    g_signal_connect(enable_bias_b,"toggled",G_CALLBACK(enable_bias_cb), r->hl2); 
    
    x++;
    
    GtkWidget *enable_bias_label = gtk_label_new("Enable bias adjustment");
    gtk_misc_set_alignment(GTK_MISC(enable_bias_label), 0, 0);    
    gtk_widget_show(enable_bias_label);
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), enable_bias_label, x, y++, 1, 1);
    x = 0;
    
    // Bias slider
    bias_label = gtk_label_new("Bias:");
    gtk_misc_set_alignment(GTK_MISC(bias_label), 0, 0);
    gtk_widget_show(bias_label);
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), bias_label, x, y, 1, 1);

    x++;    

    hl2mrf_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0, 255, 1);
    gtk_widget_set_size_request (hl2mrf_scale, 300, 25);
    gtk_range_set_value(GTK_RANGE(hl2mrf_scale), r->hl2->mrf101_bias_value);
    gtk_widget_show(hl2mrf_scale);
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), hl2mrf_scale, x, y, 1, 1);
    g_signal_connect(G_OBJECT(hl2mrf_scale), "value_changed", G_CALLBACK(hl2mrf_bias_changed_cb), r->hl2);

    // The values have so far been set in volative space in the MCP4561, this saves value to EEPROM
    x++;
    
    hl2mrf_save = gtk_button_new_with_label("Save bias");
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), hl2mrf_save, x, y, 1, 1);
    g_signal_connect(hl2mrf_save, "clicked", G_CALLBACK(hl2mrf_bias_save_cb), r->hl2);  
    
    if (r->hl2->mrf101_bias_enable == FALSE) {    
      gtk_widget_set_sensitive(hl2mrf_scale, FALSE);    
      gtk_widget_set_sensitive(hl2mrf_save, FALSE);       
    }
  }

  return grid;
}
