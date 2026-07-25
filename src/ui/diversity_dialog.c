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
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>

#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "receiver.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "diversity_mixer.h"
#include "diversity_dialog.h"
#include "math.h"

static void gain_coarse_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix=(DIVMIXER *)data;
  dmix->gain = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void phase_coarse_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix=(DIVMIXER *)data;  
  dmix->phase = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void gain_fine_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix=(DIVMIXER *)data;
  dmix->gain_fine = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void phase_fine_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix=(DIVMIXER *)data;  
  dmix->phase_fine = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void dmix_adc_cb(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  DIVMIXER *dmix=(DIVMIXER *)data;
  dmix->num_streams = gtk_drop_down_get_selected(dd);
  SetNumStreams(dmix);
}

static void calibrate_gain_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix=(DIVMIXER *)data; 
  dmix->calibrate_gain = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));  
  diversity_mix_calibrate_gain_visuals(dmix);
}

static void dir_flip_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix=(DIVMIXER *)data; 
  dmix->flip = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));  
  
  if (dmix->flip) {
    dmix->phase += 180;
  }
  else {
    dmix->phase -= 180;
  }
  
  set_gain_phase(dmix);
}


GtkWidget *create_diversity_dialog(DIVMIXER *dmix) {
  int i;
  int col=0;
  int row=0;

  GtkWidget *grid=gtk_grid_new();
  sui_style_page(grid);
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);

  row=0;
  col=0;
  
  gtk_grid_set_column_spacing (GTK_GRID(grid),10);
  gtk_grid_set_row_spacing (GTK_GRID(grid),10);

  GtkWidget *gain_coarse_label=gtk_label_new("Gain (dB, coarse):");
  gtk_label_set_xalign(GTK_LABEL(gain_coarse_label), 0.0);
  gtk_widget_set_visible(gain_coarse_label, TRUE);
  gtk_grid_attach(GTK_GRID(grid),gain_coarse_label,0,1,1,1);
  
  GtkWidget* gain_coarse_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-25.0,+25.0,0.5);
  gtk_widget_set_size_request (gain_coarse_scale, 300, 25);
  gtk_range_set_value(GTK_RANGE(gain_coarse_scale),dmix->gain);
  sui_scale_show_value(gain_coarse_scale,1);
  gtk_widget_set_visible(gain_coarse_scale, TRUE);
  gtk_grid_attach(GTK_GRID(grid),gain_coarse_scale,1,1,1,1);
  g_signal_connect(G_OBJECT(gain_coarse_scale),"value_changed",G_CALLBACK(gain_coarse_changed_cb),dmix);

  GtkWidget *phase_coarse_label=gtk_label_new("Phase (coarse):");
  gtk_label_set_xalign(GTK_LABEL(phase_coarse_label), 0.0);
  gtk_widget_set_visible(phase_coarse_label, TRUE);
  gtk_grid_attach(GTK_GRID(grid),phase_coarse_label,2,1,1,1);

  GtkWidget* phase_coarse_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-180.0,180.0,1.0);
  gtk_widget_set_size_request (phase_coarse_scale, 300, 25);
  gtk_range_set_value(GTK_RANGE(phase_coarse_scale),dmix->phase);
  sui_scale_show_value(phase_coarse_scale,0);
  gtk_widget_set_visible(phase_coarse_scale, TRUE);
  gtk_grid_attach(GTK_GRID(grid),phase_coarse_scale,3,1,1,1);
  g_signal_connect(G_OBJECT(phase_coarse_scale),"value_changed",G_CALLBACK(phase_coarse_changed_cb),dmix);

  // Fine tuning of phase/gain
  GtkWidget *gain_fine_label=gtk_label_new("Gain (dB, fine):");
  gtk_label_set_xalign(GTK_LABEL(gain_fine_label), 0.0);
  gtk_widget_set_visible(gain_fine_label, TRUE);
  gtk_grid_attach(GTK_GRID(grid),gain_fine_label,0,2,1,1);
  
  GtkWidget* gain_fine_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-2.0,+2.0,0.05);
  gtk_widget_set_size_request (gain_fine_scale, 300, 25);
  gtk_range_set_value(GTK_RANGE(gain_fine_scale), dmix->gain_fine);
  sui_scale_show_value(gain_fine_scale,2);
  gtk_widget_set_visible(gain_coarse_scale, TRUE);
  gtk_grid_attach(GTK_GRID(grid),gain_fine_scale,1,2,1,1);
  g_signal_connect(G_OBJECT(gain_fine_scale),"value_changed",G_CALLBACK(gain_fine_changed_cb),dmix);

  GtkWidget *phase_fine_label=gtk_label_new("Phase (fine):");
  gtk_label_set_xalign(GTK_LABEL(phase_fine_label), 0.0);
  gtk_widget_set_visible(phase_fine_label, TRUE);
  gtk_grid_attach(GTK_GRID(grid),phase_fine_label,2,2,1,1);

  GtkWidget* phase_fine_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,-2.0,2.0,0.05);
  gtk_widget_set_size_request (phase_fine_scale, 300, 25);
  gtk_range_set_value(GTK_RANGE(phase_fine_scale),dmix->phase_fine);
  sui_scale_show_value(phase_fine_scale,2);
  gtk_widget_set_visible(phase_fine_scale, TRUE);
  gtk_grid_attach(GTK_GRID(grid),phase_fine_scale,3,2,1,1);
  g_signal_connect(G_OBJECT(phase_fine_scale),"value_changed",G_CALLBACK(phase_fine_changed_cb),dmix);

  // ADC1, ADC2 or ADC1+ADC2 (diversity mode)
  const char *dmix_adc_opts[] = {"ADC1","ADC2","ADC1+ADC2",NULL};
  GtkWidget *dmix_adc_combo=gtk_drop_down_new_from_strings(dmix_adc_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dmix_adc_combo),dmix->num_streams);
  gtk_grid_attach(GTK_GRID(grid),dmix_adc_combo,0,3,2,1);
  g_signal_connect(dmix_adc_combo,"notify::selected",G_CALLBACK(dmix_adc_cb),dmix);

  // Calibrate gain
  GtkWidget *calibrate_gain_label = gtk_label_new("Calibrate gain:");
  gtk_label_set_xalign(GTK_LABEL(calibrate_gain_label), 0.0);    
  gtk_widget_set_visible(calibrate_gain_label, TRUE);
  gtk_grid_attach(GTK_GRID(grid), calibrate_gain_label, 0, 4, 1, 1);
  
  GtkWidget *calibrate_gain_b = gtk_check_button_new();
  gtk_check_button_set_active(GTK_CHECK_BUTTON(calibrate_gain_b), dmix->calibrate_gain);
  gtk_widget_set_visible(calibrate_gain_b, TRUE);
  gtk_grid_attach(GTK_GRID(grid), calibrate_gain_b, 1, 4, 1, 1);
  g_signal_connect(calibrate_gain_b,"toggled",G_CALLBACK(calibrate_gain_cb), dmix); 


  // 180 deg flip on phase
  GtkWidget *dir_flip_label = gtk_label_new("Flip:");
  gtk_label_set_xalign(GTK_LABEL(dir_flip_label), 0.0);    
  gtk_widget_set_visible(dir_flip_label, TRUE);
  gtk_grid_attach(GTK_GRID(grid), dir_flip_label, 0, 5, 1, 1);
  
  GtkWidget *dir_flip_b = gtk_check_button_new();
  gtk_check_button_set_active(GTK_CHECK_BUTTON(dir_flip_b), dmix->flip);
  gtk_widget_set_visible(dir_flip_b, TRUE);
  gtk_grid_attach(GTK_GRID(grid), dir_flip_b, 1, 5, 1, 1);
  g_signal_connect(dir_flip_b,"toggled",G_CALLBACK(dir_flip_cb), dmix);

  // Honest disclaimer (mirrors the PureSignal note): the diversity path needs
  // two coherent ADC streams and has not been verified on hardware in this fork.
  GtkWidget *note=gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(note),
    "<small><i>Note: diversity reception combines two coherent ADC streams to null "
    "interference / fight fading. Like PureSignal it is Protocol 1 only and has NOT "
    "been verified on hardware in this fork — it needs a radio with two receivers/ADCs. "
    "Requires testing on real hardware; use at your own risk.</i></small>");
  gtk_label_set_wrap(GTK_LABEL(note),TRUE);
  gtk_label_set_xalign(GTK_LABEL(note),0.0);
  gtk_widget_set_size_request(note,600,-1);
  gtk_grid_attach(GTK_GRID(grid),note,0,6,4,1);

  return grid;
}
