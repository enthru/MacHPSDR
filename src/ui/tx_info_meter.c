/* Copyright (C)
* 2021 - m5evt
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

#include <wdsp.h>

#include "discovered.h"
#include "bpsk.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"

#include "transmitter.h"

#include "level_meter.h"
#include "radio.h"
#include "pana_view.h"

#include "tx_info_meter.h"

// GPU render-node builder (PanaView). Reads the value/peak stashed on the meter
// by update_tx_info_meter().
static void tx_info_meter_build(GtkSnapshot *snapshot,int width,int height,gpointer data) {
  TXMETER *meter=(TXMETER *)data;
  GtkWidget *widget=meter->tx_meter_drawing;
  if(width<=0 || height<=0) return;

  int bar_width=width-10;
  double value_plot=(bar_width/meter->meter_max)*meter->cur_value;
  level_meter_draw_node(snapshot, value_plot, width, height, INFO_ON);

  // peak marker
  GdkRGBA warn=skin_rgba(WARNING,1.0);
  double peak_plot=(bar_width/meter->meter_max)*meter->cur_peak;
  lm_line(snapshot,peak_plot+5.0,1,peak_plot+5.0,height/2,1.0,&warn);

  GdkRGBA tb=skin_rgba(TEXT_B,1.0);
  lm_text(snapshot,widget,5+width/2,height-2,10,&tb,meter->label,TRUE);

  char text[32];
  if(meter->cur_peak < 10.0) sprintf(text,"%2.2f",meter->cur_peak);
  else                       sprintf(text,"%2.0f",meter->cur_peak);
  lm_text(snapshot,widget,3*(width/4),height-2,10,&tb,text,FALSE);
}

GtkWidget *create_tx_meter(TXMETER *tx_meter) {
  tx_meter->tx_meter_drawing = pana_view_new(tx_info_meter_build,(gpointer)tx_meter);
  gtk_widget_set_size_request(tx_meter->tx_meter_drawing, 252, 50);
  return tx_meter->tx_meter_drawing;
}

TXMETER *create_tx_info_meter(void) {
  TXMETER *tx_meter = g_new0(TXMETER,1);  
  
  tx_meter->label = "Default";
  tx_meter->meter_max = 100;
  tx_meter->meter_min = 0;
  
  tx_meter->tx_info_meter_surface = NULL;
  tx_meter->tx_meter_drawing = NULL;
  
  tx_meter->tx_meter_drawing = create_tx_meter(tx_meter);
  
  return tx_meter;
}

void update_tx_info_meter(TXMETER *meter, gdouble value, gdouble peak) {
  // Publish the readings for the snapshot builder, then queue the GPU redraw.
  meter->cur_value = value;
  meter->cur_peak  = peak;
  if(meter->tx_meter_drawing != NULL) {
    gtk_widget_queue_draw(meter->tx_meter_drawing);
  }
}

void configure_meter(TXMETER *meter, char *title, gdouble min_val, gdouble max_val) {
  meter->label = title;
  meter->meter_max = max_val;
  meter->meter_min = min_val;
}
