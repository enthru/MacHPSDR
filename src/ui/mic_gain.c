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
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "main.h"
#include "mic_gain.h"
#include "vfo.h"
#include "level_meter.h"


static char *title="Microphone Gain";

// GTK4: draw func signature is (area, cr, width, height, data).
static void mic_gain_draw_cb(GtkDrawingArea *area,cairo_t *cr,int width,int height,gpointer data) {
  cairo_text_extents_t extents;
  char t[32];

  double bar_width=(double)width-10;

  double v=radio->transmitter->mic_gain+10.0; // move from rabd -10..50 to range 0..60
  double x = (bar_width/60.0)*v;

  level_meter_draw(cr, x, width, height, TEXT_A);

  // 0 dB marker
  SetColour(cr, WARNING);
  x=(10.0/60.0)*(double)bar_width;
  cairo_move_to(cr,x+5.0,(double)(height/2)-8.0);
  cairo_line_to(cr,x+5.0,height/2-1);
  cairo_stroke(cr);  
  
  SetColour(cr, TEXT_B);
  cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);  
  cairo_set_font_size(cr,10);
  cairo_text_extents(cr, title, &extents);
  sprintf(t,"%s (%ddB)",title,(int)radio->transmitter->mic_gain);
  cairo_move_to(cr,(5+width/2)-(extents.width/2.0),height-2);
  cairo_show_text(cr,t);
}

// GTK4: GtkGestureClick "pressed" handler (x in widget coords).
static void mic_gain_pressed_cb(GtkGestureClick *gesture,int n_press,double px,double py,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int width=gtk_widget_get_width(radio->mic_gain)-10;
  double x=px-5.0;
  x=((x/(double)width)*60.0)-10.0;
  tx->mic_gain=x;
  if(tx->mic_gain<-10.0) tx->mic_gain=-10.0;
  if(tx->mic_gain>50.0) tx->mic_gain=50.0;
  SetTXAPanelGain1(tx->channel,pow(10.0, tx->mic_gain / 20.0));
  gtk_widget_queue_draw(radio->mic_gain);
}

// GTK4: GtkEventControllerScroll "scroll" handler (dy<0 = up).
static gboolean mic_gain_scroll_cb(GtkEventControllerScroll *controller,double dx,double dy,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  if(dy<0) {
    tx->mic_gain+=1.0;
  } else {
    tx->mic_gain-=1.0;
  }
  if(tx->mic_gain<-10.0) tx->mic_gain=-10.0;
  if(tx->mic_gain>50.0) tx->mic_gain=50.0;
  SetTXAPanelGain1(tx->channel,pow(10.0, tx->mic_gain / 20.0));
  gtk_widget_queue_draw(radio->mic_gain);
  return TRUE;
}

GtkWidget *create_mic_gain(TRANSMITTER *tx) {

  radio->mic_gain=gtk_drawing_area_new();
  gtk_widget_set_size_request(radio->mic_gain, 170, 34);
  // GTK4: a persistent resize cursor replaces the GTK3 enter/leave dance.
  gtk_widget_set_cursor_from_name(radio->mic_gain,"ew-resize");

  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(radio->mic_gain),mic_gain_draw_cb,(gpointer)tx,NULL);

  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click,"pressed",G_CALLBACK(mic_gain_pressed_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->mic_gain,GTK_EVENT_CONTROLLER(click));

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(mic_gain_scroll_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->mic_gain,scroll);

  return radio->mic_gain;
}
