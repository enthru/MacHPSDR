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
#include "pana_view.h"


static char *title="Microphone Gain";

// GPU render-node builder (PanaView).
static void mic_gain_build(GtkSnapshot *snapshot,int width,int height,gpointer data) {
  GtkWidget *widget=radio->mic_gain;
  char t[32];

  double bar_width=(double)width-10;

  double v=radio->transmitter->mic_gain+10.0; // move from range -10..50 to range 0..60
  double x = (bar_width/60.0)*v;

  level_meter_draw_node(snapshot, x, width, height, TEXT_A);

  // 0 dB marker
  GdkRGBA warn=skin_rgba(WARNING,1.0);
  x=(10.0/60.0)*(double)bar_width;
  lm_line(snapshot,x+5.0,(double)(height/2)-8.0,x+5.0,height/2-1,1.0,&warn);

  GdkRGBA tb=skin_rgba(TEXT_B,1.0);
  sprintf(t,"%s (%ddB)",title,(int)radio->transmitter->mic_gain);
  // centred by the title's width (matching the old cairo layout).
  double lw=lm_measure(widget,10,title);
  lm_text(snapshot,widget,(5+width/2)-lw/2.0,height-2,10,&tb,t,FALSE);
}

// GTK4: GtkGestureClick "pressed" handler (x in widget coords).
static void mic_gain_pressed_cb(GtkGestureClick *gesture,int n_press,double px,double py,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int width=gtk_widget_get_width(radio->mic_gain)-10;
  if(width<=0) return;                       // unallocated: no scale to invert
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
  // scroll_notches(), not a raw dy -- see mic_level.c. n<0 is up.
  int n=scroll_notches(controller,dy);
  if(n==0) return TRUE;
  tx->mic_gain-=1.0*(double)n;
  if(tx->mic_gain<-10.0) tx->mic_gain=-10.0;
  if(tx->mic_gain>50.0) tx->mic_gain=50.0;
  SetTXAPanelGain1(tx->channel,pow(10.0, tx->mic_gain / 20.0));
  gtk_widget_queue_draw(radio->mic_gain);
  return TRUE;
}

GtkWidget *create_mic_gain(TRANSMITTER *tx) {

  radio->mic_gain=pana_view_new(mic_gain_build,(gpointer)tx);
  gtk_widget_set_tooltip_text(radio->mic_gain,
      "Microphone gain, -10…+50 dB — click or scroll to set");
  gtk_widget_set_size_request(radio->mic_gain, 170, 34);
  // GTK4: a persistent resize cursor replaces the GTK3 enter/leave dance.
  gtk_widget_set_cursor_from_name(radio->mic_gain,"ew-resize");

  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click,"pressed",G_CALLBACK(mic_gain_pressed_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->mic_gain,GTK_EVENT_CONTROLLER(click));

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(mic_gain_scroll_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->mic_gain,scroll);

  return radio->mic_gain;
}
