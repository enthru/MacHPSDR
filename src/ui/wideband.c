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

/* The one WIDEBAND, alive from the first Add Wideband to process exit -- see the
   note on create_wideband(). */
static WIDEBAND *the_wideband=NULL;

/* Whether wideband_save_state() has anything to write.  The singleton outlives
   the window, so "opened at some point this session" is the question -- and it
   is the one radio_save_state() needs, since it must retain the saved
   wideband.* keys only when nothing is going to be written over them. */
gboolean wideband_has_state(void) {
  return the_wideband!=NULL;
}

void wideband_save_state(WIDEBAND *w) {
  char name[80];
  char value[80];
  gint x;
  gint y;
  gint width;
  gint height;

  // radio_state.c passes radio->wideband, which delete_wideband() sets to NULL
  // the moment the operator closes the window -- so without this fallback every
  // scale they set in there is saved only in the one case where the wideband
  // window is still open when the application quits.
  if(w==NULL) w=the_wideband;
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

  // The window is gone once the operator closes the wideband display, and the
  // sizes it would report are then 0 -- which would come back as a zero-sized
  // window.  window_delete() copies the last live size into window_width/height
  // for exactly this reason: the state is saved at application exit, which for
  // any operator who closed the window first is long after the widget went.
  width=w->window_width;
  height=w->window_height;
  if(w->window!=NULL) {
    width=gtk_widget_get_width(w->window);
    height=gtk_widget_get_height(w->window);
  }
  if(width>0 && height>0) {
    sprintf(name,"wideband.width");
    sprintf(value,"%d",width);
    setProperty(name,value);
    sprintf(name,"wideband.height");
    sprintf(value,"%d",height);
    setProperty(name,value);
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
//
// Everything below the window is deliberately NOT torn down -- see the "closing
// the window keeps the analyzer" note on create_wideband().  What must go is
// everything that would outlive the widgets: the display timer, the two
// debounce timers (both fire 250 ms later into a WIDEBAND whose panadapter and
// waterfall pointers GTK has by then finalised -- waterfall.c's resize_timeout
// tests w->waterfall for NULL and would have allocated a fresh pixbuf for a
// window that no longer exists), the pixbuf itself, and the widget pointers,
// which every draw path here tests for NULL and none of which was ever cleared.
static gboolean window_delete(GtkWindow *window, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w->update_timer_id>0) {
    g_source_remove(w->update_timer_id);
    w->update_timer_id=0;
  }
  if(w->panadapter_resize_timer!=(guint)-1) {
    g_source_remove(w->panadapter_resize_timer);
    w->panadapter_resize_timer=(guint)-1;
  }
  if(w->waterfall_resize_timer!=(guint)-1) {
    g_source_remove(w->waterfall_resize_timer);
    w->waterfall_resize_timer=(guint)-1;
  }
  if(w->waterfall_pixbuf!=NULL) {
    g_object_unref((gpointer)w->waterfall_pixbuf);
    w->waterfall_pixbuf=NULL;
  }
  // Keep the size the operator left the window at: wideband_save_state() runs
  // at application exit, by which time gtk_widget_get_width() has nothing to
  // ask.  Without this the geometry is persisted only when the window happens
  // to still be open when the app quits.
  if(w->window!=NULL) {
    gint ww=gtk_widget_get_width(w->window);
    gint wh=gtk_widget_get_height(w->window);
    if(ww>0 && wh>0) { w->window_width=ww; w->window_height=wh; }
  }
  w->panadapter=NULL;
  w->waterfall=NULL;
  w->waterfall_width=0;
  w->waterfall_height=0;
  w->panadapter_width=0;
  w->panadapter_height=0;
  w->cursor_valid=FALSE;
  w->pointer_pressed=FALSE;
  delete_wideband(w);
  w->window=NULL;
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

/* ---------------------------------------------------------------------------
   The three decisions the pointer makes here, as pure functions of the WIDEBAND
   state: no widgets, no events, no GTK.  The callbacks below are thin adapters
   that work out which surface the pointer is on and how many notches a scroll
   was worth, and then call these.

   The split is not decoration.  GTK4 has no public event-injection API, so a
   press or a drag cannot be synthesised, and this window needs a wideband ADC
   feed no hardware here provides -- which is why none of this code had ever
   executed.  With the arithmetic separated it can be asserted headlessly
   (wb_selftest below), and what is left unproven is only the adapter layer.
   --------------------------------------------------------------------------- */

/* Double-click: the scale of the surface it happened on goes back to default.
   The waterfall's automatic flag is deliberately left alone -- it is an explicit
   Configure choice, not part of "the scale". */
static void wb_scale_reset(WIDEBAND *w, gboolean on_waterfall) {
  if(on_waterfall) {
    w->waterfall_high=WB_WATERFALL_HIGH_DEFAULT;
    w->waterfall_low=WB_WATERFALL_LOW_DEFAULT;
  } else {
    w->panadapter_high=WB_PANADAPTER_HIGH_DEFAULT;
    w->panadapter_low=WB_PANADAPTER_LOW_DEFAULT;
  }
}

/* `n` is signed notches as scroll_notches() returns them (negative = scroll up),
   x/y are surface-local.  Returns TRUE when something moved, i.e. when a redraw
   is worth queueing. */
static gboolean wb_scale_scroll(WIDEBAND *w, gboolean on_waterfall, int n, gint x, gint y) {
  gboolean up=n<0;
  int mag=n<0?-n:n;                 /* notches this event (>1 on a fast flick) */
  int step=WB_DB_STEP*mag;

  if(on_waterfall) {
    /* Same reason the RX dB strip is inert under Panadapter Automatic: with the
       waterfall tracking the average, the next frame overwrites both ends. */
    if(w->waterfall_automatic) return FALSE;
    if(w->waterfall_height<=0) return FALSE;
    if(y < w->waterfall_height/2) {
      w->waterfall_high += up ? -step : step;
    } else {
      w->waterfall_low += up ? -step : step;
    }
    wb_clamp_pair(&w->waterfall_low,&w->waterfall_high);
    return TRUE;
  }

  if(w->panadapter_height<=0) return FALSE;
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
    /* Anywhere else the RX panadapter would tune, and there is nothing here to
       tune: slide the whole window instead, keeping its span. */
    wb_shift_window(&w->panadapter_low,&w->panadapter_high,up ? -step : step);
  }
  return TRUE;
}

/* Vertical drag from w->last_y to y.  Returns TRUE when the window moved by a
   whole dB (last_y is advanced only then, so a slow drag accumulates instead of
   rounding to nothing every frame). */
static gboolean wb_scale_drag(WIDEBAND *w, gboolean on_waterfall, gint y) {
  gint *low=NULL;
  gint *high=NULL;
  int height=0;

  if(on_waterfall) {
    /* Inert while the waterfall tracks the noise floor itself -- the next frame
       would recompute both ends anyway. */
    if(w->waterfall_automatic) return FALSE;
    low=&w->waterfall_low; high=&w->waterfall_high; height=w->waterfall_height;
  } else {
    low=&w->panadapter_low; high=&w->panadapter_high; height=w->panadapter_height;
  }
  if(height<=0) return FALSE;

  int span=*high-*low;
  /* Drag the window so the trace follows the pointer: a sample drawn at screen
     y sits at y=(high-s)*H/span, so shifting both ends by delta moves it by
     delta*H/span pixels. */
  int delta=(int)lround((double)(y-w->last_y)*(double)span/(double)height);
  if(delta==0) return FALSE;
  wb_shift_window(low,high,delta);
  w->last_y=y;
  return TRUE;
}

void wideband_pressed_cb(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(w==NULL) return;
  if(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))!=1) return;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  if(n_press>=2) {
    /* Double-click resets the scale of the surface it happened on, the way the
       RX raster views refit on a double-click. */
    wb_scale_reset(w,(gboolean)(widget!=NULL && widget==w->waterfall));
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
    if(wb_scale_drag(w,(gboolean)(widget!=NULL && widget==w->waterfall),y)) {
      w->has_moved=TRUE;
      wb_queue_draw(w);
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
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));

  /* The scroll event carries no coordinates; the position the motion handler
     stashed is what says whether the pointer is over the dBm strip. */
  if(wb_scale_scroll(w,(gboolean)(widget!=NULL && widget==w->waterfall),n,
                     w->cursor_x,w->cursor_y)) {
    wb_queue_draw(w);
  }
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


/* Closing the window keeps everything below it, and the SAME WIDEBAND comes back
   on the next Add Wideband.

   Nothing here can be freed safely: protocol2_thread() reads radio->wideband
   without a lock and hands the pointer straight to process_wideband_data(), so
   a packet already in flight when the window closes would write through
   whatever was freed.  What can be done -- and what the singleton does -- is
   never allocate any of it twice.  It was allocated twice: every press of Add
   Wideband ran XCreateAnalyzer() on the SAME WDSP channel over the top of the
   live one and leaked a fresh 256 kB input buffer with it, which is exactly the
   disease delete_receiver() had (see "Receiver lifecycle" in CLAUDE.md) on a
   window that is even easier to open and close repeatedly.  Measured over eight
   open/close cycles against tools/p2_emu.c: 75 MB of RSS per cycle before,
   33 MB after. */

WIDEBAND *create_wideband(int channel) {
  char name [80];
  char *value;
  gint x=-1;
  gint y=-1;
  gint width=512;
  gint height=180;

  if(the_wideband!=NULL) {
    WIDEBAND *w=the_wideband;
    log_info("create_wideband: reopening channel=%d\n",w->channel);
    /* The analyzer, its input buffer and the restored/adjusted dB scales all
       survive; only the widget tree was destroyed.  create_visual() re-arms the
       resize debounce, which re-runs wideband_init_analyzer() for the new
       geometry. */
    w->samples=0;
    create_visual(w);
    if(w->window!=NULL) {
      if(w->window_width>0 && w->window_height>0) {
        gtk_window_set_default_size(GTK_WINDOW(w->window),w->window_width,w->window_height);
      }
      gtk_widget_set_visible(w->window,TRUE);
    }
    w->update_timer_id=g_timeout_add(1000/w->fps,update_timer_cb,(gpointer)w);
    return w;
  }

  WIDEBAND *w=g_new0(WIDEBAND,1);
  the_wideband=w;

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
      // Kept on the struct too: it is what a reopen and wideband_save_state()
      // fall back to once the widget is gone.
      w->window_width=width;
      w->window_height=height;
      gtk_window_set_default_size(GTK_WINDOW(w->window),width,height);
    }
    (void)x; (void)y;
    gtk_widget_set_visible(w->window,TRUE);
  }

log_info("create_widband: update_timer: %d\n",1000/w->fps);
  w->update_timer_id=g_timeout_add(1000/w->fps,update_timer_cb,(gpointer)w);
  return w;
}

/* ===========================================================================
   MACHPSDR_WIDEBAND=<n> -- open the wideband window n times, closing it in
   between, then quit; n=0 opens it once and leaves it up.
   MACHPSDR_WIDEBAND_TEST=1 -- additionally resize the window, drive the pointer
   handlers and assert the scale arithmetic while each window is open.

   Why this exists.  The wideband window is reachable only by clicking "Add
   Wideband", exactly as a second receiver is reachable only by clicking "Add
   Receiver" (hence MACHPSDR_RX_CHURN), and it draws nothing at all without a
   wideband ADC feed -- which no radio here has and no recording can replay.
   Between the two, this file, wideband_panadapter.c and wideband_waterfall.c
   had never executed.  tools/p2_emu.c supplies the feed; this supplies the
   click, so the pair can be run under a sanitiser:

     ./p2_emu --pace 8 --wb-tone 24000 &
     HOME=$(mktemp -d) MACHPSDR_WIDEBAND=3 MACHPSDR_WIDEBAND_TEST=1 \
       ./machpsdr --open Angelia

   It calls what the button's handler calls and nothing else.  The one thing it
   cannot mirror is the handler greying "Add Wideband" out afterwards
   (add_wideband_b is private to radio.c), which is decoration -- delete_wideband
   re-sensitises it either way.

   ITS CALL SITE IS ONE LINE at the end of create_radio() in radio.c, beside the
   other env-driven hooks:

       wideband_test_init(r);

   there and not here because that is where MACHPSDR_RX_CHURN and
   MACHPSDR_DIVERSITY are armed, and because add_wideband() lives in that file.
   =========================================================================== */

/* radio.c.  radio.h does not declare it because the toolbar button is its only
   caller; the hook below is the second, and must not become a different way in. */
extern int add_wideband(void *data);

static int    wb_test_cycles=0;      /* open/close cycles still to run */
static int    wb_test_done=0;
static int    wb_test_step=0;
static gboolean wb_test_scale=FALSE; /* MACHPSDR_WIDEBAND_TEST */
static int    wb_test_fails=0;
static int    wb_test_checks=0;

static void wb_expect(gboolean ok,const char *what) {
  wb_test_checks++;
  if(ok) {
    log_info("wideband-test: ok   %s\n",what);
  } else {
    wb_test_fails++;
    log_error("wideband-test: FAIL %s\n",what);
  }
}

/* Every write to a dB pair has to leave it inside the range with a span the
   draw and the colour map can divide by; this is the one invariant asserted
   after every single operation below rather than at the end. */
static gboolean wb_pair_sane(gint low,gint high) {
  return low>=WB_DB_MIN && high<=WB_DB_MAX && (high-low)>=WB_DB_MIN_SPAN;
}

/* The screen row a sample of `s` dBm is drawn on, as wb_pana_build() computes
   it.  Used to assert the drag really is 1:1 under the pointer. */
static double wb_row_of(gint low,gint high,int height,double s) {
  return (double)(high-s)*(double)height/(double)(high-low);
}

static void wb_selftest_scale(void) {
  WIDEBAND t;
  memset(&t,0,sizeof(t));
  t.panadapter_height=400;
  t.waterfall_height=200;

  /* --- the dBm strip: top half moves the top of the scale, bottom half the
     bottom, 5 dB a notch, scroll up = down the dB axis (receiver_scroll_cb). */
  t.panadapter_high=0; t.panadapter_low=-140;
  wb_scale_scroll(&t,FALSE,-1,20,10);
  wb_expect(t.panadapter_high==-5 && t.panadapter_low==-140,"strip top half, scroll up: high -5 dB");
  wb_scale_scroll(&t,FALSE,+1,20,10);
  wb_expect(t.panadapter_high==0 && t.panadapter_low==-140,"strip top half, scroll down: high back");
  wb_scale_scroll(&t,FALSE,-1,20,390);
  wb_expect(t.panadapter_low==-145 && t.panadapter_high==0,"strip bottom half, scroll up: low -5 dB");
  wb_scale_scroll(&t,FALSE,-3,20,390);
  wb_expect(t.panadapter_low==-160,"a 3-notch flick moves 3x5 dB");

  /* --- off the strip: the whole window slides, span preserved exactly.  The
     direction follows the strip's (receiver_scroll_cb's): scrolling up takes
     the dB numbers DOWN, which moves the trace up the screen. */
  t.panadapter_high=-20; t.panadapter_low=-160;
  int span=t.panadapter_high-t.panadapter_low;
  wb_scale_scroll(&t,FALSE,-2,200,200);
  wb_expect(t.panadapter_high==-30 && t.panadapter_low==-170,"off-strip scroll up slides the window down the dB axis");
  wb_expect((t.panadapter_high-t.panadapter_low)==span,"off-strip scroll preserves the span");
  wb_scale_scroll(&t,FALSE,+2,200,200);
  wb_expect(t.panadapter_high==-20 && t.panadapter_low==-160,"off-strip scroll down slides it back");

  /* --- the rails clip the slide instead of squashing the span. */
  t.panadapter_high=WB_DB_MAX; t.panadapter_low=WB_DB_MIN;
  wb_scale_scroll(&t,FALSE,-4,200,200);
  wb_expect(t.panadapter_high==WB_DB_MAX && t.panadapter_low==WB_DB_MIN,"full-range slide is refused, not clipped one end");
  t.panadapter_high=-10; t.panadapter_low=-30;
  wb_scale_scroll(&t,FALSE,+8,200,200);   /* asks for 40 dB, 30 to the ceiling */
  wb_expect(t.panadapter_high==WB_DB_MAX && (t.panadapter_high-t.panadapter_low)==20,"slide stops at the ceiling with its span intact");
  t.panadapter_high=-170; t.panadapter_low=-190;
  wb_scale_scroll(&t,FALSE,-8,200,200);   /* asks for 40 dB, 10 to the floor */
  wb_expect(t.panadapter_low==WB_DB_MIN && (t.panadapter_high-t.panadapter_low)==20,"slide stops at the floor with its span intact");

  /* --- the clamp, driven past both ends from the strip. */
  t.panadapter_high=0; t.panadapter_low=-140;
  for(int i=0;i<80;i++) {
    wb_scale_scroll(&t,FALSE,-1,20,10);          /* drive high down onto low */
    if(!wb_pair_sane(t.panadapter_low,t.panadapter_high)) break;
  }
  wb_expect(wb_pair_sane(t.panadapter_low,t.panadapter_high),"high driven onto low keeps a 10 dB span in range");
  t.panadapter_high=0; t.panadapter_low=-140;
  for(int i=0;i<80;i++) {
    wb_scale_scroll(&t,FALSE,+1,20,390);         /* drive low up onto high */
    if(!wb_pair_sane(t.panadapter_low,t.panadapter_high)) break;
  }
  wb_expect(wb_pair_sane(t.panadapter_low,t.panadapter_high),"low driven onto high keeps a 10 dB span in range");
  t.panadapter_high=0; t.panadapter_low=-140;
  for(int i=0;i<80;i++) {
    wb_scale_scroll(&t,FALSE,+1,20,10);          /* high up past the ceiling */
    wb_scale_scroll(&t,FALSE,-1,20,390);         /* low down past the floor */
    if(!wb_pair_sane(t.panadapter_low,t.panadapter_high)) break;
  }
  wb_expect(wb_pair_sane(t.panadapter_low,t.panadapter_high),"both ends driven past the rails stay in range");

  /* --- a zero-height surface must not be scrolled at all (height is a divisor
     in the drag, and half of it decides which end the strip moves). */
  t.panadapter_height=0;
  t.panadapter_high=0; t.panadapter_low=-140;
  wb_expect(!wb_scale_scroll(&t,FALSE,-1,20,10),"scroll on an unallocated panadapter is a no-op");
  t.panadapter_height=400;

  /* --- the waterfall is inert while it tracks the noise floor itself. */
  t.waterfall_automatic=TRUE;
  t.waterfall_high=0; t.waterfall_low=-140;
  wb_expect(!wb_scale_scroll(&t,TRUE,-1,20,10),"waterfall scroll is inert under Waterfall Automatic");
  wb_expect(t.waterfall_high==0 && t.waterfall_low==-140,"...and changed nothing");
  wb_expect(!wb_scale_drag(&t,TRUE,120),"waterfall drag is inert under Waterfall Automatic");
  t.waterfall_automatic=FALSE;
  t.last_y=100;
  wb_expect(wb_scale_scroll(&t,TRUE,-1,20,10),"waterfall scroll works with Automatic off");
  wb_expect(t.waterfall_high==-5,"...on the top half, moving the top of the scale");

  /* --- the drag is 1:1: the sample under the pointer stays under the pointer. */
  t.panadapter_high=-20; t.panadapter_low=-160; t.panadapter_height=200;
  t.last_y=100;
  double sample=-20-(100.0*140.0/200.0);                  /* the dBm at row 100 */
  wb_expect(wb_scale_drag(&t,FALSE,150),"a 50 px drag moves the panadapter scale");
  double row=wb_row_of(t.panadapter_low,t.panadapter_high,200,sample);
  wb_expect(fabs(row-150.0)<1.5,"...and the sample under the pointer follows it 1:1");
  wb_expect(t.last_y==150,"...and the drag origin advances with it");
  wb_expect(wb_pair_sane(t.panadapter_low,t.panadapter_high),"...leaving the pair in range");

  /* A drag too small to be worth a whole dB leaves last_y alone, so a slow drag
     accumulates instead of rounding to nothing every motion event. */
  t.panadapter_high=-20; t.panadapter_low=-160; t.panadapter_height=2000;
  t.last_y=100;
  wb_expect(!wb_scale_drag(&t,FALSE,103),"a sub-dB drag step changes nothing");
  wb_expect(t.last_y==100,"...and does not consume the movement");
  wb_expect(wb_scale_drag(&t,FALSE,108),"...which then accumulates into a step");

  /* --- double-click resets one surface and leaves the other, and never touches
     the Waterfall Automatic flag. */
  t.panadapter_high=-33; t.panadapter_low=-77;
  t.waterfall_high=-44; t.waterfall_low=-88; t.waterfall_automatic=FALSE;
  wb_scale_reset(&t,FALSE);
  wb_expect(t.panadapter_high==WB_PANADAPTER_HIGH_DEFAULT && t.panadapter_low==WB_PANADAPTER_LOW_DEFAULT,"double-click on the panadapter restores its default scale");
  wb_expect(t.waterfall_high==-44 && t.waterfall_low==-88,"...and leaves the waterfall alone");
  wb_scale_reset(&t,TRUE);
  wb_expect(t.waterfall_high==WB_WATERFALL_HIGH_DEFAULT && t.waterfall_low==WB_WATERFALL_LOW_DEFAULT,"double-click on the waterfall restores its default scale");
  wb_expect(t.waterfall_automatic==FALSE,"...without turning Waterfall Automatic back on");
}

/* wideband_save_state() was dead code until recently -- nothing called it, so no
   wideband setting had ever been persisted.  Round-trip every field it writes
   through the real property store, and check the clamp catches a hostile file. */
static void wb_selftest_props(WIDEBAND *live) {
  WIDEBAND t;
  gint keep_pl=live->panadapter_low,keep_ph=live->panadapter_high;
  gint keep_wl=live->waterfall_low,keep_wh=live->waterfall_high;
  gboolean keep_auto=live->waterfall_automatic;
  gint keep_ww=live->window_width,keep_wh2=live->window_height;

  live->panadapter_low=-123; live->panadapter_high=-13;
  live->waterfall_low=-111;  live->waterfall_high=-41;
  live->waterfall_automatic=FALSE;
  wideband_save_state(live);

  memset(&t,0,sizeof(t));
  t.panadapter_low=1; t.panadapter_high=2; t.waterfall_low=3; t.waterfall_high=4;
  t.waterfall_automatic=TRUE;
  wideband_restore_state(&t);
  wb_expect(t.panadapter_low==-123 && t.panadapter_high==-13,"panadapter scale survives save/restore");
  wb_expect(t.waterfall_low==-111 && t.waterfall_high==-41,"waterfall scale survives save/restore");
  wb_expect(t.waterfall_automatic==FALSE,"Waterfall Automatic survives save/restore");
  {
    char *v=getProperty("wideband.width");
    char *h=getProperty("wideband.height");
    wb_expect(v!=NULL && h!=NULL && atoi(v)>0 && atoi(h)>0,"window geometry is written even with the window closed");
  }

  /* A hand-edited or truncated props must not produce a divisor of zero. */
  setProperty("wideband.panadapter_low","40");
  setProperty("wideband.panadapter_high","-500");
  setProperty("wideband.waterfall_low","0");
  setProperty("wideband.waterfall_high","0");
  memset(&t,0,sizeof(t));
  wideband_restore_state(&t);
  wb_expect(wb_pair_sane(t.panadapter_low,t.panadapter_high),"an absurd panadapter pair in props is clamped");
  wb_expect(wb_pair_sane(t.waterfall_low,t.waterfall_high),"a zero-span waterfall pair in props is clamped");

  live->panadapter_low=keep_pl; live->panadapter_high=keep_ph;
  live->waterfall_low=keep_wl;  live->waterfall_high=keep_wh;
  live->waterfall_automatic=keep_auto;
  live->window_width=keep_ww;   live->window_height=keep_wh2;
  wideband_save_state(live);
}

/* Drive the REAL event controllers on the REAL widgets, so the adapter half of
   each handler runs too: which surface the event arrived on, scroll_notches(),
   the stashed cursor.  GTK4 has no public event-injection API, so the two
   gestures that need a button state -- the drag and the double-click -- cannot
   be reached this way; their arithmetic is what wb_selftest_scale() covers. */
static GtkEventController *wb_find_controller(GtkWidget *widget,GType type) {
  if(widget==NULL) return NULL;
  GListModel *m=gtk_widget_observe_controllers(widget);
  GtkEventController *found=NULL;
  guint n=g_list_model_get_n_items(m);
  for(guint i=0;i<n;i++) {
    GtkEventController *c=g_list_model_get_item(m,i);
    if(found==NULL && G_TYPE_CHECK_INSTANCE_TYPE(c,type)) found=c;
    else g_object_unref(c);
  }
  g_object_unref(m);
  return found;
}

static void wb_drive_controllers(WIDEBAND *w) {
  GtkEventController *pm=wb_find_controller(w->panadapter,GTK_TYPE_EVENT_CONTROLLER_MOTION);
  GtkEventController *ps=wb_find_controller(w->panadapter,GTK_TYPE_EVENT_CONTROLLER_SCROLL);
  GtkEventController *wm=wb_find_controller(w->waterfall,GTK_TYPE_EVENT_CONTROLLER_MOTION);
  GtkEventController *ws=wb_find_controller(w->waterfall,GTK_TYPE_EVENT_CONTROLLER_SCROLL);
  gboolean handled=FALSE;

  wb_expect(pm!=NULL && ps!=NULL && wm!=NULL && ws!=NULL,"both surfaces carry a motion and a scroll controller");
  if(pm==NULL||ps==NULL||wm==NULL||ws==NULL) goto out;

  /* motion over the panadapter: the readout position is claimed here and only
     here (the waterfall's motion updates the position but not cursor_valid). */
  g_signal_emit_by_name(pm,"motion",(double)(w->panadapter_width/2),(double)(w->panadapter_height/2));
  wb_expect(w->cursor_valid && w->cursor_x==w->panadapter_width/2,"motion over the panadapter claims the readout");
  g_signal_emit_by_name(wm,"motion",7.0,9.0);
  wb_expect(!w->cursor_valid && w->cursor_x==7,"motion over the waterfall moves the position without claiming the readout");
  g_signal_emit_by_name(pm,"motion",20.0,3.0);              /* onto the dBm strip */

  /* scroll over the dBm strip, through the real controller and the real
     scroll_notches() (a synthesised event reads as one mouse-wheel detent). */
  {
    gint before=w->panadapter_high;
    g_signal_emit_by_name(ps,"scroll",0.0,-1.0,&handled);
    wb_expect(w->panadapter_high==before-WB_DB_STEP,"real scroll controller over the strip moves the top of the scale");
    g_signal_emit_by_name(ps,"scroll",0.0,1.0,&handled);
    wb_expect(w->panadapter_high==before,"...and back");
  }
  /* scroll away from the strip slides the window */
  g_signal_emit_by_name(pm,"motion",(double)(w->panadapter_width/2),(double)(w->panadapter_height/2));
  {
    gint lo=w->panadapter_low,hi=w->panadapter_high;
    g_signal_emit_by_name(ps,"scroll",0.0,-1.0,&handled);
    wb_expect(w->panadapter_low==lo-WB_DB_STEP && w->panadapter_high==hi-WB_DB_STEP,"real scroll controller off the strip slides the whole window");
    g_signal_emit_by_name(ps,"scroll",0.0,1.0,&handled);
  }
  /* the waterfall's own controller, with Automatic on, must change nothing */
  {
    gboolean keep=w->waterfall_automatic;
    gint lo=w->waterfall_low,hi=w->waterfall_high;
    w->waterfall_automatic=TRUE;
    g_signal_emit_by_name(wm,"motion",20.0,3.0);
    g_signal_emit_by_name(ws,"scroll",0.0,-1.0,&handled);
    wb_expect(w->waterfall_low==lo && w->waterfall_high==hi,"real scroll controller on the waterfall is inert under Automatic");
    w->waterfall_automatic=keep;
  }
  /* leaving drops the readout */
  g_signal_emit_by_name(pm,"leave");
  wb_expect(!w->cursor_valid,"leaving the panadapter drops the readout");

out:
  if(pm) g_object_unref(pm);
  if(ps) g_object_unref(ps);
  if(wm) g_object_unref(wm);
  if(ws) g_object_unref(ws);
}

/* The waterfall row loop, driven through every disagreement between the pixbuf
   and the analyzer buffer.

   Those two are sized by INDEPENDENT 250 ms debounce timers, so the state exists
   -- but it cannot be produced to order: every resize re-arms both timers within
   the same allocation pass, and reaching update_wideband_waterfall() while only
   one of them has fired needs the 100 ms display timer to be dispatched between
   the two in the same main-loop round.  Measured over a resize storm, it is not:
   the sampled geometry agreed every time.  So the pair is CONSTRUCTED here and
   the real function is called on it.  The state is synthetic; the code walking
   it, the pixbuf it writes into and AddressSanitizer's verdict are not.  With
   the two bounds in wideband_waterfall.c removed this reports a heap overflow on
   the first case, which is what makes a clean run mean anything. */
static void wb_selftest_waterfall_bounds(WIDEBAND *live) {
  static const struct { int pixels; int pbw; int pbh; const char *what; } cases[]={
    /* First, because it is the one the bounds were added for: a window widened
       from 400 to 900 with the waterfall pane dragged down to a couple of rows.
       3 bytes per column for w->pixels columns is 2700 bytes into a 2400-byte
       pixbuf -- off the end of the heap block, not merely off the end of row 0. */
    { 900, 400,  2,"a short pane with the analyzer buffer wider than the pixbuf" },
    { 560,1024,300,"pixbuf wider than the analyzer buffer (window grown, only the waterfall timer fired)" },
    { 1024,560,300,"pixbuf narrower than the analyzer buffer (window shrunk)" },
    { 1,   800,200,"a one-pixel analyzer buffer under a full-width pixbuf" },
    { 800,   1,200,"a one-column pixbuf (the width-2 divisor)" },
    { 800,   2,200,"a two-column pixbuf (the width-2 divisor is zero)" },
    { 800, 300,  1,"a one-row pixbuf" },
    { 0,   400,200,"no analyzer buffer at all" },
  };
  for(unsigned c=0;c<sizeof(cases)/sizeof(cases[0]);c++) {
    WIDEBAND t;
    memset(&t,0,sizeof(t));
    t.waterfall=live->waterfall;          /* borrowed: only queue_draw touches it */
    t.pixels=cases[c].pixels;
    t.waterfall_low=-140; t.waterfall_high=0;
    t.waterfall_automatic=TRUE;           /* also runs the averaging divisor */
    t.waterfall_width=cases[c].pbw;
    t.waterfall_height=cases[c].pbh;
    if(t.pixels>0) {
      t.pixel_samples=g_new0(float,t.pixels*2);
      for(int i=0;i<t.pixels*2;i++) t.pixel_samples[i]=-100.0f+(float)(i%40);
    }
    t.waterfall_pixbuf=gdk_pixbuf_new(GDK_COLORSPACE_RGB,FALSE,8,cases[c].pbw,cases[c].pbh);
    memset(gdk_pixbuf_get_pixels(t.waterfall_pixbuf),0,
           (size_t)gdk_pixbuf_get_rowstride(t.waterfall_pixbuf)*(size_t)cases[c].pbh);
    update_wideband_waterfall(&t);
    wb_expect(wb_pair_sane(t.waterfall_low,t.waterfall_high) || t.waterfall_low<=t.waterfall_high,
              cases[c].what);
    g_object_unref(t.waterfall_pixbuf);
    if(t.pixel_samples) g_free(t.pixel_samples);
  }
}

/* What the sweep actually drew.  A tone is measured, not eyeballed: with
   p2_emu --wb-tone the peak's position as a FRACTION of the span is the only
   thing an emulator can vouch for (nothing here knows the real board's wideband
   sample rate), and that fraction is exactly what a mirrored or half-span axis
   would get wrong. */
static void wb_measure(WIDEBAND *w) {
  if(w->pixel_samples==NULL || w->pixels<=0) {
    log_error("wideband-test: no analyzer pixels yet -- nothing was drawn\n");
    wb_test_fails++;
    return;
  }
  float *s=w->pixel_samples+w->pixels;      /* the trace half, see wb_pana_build */
  int n=w->pixels;
  int best=1; double sum=0.0;
  for(int i=1;i<n-1;i++) {                  /* ends hold the -200 dBm markers */
    sum+=s[i];
    if(s[i]>s[best]) best=i;
  }
  double mean=sum/(double)(n-2);
  int guard=n/50+2;
  int second=-1;
  for(int i=1;i<n-1;i++) {
    if(i>best-guard && i<best+guard) continue;
    if(second<0 || s[i]>s[second]) second=i;
  }
  log_info("wideband-test: %d pixels, %.0f Hz/pixel, peak %.1f dBm at bin %d "
           "(%.4f of span = %.3f MHz), next peak %.1f dBm at %.4f, mean %.1f dBm\n",
           n,w->hz_per_pixel,(double)s[best],best,(double)best/(double)n,
           (double)best*w->hz_per_pixel/1.0e6,
           second>=0?(double)s[second]:0.0,second>=0?(double)second/(double)n:0.0,mean);
  log_info("wideband-test: waterfall low=%d high=%d (automatic=%d), "
           "panadapter low=%d high=%d\n",
           w->waterfall_low,w->waterfall_high,w->waterfall_automatic,
           w->panadapter_low,w->panadapter_high);
  wb_expect(s[best]>mean+3.0,"the sweep drew something above its own mean");
}

/* The resize path.  The panadapter's analyzer buffer and the waterfall's pixbuf
   are resized by two INDEPENDENT 250 ms debounce timers, and the window between
   a resize and those timers is where both files index one by the other.  A
   window resize re-arms BOTH timers; moving the paned divider ~150 ms later
   re-arms only the waterfall's, which is what leaves the pixbuf a different
   width from w->pixels for a couple of hundred ms with the 100 ms display timer
   firing straight through it. */
static void wb_resize(WIDEBAND *w,int width,int height) {
  if(w->window==NULL) return;
  gtk_window_set_default_size(GTK_WINDOW(w->window),width,height);
}

/* Log the four numbers the two files index each other by, whenever any of them
   moves.  Without this a clean sanitiser run proves nothing: it would be just as
   clean if the resizes never took effect and the analyzer buffer, the pixbuf and
   the two allocations had agreed the whole time. */
static void wb_log_geometry(WIDEBAND *w,const char *tag) {
  static int last[5]={-1,-1,-1,-1,-1};
  int pw=(w->panadapter!=NULL)?gtk_widget_get_width(w->panadapter):0;
  int ww=(w->waterfall!=NULL)?gtk_widget_get_width(w->waterfall):0;
  int pb=(w->waterfall_pixbuf!=NULL)?gdk_pixbuf_get_width(w->waterfall_pixbuf):0;
  int now[5]={pw,w->pixels,ww,pb,w->waterfall_height};
  if(memcmp(now,last,sizeof(now))==0) return;
  memcpy(last,now,sizeof(now));
  log_info("wideband-test: %s allocation=%dx? analyzer pixels=%d, waterfall alloc=%d "
           "pixbuf=%dx%d%s\n",tag,pw,w->pixels,ww,pb,w->waterfall_height,
           (pb>0 && pb!=w->pixels)?"   <-- pixbuf and analyzer disagree":"");
}

static void wb_move_divider(WIDEBAND *w,int pos) {
  if(w->panadapter==NULL) return;
  GtkWidget *paned=gtk_widget_get_parent(w->panadapter);
  if(paned!=NULL && GTK_IS_PANED(paned)) gtk_paned_set_position(GTK_PANED(paned),pos);
}

static gboolean wb_test_tick(gpointer data) {
  RADIO *r=(RADIO *)data;
  WIDEBAND *w=r->wideband;
  int step=wb_test_step++;

  if(w==NULL) {
    /* between cycles */
    if(wb_test_cycles<=0) {
      log_info("wideband-test: %d open/close cycles completed, %d checks, %d failures\n",
               wb_test_done,wb_test_checks,wb_test_fails);
      /* Leave through the main window's close-request, not g_application_quit():
         main_delete() is where radio_save_state() lives, so quitting the other
         way writes no props at all -- and whether the wideband settings reach
         the file is half of what this is here to check. */
      if(main_window!=NULL) gtk_window_close(GTK_WINDOW(main_window));
      else g_application_quit(g_application_get_default());
      return FALSE;
    }
    if(step<2) return TRUE;
    wb_test_step=0;
    add_wideband(r);
    log_info("wideband-test: cycle %d: window opened\n",wb_test_done+1);
    return TRUE;
  }

  if(!wb_test_scale) {
    /* Open-only mode: hold the window for a few seconds so the feed runs. */
    if(step<40) return TRUE;
  } else {
    switch(step) {
      case  2: wb_resize(w,1024,600); return TRUE;
      case  5: wb_move_divider(w,120); return TRUE;
      case 14: wb_resize(w,560,300);   return TRUE;    /* shrink: pixbuf wider than w->pixels */
      case 17: wb_move_divider(w,80);  return TRUE;
      case 26: wb_resize(w,900,520);   return TRUE;
      case 29: wb_move_divider(w,220); return TRUE;
      case 38: wb_resize(w,512,240);   return TRUE;    /* shrink again */
      case 41: wb_move_divider(w,60);  return TRUE;
      /* A gap after the last resize before measuring: every resize re-runs
         SetAnalyzer, and the sweep needs a buffer_size of samples (16384, i.e.
         170 ms at 96 kS/s) plus a display tick to have anything in it.  Measured
         at step 50 it read a flat zero buffer about half the time. */
      case 60: wb_measure(w);          return TRUE;
      case 62: wb_drive_controllers(w);return TRUE;
      case 64:
        if(wb_test_done==0) {           /* the pure arithmetic, once is enough */
          wb_selftest_scale();
          wb_selftest_waterfall_bounds(w);
          wb_selftest_props(w);
        }
        return TRUE;
      default:
        if(step<68) { wb_log_geometry(w,"resize:"); return TRUE; }
        break;
    }
  }

  /* On the way out of the last window, leave a scale no default would produce,
     so what lands in the props file afterwards can be checked against it: the
     settings are saved at application exit, long after this window is gone. */
  if(wb_test_cycles<=1) {
    wb_scale_scroll(w,FALSE,-3,200,200);          /* off-strip slide, -15 dB */
    w->waterfall_automatic=FALSE;
    wb_scale_scroll(w,TRUE,+2,20,3);              /* waterfall top, +10 dB */
    log_info("wideband-test: leaving panadapter %d..%d waterfall %d..%d "
             "automatic=%d for the props file\n",
             w->panadapter_low,w->panadapter_high,
             w->waterfall_low,w->waterfall_high,w->waterfall_automatic);
  }

  /* Close it exactly as the operator does: the window manager's close button
     raises "close-request", which is what window_delete() is connected to. */
  gtk_window_close(GTK_WINDOW(w->window));
  wb_test_done++;
  wb_test_cycles--;
  wb_test_step=0;
  log_info("wideband-test: cycle %d closed, %d to go\n",wb_test_done,wb_test_cycles);
  return TRUE;
}

void wideband_test_init(RADIO *r) {
  const char *e=g_getenv("MACHPSDR_WIDEBAND");
  if(e==NULL || *e=='\0') return;
  wb_test_scale=(g_getenv("MACHPSDR_WIDEBAND_TEST")!=NULL);
  wb_test_cycles=atoi(e);
  if(wb_test_cycles<=0) {
    /* 0: open it once and leave it up -- the interactive case. */
    log_info("wideband-test: opening the wideband window and leaving it open\n");
    add_wideband(r);
    return;
  }
  log_info("wideband-test: %d open/close cycles requested%s\n",
           wb_test_cycles,wb_test_scale?" (with resize + pointer checks)":"");
  /* 50 ms: shorter than the 250 ms resize debounce, which is the whole point --
     the steps above have to land inside it. */
  g_timeout_add(50,wb_test_tick,(gpointer)r);
}
