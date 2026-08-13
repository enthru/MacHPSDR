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
#include <string.h>

#include "wideband.h"
#include "wideband_waterfall.h"
#include "gpu_image.h"

static int colorLowR=0; // black
static int colorLowG=0;
static int colorLowB=0;

static int colorHighR=255; // yellow
static int colorHighG=255;
static int colorHighB=0;

static gboolean resize_timeout(void *data) {
  WIDEBAND *w=(WIDEBAND *)data;

  w->waterfall_width=w->waterfall_resize_width;
  w->waterfall_height=w->waterfall_resize_height;

  if(w->waterfall_pixbuf) {
    g_object_unref((gpointer)w->waterfall_pixbuf);
    w->waterfall_pixbuf=NULL;
  }

  if(w->waterfall!=NULL) {
    w->waterfall_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w->waterfall_width, w->waterfall_height);
    guchar *pixels = gdk_pixbuf_get_pixels (w->waterfall_pixbuf);
    memset(pixels, 0, w->waterfall_width*w->waterfall_height*3);
  }
  w->waterfall_frequency=0;
  w->waterfall_sample_rate=0;
  w->waterfall_resize_timer=-1;
  return FALSE;
}

// GTK4: GtkDrawingArea "resize" signal replaces GTK3 "configure-event".
static void waterfall_resize_cb(GtkDrawingArea *area,int width,int height,gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  if(width!=w->waterfall_width || height!=w->waterfall_height) {
    w->waterfall_resize_width=width;
    w->waterfall_resize_height=height;
    if(w->waterfall_resize_timer!=-1) {
      g_source_remove(w->waterfall_resize_timer);
    }
    w->waterfall_resize_timer=g_timeout_add(250,resize_timeout,(gpointer)w);
  }
}


// GPU path: GpuImage pulls the current pixbuf here at snapshot time.
static GdkPixbuf *waterfall_source(gpointer data) {
  WIDEBAND *w=(WIDEBAND *)data;
  return w->waterfall_pixbuf;
}

GtkWidget *create_wideband_waterfall(WIDEBAND *w) {
  GtkWidget *waterfall;

  w->waterfall_width=0;
  w->waterfall_height=0;
  w->waterfall_resize_timer=-1;
  w->waterfall_pixbuf=NULL;

  waterfall = gpu_image_new(waterfall_source,(gpointer)w);
  //gtk_widget_set_size_request (waterfall, w->width, w->height/3);
  g_signal_connect(waterfall,"resize",G_CALLBACK (waterfall_resize_cb),(gpointer)w);

  // GTK4: pointer input via event controllers, shared with the panadapter
  // (wideband.c decides what a gesture means from the widget it arrived on).
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),0);
  g_signal_connect(click,"pressed",G_CALLBACK(wideband_pressed_cb),w);
  g_signal_connect(click,"released",G_CALLBACK(wideband_released_cb),w);
  gtk_widget_add_controller(waterfall,GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion=gtk_event_controller_motion_new();
  g_signal_connect(motion,"motion",G_CALLBACK(wideband_motion_cb),w);
  g_signal_connect(motion,"leave",G_CALLBACK(wideband_leave_cb),w);
  gtk_widget_add_controller(waterfall,motion);

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(wideband_scroll_cb),w);
  gtk_widget_add_controller(waterfall,scroll);

  return waterfall;
}

void update_wideband_waterfall(WIDEBAND *w) {
  int i;

  float *samples;
  if(w->waterfall_pixbuf && w->waterfall_height>1) {
    guchar *pixels = gdk_pixbuf_get_pixels (w->waterfall_pixbuf);

    int width=gdk_pixbuf_get_width(w->waterfall_pixbuf);
    int height=gdk_pixbuf_get_height(w->waterfall_pixbuf);
    int rowstride=gdk_pixbuf_get_rowstride(w->waterfall_pixbuf);

    //memset(pixels, 0, width*height*3);

    memmove(&pixels[rowstride],pixels,(height-1)*rowstride);

    float sample;
    int average=0;
    guchar *p;
    p=pixels;
    samples=w->pixel_samples;
    // The pixbuf and pixel_samples are resized by two INDEPENDENT 250 ms
    // debounce timers (this file's resize_timeout and wideband_panadapter.c's,
    // which is the only place w->pixels changes), so between a resize and those
    // timers the two disagree in BOTH directions.  The loop ran to w->pixels
    // while writing three bytes per column into a pixbuf `width` wide: with
    // w->pixels > width that walks off the end of the first row and, on a short
    // pane, off the end of the pixbuf's heap block entirely -- the same class of
    // bug AddressSanitizer found in rx_panadapter.c under tools/p2_emu.c (see
    // pan_sample_width() there).  Bound the loop by the PIXBUF, as waterfall.c
    // does, and clamp the sample index to what the analyzer buffer actually
    // holds: repeating the last valid column for one frame of a resize is
    // invisible, and it fills every pixel of the row rather than leaving the
    // right-hand end holding the previous line.
    int last_sample=w->pixels-1;
    // Nothing to read from at all (no analyzer buffer yet -- w->pixels is 0
    // until the first resize_timeout): leave the picture as it is rather than
    // dereferencing NULL.
    if(samples==NULL || last_sample<0) { gtk_widget_queue_draw(w->waterfall); return; }

    for(i=0;i<width;i++) {
            int si=i+w->pixels;                 // second half = the trace bins
            if(i>last_sample) si=last_sample+w->pixels;
            sample=samples[si];
            // Interior columns only: the last bin holds the -200 dBm end marker
            // wideband_panadapter.c writes, and the condition was `||` (always
            // true), so the marker dragged the automatic waterfall_low down
            // every frame. `&&` gives the mean over exactly width-2 samples,
            // which is the divisor used below.
            if(i>0 && i<(width-1)) {
                average+=(int)sample;
            }
            if(sample<(float)w->waterfall_low) {
                *p++=colorLowR;
                *p++=colorLowG;
                *p++=colorLowB;
            } else if(sample>(float)w->waterfall_high) {
                *p++=colorHighR;
                *p++=colorHighG;
                *p++=colorHighB;
            } else {
                float range=(float)w->waterfall_high-(float)w->waterfall_low;
                float offset=sample-(float)w->waterfall_low;
                float percent=offset/range;
                if(percent<(2.0f/9.0f)) {
                    float local_percent = percent / (2.0f/9.0f);
                    *p++ = (int)((1.0f-local_percent)*colorLowR);
                    *p++ = (int)((1.0f-local_percent)*colorLowG);
                    *p++ = (int)(colorLowB + local_percent*(255-colorLowB));
                } else if(percent<(3.0f/9.0f)) {
                    float local_percent = (percent - 2.0f/9.0f) / (1.0f/9.0f);
                    *p++ = 0;
                    *p++ = (int)(local_percent*255);
                    *p++ = (char)255;
                } else if(percent<(4.0f/9.0f)) {
                     float local_percent = (percent - 3.0f/9.0f) / (1.0f/9.0f);
                     *p++ = 0;
                     *p++ = (char)255;
                     *p++ = (int)((1.0f-local_percent)*255);
                } else if(percent<(5.0f/9.0f)) {
                     float local_percent = (percent - 4.0f/9.0f) / (1.0f/9.0f);
                     *p++ = (int)(local_percent*255);
                     *p++ = (char)255;
                     *p++ = 0;
                } else if(percent<(7.0f/9.0f)) {
                     float local_percent = (percent - 5.0f/9.0f) / (2.0f/9.0f);
                     *p++ = (char)255;
                     *p++ = (int)((1.0f-local_percent)*255);
                     *p++ = 0;
                } else if(percent<(8.0f/9.0f)) {
                     float local_percent = (percent - 7.0f/9.0f) / (1.0f/9.0f);
                     *p++ = (char)255;
                     *p++ = 0;
                     *p++ = (int)(local_percent*255);
                } else {
                     float local_percent = (percent - 8.0f/9.0f) / (1.0f/9.0f);
                     *p++ = (int)((0.75f + 0.25f*(1.0f-local_percent))*255.0f);
                     *p++ = (int)(local_percent*255.0f*0.5f);
                     *p++ = (char)255;
                }
            }
    }


    // width>2 or the divisor below is zero (integer division by zero, i.e. a
    // crash, not a wrong colour) -- a pane can be dragged that narrow.
    if(w->waterfall_automatic && width>2) {
      w->waterfall_low=average/(width-2);
      w->waterfall_high=w->waterfall_low+50;
    }

    gtk_widget_queue_draw (w->waterfall);
  }
}
