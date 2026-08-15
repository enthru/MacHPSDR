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
#include "pana_view.h"

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

// GPU render-node builder (PanaView).
static void drive_level_build(GtkSnapshot *snapshot,int width,int height,gpointer data) {
  GtkWidget *widget=radio->drive_level;
  char t[32];

  double bar_width=(double)width-10;

  double v=radio->transmitter->drive;
  double x=(bar_width/100.0)*v;

  level_meter_draw_node(snapshot, x, width, height, BOX_ON);

  GdkRGBA tb=skin_rgba(TEXT_B,1.0);
  sprintf(t,"%s (%d%%)",title,(int)radio->transmitter->drive);
  double lw=lm_measure(widget,10,title);
  lm_text(snapshot,widget,(5+width/2)-lw/2.0,height-2,10,&tb,t,FALSE);
}

// GTK4: GtkGestureClick "pressed" handler (x in widget coords).
static void drive_level_pressed_cb(GtkGestureClick *gesture,int n_press,double px,double py,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  int width=gtk_widget_get_width(radio->drive_level)-10;
  if(width<=0) return;                       // unallocated: no scale to invert
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
  // scroll_notches(), not a raw dy -- see mic_level.c. n<0 is up.
  int n=scroll_notches(controller,dy);
  if(n==0) return TRUE;
  tx->drive-=1.0*(double)n;
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

  radio->drive_level=pana_view_new(drive_level_build,(gpointer)tx);
  gtk_widget_set_size_request(radio->drive_level, 170, 34);
  gtk_widget_set_cursor_from_name(radio->drive_level,"ew-resize");

  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click,"pressed",G_CALLBACK(drive_level_pressed_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->drive_level,GTK_EVENT_CONTROLLER(click));

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(drive_level_scroll_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->drive_level,scroll);

  return radio->drive_level;
}
