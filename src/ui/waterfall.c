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

#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "adc.h"
#include "dac.h"
#include "wideband.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "waterfall.h"
#include "waterfall_theme.h"
#include "gpu_image.h"
#include "rx_panadapter.h"
#include "main.h"

static int colorLowR=0; // black
static int colorLowG=0;
static int colorLowB=0;

static int colorHighR=255; // yellow
static int colorHighG=255;
static int colorHighB=0;

static gboolean resize_timeout(void *data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->waterfall_width=rx->waterfall_resize_width;
  rx->waterfall_height=rx->waterfall_resize_height;
  if(rx->waterfall_pixbuf) {
    g_object_unref((gpointer)rx->waterfall_pixbuf);
    rx->waterfall_pixbuf=NULL;
  }
  if(rx->waterfall!=NULL) {
    rx->waterfall_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, rx->waterfall_width, rx->waterfall_height);
    guchar *pixels = gdk_pixbuf_get_pixels (rx->waterfall_pixbuf);
    // Fill with the theme grey (~@BACKGROUND) not black, so the not-yet-scrolled
    // area after a resize matches the UI instead of showing a black band.
    memset(pixels, 23, rx->waterfall_width*rx->waterfall_height*3);
  }
  rx->waterfall_frequency=0;
  rx->waterfall_sample_rate=0;
  // When the panadapter (spectroscope) is hidden it no longer drives the
  // analyzer bin count (rx->pixels), so the waterfall takes over: it spans the
  // same horizontal extent, so mirror its width into panadapter_width/pixels
  // and re-init the analyzer. (When the panadapter is visible its own resize
  // owns this and we leave it alone.)
  if(!rx->show_panadapter && rx->waterfall_width>0) {
    g_mutex_lock(&rx->mutex);
    rx->panadapter_width=rx->waterfall_width;
    int max_zoom=16384/rx->panadapter_width;
    if(max_zoom<1) max_zoom=1;
    if(rx->zoom>max_zoom) rx->zoom=max_zoom;
    rx->pixels=rx->panadapter_width*rx->zoom;
    receiver_init_analyzer(rx);
    g_mutex_unlock(&rx->mutex);
  }
  rx->waterfall_resize_timer=-1;
  return FALSE;
}

// GTK4: GtkDrawingArea "resize" signal replaces GTK3 "configure-event".
static void waterfall_resize_cb(GtkDrawingArea *area,int width,int height,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(width!=rx->waterfall_width || height!=rx->waterfall_height) {
    rx->waterfall_resize_width=width;
    rx->waterfall_resize_height=height;
    if(rx->waterfall_resize_timer!=-1) {
      g_source_remove(rx->waterfall_resize_timer);
    }
    rx->waterfall_resize_timer=g_timeout_add(250,resize_timeout,(gpointer)rx);
  }
}

// GPU path: GpuImage pulls the current waterfall pixbuf here at snapshot time.
// The theme-grey background (~@BACKGROUND, not pure black) is painted by the
// widget itself (gpu_image_set_background below).
static GdkPixbuf *waterfall_source(gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  return rx->waterfall_pixbuf;
}

// Vector overlay drawn on top of the texture: the receive passband and
// centre-frequency cursor, using the same x-mapping as the panadapter so the
// marker lines up column-for-column.
static void waterfall_overlay_cb(cairo_t *cr,int cwidth,int cheight,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(rx->hz_per_pixel!=0.0) {
    double height=(double)cheight;

    // CW sidetone offset (cursor sits on the tone, matching the panadapter)
    double cw_offset=0.0;
    if(rx->mode_a==CWL) {
      cw_offset=+radio->cw_keyer_sidetone_frequency;
    } else if(rx->mode_a==CWU) {
      cw_offset=-radio->cw_keyer_sidetone_frequency;
    }

    double centre=((double)rx->pixels/2.0)-(double)rx->pan
                  +(rx->ctun_offset/rx->hz_per_pixel)
                  -(cw_offset/rx->hz_per_pixel);
    double filter_left=((double)rx->pixels/2.0)-(double)rx->pan
                       +(((double)rx->filter_low_a+rx->ctun_offset)/rx->hz_per_pixel);
    double filter_right=((double)rx->pixels/2.0)-(double)rx->pan
                        +(((double)rx->filter_high_a+rx->ctun_offset)/rx->hz_per_pixel);

    // passband band
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.30);
    cairo_rectangle(cr, filter_left, 0.0, filter_right-filter_left, height);
    cairo_fill(cr);

    // centre cursor
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
    cairo_move_to(cr, centre, 0.0);
    cairo_line_to(cr, centre, height);
    cairo_stroke(cr);

    // DX-cluster spots on the waterfall (top edge = newest), when the operator
    // routed them here: cluster_spots_on 1 (waterfall) or 2 (both).
    if(radio->cluster_enable && radio->cluster_spots_show &&
       (radio->cluster_spots_on==1 || radio->cluster_spots_on==2)) {
      receiver_draw_cluster_spots(cr, rx, cwidth);
    }
  }
}


GtkWidget *create_waterfall(RECEIVER *rx) {
  GtkWidget *waterfall;

  // Инициализация тем (один раз)
  static gboolean themes_initialized = FALSE;
  if(!themes_initialized) {
    init_waterfall_themes();
    themes_initialized = TRUE;
  }

  rx->waterfall_width=0;
  rx->waterfall_height=0;
  rx->waterfall_resize_timer=-1;
  rx->waterfall_pixbuf=NULL;

  waterfall = gpu_image_new(waterfall_source,(gpointer)rx);
  gpu_image_set_overlay(GPU_IMAGE(waterfall),waterfall_overlay_cb);
  gpu_image_set_background(GPU_IMAGE(waterfall),0.09f,0.09f,0.10f);
  g_signal_connect(waterfall,"resize",G_CALLBACK (waterfall_resize_cb),(gpointer)rx);

  // GTK4: pointer input via event controllers (button masks are gone).
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),0);
  g_signal_connect(click,"pressed",G_CALLBACK(receiver_pressed_cb),rx);
  g_signal_connect(click,"released",G_CALLBACK(receiver_released_cb),rx);
  gtk_widget_add_controller(waterfall,GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion=gtk_event_controller_motion_new();
  g_signal_connect(motion,"motion",G_CALLBACK(receiver_motion_cb),rx);
  gtk_widget_add_controller(waterfall,motion);

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(receiver_scroll_cb),rx);
  gtk_widget_add_controller(waterfall,scroll);

  return waterfall;
}

void update_waterfall(RECEIVER *rx) {
  int i;
  float *samples;

  if(rx->waterfall_pixbuf && rx->waterfall_height>1) {
    guchar *pixels = gdk_pixbuf_get_pixels (rx->waterfall_pixbuf);

    int width=gdk_pixbuf_get_width(rx->waterfall_pixbuf);
    int height=gdk_pixbuf_get_height(rx->waterfall_pixbuf);
    int rowstride=gdk_pixbuf_get_rowstride(rx->waterfall_pixbuf);

    if(rx->waterfall_frequency!=0 && (rx->sample_rate==rx->waterfall_sample_rate)) {
      if(rx->waterfall_frequency!=rx->frequency_a) {
        // scrolled or band change
        long long half=((long long)(rx->sample_rate/2))/(rx->zoom);
        if(rx->waterfall_frequency<(rx->frequency_a-half) || rx->waterfall_frequency>(rx->frequency_a+half)) {
          // outside of the range - blank waterfall
          memset(pixels, 0, width*height*3);
        } else {
          // rotate waterfall
          gint64 diff=rx->waterfall_frequency-rx->frequency_a;
          int rotate_pixels=(int)((double)diff/rx->hz_per_pixel);
          if(rotate_pixels<0) {
            memmove(pixels,&pixels[-rotate_pixels*3],((width*height)+rotate_pixels)*3);
            //now clear the right hand side
            for(i=0;i<height;i++) {
              memset(&pixels[((i*width)+(width+rotate_pixels))*3], 0, -rotate_pixels*3);
            }
          } else {
            memmove(&pixels[rotate_pixels*3],pixels,((width*height)-rotate_pixels)*3);
            //now clear the left hand side
            for(i=0;i<height;i++) {
              memset(&pixels[(i*width)*3], 0, rotate_pixels*3);
            }
          }
        }
      }
    } else {
      memset(pixels, 0, width*height*3);
    }

    rx->waterfall_frequency=rx->frequency_a;
    rx->waterfall_sample_rate=rx->sample_rate;

    memmove(&pixels[rowstride],pixels,(height-1)*rowstride);

    float sample;
    int average=0;
    guchar *p;
    p=pixels;
    samples=rx->pixel_samples;
    int offset=rx->pan;

    for(i=0;i<width;i++) {
        sample=samples[i+offset]+radio->adc[rx->adc].attenuation;
        // Exclude the two edge pixels from the auto-level average: the last bin
        // (i==width-1) holds the -200 dBm end marker the panadapter writes, and
        // the condition was `||` (always true), so the marker was dragging the
        // automatic waterfall_low down every frame. `&&` gives the intended
        // interior-only mean over exactly width-2 samples (the divisor below).
        if(i>0 && i<(width-1)) {
          average+=(int)sample;
        }

        // Нормализуем sample в диапазон 0-255
        int level;
        if(sample < (float)rx->waterfall_low) {
            level = 0;
        } else if(sample > (float)rx->waterfall_high) {
            level = 255;
        } else {
            float range = (float)rx->waterfall_high - (float)rx->waterfall_low;
            float offset_val = sample - (float)rx->waterfall_low;
            level = (int)((offset_val / range) * 255.0f);
            if(level < 0) level = 0;
            if(level > 255) level = 255;
        }

        // Получаем цвет из выбранной темы
        unsigned char r, g, b;
        get_waterfall_color(rx->waterfall_color_theme, level, &r, &g, &b);

        *p++ = r;
        *p++ = g;
        *p++ = b;
    }

    if(rx->waterfall_ft8_marker) {
        static int tim0=0;
        int tim=time(NULL);
        if(tim%15==0) {
          if(tim0==0) {
            p=pixels;
            for(i=0;i<width;i++) {
              *p++=(char)255;
              *p++=0;
              *p++=0;
            }
            tim0=1;
          }
        } else {
          tim0=0;
        }
    }

    if(rx->waterfall_automatic) {
      rx->waterfall_low=(average/(width-2))-14;
      rx->waterfall_high=rx->waterfall_low+80;
    }

    gtk_widget_queue_draw (rx->waterfall);
  }
}

void waterfall_set_theme(RECEIVER *rx, int theme) {
    if(theme >= 0 && theme < get_theme_count()) {
        rx->waterfall_color_theme = theme;
        if(rx->waterfall != NULL) {
            gtk_widget_queue_draw(rx->waterfall);
        }
    }
}
