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
 * Wideband panadapter — GPU-rendered via GSK render nodes (PanaView), matching
 * the RX/TX panadapters.  GetPixels() is done by the caller (wideband.c) before
 * update_wideband_panadapter(), which now just queues a redraw; wb_pana_build()
 * emits the nodes.
 */

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <wdsp.h>

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
#include "pana_view.h"

// ---- GSK render-node helpers (FreeMono bold labels, like the old cairo) -----
static void wbn_rect(GtkSnapshot *s,double x,double y,double w,double h,const GdkRGBA *c) {
  if(w<=0.0||h<=0.0) return;
  graphene_rect_t r=GRAPHENE_RECT_INIT((float)x,(float)y,(float)w,(float)h);
  gtk_snapshot_append_color(s,c,&r);
}
static void wbn_line(GtkSnapshot *s,double x1,double y1,double x2,double y2,double lw,const GdkRGBA *c) {
  GskPathBuilder *b=gsk_path_builder_new();
  gsk_path_builder_move_to(b,(float)x1,(float)y1);
  gsk_path_builder_line_to(b,(float)x2,(float)y2);
  GskPath *p=gsk_path_builder_free_to_path(b);
  GskStroke *st=gsk_stroke_new((float)lw);
  gtk_snapshot_append_stroke(s,p,st,c);
  gsk_stroke_free(st); gsk_path_unref(p);
}
static PangoLayout *wbn_layout(GtkWidget *w,const char *txt) {
  PangoLayout *l=gtk_widget_create_pango_layout(w,txt);
  PangoFontDescription *fd=pango_font_description_new();
  pango_font_description_set_family(fd,"FreeMono");
  pango_font_description_set_weight(fd,PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(fd,12*PANGO_SCALE);
  pango_layout_set_font_description(l,fd);
  pango_font_description_free(fd);
  return l;
}
// text with BASELINE at (x, base_y). Returns advance width via out_w.
static void wbn_text(GtkSnapshot *s,GtkWidget *w,double x,double base_y,const GdkRGBA *c,const char *txt,double *out_w) {
  PangoLayout *l=wbn_layout(w,txt);
  int pw=0,ph=0; pango_layout_get_pixel_size(l,&pw,&ph);
  if(out_w) *out_w=(double)pw;
  double top=base_y-(double)pango_layout_get_baseline(l)/PANGO_SCALE;
  gtk_snapshot_save(s);
  graphene_point_t pt=GRAPHENE_POINT_INIT((float)x,(float)top);
  gtk_snapshot_translate(s,&pt);
  gtk_snapshot_append_layout(s,l,c);
  gtk_snapshot_restore(s);
  g_object_unref(l);
}

static void wb_pana_build(GtkSnapshot *snapshot, int cwidth, int cheight, gpointer data);

static gboolean resize_timeout(void *data) {
  WIDEBAND *w=(WIDEBAND *)data;

  w->panadapter_width=w->panadapter_resize_width;
  w->panadapter_height=w->panadapter_resize_height;
  w->pixels=w->panadapter_width;

  wideband_init_analyzer(w);

  // GSK render-node path: no off-screen panadapter_surface to (re)allocate.
  w->panadapter_resize_timer=-1;
  return FALSE;
}

// PanaView "resize" signal (same signature as the old GtkDrawingArea one).
static void wideband_panadapter_resize_cb(GtkWidget *area,int width,int height,gpointer data) {
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

GtkWidget *create_wideband_panadapter(WIDEBAND *w) {
  GtkWidget *panadapter;

  w->panadapter_width=0;
  w->panadapter_height=0;
  w->panadapter_surface=NULL;
  w->panadapter_resize_timer=-1;

  panadapter = pana_view_new(wb_pana_build,(gpointer)w);
  g_signal_connect(panadapter,"resize",G_CALLBACK(wideband_panadapter_resize_cb),(gpointer)w);

  // GTK4: pointer input via event controllers (button masks are gone).
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),0);
  g_signal_connect(click,"pressed",G_CALLBACK(wideband_pressed_cb),w);
  g_signal_connect(click,"released",G_CALLBACK(wideband_released_cb),w);
  gtk_widget_add_controller(panadapter,GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion=gtk_event_controller_motion_new();
  g_signal_connect(motion,"motion",G_CALLBACK(wideband_motion_cb),w);
  g_signal_connect(motion,"leave",G_CALLBACK(wideband_leave_cb),w);
  gtk_widget_add_controller(panadapter,motion);

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(wideband_scroll_cb),w);
  gtk_widget_add_controller(panadapter,scroll);

  return panadapter;
}

// The caller (wideband.c) fetches fresh pixels via GetPixels() before this; here
// we just queue the GPU redraw.
void update_wideband_panadapter(WIDEBAND *w) {
  if(w->panadapter!=NULL) gtk_widget_queue_draw(w->panadapter);
}

// Snapshot builder: emit the wideband scene as GSK nodes.
static void wb_pana_build(GtkSnapshot *snapshot, int cwidth, int cheight, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  GtkWidget *widget=w->panadapter;
  long i;
  double x;

  int display_height=cheight;
  if(display_height<=1 || cwidth<=0) return;
  if(w->pixels<=0 || w->pixel_samples==NULL) return;

  double hz_per_pixel=(double)WIDEBAND_SPAN_HZ/(double)w->pixels;
  float *samples=w->pixel_samples;

  // background: opaque dark base + the translucent grey->dark gradient wash.
  GdkRGBA base=(GdkRGBA){0.05f,0.05f,0.05f,1.0f};
  wbn_rect(snapshot,0,0,cwidth,cheight,&base);
  {
    graphene_rect_t r=GRAPHENE_RECT_INIT(0,0,(float)cwidth,(float)cheight);
    GskColorStop stops[2]={ {0.0f,(GdkRGBA){0.5f,0.5f,0.5f,0.5f}}, {1.0f,(GdkRGBA){0.1f,0.1f,0.1f,0.5f}} };
    graphene_point_t p0=GRAPHENE_POINT_INIT(0.0f,0.0f), p1=GRAPHENE_POINT_INIT(0.0f,(float)cheight);
    gtk_snapshot_append_linear_gradient(snapshot,&r,&p0,&p1,stops,2);
  }

  GdkRGBA white=(GdkRGBA){1.0f,1.0f,1.0f,1.0f};
  double dbm_per_line=(double)w->panadapter_height/((double)w->panadapter_high-(double)w->panadapter_low);

  // dBm level graticule + labels
  {
    GskPathBuilder *b=gsk_path_builder_new();
    gboolean any=FALSE;
    char v[32];
    for(i=w->panadapter_high;i>=w->panadapter_low;i--) {
      if(labs(i)%20==0) {
        double y=(double)(w->panadapter_high-i)*dbm_per_line;
        gsk_path_builder_move_to(b,0.0f,(float)y);
        gsk_path_builder_line_to(b,(float)w->panadapter_width,(float)y);
        any=TRUE;
        sprintf(v,"%ld dBm",i);
        wbn_text(snapshot,widget,1,y,&white,v,NULL);
      }
    }
    GskPath *p=gsk_path_builder_free_to_path(b);
    if(any) { GskStroke *st=gsk_stroke_new(1.0f); gtk_snapshot_append_stroke(snapshot,p,st,&white); gsk_stroke_free(st); }
    gsk_path_unref(p);
  }

  // 5 MHz frequency markers + labels
  {
    GskPathBuilder *b=gsk_path_builder_new();
    gboolean any=FALSE;
    char v[32];
    for(i=5000000;i<WIDEBAND_SPAN_HZ;i+=5000000) {
      x=(double)i/hz_per_pixel;
      gsk_path_builder_move_to(b,(float)x,10.0f);
      gsk_path_builder_line_to(b,(float)x,(float)display_height);
      any=TRUE;
      sprintf(v,"%0ld",i/1000000);
      PangoLayout *ml=wbn_layout(widget,v); int pw=0,ph=0; pango_layout_get_pixel_size(ml,&pw,&ph); g_object_unref(ml);
      wbn_text(snapshot,widget,x-(double)pw/2.0,10.0,&white,v,NULL);   // centre on the marker
    }
    GskPath *p=gsk_path_builder_free_to_path(b);
    if(any) { GskStroke *st=gsk_stroke_new(1.0f); gtk_snapshot_append_stroke(snapshot,p,st,&white); gsk_stroke_free(st); }
    gsk_path_unref(p);
  }

  // cursor (active RX frequency)
  if(radio->active_receiver!=NULL) {
    GdkRGBA red=(GdkRGBA){1.0f,0.0f,0.0f,1.0f};
    x=(double)radio->active_receiver->frequency_a/hz_per_pixel;
    wbn_line(snapshot,x,0.0,x,(double)display_height,1.0,&red);
  }

  // signal trace
  samples[w->pixels]=-200.0;
  samples[(w->pixels*2)-1]=-200.0;

  GskPathBuilder *b=gsk_path_builder_new();
  double s1=(double)samples[w->pixels];
  s1=floor((w->panadapter_high - s1)*(double)display_height/(w->panadapter_high - w->panadapter_low));
  gsk_path_builder_move_to(b,0.0f,(float)s1);
  for(i=1;i<w->pixels;i++) {
    double s2=(double)samples[i+w->pixels];
    s2=floor((w->panadapter_high - s2)*(double)display_height/(w->panadapter_high - w->panadapter_low));
    gsk_path_builder_line_to(b,(float)i,(float)s2);
  }
  if(radio->display_filled) gsk_path_builder_close(b);
  GskPath *p=gsk_path_builder_free_to_path(b);

  if(radio->display_filled) {
    graphene_rect_t r=GRAPHENE_RECT_INIT(0,0,(float)cwidth,(float)cheight);
    GskColorStop stops[2]={ {0.0f,(GdkRGBA){0.0f,0.0f,1.0f,0.5f}}, {1.0f,(GdkRGBA){1.0f,1.0f,1.0f,0.5f}} };
    graphene_point_t p0=GRAPHENE_POINT_INIT(0.0f,0.0f), p1=GRAPHENE_POINT_INIT(0.0f,(float)w->panadapter_height);
    gtk_snapshot_push_fill(snapshot,p,GSK_FILL_RULE_WINDING);
    gtk_snapshot_append_linear_gradient(snapshot,&r,&p0,&p1,stops,2);
    gtk_snapshot_pop(snapshot);
  }
  GskStroke *st=gsk_stroke_new(1.0f);
  gtk_snapshot_append_stroke(snapshot,p,st,&white);
  gsk_stroke_free(st); gsk_path_unref(p);

  // Pointer readout: on a 0-61.44 MHz sweep one pixel is tens of kHz, so where
  // the pointer actually is has to be said in numbers.  Drawn last, on top of
  // the trace.  Crosshair lines are append_color rects (never a GskPath) per the
  // renderer rule; the cursor position is stashed by wideband_motion_cb().
  if(w->cursor_valid && w->cursor_x>=0 && w->cursor_x<cwidth) {
    double cx=(double)w->cursor_x;
    GdkRGBA hair=(GdkRGBA){0.9f,0.9f,0.3f,0.6f};
    wbn_rect(snapshot,cx,0.0,1.0,(double)display_height,&hair);
    if(w->cursor_y>=0 && w->cursor_y<display_height) {
      wbn_rect(snapshot,0.0,(double)w->cursor_y,(double)cwidth,1.0,&hair);
    }

    char txt[64];
    double f_mhz=cx*hz_per_pixel/1000000.0;
    // The trace level in this column is the number the sweep exists to give;
    // fall back to the frequency alone if the pixel is outside the analyzer's
    // current buffer (mid-resize, before resize_timeout re-inits it).
    if(w->cursor_x < w->pixels) {
      snprintf(txt,sizeof(txt),"%.3f MHz  %.0f dBm",f_mhz,(double)samples[w->cursor_x+w->pixels]);
    } else {
      snprintf(txt,sizeof(txt),"%.3f MHz",f_mhz);
    }

    PangoLayout *rl=wbn_layout(widget,txt);
    int rw=0,rh=0; pango_layout_get_pixel_size(rl,&rw,&rh); g_object_unref(rl);
    double tx=cx+6.0;
    if(tx+(double)rw+4.0>(double)cwidth) tx=cx-6.0-(double)rw;   // flip at the right edge
    if(tx<0.0) tx=0.0;
    // Below the frequency-marker labels, but never off the bottom of a short pane.
    double base_y=(double)(rh+22);
    if(base_y>(double)display_height-2.0) base_y=(double)display_height-2.0;
    if(base_y<(double)rh) base_y=(double)rh;
    GdkRGBA backing=(GdkRGBA){0.0f,0.0f,0.0f,0.6f};
    wbn_rect(snapshot,tx-2.0,base_y-(double)rh,(double)rw+4.0,(double)rh+3.0,&backing);
    GdkRGBA fg=(GdkRGBA){1.0f,1.0f,0.6f,1.0f};
    wbn_text(snapshot,widget,tx,base_y,&fg,txt,NULL);
  }
}
