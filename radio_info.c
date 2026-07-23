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

#include <wdsp.h>

#include "discovered.h"
#include "bpsk.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "radio_info.h"
#include "main.h"
#include "vfo.h"
#include "configure_dialog.h"

#ifdef MIDI
// GtkButton "clicked" (fires on user click, not the frequent programmatic
// set_active of this status lamp): open the Configure dialog on the MIDI page.
static void midi_b_press_cb(GtkButton *widget,gpointer data) {
  if(radio->dialog==NULL) {
    radio->dialog=create_configure_dialog(radio,-1);
  }
  configure_dialog_set_page("MIDI");
}
#endif

GtkWidget *create_radio_info_visual(RECEIVER *rx) {
  RADIO_INFO *info=g_new(RADIO_INFO,1);

  // Two rows of status buttons (warnings on top, connections below), centred
  // vertically within the info block so they line up with the VFO/meter beside
  // them. The buttons size themselves to their labels via CSS (matching the
  // VFO mode/filter buttons) rather than the old fixed 40px absolute layout.
  info->radio_info=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
  gtk_widget_set_name(info->radio_info,"info");
  gtk_widget_set_valign(info->radio_info,GTK_ALIGN_CENTER);
  gtk_widget_set_halign(info->radio_info,GTK_ALIGN_START);

  GtkWidget *row_top=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
  GtkWidget *row_bot=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
  gtk_box_append(GTK_BOX(info->radio_info),row_top);
  gtk_box_append(GTK_BOX(info->radio_info),row_bot);

  // ********** WARNINGS ****************************
  // HL2 Buffer over/underflow
  if(radio->discovered->protocol==PROTOCOL_1) {
    info->qos_b = gtk_toggle_button_new_with_label("QOS");
    gtk_widget_set_name(info->qos_b, "info-warning");
    gtk_box_append(GTK_BOX(row_top),info->qos_b);
  }

  // HERMES/HL2 ADC Clipping
  info->adc_overload_b=gtk_toggle_button_new_with_label("ADC");
  gtk_widget_set_name(info->adc_overload_b,"info-warning");
  gtk_box_append(GTK_BOX(row_top),info->adc_overload_b);

  // SWR is above a threshold
  info->swr_b=gtk_toggle_button_new_with_label("SWR");
  gtk_widget_set_name(info->swr_b,"info-warning");
  gtk_box_append(GTK_BOX(row_top),info->swr_b);

  if (radio->discovered->device == DEVICE_HERMES_LITE2) {
    info->temp_b=gtk_toggle_button_new_with_label("TEMP");
    gtk_widget_set_name(info->temp_b,"info-warning");
    gtk_box_append(GTK_BOX(row_top),info->temp_b);
  }

  // CAT
  info->cat_b=gtk_toggle_button_new_with_label("CAT");
  gtk_widget_set_name(info->cat_b,"info-button");
  gtk_box_append(GTK_BOX(row_bot),info->cat_b);

#ifdef CWDAEMON
  info->cwdaemon_b=gtk_toggle_button_new_with_label("CWD");
  gtk_widget_set_name(info->cwdaemon_b,"info-button");
  gtk_box_append(GTK_BOX(row_bot),info->cwdaemon_b);
#endif

#ifdef MIDI
  // MIDI
  info->midi_b=gtk_toggle_button_new_with_label("MIDI");
  gtk_widget_set_name(info->midi_b,"info-button");
  g_signal_connect(info->midi_b, "clicked", G_CALLBACK(midi_b_press_cb),rx);
  gtk_box_append(GTK_BOX(row_bot),info->midi_b);
#endif

  g_object_set_data ((GObject *)info->radio_info,"info_data",info);

  return info->radio_info;
}

extern int midi_rx;

void update_radio_info(RECEIVER *rx) {
  RADIO_INFO *info=(RADIO_INFO *)g_object_get_data((GObject *)rx->radio_info,"info_data");

  if(info==NULL) return;

  if(radio->discovered->protocol==PROTOCOL_1) {    
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info->qos_b), radio->qos_flag);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info->adc_overload_b),radio->adc_overload && (!isTransmitting(radio)));


  if(radio->transmitter!=NULL) {
    if (radio->discovered->device == DEVICE_HERMES_LITE2) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info->temp_b),radio->transmitter->temperature > radio->temperature_alarm_value);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info->swr_b),radio->transmitter->swr>radio->swr_alarm_value);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info->cat_b), rx->cat_client_connected);

#ifdef MIDI
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info->midi_b),radio->midi_enabled && (radio->receiver[midi_rx]->channel==rx->channel));
#endif

#ifdef CWDAEMON
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info->cwdaemon_b),radio->cwdaemon);
#endif
}
