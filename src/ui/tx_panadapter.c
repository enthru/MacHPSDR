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

/*
 * TX "monitor" panadapter — GPU-rendered via GSK render nodes (PanaView),
 * matching the RX panadapter.  update_tx_panadapter() keeps the one piece of
 * per-frame STATE (the SWR exponential average) and queues a redraw; the
 * tx_pana_build() snapshot builder emits the nodes.
 */

#include <gtk/gtk.h>
#include "log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
#include "pana_view.h"

#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif

// ---- GSK render-node helpers (skin colour + rect/line/text) ----------------
static GdkRGBA txn_css(const char *name,double r,double g,double b,double a) {
  css_rgb(name,&r,&g,&b); return (GdkRGBA){(float)r,(float)g,(float)b,(float)a};
}
static void txn_rect(GtkSnapshot *s,double x,double y,double w,double h,const GdkRGBA *c) {
  if(w<=0.0||h<=0.0) return;
  graphene_rect_t r=GRAPHENE_RECT_INIT((float)x,(float)y,(float)w,(float)h);
  gtk_snapshot_append_color(s,c,&r);
}
static void txn_line(GtkSnapshot *s,double x1,double y1,double x2,double y2,double lw,const GdkRGBA *c) {
  GskPathBuilder *b=gsk_path_builder_new();
  gsk_path_builder_move_to(b,(float)x1,(float)y1);
  gsk_path_builder_line_to(b,(float)x2,(float)y2);
  GskPath *p=gsk_path_builder_free_to_path(b);
  GskStroke *st=gsk_stroke_new((float)lw);
  gtk_snapshot_append_stroke(s,p,st,c);
  gsk_stroke_free(st); gsk_path_unref(p);
}
static PangoLayout *txn_layout(GtkWidget *w,double size,const char *txt) {
  PangoLayout *l=gtk_widget_create_pango_layout(w,txt);
  PangoFontDescription *fd=pango_font_description_new();
  pango_font_description_set_family(fd,css_ui_font());
  pango_font_description_set_absolute_size(fd,size*PANGO_SCALE);
  pango_layout_set_font_description(l,fd);
  pango_font_description_free(fd);
  return l;
}
// text with BASELINE at (x, base_y) — matches cairo move_to+show_text.
static void txn_text(GtkSnapshot *s,GtkWidget *w,double x,double base_y,double size,const GdkRGBA *c,const char *txt) {
  PangoLayout *l=txn_layout(w,size,txt);
  double top=base_y-(double)pango_layout_get_baseline(l)/PANGO_SCALE;
  gtk_snapshot_save(s);
  graphene_point_t pt=GRAPHENE_POINT_INIT((float)x,(float)top);
  gtk_snapshot_translate(s,&pt);
  gtk_snapshot_append_layout(s,l,c);
  gtk_snapshot_restore(s);
  g_object_unref(l);
}
// text with its TOP-LEFT at (x, top_y) — used for the frequency so tall digits
// are not clipped by the top edge.
static void txn_text_top(GtkSnapshot *s,GtkWidget *w,double x,double top_y,double size,const GdkRGBA *c,const char *txt) {
  PangoLayout *l=txn_layout(w,size,txt);
  gtk_snapshot_save(s);
  graphene_point_t pt=GRAPHENE_POINT_INIT((float)x,(float)top_y);
  gtk_snapshot_translate(s,&pt);
  gtk_snapshot_append_layout(s,l,c);
  gtk_snapshot_restore(s);
  g_object_unref(l);
}
// text whose RIGHT edge sits at right_x (baseline at base_y). Used for the
// W/SWR/ALC readouts so an extra digit grows the text leftward.
static void txn_text_right(GtkSnapshot *s,GtkWidget *w,double right_x,double base_y,double size,const GdkRGBA *c,const char *txt) {
  PangoLayout *l=txn_layout(w,size,txt);
  int pw=0,ph=0; pango_layout_get_pixel_size(l,&pw,&ph);
  double top=base_y-(double)pango_layout_get_baseline(l)/PANGO_SCALE;
  gtk_snapshot_save(s);
  graphene_point_t pt=GRAPHENE_POINT_INIT((float)(right_x-(double)pw),(float)top);
  gtk_snapshot_translate(s,&pt);
  gtk_snapshot_append_layout(s,l,c);
  gtk_snapshot_restore(s);
  g_object_unref(l);
}

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
static void tx_pana_build(GtkSnapshot *snapshot, int width, int height, gpointer data);

// PanaView "resize" signal (same signature as the old GtkDrawingArea one).
static void tx_panadapter_resize_cb(GtkWidget *area,int width,int height,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  tx->panadapter_width=width;
  tx->panadapter_height=height;
  if(tx->panadapter_width>1 && tx->panadapter_height>1) {
    log_info("tx_panadapter_resize: width=%d height=%d\n",tx->panadapter_width,tx->panadapter_height);
    /* The analyzer is zoomed onto the TX_MONITOR_SPAN_HZ window, so one pixel per
       screen column suffices — the whole pixel budget lands inside the window. */
    tx->pixels=tx->panadapter_width;
    if(tx->panadapter!=NULL) {
      transmitter_init_analyzer(tx);
      /* Repaint now so every resize refreshes immediately (the periodic RX-loop
         refresh is gated on !tx->updated, only reset in the Soapy path). */
      update_tx_panadapter(radio);
    }
  }
}

GtkWidget *create_tx_panadapter(TRANSMITTER *tx) {

  tx->panadapter_width=0;
  tx->panadapter_height=0;
  tx->panadapter_surface=NULL;
  tx->panadapter=pana_view_new(tx_pana_build,(gpointer)tx);
  gtk_widget_set_size_request(tx->panadapter, 300, 120);

  g_signal_connect(tx->panadapter,"resize",G_CALLBACK(tx_panadapter_resize_cb),(gpointer)tx);

  // GTK4: click via a gesture controller (button masks are gone).
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),0);
  g_signal_connect(click,"pressed",G_CALLBACK(transmitter_pressed_cb),(gpointer)tx);
  gtk_widget_add_controller(tx->panadapter,GTK_EVENT_CONTROLLER(click));

  return tx->panadapter;
}

// Per-frame STATE (fps timer / event callers): update the SWR exponential
// average while transmitting, then queue a redraw. All drawing is in tx_pana_build.
void update_tx_panadapter(RADIO *r) {
  TRANSMITTER *tx=r->transmitter;
  if(tx==NULL || tx->panadapter==NULL) return;

  if(isTransmitting(radio) && tx->fwd > 1E-1) {
    double reflection_coefficient = sqrt(tx->rev/tx->fwd);
    double this_swr;
    if (1 - reflection_coefficient == 0.0) this_swr = 9999.9;
    else this_swr = (1 + reflection_coefficient) / (1 - reflection_coefficient);
    if (this_swr < 0.0) this_swr=1.0;
    double alpha = 0.7;                 // exponential moving average
    tx->swr = (alpha * this_swr) + (1 - alpha) * tx->swr;
  }

  tx->updated=TRUE;
  gtk_widget_queue_draw(tx->panadapter);
}

// Snapshot builder: emit the whole TX monitor scene as GSK nodes.
static void tx_pana_build(GtkSnapshot *snapshot, int width, int height, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  GtkWidget *widget=tx->panadapter;
  if(width<=0 || height<=1) return;

  float *samples=tx->pixel_samples;
  double hz_per_pixel=TX_MONITOR_SPAN_HZ/(double)(width>0?width:1);
  char text[32];
  int i;

  // background
  GdkRGBA surf=txn_css("SURFACE",0.16,0.16,0.19,1.0);
  txn_rect(snapshot,0,0,width,height,&surf);

  double dbm_per_line=(double)height/((double)tx->panadapter_high-(double)tx->panadapter_low);
  GdkRGBA body=txn_css("OFF_WHITE",0.9,0.9,0.9,1.0);   // labels, follow the skin

  // filter passband
  GdkRGBA filt=(GdkRGBA){0.5f,0.5f,0.5f,0.75f};
  double filter_left=(double)width/2.0+((double)tx->actual_filter_low/hz_per_pixel);
  double filter_right=(double)width/2.0+((double)tx->actual_filter_high/hz_per_pixel);
  txn_rect(snapshot,filter_left,20.0,filter_right-filter_left,(double)height-20.0,&filt);

  // dBm level graticule + labels
  {
    GskPathBuilder *b=gsk_path_builder_new();
    gboolean any=FALSE;
    for(i=tx->panadapter_high;i>=tx->panadapter_low;i--) {
      if(abs(i)%20==0) {
        double y=(double)(tx->panadapter_high-i)*dbm_per_line;
        gsk_path_builder_move_to(b,0.0f,(float)y);
        gsk_path_builder_line_to(b,(float)width,(float)y);
        any=TRUE;
        sprintf(text,"%d dBm",i);
        txn_text(snapshot,widget,1,y-4,12,&body,text);
      }
    }
    GskPath *p=gsk_path_builder_free_to_path(b);
    if(any) { GskStroke *st=gsk_stroke_new(1.0f); gtk_snapshot_append_stroke(snapshot,p,st,&body); gsk_stroke_free(st); }
    gsk_path_unref(p);
  }

  // cursor
  GdkRGBA ta=skin_rgba(TEXT_A,1.0);
  txn_line(snapshot,(double)(width/2.0),20.0,(double)(width/2.0),(double)height,1.0,&ta);

  if(tx->rx->mode_a==CWU || tx->rx->mode_a==CWL) {
    GdkRGBA tb=skin_rgba(TEXT_B,1.0);
    double cw_frequency=filter_left+((filter_right-filter_left)/2.0);
    txn_line(snapshot,cw_frequency,20.0,cw_frequency,(double)height,1.0,&tb);
  }

  // signal trace (red) + readouts, only while transmitting
  if(isTransmitting(radio)) {
    int np=tx->pixels;
    if(np<2) np=2;
    samples[0]=-200.0;
    samples[np-1]=-200.0;

    double span_den=(width>1)?(double)(width-1):1.0;
    GskPathBuilder *b=gsk_path_builder_new();
    for(i=0;i<width;i++) {
      double fidx=(double)i*(double)(np-1)/span_den;
      int i0=(int)floor(fidx);
      if(i0<0) i0=0;
      if(i0>np-2) i0=np-2;
      double frac=fidx-(double)i0;
      double v=(double)samples[i0]*(1.0-frac)+(double)samples[i0+1]*frac;
      double y=floor((tx->panadapter_high - v)*(double)height/(tx->panadapter_high - tx->panadapter_low));
      if(i==0) gsk_path_builder_move_to(b,0.0f,(float)y);
      else     gsk_path_builder_line_to(b,(float)i,(float)y);
    }
    GskPath *p=gsk_path_builder_free_to_path(b);
    GdkRGBA red=(GdkRGBA){1.0f,0.0f,0.0f,1.0f};
    GskStroke *st=gsk_stroke_new(1.0f);
    gtk_snapshot_append_stroke(snapshot,p,st,&red);
    gsk_stroke_free(st); gsk_path_unref(p);

    // Right-aligned readouts to the pane edge.
    double readout_right=(double)width-4.0;
    sprintf(text,"%.1f W",tx->fwd);
    txn_text_right(snapshot,widget,readout_right,34,16,&ta,text);

    if (tx->fwd > 1E-1) {   // SWR (tx->swr is the EMA maintained in update_tx_panadapter)
      sprintf(text,"SWR: %1.1f:1", tx->swr);
      txn_text_right(snapshot,widget,readout_right,56,16,&ta,text);
    }
    if(tx->rx->mode_a!=CWU && tx->rx->mode_a!=CWL) {
      sprintf(text,"ALC: %2.1f dB",tx->alc);
      txn_text_right(snapshot,widget,readout_right,80,16,&ta,text);
      if(tx->leveler)    { sprintf(text,"LVL: %2.1f dB",tx->lvlr_gain); txn_text_right(snapshot,widget,readout_right,104,16,&ta,text); }
      if(tx->cfc_run)    { sprintf(text,"CFC: %2.1f dB",tx->cfc_gain);  txn_text_right(snapshot,widget,readout_right,128,16,&ta,text); }
      if(tx->compressor) { sprintf(text,"COMP: %2.1f dB",tx->comp_pk);  txn_text_right(snapshot,widget,readout_right,152,16,&ta,text); }
    }
  }

  // frequency
  if(tx->rx!=NULL) {
    long long f=tx->rx->frequency_a+tx->rx->ctun_offset-tx->rx->lo_tx;
    if(tx->rx->split) f=tx->rx->frequency_b;
    char temp[32];
    sprintf(temp,"%5lld.%03lld.%03lld",f/(long long)1000000,(f%(long long)1000000)/(long long)1000,f%(long long)1000);
    GdkRGBA fc;
    if(isTransmitting(radio))      fc=skin_rgba(WARNING,1.0);
    else if(tx->rx->split)         fc=skin_rgba(TEXT_A,1.0);
    else                           fc=skin_rgba(TEXT_B,1.0);
    // top-left at (centre+2, 1) so the tall 21px digits aren't clipped by the top.
    txn_text_top(snapshot,widget,((double)width/2.0)+2.0,1.0,21,&fc,temp);
  }

  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
    sprintf(text,"%2.0f degC",tx->temperature);
    txn_text(snapshot,widget,220,height-8,12,&ta,text);
  }
#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    if(radio->discovered->info.soapy.has_temp) {
      int y=height-40;
      for (size_t si = 0; si < radio->discovered->info.soapy.sensors; si++) {
        if(strstr(radio->discovered->info.soapy.sensor[si],"temp")!=NULL) {
          char *value=soapy_protocol_read_sensor(radio->discovered->info.soapy.sensor[si]);
          int v=(int)g_ascii_strtod(value,NULL);   // the driver's string is ASCII
          if(strcmp(radio->discovered->info.soapy.sensor[si],"xadc_temp0")==0) sprintf(text,"zynq = %dC",v);
          else if(strcmp(radio->discovered->info.soapy.sensor[si],"ad9361-phy_temp0")==0) sprintf(text,"pluto = %dC",v);
          else sprintf(text,"%s = %dC",radio->discovered->info.soapy.sensor[si],v);
          txn_text(snapshot,widget,width-(width/4),y,12,&ta,text);
          y+=15;
        } else if(strcmp(radio->discovered->info.soapy.sensor[si],"adm1177_voltage0")==0) {
          char *value=soapy_protocol_read_sensor(radio->discovered->info.soapy.sensor[si]);
          double v=g_ascii_strtod(value,NULL);   // the driver's string is ASCII
          sprintf(text,"volts = %0.1fv",v);
          txn_text(snapshot,widget,width-(width/4),y,12,&ta,text);
          y+=15;
        }
      }
    }
  }
#endif
}
