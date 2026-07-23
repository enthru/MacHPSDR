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
#include "meter.h"
#include "main.h"
#include "vfo.h"
#include "level_meter.h"

typedef struct _choice {
  RECEIVER *rx;
  int selection;
} CHOICE;

// GTK4: GtkDrawingArea "resize" signal replaces GTK3 "configure-event";
// the backing store is an off-screen image surface (no GdkWindow).
static void meter_resize_cb(GtkDrawingArea *area,int meter_width,int meter_height,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if (rx->meter_surface) {
    cairo_surface_destroy (rx->meter_surface);
    rx->meter_surface=NULL;
  }
  if(meter_width>0 && meter_height>0) {
    rx->meter_surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,meter_width,meter_height);
  }
}


// GTK4: draw func signature is (area, cr, width, height, data).
static void meter_draw_cb(GtkDrawingArea *area,cairo_t *cr,int cwidth,int cheight,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(rx->meter_surface!=NULL) {
    cairo_set_source_surface (cr, rx->meter_surface, 0.0, 0.0);
    cairo_paint (cr);
  }
}

// A menu-item button in the choice popover applies its selection and dismisses.
static void meter_choice_clicked(GtkButton *b,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  choice->rx->smeter=choice->selection;
  GtkWidget *pop=gtk_widget_get_ancestor(GTK_WIDGET(b),GTK_TYPE_POPOVER);
  if(pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void meter_add_choice(GtkWidget *box,RECEIVER *rx,const char *label,int sel) {
  GtkWidget *b=gtk_button_new_with_label(label);
  gtk_widget_add_css_class(b,"flat");
  CHOICE *choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=sel;
  g_object_set_data_full(G_OBJECT(b),"choice",choice,g_free);
  g_signal_connect(b,"clicked",G_CALLBACK(meter_choice_clicked),choice);
  gtk_box_append(GTK_BOX(box),b);
}

// GTK4: GtkMenu is gone — a small GtkPopover of flat buttons is the context menu.
static void meter_pressed_cb(GtkGestureClick *gesture,int n_press,double x,double y,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))!=1) return;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  GtkWidget *pop=gtk_popover_new();
  gtk_widget_set_parent(pop,widget);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop),&(GdkRectangle){(int)x,(int)y,1,1});
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  gtk_popover_set_child(GTK_POPOVER(pop),box);
  meter_add_choice(box,rx,"S Meter Peak",RXA_S_PK);
  meter_add_choice(box,rx,"S Meter AVERAGE",RXA_S_AV);
  g_signal_connect_swapped(pop,"closed",G_CALLBACK(gtk_widget_unparent),pop);
  gtk_popover_popup(GTK_POPOVER(pop));
}

GtkWidget *create_meter_visual(RECEIVER *rx) {

  GtkWidget *meter = gtk_drawing_area_new ();

  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(meter),meter_draw_cb,(gpointer)rx,NULL);
  g_signal_connect (meter,"resize", G_CALLBACK (meter_resize_cb), (gpointer)rx);

  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click,"pressed",G_CALLBACK(meter_pressed_cb),(gpointer)rx);
  gtk_widget_add_controller(meter,GTK_EVENT_CONTROLLER(click));

  return meter;

}

void update_meter(RECEIVER *rx) {
  char sf[32];
  cairo_t *cr;

  int meter_width=gtk_widget_get_width (rx->meter);
  int meter_height=gtk_widget_get_height (rx->meter);

  cr = cairo_create (rx->meter_surface);

  SetColour(cr, BACKGROUND);
  cairo_paint (cr);
  cairo_set_font_size(cr, 12);
  cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  
  double attenuation = radio->adc[rx->adc].attenuation;

  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
      attenuation = attenuation * -1;
  }

  double level=rx->meter_db+attenuation;

  // --- Analog-meter ballistics -------------------------------------------
  // Ease the needle toward the measured level instead of snapping to it each
  // frame, so it moves like a mechanical S-meter. Fast attack (rising signal
  // catches up quickly) and a slower decay (peaks linger, then settle). The
  // per-frame coefficients are derived from time constants so the feel stays
  // consistent regardless of the receiver's display fps.
  {
    int fps = rx->fps > 0 ? rx->fps : 20;
    const double t_attack = 0.050;   // seconds to ~63% on a rising reading
    const double t_decay  = 0.250;   // seconds to ~63% on a falling reading
    double a_attack = 1.0 - exp(-1.0 / ((double)fps * t_attack));
    double a_decay  = 1.0 - exp(-1.0 / ((double)fps * t_decay));
    if(!rx->meter_needle_init) {
      rx->meter_needle_db = level;   // seed on first draw: no wild sweep from 0
      rx->meter_needle_init = 1;
    } else {
      double a = (level > rx->meter_needle_db) ? a_attack : a_decay;
      rx->meter_needle_db += (level - rx->meter_needle_db) * a;
    }
  }
  double needle_level = rx->meter_needle_db;

  double offset=210.0;
  int i;
  double x;
  double y;
  double angle;
  double radians;
  double cx=(double)meter_width-100.0;
  double cy=100.0;
  double radius=cy-20.0;

  cairo_set_line_width(cr, 1.0);
  SetColour(cr, OFF_WHITE);
  cairo_arc(cr, cx, cy, radius, 216.0*M_PI/180.0, 324.0*M_PI/180.0);
  cairo_stroke(cr);

  cairo_set_line_width(cr, 2.5);
  SetColour(cr, WARNING);        // S9+ overload zone
  cairo_arc(cr, cx, cy, radius+2, 264.0*M_PI/180.0, 324.0*M_PI/180.0);
  cairo_stroke(cr);

  cairo_set_line_width(cr, 1.0);

  for(i=1;i<10;i++) {
    angle=((double)i*6.0)+offset;
    radians=angle*M_PI/180.0;

    if((i%2)==1) {
      SetColour(cr, OFF_WHITE);   // major ticks + numbers: bright
      cairo_arc(cr, cx, cy, radius+4, radians, radians);
      cairo_get_current_point(cr, &x, &y);
      cairo_arc(cr, cx, cy, radius, radians, radians);
      cairo_line_to(cr, x, y);
      cairo_stroke(cr);
      sprintf(sf,"%d",i);
      cairo_arc(cr, cx, cy, radius+5, radians, radians);
      cairo_get_current_point(cr, &x, &y);
      cairo_new_path(cr);
      x-=4.0;
      cairo_move_to(cr, x, y);
      cairo_show_text(cr, sf);
    } else {
      SetColour(cr, DARK_TEXT);   // minor ticks: dimmed
      cairo_arc(cr, cx, cy, radius+2, radians, radians);
      cairo_get_current_point(cr, &x, &y);
      cairo_arc(cr, cx, cy, radius, radians, radians);
      cairo_line_to(cr, x, y);
      cairo_stroke(cr);
    }
    cairo_new_path(cr);
  }

  SetColour(cr, OFF_WHITE);   // +20/+40/+60 major ticks + labels
  for(i=20;i<=60;i+=20) {
    angle=((double)i+54.0)+offset;
    radians=angle*M_PI/180.0;
    cairo_arc(cr, cx, cy, radius+4, radians, radians);
    cairo_get_current_point(cr, &x, &y);
    cairo_arc(cr, cx, cy, radius, radians, radians);
    cairo_line_to(cr, x, y);
    cairo_stroke(cr);

    sprintf(sf,"+%d",i);
    cairo_arc(cr, cx, cy, radius+5, radians, radians);
    cairo_get_current_point(cr, &x, &y);
    cairo_new_path(cr);
    x-=4.0;
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, sf);
    cairo_new_path(cr);
  }

  cairo_set_line_width(cr, 2.0);
  SetColour(cr, TEXT_B);   // themed accent needle

  angle=needle_level+127.0+offset;
  radians=angle*M_PI/180.0;
  cairo_arc(cr, cx, cy, radius+8, radians, radians);
  cairo_line_to(cr, cx, cy);
  cairo_stroke(cr);
  // pivot hub
  cairo_arc(cr, cx, cy, 3.0, 0.0, 2.0*M_PI);
  cairo_fill(cr);

  SetColour(cr, TEXT_A);
  sprintf(sf,"%d dBm %s",(int)level,rx->smeter==RXA_S_AV?"Av":"Pk");
  cairo_move_to(cr, meter_width-130, meter_height-2);
  cairo_show_text(cr, sf);

  SetColour(cr, TEXT_C);
  cairo_set_font_size(cr, 36);

  static double smax;

  level=level+127;
  if (level<0) {
    level=0;
  }
  if (level>smax) {
    smax=level;
  } else {
    if (level>54) {
      smax=smax-((smax-level)/(3*rx->fps));
    } else {
      smax = smax-((smax-level)/(rx->fps/2));
    }
  }
  i=(int)(smax/6);
  if(i>9) {
    i=9;
  }

  sprintf(sf,"S%d", i);
  cairo_move_to(cr, meter_width-250, meter_height-20);
  cairo_show_text(cr, sf);

  i=(int)smax;
  if(i>54) {
    i=i-54;
    cairo_set_font_size(cr, 20);
    sprintf(sf, "+%d", i);
    cairo_move_to(cr, meter_width-210, (meter_height/2)+5);
    cairo_show_text(cr, sf);
  }

  cairo_destroy(cr);
  gtk_widget_queue_draw (rx->meter);

}
