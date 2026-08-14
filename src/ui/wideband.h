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

#ifndef WIDEBAND_H
#define WIDEBAND_H

/* The wideband display is a fixed sweep of the whole ADC span — there is no
   tuning cursor and no receiver to retune, so the span is a constant rather
   than a per-receiver sample rate. */
#define WIDEBAND_SPAN_HZ 61440000

/* The packet geometry the general registers ask a board for, and the geometry
   process_wideband_data() reads back — one number, so the two halves cannot
   drift.  A board programs its wideband IP with exactly what byte 24..28 carry
   and sends `samples*2 + 4` bytes per datagram (4-byte sequence, then 16-bit
   real ADC samples, MSB first); zero in those fields is a request for NO data,
   not a request for the defaults. */
#define WIDEBAND_SAMPLES_PER_PACKET 512
#define WIDEBAND_SAMPLE_BITS        16

typedef struct _wideband {
  gint channel; // WDSP channel
  gint adc;
  gint buffer_size;
  gint fft_size;

  guint32 sequence;
  gdouble *input_buffer;
  gfloat *pixel_samples;

  gint update_timer_id;

  gint samples;
  gint pixels;
  gint fps;


  GtkWidget *window;
  GtkWidget *table;

  gint window_x;
  gint window_y;
  gint window_width;
  gint window_height;

  GtkWidget *panadapter;
  gint panadapter_width;
  gint panadapter_height;
  gint panadapter_resize_width;
  gint panadapter_resize_height;
  guint panadapter_resize_timer;
  cairo_surface_t *panadapter_surface;

  gint panadapter_low;
  gint panadapter_high;

  GtkWidget *waterfall;
  gint waterfall_width;
  gint waterfall_height;
  gint waterfall_resize_width;
  gint waterfall_resize_height;
  guint waterfall_resize_timer;
  GdkPixbuf *waterfall_pixbuf;

  gint waterfall_low;
  gint waterfall_high;
  gboolean waterfall_automatic;
  gint64 waterfall_frequency;
  gint waterfall_sample_rate;
  
  gdouble hz_per_pixel;

  gboolean has_moved;
  gint last_x;
  gint last_y;
  /* Pointer position stashed by the motion handler: the scroll signal carries
     no coordinates (same reason RECEIVER keeps cursor_x/cursor_y).  cursor_valid
     is TRUE only while the pointer is over the panadapter, which is where the
     readout is drawn — the waterfall's motion handler updates the position for
     its own scroll handling but does not claim the readout. */
  gint cursor_x;
  gint cursor_y;
  gboolean cursor_valid;
  /* TRUE only between a press WE received and its release: a button held down
     by another process must not read as a drag on the spectrum (see
     RECEIVER.pointer_pressed). */
  gboolean pointer_pressed;

  GtkWidget *dialog;

} WIDEBAND;

extern WIDEBAND *create_wideband(int channel);
extern void wideband_init_analyzer(WIDEBAND *w);
extern void add_wideband_sample(WIDEBAND *w,double sample);
/* GTK4: pointer input via gesture/scroll controllers (see wideband.c). */
extern void wideband_pressed_cb(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data);
extern void wideband_released_cb(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data);
extern void wideband_motion_cb(GtkEventControllerMotion *controller, double x, double y, gpointer data);
extern void wideband_leave_cb(GtkEventControllerMotion *controller, gpointer data);
extern gboolean wideband_scroll_cb(GtkEventControllerScroll *controller, double dx, double dy, gpointer data);
extern void wideband_save_state(WIDEBAND *w);
extern void reset_wideband_buffer_index(WIDEBAND *w);
/* MACHPSDR_WIDEBAND=<n> / MACHPSDR_WIDEBAND_TEST -- headless open/close of the
   wideband window (see the block comment at the end of wideband.c).  Called from
   create_radio() alongside rx_churn_init(); a no-op unless the variable is set. */
struct _radio;
extern void wideband_test_init(struct _radio *r);

#endif
