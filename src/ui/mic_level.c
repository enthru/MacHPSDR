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

#include "discovered.h"
#include "bpsk.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "main.h"
#include "mic_level.h"
#include "vfo.h"
#include "level_meter.h"
#include "pana_view.h"

static char *title="Microphone Level";

// GPU render-node builder (PanaView). Same look as the old cairo draw.
static void mic_level_build(GtkSnapshot *snapshot,int width,int height,gpointer data) {
  GtkWidget *widget=radio->mic_level;
  int bar_width=width-10;

  double peak=radio->vox_peak*(double)bar_width;
  level_meter_draw_node(snapshot, peak, width, height, TEXT_B);

  // Vox threshold marker
  GdkRGBA warn=skin_rgba(WARNING,1.0);
  double threshold=radio->vox_threshold*(double)bar_width;
  lm_line(snapshot,threshold+5.0,1,threshold+5.0,height/2,1.0,&warn);

  GdkRGBA tb=skin_rgba(TEXT_B,1.0);
  lm_text(snapshot,widget,5+width/2,height-2,10,&tb,title,TRUE);
}

// GTK4: GtkGestureClick "pressed" handler (x in widget coords).
static void mic_level_pressed_cb(GtkGestureClick *gesture,int n_press,double px,double py,gpointer data) {
  // The bar the builder draws is width-10 wide, starting at x=5, so the inverse
  // has to divide by THAT and not by the whole widget -- it did not, and the
  // marker therefore never landed under the pointer that placed it (at 170 px,
  // clicking exactly on the marker moved it ~6% left). Its two sibling widgets,
  // mic_gain and drive_level, already subtract the 10.
  int width=gtk_widget_get_width(radio->mic_level)-10;
  if(width<=0) return;                       // unallocated: no scale to invert
  radio->vox_threshold=(px-5.0)/(double)width;
  if(radio->vox_threshold<0.0) {
    radio->vox_threshold=0.0;
  } else if(radio->vox_threshold>1.0) {
    radio->vox_threshold=1.0;
  }
  update_mic_level(radio);
}

// GTK4: GtkEventControllerScroll "scroll" handler (dy<0 = up).
static gboolean mic_level_scroll_cb(GtkEventControllerScroll *controller,double dx,double dy,gpointer data) {
  // Through scroll_notches(), never a raw dy: a trackpad delivers a stream of
  // small precise deltas per swipe, so testing the sign moved this control the
  // whole way across on one flick. n<0 is up.
  int n=scroll_notches(controller,dy);
  if(n==0) return TRUE;
  radio->vox_threshold-=0.01*(double)n;
  if(radio->vox_threshold<0.0) radio->vox_threshold=0.0;
  if(radio->vox_threshold>1.0) radio->vox_threshold=1.0;
  update_mic_level(radio);
  return TRUE;
}

GtkWidget *create_mic_level(TRANSMITTER *tx) {

  radio->mic_level_surface=NULL;
  radio->mic_level=pana_view_new(mic_level_build,(gpointer)tx);
  gtk_widget_set_size_request(radio->mic_level, 170, 34);
  gtk_widget_set_cursor_from_name(radio->mic_level,"ew-resize");

  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click,"pressed",G_CALLBACK(mic_level_pressed_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->mic_level,GTK_EVENT_CONTROLLER(click));

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(mic_level_scroll_cb),(gpointer)tx);
  gtk_widget_add_controller(radio->mic_level,scroll);

  return radio->mic_level;
}

void update_mic_level(RADIO *r) {
  if(r->mic_level!=NULL) {
    gtk_widget_queue_draw(r->mic_level);
  }
}
