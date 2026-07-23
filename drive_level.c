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

#include <wdsp.h>

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "discovered.h"
#include "bpsk.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "main.h"
#include "drive_level.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "vfo.h"
#include "level_meter.h"

// On SoapySDR (e.g. HackRF) the drive slider maps onto the hardware TX gain,
// exactly like the RX gain slider - there is no digital drive scaling.
static inline void drive_level_apply(TRANSMITTER *tx) {
#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    soapy_protocol_set_tx_drive(tx->drive);
  }
#endif
}

static char *title="Drive";

// GTK4: draw func signature is (area, cr, width, height, data).
static void drive_level_draw_cb(GtkDrawingArea *area,cairo_t *cr,int width,int height,gpointer data) {
  cairo_text_extents_t extents;
  char t[32];

  double bar_width=(double)width-10;

  cairo_set_line_width(cr,1.0);

  double v=radio->transmitter->drive;
  double x=(bar_width/100.0)*v;
  
  level_meter_draw(cr, x, width, height, BOX_ON);
  
  SetColour(cr, TEXT_B);
  cairo_select_font_face(cr, "w", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);  
  cairo_set_font_size(cr,10);
  cairo_text_extents(cr, title, &extents);
  sprintf(t,"%s (%d%%)",title,(int)radio->transmitter->drive);
  cairo_move_to(cr,(5+width/2)-(extents.width/2.0),height-2);
  cairo_show_text(cr,t);
}

// GTK4: GtkGestureClick "pressed" handler (x in widget coords).
static void drive_level_pressed_cb(GtkGestureClick *gesture,int n_press,double px,double py,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int width=gtk_widget_get_width(radio->drive_level)-10;
  double x=px-5.0;
  x=(x/(double)width)*100.0;
  tx->drive=x;
  if(tx->drive<0.0) {
    tx->drive=0.0;
  } else if(tx->drive>100.0) {
    tx->drive=100.0;
  }
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
  drive_level_apply(tx);
  gtk_widget_queue_draw(radio->drive_level);
}

// GTK4: GtkEventControllerScroll "scroll" handler (dy<0 = up).
static gboolean drive_level_scroll_cb(GtkEventControllerScroll *controller,double dx,double dy,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  if(dy<0) {
    tx->drive+=1.0;
  } else {
    tx->drive-=1.0;
  }
  if(tx->drive<0.0) tx->drive=0.0;
  if(tx->drive>100.0) tx->drive=100.0;
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
  drive_level_apply(tx);
  gtk_widget_queue_draw(radio->drive_level);
  return TRUE;
}

GtkWidget *create_drive_level(TRANSMITTER *tx) {

  radio->drive_level=gtk_drawing_area_new();
  gtk_widget_set_size_request(radio->drive_level, 170, 34);
  gtk_widget_set_cursor_from_name(radio->drive_level,"ew-resize");

  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(radio->drive_level),drive_level_draw_cb,(gpointer)tx,NULL);

  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click,"pressed",G_CALLBACK(drive_level_pressed_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->drive_level,GTK_EVENT_CONTROLLER(click));

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(drive_level_scroll_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->drive_level,scroll);

  return radio->drive_level;
}
