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
#include <wdsp.h>

#include "agc.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "bandstack.h"
#include "band.h"
#include "discovered.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "vfo.h"
#include "meter.h"
#include "wideband_panadapter.h"
#include "wideband_waterfall.h"
#include "waterfall.h"
#include "protocol1.h"
#include "protocol2.h"
#include "receiver_dialog.h"
#ifdef TOOLBAR
#include "receiver_toolbar.h"
#endif
#include "property.h"

/* Display-level limits.  The two ranges match the Configure -> Wideband sliders,
   and the minimum span exists because (high-low) is a divisor in both the
   panadapter draw and the waterfall colour mapping. */
#define WB_DB_MIN         (-200)
#define WB_DB_MAX         20
#define WB_DB_MIN_SPAN    10

#define WB_PANADAPTER_HIGH_DEFAULT 0
#define WB_PANADAPTER_LOW_DEFAULT  (-140)
#define WB_WATERFALL_HIGH_DEFAULT  0
#define WB_WATERFALL_LOW_DEFAULT   (-140)

/* Keep a (low,high) pair inside the range and never let the span collapse — a
   hand-edited or truncated props file must not produce a division by zero. */
static void wb_clamp_pair(gint *low, gint *high) {
  if(*high > WB_DB_MAX) *high = WB_DB_MAX;
  if(*high < WB_DB_MIN + WB_DB_MIN_SPAN) *high = WB_DB_MIN + WB_DB_MIN_SPAN;
  if(*low  < WB_DB_MIN) *low = WB_DB_MIN;
  if(*low  > *high - WB_DB_MIN_SPAN) *low = *high - WB_DB_MIN_SPAN;
}

void wideband_save_state(WIDEBAND *w) {
  char name[80];
  char value[80];
  gint x;
  gint y;
  gint width;
  gint height;

  if(w==NULL) return;

  sprintf(name,"wideband.channel");
  sprintf(value,"%d",w->channel);
  setProperty(name,value);
  sprintf(name,"wideband.adc");
  sprintf(value,"%d",w->adc);
  setProperty(name,value);
  sprintf(name,"wideband.buffer_size");
  sprintf(value,"%d",w->buffer_size);
  setProperty(name,value);
  sprintf(name,"wideband.fft_size");
  sprintf(value,"%d",w->fft_size);
  setProperty(name,value);
  sprintf(name,"wideband.fps");
  sprintf(value,"%d",w->fps);
  setProperty(name,value);

  // Display levels: operator-visible settings the pointer gestures below change,
  // so they have to survive a restart the way the RX panadapter's do.
  sprintf(name,"wideband.panadapter_low");
  sprintf(value,"%d",w->panadapter_low);
  setProperty(name,value);
  sprintf(name,"wideband.panadapter_high");
  sprintf(value,"%d",w->panadapter_high);
  setProperty(name,value);
  sprintf(name,"wideband.waterfall_low");
  sprintf(value,"%d",w->waterfall_low);
  setProperty(name,value);
  sprintf(name,"wideband.waterfall_high");
  sprintf(value,"%d",w->waterfall_high);
  setProperty(name,value);
  sprintf(name,"wideband.waterfall_automatic");
  sprintf(value,"%d",w->waterfall_automatic);
  setProperty(name,value);

  // GTK4: no client-side window position; persist -1 (ignored on restore).
  x=-1; y=-1;
  sprintf(name,"wideband.x");
  sprintf(value,"%d",x);
  setProperty(name,value);
  sprintf(name,"wideband.y");
  sprintf(value,"%d",y);
  setProperty(name,value);

  // The window is gone once the operator closes the wideband display; the sizes
  // it would report are then 0, which would come back as a zero-sized window.
  if(w->window!=NULL) {
    width=gtk_widget_get_width(w->window);
    height=gtk_widget_get_height(w->window);
    if(width>0 && height>0) {
      sprintf(name,"wideband.width");
      sprintf(value,"%d",width);
      setProperty(name,value);
      sprintf(name,"wideband.height");
      sprintf(value,"%d",height);
      setProperty(name,value);
    }
  }
}

void wideband_restore_state(WIDEBAND *w) {
  char name[80];
  char *value;

  sprintf(name,"wideband.channel");
  value=getProperty(name);
  if(value) w->channel=atoi(value);
  sprintf(name,"wideband.adc");
  value=getProperty(name);
  if(value) w->adc=atoi(value);

  sprintf(name,"wideband.panadapter_low");
  value=getProperty(name);
  if(value) w->panadapter_low=atoi(value);
  sprintf(name,"wideband.panadapter_high");
  value=getProperty(name);
  if(value) w->panadapter_high=atoi(value);
  wb_clamp_pair(&w->panadapter_low,&w->panadapter_high);

  sprintf(name,"wideband.waterfall_low");
  value=getProperty(name);
  if(value) w->waterfall_low=atoi(value);
  sprintf(name,"wideband.waterfall_high");
  value=getProperty(name);
  if(value) w->waterfall_high=atoi(value);
  wb_clamp_pair(&w->waterfall_low,&w->waterfall_high);

  sprintf(name,"wideband.waterfall_automatic");
  value=getProperty(name);
  if(value) w->waterfall_automatic=atoi(value)?TRUE:FALSE;
}

// GTK4: window "close-request" replaces "delete-event".
static gboolean window_delete(GtkWindow *window, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  g_source_remove(w->update_timer_id);
  delete_wideband(w);
  return FALSE;
}

/* ---------------------------------------------------------------------------
   Pointer input.

   The wideband display is a fixed full-ADC-span sweep: there is no tuning
   cursor to drag and no receiver to retune, so everything the RX panadapter
   does with the pointer that is about *frequency* has no counterpart here.
   What is left — and what these handlers implement — is the dB scale:

     scroll over the left dB strip  : high (top half) / low (bottom half), as
                                      receiver_scroll_cb() does, 5 dB a notch
     scroll elsewhere on the pana   : slide the whole dB window, span kept
     scroll over the waterfall      : waterfall high/low, same halves rule
     vertical drag                  : pan the dB window under the pointer, so
                                      the trace follows the pointer 1:1
     double-click                   : back to the default scale
     motion                         : crosshair + frequency/level readout

   Scroll always goes through scroll_notches() (receiver.c): a macOS trackpad
   delivers GDK_SCROLL_UNIT_SURFACE streams and without it one swipe would fire
   dozens of steps.
   --------------------------------------------------------------------------- */

#define WB_DB_STEP        5      /* dB per scroll notch (as receiver_scroll_cb) */
#define WB_DB_STRIP_LEFT  4      /* the dBm-label strip, as on the RX panadapter */
#define WB_DB_STRIP_RIGHT 35

/* Slide the window, keeping its span: the shift is clipped against the rails
   first, so hitting the end of the range stops the pan instead of squashing
   the span (which clamping the two ends independently would do). */
static void wb_shift_window(gint *low, gint *high, int delta) {
  if(delta > 0) {
    int room = WB_DB_MAX - *high;
    if(delta > room) delta = room;
  } else {
    int room = WB_DB_MIN - *low;          /* negative */
    if(delta < room) delta = room;
  }
  if(delta == 0) return;
  *low += delta;
  *high += delta;
}

static void wb_queue_draw(WIDEBAND *w) {
  if(w->panadapter != NULL) gtk_widget_queue_draw(w->panadapter);
  if(w->waterfall != NULL) gtk_widget_queue_draw(w->waterfall);
}

void wideband_pressed_cb(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w==NULL) return;
  if(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))!=1) return;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  if(n_press>=2) {
    /* Double-click resets the scale of the surface it happened on, the way the
       RX raster views refit on a double-click.  The waterfall's automatic flag
       is deliberately left alone — it is an explicit Configure choice, not part
       of "the scale". */
    if(widget!=NULL && widget==w->waterfall) {
      w->waterfall_high=WB_WATERFALL_HIGH_DEFAULT;
      w->waterfall_low=WB_WATERFALL_LOW_DEFAULT;
    } else {
      w->panadapter_high=WB_PANADAPTER_HIGH_DEFAULT;
      w->panadapter_low=WB_PANADAPTER_LOW_DEFAULT;
    }
    w->pointer_pressed=FALSE;
    w->has_moved=FALSE;
    wb_queue_draw(w);
    return;
  }

  w->last_x=(gint)x;
  w->last_y=(gint)y;
  w->has_moved=FALSE;
  w->pointer_pressed=TRUE;
}

void wideband_released_cb(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w==NULL) return;
  if(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))!=1) return;
  w->last_x=(gint)x;
  w->last_y=(gint)y;
  w->pointer_pressed=FALSE;
  w->has_moved=FALSE;
}

void wideband_motion_cb(GtkEventControllerMotion *controller, double ex, double ey, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w==NULL) return;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  gint x=(gint)ex;
  gint y=(gint)ey;
  gboolean on_panadapter=(widget!=NULL && widget==w->panadapter);
  gboolean moved_pointer=(x!=w->cursor_x || y!=w->cursor_y || on_panadapter!=w->cursor_valid);

  w->cursor_x=x;
  w->cursor_y=y;
  w->cursor_valid=on_panadapter;

  /* A drag is button 1 down in a press we received ourselves; a motion with no
     button down clears a press whose release never arrived (grab broken,
     released off-window). */
  gboolean button1=(state & GDK_BUTTON1_MASK)==GDK_BUTTON1_MASK;
  if(!button1) w->pointer_pressed=FALSE;

  if(button1 && w->pointer_pressed) {
    gint *low=NULL;
    gint *high=NULL;
    int height=0;
    if(widget!=NULL && widget==w->waterfall) {
      /* Inert while the waterfall tracks the noise floor itself — the next
         frame would recompute both ends anyway. */
      if(!w->waterfall_automatic) {
        low=&w->waterfall_low; high=&w->waterfall_high; height=w->waterfall_height;
      }
    } else {
      low=&w->panadapter_low; high=&w->panadapter_high; height=w->panadapter_height;
    }
    if(low!=NULL && height>0) {
      int span=*high-*low;
      /* Drag the window so the trace follows the pointer: a sample drawn at
         screen y sits at y=(high-s)*H/span, so shifting both ends by delta
         moves it by delta*H/span pixels. */
      int delta=(int)lround((double)(y-w->last_y)*(double)span/(double)height);
      if(delta!=0) {
        wb_shift_window(low,high,delta);
        w->last_y=y;                 /* only on a real step, so a slow drag accumulates */
        w->has_moved=TRUE;
        wb_queue_draw(w);
      }
    }
    w->last_x=x;
    return;
  }

  if(widget!=NULL) {
    gtk_widget_set_cursor_from_name(widget,
        (on_panadapter && x>WB_DB_STRIP_LEFT && x<WB_DB_STRIP_RIGHT) ? "ns-resize" : "crosshair");
  }
  /* The readout follows the pointer; the fps timer only redraws when the WDSP
     analyzer has fresh pixels, so it cannot be relied on here. */
  if(moved_pointer && w->panadapter!=NULL) gtk_widget_queue_draw(w->panadapter);
}

void wideband_leave_cb(GtkEventControllerMotion *controller, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w==NULL) return;
  if(!w->cursor_valid) return;
  w->cursor_valid=FALSE;
  if(w->panadapter!=NULL) gtk_widget_queue_draw(w->panadapter);
}

gboolean wideband_scroll_cb(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w==NULL) return TRUE;
  /* Trackpad desensitising: mandatory, see scroll_notches() in receiver.c. */
  int n=scroll_notches(controller,dy);
  if(n==0) return TRUE;             /* trackpad delta below the notch threshold */
  gboolean up=n<0;
  int mag=n<0?-n:n;                 /* notches this event (>1 on a fast flick) */
  int step=WB_DB_STEP*mag;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  gint x=w->cursor_x;
  gint y=w->cursor_y;

  if(widget!=NULL && widget==w->waterfall) {
    /* Same reason the RX dB strip is inert under Panadapter Automatic: with the
       waterfall tracking the average, the next frame overwrites both ends. */
    if(w->waterfall_automatic) return TRUE;
    if(w->waterfall_height<=0) return TRUE;
    if(y < w->waterfall_height/2) {
      w->waterfall_high += up ? -step : step;
    } else {
      w->waterfall_low += up ? -step : step;
    }
    wb_clamp_pair(&w->waterfall_low,&w->waterfall_high);
  } else {
    if(w->panadapter_height<=0) return TRUE;
    if(x>WB_DB_STRIP_LEFT && x<WB_DB_STRIP_RIGHT) {
      /* Over the dBm labels: stretch/compress the scale, exactly as
         receiver_scroll_cb() does over the RX panadapter's strip. */
      if(y < w->panadapter_height/2) {
        w->panadapter_high += up ? -step : step;
      } else {
        w->panadapter_low += up ? -step : step;
      }
      wb_clamp_pair(&w->panadapter_low,&w->panadapter_high);
    } else {
      /* Anywhere else the RX panadapter would tune, and there is nothing here
         to tune: slide the whole window instead, keeping its span. */
      wb_shift_window(&w->panadapter_low,&w->panadapter_high,up ? -step : step);
    }
  }
  wb_queue_draw(w);
  return TRUE;
}

static gboolean update_timer_cb(void *data) {
  int rc;
  WIDEBAND *w=(WIDEBAND *)data;

  // pixel_samples is NULL until the panadapter's first resize_timeout runs
  // (w->pixels starts at 0, so wideband_init_analyzer() allocates nothing), and
  // this timer starts before that: GetPixels() would write the analyzer's output
  // through a NULL pointer.  Same unguarded-NULL as the waterfall had.
  if(w->panadapter_resize_timer==-1 && w->pixel_samples!=NULL) {
    GetPixels(w->channel,0,w->pixel_samples,&rc);
    if(rc) {
      update_wideband_panadapter(w);
      update_wideband_waterfall(w);
    }
  }
  return TRUE;
}
 
static void full_wideband_buffer(WIDEBAND *w) {
  Spectrum0(1, w->channel, 0, 0, w->input_buffer);
}

void reset_wideband_buffer_index(WIDEBAND *w) {
  if(w!=NULL) {
    w->samples=0;
  }
}

void add_wideband_sample(WIDEBAND *w,double sample) {
  w->input_buffer[w->samples*2]=sample;
  w->input_buffer[(w->samples*2)+1]=0.0;
  w->samples=w->samples+1;
  if(w->samples>=w->buffer_size) {
    full_wideband_buffer(w);
    w->samples=0;
  }
}

void wideband_update_title(WIDEBAND *w) {
  gchar title[32];
  g_snprintf((gchar *)&title,sizeof(title),"Linux HPSDR: Wideband ADC-%d",w->adc);
log_info("create_visual: %s\n",title);
  gtk_window_set_title(GTK_WINDOW(w->window),title);
}

static void create_visual(WIDEBAND *w) {

  w->window=gtk_window_new();
  g_signal_connect(w->window,"close-request",G_CALLBACK (window_delete), w);

  wideband_update_title(w);

  gtk_window_set_default_size(GTK_WINDOW(w->window),w->window_width,w->window_height);

  // GTK4: GtkTable is gone — a single-cell GtkGrid holds the spectrum split.
  w->table=gtk_grid_new();

  GtkWidget *vpaned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand(vpaned,TRUE);
  gtk_widget_set_vexpand(vpaned,TRUE);
  gtk_grid_attach(GTK_GRID(w->table), vpaned, 0, 0, 1, 1);

  w->panadapter=create_wideband_panadapter(w);
  gtk_paned_set_start_child (GTK_PANED(vpaned), w->panadapter);
  gtk_paned_set_resize_start_child (GTK_PANED(vpaned), TRUE);
  gtk_paned_set_shrink_start_child (GTK_PANED(vpaned), TRUE);

  w->waterfall=create_wideband_waterfall(w);
  gtk_paned_set_end_child (GTK_PANED(vpaned), w->waterfall);
  gtk_paned_set_resize_end_child (GTK_PANED(vpaned), TRUE);
  gtk_paned_set_shrink_end_child (GTK_PANED(vpaned), TRUE);

  gtk_window_set_child(GTK_WINDOW(w->window),w->table);
}

void wideband_init_analyzer(WIDEBAND *w) {
    int flp[] = {0};
    double keep_time = 0.1;
    int n_pixout=1;
    int spur_elimination_ffts = 1;
    int data_type = 1;
    int fft_size = w->fft_size;
    int window_type = 4;
    double kaiser_pi = 14.0;
    int overlap = 0; //1024; //4096;
    int clip = 0;
    int span_clip_l = 0;
    int span_clip_h = 0;
    int pixels=w->pixels;
    int stitches = 1;
    int calibration_data_set = 0;
    double span_min_freq = 0.0;
    double span_max_freq = 0.0;


  if(w->pixel_samples!=NULL) {
    g_free(w->pixel_samples);
    w->pixel_samples=NULL;
  }
  // The one place w->pixels changes, so the one place the axis scale is derived.
  w->hz_per_pixel = (w->pixels>0) ? (double)WIDEBAND_SPAN_HZ/(double)w->pixels : 0.0;
  if(w->pixels>0) {
    w->pixel_samples=g_new0(float,w->pixels*2);
    int max_w = fft_size + (int) min(keep_time * (double) w->fps, keep_time * (double) fft_size * (double) w->fps);

    //overlap = (int)max(0.0, ceil(fft_size - (double)w->sample_rate / (double)w->fps));

    SetAnalyzer(w->channel,
            n_pixout,
            spur_elimination_ffts, //number of LO frequencies = number of ffts used in elimination
            data_type, //0 for real input data (I only); 1 for complex input data (I & Q)
            flp, //vector with one elt for each LO frequency, 1 if high-side LO, 0 otherwise
            fft_size, //size of the fft, i.e., number of input samples
            w->buffer_size, //number of samples transferred for each OpenBuffer()/CloseBuffer()
            window_type, //integer specifying which window function to use
            kaiser_pi, //PiAlpha parameter for Kaiser window
            overlap, //number of samples each fft (other than the first) is to re-use from the previous
            clip, //number of fft output bins to be clipped from EACH side of each sub-span
            span_clip_l, //number of bins to clip from low end of entire span
            span_clip_h, //number of bins to clip from high end of entire span
            pixels*2, //number of pixel values to return.  may be either <= or > number of bins
            stitches, //number of sub-spans to concatenate to form a complete span
            calibration_data_set, //identifier of which set of calibration data to use
            span_min_freq, //frequency at first pixel value8192
            span_max_freq, //frequency at last pixel value
            max_w //max samples to hold in input ring buffers
    );
  }

}


WIDEBAND *create_wideband(int channel) {
  WIDEBAND *w=g_new0(WIDEBAND,1);
  char name [80];
  char *value;
  gint x=-1;
  gint y=-1;
  gint width=512;
  gint height=180;

log_info("create_wideband: channel=%d\n",channel);
  w->channel=channel;
  w->adc=0;

  w->pixels=0;
  w->pixel_samples=NULL;
  w->sequence=0;
  w->buffer_size=16384;
  w->input_buffer=g_new0(gdouble,w->buffer_size*2);

  w->fps=10;

  w->fft_size=w->buffer_size;

  // set default location and sizes
  w->window_x=channel*30;
  w->window_y=channel*30;
  w->window_width=512;
  w->window_height=180;
  w->window=NULL;
  w->panadapter_high=WB_PANADAPTER_HIGH_DEFAULT;
  w->panadapter_low=WB_PANADAPTER_LOW_DEFAULT;

  w->waterfall_high=WB_WATERFALL_HIGH_DEFAULT;
  w->waterfall_low=WB_WATERFALL_LOW_DEFAULT;
  w->waterfall_automatic=TRUE;

  w->cursor_x=-1;
  w->cursor_y=-1;
  w->cursor_valid=FALSE;
  w->pointer_pressed=FALSE;

  wideband_restore_state(w);

  int result;
  XCreateAnalyzer(w->channel, &result, 262144, 1, 1, "");
  if(result != 0) {
    log_info("XCreateAnalyzer channel=%d failed: %d\n", w->channel, result);
  } else {
    wideband_init_analyzer(w);
  }

  SetDisplayDetectorMode(w->channel, 0, DETECTOR_MODE_AVERAGE/*display_detector_mode*/);
  SetDisplayAverageMode(w->channel, 0,  AVERAGE_MODE_LOG_RECURSIVE/*display_average_mode*/);


  create_visual(w);
  if(w->window!=NULL) {
    // GTK4: no client-side window_move; only the size is restorable.
    sprintf(name,"wideband.width");
    value=getProperty(name);
    if(value) width=atoi(value);
    sprintf(name,"wideband.height");
    value=getProperty(name);
    if(value) height=atoi(value);
    if(width>0 && height>0) {
      gtk_window_set_default_size(GTK_WINDOW(w->window),width,height);
    }
    (void)x; (void)y;
    gtk_widget_set_visible(w->window,TRUE);
  }

log_info("create_widband: update_timer: %d\n",1000/w->fps);
  w->update_timer_id=g_timeout_add(1000/w->fps,update_timer_cb,(gpointer)w);
  return w;
}
