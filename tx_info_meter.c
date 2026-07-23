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

#include "tx_info_meter.h"

// GTK4: GtkDrawingArea "resize" signal replaces GTK3 "configure-event";
// off-screen image surface (no GdkWindow to back a similar surface).
static void tx_info_meter_resize_cb(GtkDrawingArea *area, int width, int height, gpointer data) {
  TXMETER *tx_meter = (TXMETER *)data;

  if(tx_meter->tx_info_meter_surface) {
    cairo_surface_destroy(tx_meter->tx_info_meter_surface);
    tx_meter->tx_info_meter_surface = NULL;
  }

  if(tx_meter->tx_meter_drawing != NULL && width>0 && height>0) {
    tx_meter->tx_info_meter_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);

    cairo_t *cr;
    cr = cairo_create(tx_meter->tx_info_meter_surface);
    SetColour(cr, BACKGROUND);
    cairo_paint (cr);
    cairo_destroy(cr);
  }
}

// GTK4: draw func signature is (area, cr, width, height, data).
static void tx_info_meter_draw_cb(GtkDrawingArea *area,cairo_t *cr,int cwidth,int cheight,gpointer data) {
  TXMETER *tx_meter = (TXMETER *)data;

  if(tx_meter->tx_info_meter_surface != NULL) {
    cairo_set_source_surface(cr, tx_meter->tx_info_meter_surface, 0.0, 0.0);
    cairo_paint(cr);
  }
}

GtkWidget *create_tx_meter(TXMETER *tx_meter) {
  tx_meter->tx_meter_drawing = gtk_drawing_area_new();

  gtk_widget_set_size_request(tx_meter->tx_meter_drawing, 252, 50);

  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(tx_meter->tx_meter_drawing), tx_info_meter_draw_cb, (gpointer)tx_meter, NULL);
  g_signal_connect(tx_meter->tx_meter_drawing, "resize", G_CALLBACK(tx_info_meter_resize_cb), (gpointer)tx_meter);

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
  int i;
  double x;
  cairo_text_extents_t extents;

  if(meter->tx_info_meter_surface != NULL) {
    cairo_t *cr;
    cr = cairo_create(meter->tx_info_meter_surface);

    int width = gtk_widget_get_allocated_width(meter->tx_meter_drawing);
    int height = gtk_widget_get_allocated_height(meter->tx_meter_drawing);
    
    int bar_width = width-10;

    double value_plot = (bar_width / meter->meter_max) * value;
    level_meter_draw(cr, value_plot, width, height, INFO_ON);    
    
    // Peak value
    SetColour(cr, WARNING);
    double peak_plot = (bar_width / meter->meter_max) * peak;
    cairo_move_to(cr, peak_plot+5.0, 1);
    cairo_line_to(cr, peak_plot+5.0, height/2);
    cairo_stroke(cr);
    
    SetColour(cr, TEXT_B);    
    cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);    
    cairo_set_font_size(cr,10);
    cairo_text_extents(cr, meter->label, &extents);
    cairo_move_to(cr,(5+width/2)-(extents.width/2.0),height-2);
    cairo_show_text(cr, meter->label);

    cairo_set_font_size(cr,10);
    
    // Display the value as numbers on the display
    char text[32];
    if (peak < 10.0) {
      sprintf(text,"%2.2f", peak);
    } else {
      sprintf(text,"%2.0f", peak);
    }
    cairo_text_extents(cr, text, &extents);
    cairo_move_to(cr, (3*(width/4)), height-2);
    cairo_show_text(cr, text);


    cairo_destroy(cr);
    gtk_widget_queue_draw(meter->tx_meter_drawing);
  }
}

void configure_meter(TXMETER *meter, char *title, gdouble min_val, gdouble max_val) {
  meter->label = title;
  meter->meter_max = max_val;
  meter->meter_min = min_val;
}
