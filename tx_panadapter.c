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
#include "log.h"
#include <math.h>
#include <stdlib.h>

#include "mode.h"
#include "discovered.h"
#include "bpsk.h"
#include "adc.h"
#include "dac.h"
#include "wideband.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "vfo.h"
#include "level_meter.h"
#include "tx_info.h"
//#include "transmitter_dialog.h"
#include "configure_dialog.h"
#include "css.h"
#include "main.h"

// Set the Cairo source to a skin palette color (fallback if the name is absent).
static void txpan_rgb(cairo_t *cr, const char *name, double r, double g, double b) {
  css_rgb(name,&r,&g,&b);
  cairo_set_source_rgb(cr,r,g,b);
}
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif

// GTK4: GtkGestureClick "pressed" handler. The button is read from the gesture.
static void transmitter_pressed_cb(GtkGestureClick *gesture,int n_press,double x,double y,gpointer data) {
  guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  switch(button) {
    case 1: // left
      break;
    case 3: // right
      if(radio->transmitter->tx_info  == NULL) {
        if ((radio->can_transmit) && (radio->hl2 != NULL)) {
          // Only tested with HL2 for now
          radio->transmitter->tx_info = create_tx_info(radio->transmitter);
        }
      }
      break;
  }
}

void update_tx_panadapter(RADIO *r);

// GTK4: GtkDrawingArea "resize" signal replaces GTK3 "configure-event".
static void tx_panadapter_resize_cb(GtkDrawingArea *area,int width,int height,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->panadapter_width=width;
  tx->panadapter_height=height;
  if(tx->panadapter_width>1 && tx->panadapter_height>1) {
  if(tx->panadapter_surface) {
    cairo_surface_destroy(tx->panadapter_surface);
  }
  log_info("tx_panadapter_resize: width=%d height=%d\n",tx->panadapter_width,tx->panadapter_height);
  /* The analyzer is zoomed onto the TX_MONITOR_SPAN_HZ window (see
     transmitter_init_analyzer), so one pixel per screen column suffices — the
     whole pixel budget lands inside the visible window. (Previously pixels was
     width*3/width*12 across the full span and the draw cropped the centre.) */
  tx->pixels=tx->panadapter_width;
  if(tx->panadapter!=NULL) {
    // GTK4: off-screen image surface (no GdkWindow to back a similar surface).
    tx->panadapter_surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24,
                                       tx->panadapter_width,
                                       tx->panadapter_height);
    cairo_t *cr;
    cr = cairo_create (tx->panadapter_surface);
    txpan_rgb(cr,"SURFACE",0.16,0.16,0.19);
    cairo_paint (cr);
    cairo_destroy(cr);
    transmitter_init_analyzer(tx);
    /* The surface was just recreated blank. The periodic RX-loop refresh is
       gated on !tx->updated, which is only reset in the Soapy path — so on
       non-Soapy devices (e.g. fake) it would stay blank until MOX/freq change.
       Repaint now so every resize refreshes immediately. */
    update_tx_panadapter(radio);
  }
  }
}

// GTK4: draw func signature is (area, cr, width, height, data).
static void tx_panadapter_draw_cb(GtkDrawingArea *area,cairo_t *cr,int width,int height,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  txpan_rgb(cr,"SURFACE",0.16,0.16,0.19);
  cairo_paint(cr);
  if(tx->panadapter_surface!=NULL) {
    cairo_set_source_surface (cr, tx->panadapter_surface, 0.0, 0.0);
    cairo_paint (cr);
  }
}

GtkWidget *create_tx_panadapter(TRANSMITTER *tx) {

  tx->panadapter_width=0;
  tx->panadapter_height=0;
  tx->panadapter_surface=NULL;
  tx->panadapter=gtk_drawing_area_new();
  gtk_widget_set_size_request(tx->panadapter, 300, 120);

  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(tx->panadapter),tx_panadapter_draw_cb,(gpointer)tx,NULL);
  g_signal_connect(tx->panadapter,"resize",G_CALLBACK(tx_panadapter_resize_cb),(gpointer)tx);

  // GTK4: click via a gesture controller (button masks are gone).
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),0);
  g_signal_connect(click,"pressed",G_CALLBACK(transmitter_pressed_cb),(gpointer)tx);
  gtk_widget_add_controller(tx->panadapter,GTK_EVENT_CONTROLLER(click));

  return tx->panadapter;
}

void update_tx_panadapter(RADIO *r) {
  TRANSMITTER *tx=r->transmitter;
  int width=gtk_widget_get_width(tx->panadapter);
  int height=gtk_widget_get_height(tx->panadapter);
  float *samples=tx->pixel_samples;
  /* Screen scale follows the fixed monitor span, not the analyzer's full span,
     so the filter overlay/grid stay consistent with the cropped signal below. */
  double hz_per_pixel=TX_MONITOR_SPAN_HZ/(double)(width>0?width:1);
  char text[32];
  int i;

  if(tx->panadapter_surface!=NULL) {
    cairo_t *cr;
    cr = cairo_create (tx->panadapter_surface);
    cairo_set_line_width(cr, 1.0);

    txpan_rgb(cr,"SURFACE",0.16,0.16,0.19);
    //cairo_pattern_t *pat=cairo_pattern_create_linear(0.0,0.0,0.0,height);
    //cairo_pattern_add_color_stop_rgba(pat,1.0,0.1,0.1,0.1,0.5);
    //cairo_pattern_add_color_stop_rgba(pat,0.0,0.5,0.5,0.5,0.5);
    cairo_rectangle(cr, 0,0,width,height);
    //cairo_set_source (cr, pat);
    cairo_fill(cr);
    //cairo_pattern_destroy(pat);

    double dbm_per_line=(double)height/((double)tx->panadapter_high-(double)tx->panadapter_low);

    txpan_rgb(cr,"OFF_WHITE",0.9,0.9,0.9);   // body text, follows the skin (readable on SURFACE)
    cairo_set_line_width(cr, 1.0);
    cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);   // levels (dBm scale) -15%
    char v[32];

    // filter
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.75);
    double filter_left=(double)width/2.0+((double)tx->actual_filter_low/hz_per_pixel);
    double filter_right=(double)width/2.0+((double)tx->actual_filter_high/hz_per_pixel);
    cairo_rectangle(cr, filter_left, 20.0, filter_right-filter_left, (double)height-20.0);
    cairo_fill(cr);

    // levels
    for(i=tx->panadapter_high;i>=tx->panadapter_low;i--) {
      int mod=abs(i)%20;
      if(mod==0) {
        double y = (double)(tx->panadapter_high-i)*dbm_per_line;
        cairo_move_to(cr,0.0,y);
        cairo_line_to(cr,(double)width,y);
  
        sprintf(v,"%d dBm",i);
        cairo_move_to(cr, 1, y-4);   // lift the label clear of the graticule line
        cairo_show_text(cr, v);
      }
    }
    cairo_stroke(cr);

    // cursor
    SetColour(cr, TEXT_A);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr,(double)(width/2.0),20.0);
    cairo_line_to(cr,(double)(width/2.0),(double)height);
    cairo_stroke(cr);

    if(tx->rx->mode_a==CWU || tx->rx->mode_a==CWL) {
      SetColour(cr, TEXT_B);
      double cw_frequency=filter_left+((filter_right-filter_left)/2.0);
      cairo_move_to(cr,cw_frequency,20.0);
      cairo_line_to(cr,cw_frequency,(double)height);
      cairo_stroke(cr);
    }
    
    // signal
    if(isTransmitting(radio)) {
/*
      int offset=tx->pixels/3;
      if(radio->discovered->protocol==PROTOCOL_2) {
        offset=(tx->pixels/24)*11;
      }
*/
      /* The analyzer produces tx->pixels bins across the full iq_output_rate.
         Crop the central TX_MONITOR_SPAN_HZ worth of bins and map them onto the
         `width` screen columns (decimating when crop > width). This makes the
         visible span a fixed 24 kHz on every protocol instead of the old
         protocol-derived 16 kHz center crop. */
      /* The analyzer already produced tx->pixels bins spanning exactly the
         TX_MONITOR_SPAN_HZ window (carrier-centred), so map the pixel array
         straight onto the screen columns. Linearly interpolate the fractional
         position so it stays smooth even if the widget was resized since the
         analyzer was last configured (tx->pixels != width). */
      int np=tx->pixels;
      if(np<2) np=2;
      /* Tie the trace ends to the graph floor (cosmetic, as before). */
      samples[0]=-200.0;
      samples[np-1]=-200.0;

      double span_den=(width>1)?(double)(width-1):1.0;
      for(i=0;i<width;i++) {
        double fidx=(double)i*(double)(np-1)/span_den;
        int i0=(int)floor(fidx);
        if(i0<0) i0=0;
        if(i0>np-2) i0=np-2;
        double frac=fidx-(double)i0;
        double v=(double)samples[i0]*(1.0-frac)+(double)samples[i0+1]*frac;
        double y=floor((tx->panadapter_high - v)
                            * (double)height
                            / (tx->panadapter_high - tx->panadapter_low));
        if(i==0) cairo_move_to(cr, 0.0, y);
        else     cairo_line_to(cr, (double)i, y);
      }

      /*
      if(radio->display_filled) {
        cairo_close_path (cr);
        cairo_pattern_t *pat=cairo_pattern_create_linear(0.0,0.0,0.0,height);
        //cairo_pattern_add_color_stop_rgba(pat,1.0,0.1,0.0,0.0,0.5);
        //cairo_pattern_add_color_stop_rgba(pat,0.0,0.5,0.0,0.0,0.5);
        
        cairo_set_source (cr, pat);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pat);
      }
      */
      
      SetColour(cr, BACKGROUND);
      cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      
      cairo_set_font_size(cr, 16);

      SetColour(cr, TEXT_A);
      sprintf(text,"%.1f W",tx->fwd);
      cairo_move_to(cr, 206, 34);
      cairo_show_text(cr, text);
  
      // Won't show SWR if power out is less than
      // 100 mW, potentially improve this in the future?
      if (tx->fwd > 1E-1) {
         double this_swr;
         double reflection_coefficient = sqrt(tx->rev/tx->fwd);
         if (1 - reflection_coefficient == 0.0) {
           this_swr = 9999.9;
         }
         else {
           this_swr = (1 + reflection_coefficient) / (1 - reflection_coefficient);
         }
        if (this_swr < 0.0) this_swr=1.0;
        
        // Exponential moving average filter
        double alpha = 0.7;
        tx->swr = (alpha * this_swr) + (1 - alpha) * tx->swr;
        
        sprintf(text,"SWR: %1.1f:1", tx->swr);
        cairo_move_to(cr, 206, 56);
        cairo_show_text(cr, text);
      }
  
      if(tx->rx->mode_a!=CWU && tx->rx->mode_a!=CWL) {
        sprintf(text,"ALC: %2.1f dB",tx->alc);
        cairo_move_to(cr, 206, 80);
        cairo_show_text(cr, text);
      }
    }

    // frequency
    if(tx->rx!=NULL) {
      long long f=tx->rx->frequency_a+tx->rx->ctun_offset-tx->rx->lo_tx;
      if(tx->rx->split) {
        f=tx->rx->frequency_b;
      }
      char temp[32];
      sprintf(temp,"%5lld.%03lld.%03lld",f/(long long)1000000,(f%(long long)1000000)/(long long)1000,f%(long long)1000);
      if(isTransmitting(radio)) {
        SetColour(cr, WARNING);
      } else {
        if(tx->rx->split) {
          SetColour(cr, TEXT_A);
        } else {
          SetColour(cr, TEXT_B);          
        }
      }
      cairo_set_font_size(cr, 21);   // frequency
      cairo_move_to(cr,((double)width/2.0)+2.0,15.0);
      cairo_show_text(cr, temp);
    }
    
    if(radio->discovered->device==DEVICE_HERMES_LITE2) {   
      cairo_set_font_size(cr, 12);       
      SetColour(cr, TEXT_A);
      sprintf(text,"%2.0f degC",tx->temperature);
      cairo_move_to(cr, 220, height-8);
      cairo_show_text(cr, text);
    }
#ifdef SOAPYSDR
    if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {   
      if(radio->discovered->info.soapy.has_temp) {
        cairo_set_font_size(cr, 12);       
        SetColour(cr, TEXT_A);
        int y=height-40;
        for (size_t i = 0; i < radio->discovered->info.soapy.sensors; i++) {
          if(strstr(radio->discovered->info.soapy.sensor[i],"temp")!=NULL) {
            char *value=soapy_protocol_read_sensor(radio->discovered->info.soapy.sensor[i]);
            int v=(int)atof(value);
            if(strcmp(radio->discovered->info.soapy.sensor[i],"xadc_temp0")==0) {
              sprintf(text,"zynq = %dC",v);
            } else if(strcmp(radio->discovered->info.soapy.sensor[i],"ad9361-phy_temp0")==0) {
              sprintf(text,"pluto = %dC",v);
            } else {
              sprintf(text,"%s = %dC",radio->discovered->info.soapy.sensor[i],v);;
            }
            cairo_move_to(cr, width-(width/4), y);
            cairo_show_text(cr, text);
            y+=15;
          } else if(strcmp(radio->discovered->info.soapy.sensor[i],"adm1177_voltage0")==0) {
            char *value=soapy_protocol_read_sensor(radio->discovered->info.soapy.sensor[i]);
            double v=atof(value);
            sprintf(text,"volts = %0.1fv",v);
            cairo_move_to(cr, width-(width/4), y);
            cairo_show_text(cr, text);
            y+=15;
          }
        }
      }
    }
#endif

    cairo_stroke(cr);    
    cairo_destroy(cr);
    gtk_widget_queue_draw(tx->panadapter);

    tx->updated=TRUE;
  }
}
