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
#include <stdlib.h>
#include <wdsp.h>
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr

#include "agc.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "wideband_panadapter.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "vfo.h"

#define GRADIANT

static gboolean resize_timeout(void *data) {
  WIDEBAND *w=(WIDEBAND *)data;

  w->panadapter_width=w->panadapter_resize_width;
  w->panadapter_height=w->panadapter_resize_height;
  w->pixels=w->panadapter_width;

  wideband_init_analyzer(w);

  if (w->panadapter_surface) {
    cairo_surface_destroy (w->panadapter_surface);
  }

  if(w->panadapter!=NULL && w->panadapter_width>0 && w->panadapter_height>0) {
    // GTK4: off-screen image surface (no GdkWindow to back a similar surface).
    w->panadapter_surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24,
                                       w->panadapter_width,
                                       w->panadapter_height);

    /* Initialize the surface to black */
    cairo_t *cr;
    cr = cairo_create (w->panadapter_surface);
#ifdef GRADIANT
    cairo_pattern_t *pat=cairo_pattern_create_linear(0.0,0.0,0.0,w->panadapter_height);
    cairo_pattern_add_color_stop_rgba(pat,1.0,0.1,0.1,0.1,0.5);
    cairo_pattern_add_color_stop_rgba(pat,0.0,0.5,0.5,0.5,0.5);
    cairo_rectangle(cr, 0,0,w->panadapter_width,w->panadapter_height);
    cairo_set_source (cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
#else
    cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
    //cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
    cairo_paint (cr);
#endif
    cairo_destroy(cr);
  }

  w->panadapter_resize_timer=-1;

  return FALSE;
}

// GTK4: GtkDrawingArea "resize" signal replaces GTK3 "configure-event".
static void wideband_panadapter_resize_cb(GtkDrawingArea *area,int width,int height,gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(width!=w->panadapter_width || height!=w->panadapter_height) {
    w->panadapter_resize_width=width;
    w->panadapter_resize_height=height;
    if(w->panadapter_resize_timer!=-1) {
      g_source_remove(w->panadapter_resize_timer);
    }
    w->panadapter_resize_timer=g_timeout_add(250,resize_timeout,(gpointer)w);
  }
}

// GTK4: draw func signature is (area, cr, width, height, data).
static void wideband_panadapter_draw_cb(GtkDrawingArea *area,cairo_t *cr,int cwidth,int cheight,gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w->panadapter_surface!=NULL) {
    cairo_set_source_surface (cr, w->panadapter_surface, 0.0, 0.0);
    cairo_paint (cr);
  }
}

/*
static gboolean wideband_panadapter_press_event_cb(GtkWidget *widget,GdkEventButton *event,gpointer data) {
  return TRUE;
}
*/

GtkWidget *create_wideband_panadapter(WIDEBAND *w) {
  GtkWidget *panadapter;

  w->panadapter_width=0;
  w->panadapter_height=0;
  w->panadapter_surface=NULL;
  w->panadapter_resize_timer=-1;

  panadapter = gtk_drawing_area_new ();

  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(panadapter),wideband_panadapter_draw_cb,(gpointer)w,NULL);
  g_signal_connect(panadapter,"resize",G_CALLBACK(wideband_panadapter_resize_cb),(gpointer)w);

  // GTK4: pointer input via event controllers (button masks are gone).
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),0);
  g_signal_connect(click,"pressed",G_CALLBACK(wideband_pressed_cb),w);
  g_signal_connect(click,"released",G_CALLBACK(wideband_released_cb),w);
  gtk_widget_add_controller(panadapter,GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion=gtk_event_controller_motion_new();
  g_signal_connect(motion,"motion",G_CALLBACK(wideband_motion_cb),w);
  gtk_widget_add_controller(panadapter,motion);

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(wideband_scroll_cb),w);
  gtk_widget_add_controller(panadapter,scroll);

  return panadapter;
}

void update_wideband_panadapter(WIDEBAND *w) {
  long i;
  float *samples;
  cairo_text_extents_t extents;
  gdouble hz_per_pixel;
  gdouble x;

  int display_height=gtk_widget_get_allocated_height (w->panadapter);

  if(display_height<=1) return;

  hz_per_pixel=(gdouble)61440000/(gdouble)w->pixels;
    
  samples=w->pixel_samples;

  //clear_panadater_surface();
  cairo_t *cr;
  cr = cairo_create (w->panadapter_surface);
  cairo_set_line_width(cr, 1.0);
#ifdef GRADIANT
    cairo_pattern_t *pat=cairo_pattern_create_linear(0.0,0.0,0.0,w->panadapter_height);
    cairo_pattern_add_color_stop_rgba(pat,1.0,0.1,0.1,0.1,0.5);
    cairo_pattern_add_color_stop_rgba(pat,0.0,0.5,0.5,0.5,0.5);
    cairo_rectangle(cr, 0,0,w->panadapter_width,w->panadapter_height);
    cairo_set_source (cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
#else
    cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
    cairo_rectangle(cr,0,0,w->panadapter_width,w->panadapter_height);
    cairo_fill(cr);
#endif


  // plot the levels
  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  double dbm_per_line=(double)w->panadapter_height/((double)w->panadapter_high-(double)w->panadapter_low);
  cairo_set_line_width(cr, 1.0);
  cairo_select_font_face(cr, "FreeMono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 12);
  char v[32];

  for(i=w->panadapter_high;i>=w->panadapter_low;i--) {
    int mod=labs(i)%20;
    if(mod==0) {
      double y = (double)(w->panadapter_high-i)*dbm_per_line;
      cairo_move_to(cr,0.0,y);
      cairo_line_to(cr,(double)w->panadapter_width,y);

      sprintf(v,"%ld dBm",i);
      cairo_move_to(cr, 1, y);  
      cairo_show_text(cr, v);
    }
  }
  cairo_stroke(cr);


  for(i=5000000;i<61440000;i+=5000000) {
    x=(gdouble)i/hz_per_pixel;
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr,x,10.0);
    cairo_line_to(cr,x,(double)display_height);

    cairo_select_font_face(cr, "FreeMono",
                        CAIRO_FONT_SLANT_NORMAL,
                        CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    char v[32];
    sprintf(v,"%0ld",i/1000000);
    cairo_text_extents(cr, v, &extents);
    cairo_move_to(cr,x-(extents.width/2.0), 10.0);  
    cairo_show_text(cr, v);
  }
  cairo_stroke(cr);

  // cursor
  if(radio->active_receiver!=NULL) {
    cairo_set_source_rgb (cr, 1.0, 0.0, 0.0);
    cairo_set_line_width(cr, 1.0);
    x=(gdouble)radio->active_receiver->frequency_a/hz_per_pixel;
    cairo_move_to(cr,x,0.0);
    cairo_line_to(cr,x,(double)display_height);
    cairo_stroke(cr);
  }

  // signal
  double s1,s2;
  samples[w->pixels]=-200.0;
  samples[(w->pixels*2)-1]=-200.0;

  s1=(double)samples[w->pixels];
  s1 = floor((w->panadapter_high - s1)
                        * (double) display_height
                        / (w->panadapter_high - w->panadapter_low));
  cairo_move_to(cr, 0.0, s1);
  for(i=1;i<w->pixels;i++) {
    s2=(double)samples[i+w->pixels];
    s2 = floor((w->panadapter_high - s2)
                            * (double) display_height
                            / (w->panadapter_high - w->panadapter_low));
    cairo_line_to(cr, (double)i, s2);
  }

  if(radio->display_filled) {
    cairo_close_path (cr);
    cairo_pattern_t *pat=cairo_pattern_create_linear(0.0,0.0,0.0,w->panadapter_height);
    cairo_pattern_add_color_stop_rgba(pat,0.0,0.0,0.0,1.0,0.5);
    cairo_pattern_add_color_stop_rgba(pat,1.0,1.0,1.0,1.0,0.5);
    cairo_set_source (cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);
  }
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  cairo_destroy (cr);
  gtk_widget_queue_draw (w->panadapter);

}
