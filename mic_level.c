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

static char *title="Microphone Level";

static gboolean mic_level_configure_event_cb(GtkWidget *widget,GdkEventConfigure *event,gpointer data) {
  return TRUE;
}

// Draw directly (same style as mic_gain / drive) instead of a cached surface,
// so the meter looks identical to the other two.
static gboolean mic_level_draw_cb(GtkWidget *widget,cairo_t *cr,gpointer data) {
  cairo_text_extents_t extents;
  int width=gtk_widget_get_allocated_width(widget);
  int height=gtk_widget_get_allocated_height(widget);
  int bar_width=width-10;

  double peak=radio->vox_peak*(double)bar_width;
  level_meter_draw(cr, peak, width, height, TEXT_B);

  // Vox threshold marker
  SetColour(cr, WARNING);
  double threshold=radio->vox_threshold*(double)bar_width;
  cairo_move_to(cr,threshold+5.0,1);
  cairo_line_to(cr,threshold+5.0,height/2);
  cairo_stroke(cr);

  SetColour(cr, TEXT_B);
  cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr,10);
  cairo_text_extents(cr, title, &extents);
  cairo_move_to(cr,(5+width/2)-(extents.width/2.0),height-2);
  cairo_show_text(cr,title);
  return TRUE;
}

static gboolean mic_level_press_event_cb(GtkWidget *widget,GdkEventButton *event,gpointer data) {
  int width=gtk_widget_get_allocated_width(widget);
  double x=event->x;
  radio->vox_threshold=(x-5.0)/(double)width;
  if(radio->vox_threshold<0.0) {
    radio->vox_threshold=0.0;
  } else if(radio->vox_threshold>1.0) {
    radio->vox_threshold=1.0;
  }
  update_mic_level(radio);
  return TRUE;
}

static gboolean mic_level_scroll_event_cb(GtkWidget *widget,GdkEventScroll *event,gpointer data) {
  if(event->direction==GDK_SCROLL_UP) {
    radio->vox_threshold+=0.01;
  } else {
    radio->vox_threshold-=0.01;
  }
  if(radio->vox_threshold<0.0) radio->vox_threshold=0.0;
  if(radio->vox_threshold>1.0) radio->vox_threshold=1.0;
  update_mic_level(radio);
  return TRUE;
}

static gboolean enter (GtkWidget *ebox, GdkEventCrossing *event) {
   //gdk_window_set_cursor(gtk_widget_get_window(ebox),gdk_cursor_new(GDK_DOUBLE_ARROW));
   gdk_window_set_cursor(gtk_widget_get_window(ebox),gdk_cursor_new(GDK_SB_H_DOUBLE_ARROW));
   return FALSE;
}


static gboolean leave (GtkWidget *ebox, GdkEventCrossing *event) {
   gdk_window_set_cursor(gtk_widget_get_window(ebox),gdk_cursor_new(GDK_ARROW));
   return FALSE;
}

GtkWidget *create_mic_level(TRANSMITTER *tx) {

  radio->mic_level_surface=NULL;
  radio->mic_level=gtk_drawing_area_new();
  gtk_widget_set_size_request(radio->mic_level, 170, 34);

  g_signal_connect(radio->mic_level,"configure-event",G_CALLBACK(mic_level_configure_event_cb),(gpointer)tx);
  g_signal_connect(radio->mic_level,"draw",G_CALLBACK(mic_level_draw_cb),(gpointer)tx);
  g_signal_connect(radio->mic_level,"enter-notify-event",G_CALLBACK (enter),NULL);
  g_signal_connect(radio->mic_level,"leave-notify-event",G_CALLBACK (leave),NULL);
  g_signal_connect(radio->mic_level,"button-press-event",G_CALLBACK(mic_level_press_event_cb),(gpointer)tx);
  g_signal_connect(radio->mic_level,"scroll_event",G_CALLBACK(mic_level_scroll_event_cb),(gpointer)tx);
  gtk_widget_set_events (radio->mic_level, gtk_widget_get_events (radio->mic_level)
                     | GDK_BUTTON_PRESS_MASK
                     | GDK_SCROLL_MASK
                     | GDK_ENTER_NOTIFY_MASK
                     | GDK_LEAVE_NOTIFY_MASK);


  return radio->mic_level;
}

void update_mic_level(RADIO *r) {
  if(r->mic_level!=NULL) {
    gtk_widget_queue_draw(r->mic_level);
  }
}
