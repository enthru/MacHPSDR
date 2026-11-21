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

  band->disablePA = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  //if(radio->discovered->protocol==PROTOCOL_2) {
  //  protocol2_high_priority();
  //}
}

// Power-meter calibration: a forward and a reflected factor per frequency
// segment (tx_meter_cal.h), and a "match" helper that sets the factor of the
// segment being transmitted on from what an external wattmeter reads while a
// steady carrier (Tune) is on the air.
static GtkWidget *meter_cal_spin[2][TX_METER_CAL_SEGMENTS];   // [reverse][segment]
static GtkWidget *meter_cal_reading;
static GtkWidget *meter_cal_status;

static gdouble *meter_cal_field(gboolean reverse, int seg) {
  return reverse ? &radio->transmitter->rev_cal[seg] : &radio->transmitter->fwd_cal[seg];
}

static void meter_cal_factor_cb(GtkWidget *widget, gpointer data) {
  int id=GPOINTER_TO_INT(data);
  gboolean reverse=id/TX_METER_CAL_SEGMENTS;
  int seg=id%TX_METER_CAL_SEGMENTS;
  *meter_cal_field(reverse,seg)=tx_meter_cal_clamp(gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
  log_info("PA: %s %s power calibration x%.3f\n",tx_meter_cal_segment_name(seg),
           reverse?"reflected":"forward",*meter_cal_field(reverse,seg));
}

static void meter_cal_match_cb(GtkWidget *widget, gpointer data) {
  gboolean reverse=GPOINTER_TO_INT(data);
  TRANSMITTER *tx=radio->transmitter;
  double shown=reverse ? tx->rev : tx->fwd;
  double actual=gtk_spin_button_get_value(GTK_SPIN_BUTTON(meter_cal_reading));

  // The meter is only live while keyed, and a near-zero reading would turn
  // into an absurd factor -- refuse rather than guess.
  if(!isTransmitting(radio)) {
    gtk_label_set_text(GTK_LABEL(meter_cal_status),"Key a carrier (Tune) first");
    return;
  }
  if(!(shown > 0.1) || !(actual > 0.0)) {
    gtk_label_set_text(GTK_LABEL(meter_cal_status),"Too little power to match");
    return;
  }

  // The segment the meter is reading with right now, so `shown` divides
  // back to the raw watts.
  int seg=tx_meter_cal_segment(transmitter_get_frequency(tx));
  gdouble *cal=meter_cal_field(reverse,seg);
  double raw=shown/(*cal);
  double want=actual/raw;
  double got=tx_meter_cal_clamp(want);
  *cal=got;
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(meter_cal_spin[reverse][seg]),got);   // emits factor_cb, same value

  char text[96];
  g_snprintf(text,sizeof(text),"%s %s: %s x%.3f",tx_meter_cal_segment_name(seg),
             reverse?"reflected":"forward",got!=want?"clamped to":"set",got);
  gtk_label_set_text(GTK_LABEL(meter_cal_status),text);
  log_info("PA: %s %s power matched: showed %.2f W, meter %.2f W -> x%.3f\n",
           tx_meter_cal_segment_name(seg),reverse?"reflected":"forward",shown,actual,got);
}

static void meter_cal_build(GtkWidget *grid) {
  GtkWidget *fwd_head=gtk_label_new("Forward ×");
  gtk_grid_attach(GTK_GRID(grid),fwd_head,1,0,1,1);
  GtkWidget *rev_head=gtk_label_new("Reflected ×");
  gtk_grid_attach(GTK_GRID(grid),rev_head,2,0,1,1);

  for(int seg=0;seg<TX_METER_CAL_SEGMENTS;seg++) {
    GtkWidget *label=gtk_label_new(tx_meter_cal_segment_name(seg));
    sui_label_left(label);
    gtk_grid_attach(GTK_GRID(grid),label,0,seg+1,1,1);
    for(int reverse=0;reverse<2;reverse++) {
      GtkWidget *spin=gtk_spin_button_new_with_range(TX_METER_CAL_MIN,TX_METER_CAL_MAX,0.01);
      gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin),3);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin),*meter_cal_field(reverse,seg));
      gtk_widget_set_tooltip_text(spin,reverse
          ? "Multiplies the reflected power the radio reports while transmitting in this segment (1.000 = uncalibrated). SWR follows from forward and reflected together."
          : "Multiplies the forward power the radio reports while transmitting in this segment (1.000 = uncalibrated). A tandem match's coupling is not flat across HF.");
      gtk_grid_attach(GTK_GRID(grid),spin,reverse+1,seg+1,1,1);
      g_signal_connect(spin,"value_changed",G_CALLBACK(meter_cal_factor_cb),
                       GINT_TO_POINTER(reverse*TX_METER_CAL_SEGMENTS+seg));
      meter_cal_spin[reverse][seg]=spin;
    }
  }

  int y=TX_METER_CAL_SEGMENTS+1;
  GtkWidget *meter_label=gtk_label_new("External meter reads (W)");
  sui_label_left(meter_label);
  gtk_grid_attach(GTK_GRID(grid),meter_label,0,y,1,1);

  meter_cal_reading=gtk_spin_button_new_with_range(0.0,2000.0,0.1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(meter_cal_reading),1);
  gtk_grid_attach(GTK_GRID(grid),meter_cal_reading,1,y,1,1);

  for(int reverse=0;reverse<2;reverse++) {
    GtkWidget *match=gtk_button_new_with_label(reverse ? "Match reflected" : "Match forward");
    gtk_widget_set_tooltip_text(match,reverse
        ? "While transmitting into a known mismatch, set the reflected factor of the segment being transmitted on so the reflected power shown equals the external meter"
        : "While transmitting a steady carrier (Tune), set the forward factor of the segment being transmitted on so the forward power shown equals the external meter");
    gtk_grid_attach(GTK_GRID(grid),match,2+reverse,y,1,1);
    g_signal_connect(match,"clicked",G_CALLBACK(meter_cal_match_cb),GINT_TO_POINTER(reverse));
  }

  meter_cal_status=gtk_label_new("");
  sui_label_left(meter_cal_status);
  gtk_grid_attach(GTK_GRID(grid),meter_cal_status,0,y+1,4,1);
}

static void swr_protect_cb(GtkDropDown *dd, GParamSpec *pspec, gpointer data) {
  guint sel=gtk_drop_down_get_selected(dd);
  if(sel>=SWR_PROTECT_MODES) return;
  radio->swr_protect=(gint)sel;
  log_info("PA: SWR protection %s\n",sel==SWR_PROTECT_BLOCK?"block":sel==SWR_PROTECT_REDUCE?"reduce":"indicate");
  radio_swr_reset(radio);   // a block or cut from the old policy does not outlive it
}

static void swr_ignore_tune_cb(GtkWidget *widget, gpointer data) {
  radio->swr_protect_ignore_tune=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void hl2mrf_bias_save_cb(GtkWidget *widget,gpointer data) { 
  HERMESLITE2 *hl2=(HERMESLITE2 *)data;  
  log_info("Saving bias value %d\n", radio->hl2->mrf101_bias_value);
  HL2mrf101StoreBias(hl2);
}

static void hl2mrf_bias_changed_cb(GtkWidget *widget, gpointer data) {
  HERMESLITE2 *hl2=(HERMESLITE2 *)data;
  hl2->mrf101_bias_value = (int)gtk_range_get_value(GTK_RANGE(widget));
  log_info("Set bias %i\n", hl2->mrf101_bias_value);
  HL2mrf101SetBias(hl2);
}

static void enable_bias_cb(GtkWidget *widget, gpointer data) {
  HERMESLITE2 *hl2=(HERMESLITE2 *)data;  

  hl2->mrf101_bias_enable = gtk_check_button_get_active (GTK_CHECK_BUTTON (widget));
  if (hl2->mrf101_bias_enable == TRUE) {
    log_info("Bias enabled\n");
    gtk_widget_set_sensitive(hl2mrf_scale, TRUE);    
    gtk_widget_set_sensitive(hl2mrf_save, TRUE);
    log_info("Bias enabled_\n");
  }
  else {
    gtk_widget_set_sensitive(hl2mrf_scale, FALSE);
    gtk_widget_set_sensitive(hl2mrf_save, FALSE);
  }
}

GtkWidget *create_pa_dialog(RADIO *r) {
  int i;
  log_info("%s\n",__FUNCTION__);
  GtkWidget *grid=gtk_grid_new();
  sui_style_page(grid);
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
  sui_style_group(pa_grid);
  gtk_frame_set_child(GTK_FRAME(pa_frame),pa_grid);
  gtk_grid_attach(GTK_GRID(grid),pa_frame,col,row++,1,1);

  int stop_at = bandGen;
  #ifdef SOAPYSDR
    stop_at = bandAIR;
  #endif

  for(i=0; i <= stop_at; i++) {
    BAND *band=band_get_band(i);

    GtkWidget *band_label=gtk_label_new(band_display_name(i));
    gtk_widget_set_visible(band_label, TRUE);
    gtk_grid_attach(GTK_GRID(pa_grid),band_label,(i%3)*3,i/3,1,1);

    GtkWidget *pa_disable_b = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(pa_disable_b), band->disablePA);
    gtk_widget_set_visible(pa_disable_b, TRUE);   
    gtk_grid_attach(GTK_GRID(pa_grid), pa_disable_b, ((i%3)*3)+1,i/3,1,1);
    g_signal_connect(pa_disable_b,"toggled",G_CALLBACK(pa_disable_changed_cb), band); 
    
    GtkWidget *pa_r=gtk_spin_button_new_with_range(38.8,100.0,0.1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pa_r),(double)band->pa_calibration);
    gtk_widget_set_visible(pa_r, TRUE);
    gtk_grid_attach(GTK_GRID(pa_grid),pa_r,((i%3)*3)+2,i/3,1,1);
    g_signal_connect(pa_r,"value_changed",G_CALLBACK(pa_value_changed_cb),band);
  }
  
  GtkWidget *cal_frame=gtk_frame_new("Power Meter Calibration");
  GtkWidget *cal_grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(cal_grid),10);
  sui_style_group(cal_grid);
  gtk_frame_set_child(GTK_FRAME(cal_frame),cal_grid);
  gtk_grid_attach(GTK_GRID(grid),cal_frame,col,row++,1,1);
  meter_cal_build(cal_grid);

  GtkWidget *swr_frame=gtk_frame_new("SWR Protection");
  GtkWidget *swr_grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(swr_grid),10);
  sui_style_group(swr_grid);
  gtk_frame_set_child(GTK_FRAME(swr_frame),swr_grid);
  gtk_grid_attach(GTK_GRID(grid),swr_frame,col,row++,1,1);

  GtkWidget *swr_label=gtk_label_new("Above the SWR alarm");
  sui_label_left(swr_label);
  gtk_grid_attach(GTK_GRID(swr_grid),swr_label,0,0,1,1);
  // Row order is the SWR_PROTECT_* enum.
  const char *swr_modes[]={ "Indicate only", "Reduce power", "Block transmit", NULL };
  GtkWidget *swr_dd=gtk_drop_down_new_from_strings(swr_modes);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(swr_dd),(guint)r->swr_protect);
  gtk_widget_set_tooltip_text(swr_dd,
      "What happens when the SWR stays above the alarm (Configure \342\206\222 Radio) "
      "for 0.3 s while transmitting:\n"
      "Indicate only \342\200\224 the SWR indicator lights\n"
      "Reduce power \342\200\224 drive is cut to a quarter (-6 dB) for the rest of the over\n"
      "Block transmit \342\200\224 the transmitter is released, drive held at zero and keying "
      "refused until the SWR indicator is clicked");
  gtk_grid_attach(GTK_GRID(swr_grid),swr_dd,1,0,1,1);
  g_signal_connect(swr_dd,"notify::selected",G_CALLBACK(swr_protect_cb),NULL);

  GtkWidget *swr_tune_b=gtk_check_button_new_with_label("Not during Tune (ATU)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(swr_tune_b),r->swr_protect_ignore_tune);
  gtk_widget_set_tooltip_text(swr_tune_b,
      "An antenna tuner tunes exactly while the SWR is high, at the Tune power; "
      "leave this on to let it, untick to protect the Tune carrier too");
  gtk_grid_attach(GTK_GRID(swr_grid),swr_tune_b,2,0,1,1);
  g_signal_connect(swr_tune_b,"toggled",G_CALLBACK(swr_ignore_tune_cb),NULL);

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
    sui_style_group(hl2mrf_grid);
    gtk_frame_set_child(GTK_FRAME(hl2mrf_frame), hl2mrf_grid);
    gtk_grid_attach(GTK_GRID(grid), hl2mrf_frame, col, row++, 1, 1);

    int x = 0;
    int y = 0;

    // Only allow bias to be set if user enables this tick box, this acts as
    // a caution to make sure user thinks about what they are doing
    GtkWidget *enable_bias_b = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(enable_bias_b), r->hl2->mrf101_bias_enable);
    gtk_widget_set_visible(enable_bias_b, TRUE);
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), enable_bias_b, x, y, 1, 1);
    g_signal_connect(enable_bias_b,"toggled",G_CALLBACK(enable_bias_cb), r->hl2); 
    
    x++;
    
    GtkWidget *enable_bias_label = gtk_label_new("Enable bias adjustment");
    gtk_label_set_xalign(GTK_LABEL(enable_bias_label), 0.0);    
    gtk_widget_set_visible(enable_bias_label, TRUE);
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), enable_bias_label, x, y++, 1, 1);
    x = 0;
    
    // Bias slider
    bias_label = gtk_label_new("Bias:");
    gtk_label_set_xalign(GTK_LABEL(bias_label), 0.0);
    gtk_widget_set_visible(bias_label, TRUE);
    gtk_grid_attach(GTK_GRID(hl2mrf_grid), bias_label, x, y, 1, 1);

    x++;    

    hl2mrf_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0, 255, 1);
    gtk_widget_set_size_request (hl2mrf_scale, 300, 25);
    gtk_range_set_value(GTK_RANGE(hl2mrf_scale), r->hl2->mrf101_bias_value);
    sui_scale_show_value(hl2mrf_scale,0);
    gtk_widget_set_visible(hl2mrf_scale, TRUE);
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
