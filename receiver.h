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

#ifndef RECEIVER_H
#define RECEIVER_H


#include <soundio/soundio.h>
#ifndef __APPLE__
#include <pulse/simple.h>
#include <alsa/asoundlib.h>
#endif

typedef enum {SPLIT_OFF, SPLIT_ON, SPLIT_SAT, SPLIT_RSAT} split_type;

// --- WDSP spectrum-analyzer input-ring safety -------------------------------
// XCreateAnalyzer allocates its I/Q input ring at (max_size * dSAMP_BUFF_MULT).
// dSAMP_BUFF_MULT is 2 (wdsp/comm.h), so the ring holds WDSP_ANALYZER_MAX_SIZE*2
// complex samples.  Spectrum0() writes each transfer block with a plain loop and
// only wraps its write index when it reaches the ring end, so it *requires the
// transfer block to divide the ring exactly* (see the "REQUIRES buff_size IS A
// SUB-MULTIPLE OF SIZE OF INPUT SAMPLE BUFFS!" contract in wdsp/analyzer.c).  If
// it does not, the last block before the wrap runs off the end of the buffer and
// corrupts the heap (intermittent crash, layout-dependent).
//
// The wideband RX I/O block (5120 = 2^10*5) and the widest TX block (40960 =
// 2^13*5 at the 1920 kHz span) do NOT divide the 2^19 ring, so we feed the
// analyzer in ANALYZER_FEED_BLOCK-sized sub-blocks instead of one full buffer.
// 1024 = gcd(5120, 2^19) divides the ring and divides every RX/TX block we feed
// (all are 1024*N), so it is the largest universally safe transfer size.
#define WDSP_ANALYZER_MAX_SIZE 262144
#define ANALYZER_FEED_BLOCK    1024
_Static_assert((WDSP_ANALYZER_MAX_SIZE * 2) % ANALYZER_FEED_BLOCK == 0,
               "ANALYZER_FEED_BLOCK must divide the WDSP analyzer input ring");

typedef struct _receiver {

  gint channel; // WDSP channel

  gint adc;

  gint sample_rate;
  gint buffer_size;
  gint dsp_rate;
  gint output_rate;

  gint fft_size;
  gboolean low_latency;

  gdouble *buffer;

  guint32 iq_sequence;
  gdouble *iq_input_buffer;
  gdouble *diviq_input_buffer;

  guint32 audio_sequence;
  gdouble *audio_output_buffer;
  gint audio_buffer_size;
  guchar *audio_buffer;
  gfloat *pixel_samples;

  gint update_timer_id;

  gint samples;
  gint output_samples;
  gint pixels;
  gint fps;
  gdouble display_average_time;

  gboolean ctun;
  gint64 ctun_frequency;
  gint64 ctun_offset;
  gint64 ctun_min;
  gint64 ctun_max;

  gint64 frequency_min;
  gint64 frequency_max;

  gboolean qo100_beacon;

  gboolean entering_frequency;
  gint64 entered_frequency;

  gint64 frequency_a;
  gint64 lo_a;
  gint64 error_a;
  gint band_a;
  gint mode_a;
  gint filter_a;
  gint64 offset;
  gint bandstack;

  gint64 lo_tx;
  gint64 error_tx;
  gboolean tx_track_rx;

  gint64 frequency_b;
  gint64 lo_b;
  gint64 error_b;
  gint band_b;
  gint mode_b;
  gint filter_b;

  split_type split;
  gboolean mute_while_transmitting;
  gboolean duplex;

  gint filter_low_a;
  gint filter_high_a;
  gint deviation;
  gboolean squelch_enable;
  gdouble squelch;

  gint filter_low_b;
  gint filter_high_b;

  gint agc;
  gdouble agc_gain;
  gdouble agc_slope;
  gdouble agc_hang_threshold;
  // AGC hang/threshold levels for the panadapter AGC line. Read from WDSP under
  // rx->mutex in the display timer and cached here so the (unlocked) panadapter
  // render never calls WDSP concurrently with the RX thread.
  gdouble agc_hang_level;
  gdouble agc_thresh_level;

  gboolean rit_enabled;
  gint64 rit;
  gint64 rit_step;

  gint64 step;

  gboolean locked;

  gdouble volume;
  gboolean mute;

  gboolean nb;
  gboolean nb2;
  gboolean nr;
  gboolean nr2;
  gboolean anf;
  gboolean snb;

  gint nr_agc;
  gint nr2_gain_method;
  gint nr2_npe_method;
  gint nr2_ae;


  GtkWidget *window;
  GtkWidget *table;
  GtkWidget *vfo;
  cairo_surface_t *vfo_surface;
  GtkWidget *meter;
  cairo_surface_t *meter_surface;
  GtkWidget *radio_info;
  cairo_surface_t *radio_info_surface;

  gint vfo_a_x;
  gint vfo_a_digits;
  gint vfo_a_width;

  gint vfo_b_x;
  gint vfo_b_digits;
  gint vfo_b_width;

  GtkWidget *bookmark_dialog;

  gint smeter;
  double meter_db;
  int    meter_smoothing;   // S-meter needle ballistics, 0 = off (instant) .. 100 = max
  double meter_needle_db;   // smoothed value driving the analog needle (ballistics)
  int    meter_needle_init; // 0 until the needle has been seeded to the first reading

  gint window_x;
  gint window_y;
  gint window_width;
  gint window_height;

  GtkWidget *vpaned;
  gint paned_position;
  double paned_percent;
  gint paned_restore_tries;   // bounded retries in restore_paned_position_cb
  gboolean show_panadapter;   // FALSE = spectroscope hidden, waterfall full-height

  GtkWidget *panadapter;
  gint panadapter_width;
  gint panadapter_height;
  gint panadapter_resize_width;
  gint panadapter_resize_height;
  guint panadapter_resize_timer;
  cairo_surface_t *panadapter_surface;

  gint panadapter_low;
  gint panadapter_high;
  gint panadapter_step;
  gboolean panadapter_filled;
  gboolean panadapter_gradient;
  gboolean panadapter_agc_line;
  gint panadapter_single_color;

  GtkWidget *waterfall;
  GtkWidget *wf_hpaned;      // horizontal split of the waterfall row (main | FT8)
  GtkWidget *ft8_waterfall;  // FT8 band waterfall, 1/3 right (NULL unless DIGU)
  gint waterfall_width;
  gint waterfall_height;
  gint waterfall_resize_width;
  gint waterfall_resize_height;
  guint waterfall_resize_timer;
  GdkPixbuf *waterfall_pixbuf;

  gint waterfall_low;
  gint waterfall_high;
  gboolean waterfall_automatic;
  gboolean waterfall_ft8_marker;
  gint64 waterfall_frequency;
  gint waterfall_sample_rate;

  gdouble hz_per_pixel;

  gboolean is_panning;
  gboolean has_moved;
  gint last_x;
  // GTK4: the scroll controller's "scroll" signal carries no pointer position,
  // so the motion controller stashes the latest cursor coords here for it.
  gint cursor_x;
  gint cursor_y;

  gint mixed_audio;
  short mixed_left_audio;
  short mixed_right_audio;

  gboolean remote_audio;
  gboolean local_audio;
  gint local_audio_buffer_size;
  gint local_audio_buffer_offset;
  void *local_audio_buffer;
  GMutex local_audio_mutex;
  gint local_audio_latency;
  gint audio_channels;

  gchar *audio_name;
  int output_index;
  gboolean mute_when_not_active;

  struct SoundIoDevice *output_device;
  struct SoundIoOutStream *output_stream;
  struct SoundIoRingBuffer *ring_buffer;
  gboolean output_started;
  // Fractional read phase for the 48 kHz -> output-device-rate resampler,
  // used when the output device does not accept 48 kHz (e.g. a Bluetooth
  // headset locked to 44.1 kHz). 0 when the device runs natively at 48 kHz.
  gdouble audio_resample_phase;

#ifndef __APPLE__
  pa_simple* playstream;
  snd_pcm_t *playback_handle;
  snd_pcm_format_t local_audio_format;
#endif

  GtkWidget *toolbar;
  GtkWidget *dialog;
  GtkWidget *filter_frame;
  GtkWidget *filter_grid;
  GtkWidget *mode_grid;
  GtkWidget *band_grid;

  gint zoom;
  gint pan;

  gboolean enable_equalizer;
  gint equalizer[4];

  GMutex mutex;

  gboolean bpsk_enable;
  void *bpsk;

  gboolean subrx_enable;
  void *subrx;

  int resample_step;

  void *resampler;
  gdouble *resampled_buffer;
  gint resampled_buffer_size;

  GtkWidget *local_audio_b;
  GtkWidget *audio_choice_b;
  GtkWidget *tx_control_b;
  gulong audio_choice_signal_id;
  gulong local_audio_signal_id;
  gulong tx_control_signal_id;

  gboolean show_rx;
  gboolean diversity;
  gint diversity_hidden_rx;
  gint dmix_id;


  gint rigctl_port;
  gboolean rigctl_enable;
  gboolean cat_client_connected;

  GtkWidget *serial_port_entry;
  char rigctl_serial_port[80];
  gint rigctl_serial_baudrate;
  gboolean rigctl_serial_enable;


  gboolean rigctl_debug;
  void *rigctl;
  gboolean freetune;
  int waterfall_color_theme;

  /* freetune: last centre frequency actually pushed to the hardware LO, so we
     only retune the radio when the span centre moves (at the span edges), not
     on every in-span digital-shift step. */
  long long freetune_hw_frequency;

} RECEIVER;


enum {
  AUDIO_STEREO = 0,
  AUDIO_LEFT_ONLY = 1,
  AUDIO_RIGHT_ONLY = 2
};

extern RECEIVER *create_receiver(int channel,int sample_rate, gboolean show_rx);
extern void receiver_close(RECEIVER *rx);
extern void receiver_update_title(RECEIVER *rx);
extern void receiver_init_analyzer(RECEIVER *rx);
extern void receiver_apply_panadapter_visibility(RECEIVER *rx);
extern void add_iq_samples(RECEIVER *r,double left,double right);
extern void full_diviqrx_buffer(RECEIVER *rx);

// Feed `nsamples` complex I/Q samples (interleaved doubles, Q at [2i], I at
// [2i+1] as Spectrum0 expects) into WDSP's analyzer for `channel`, split into
// ANALYZER_FEED_BLOCK-sized chunks so the write never overruns the input ring.
// `nsamples` must be a multiple of ANALYZER_FEED_BLOCK; a remainder is dropped
// (and warned once) rather than risking an overrun.
extern void analyzer_feed(int channel, double *iq, int nsamples);

/* GTK4: RX panadapter pointer input comes from gesture/motion controllers
 * (see receiver.c create_visual). These are the controller signal handlers. */
extern void receiver_pressed_cb(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data);
extern void receiver_released_cb(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data);
extern void receiver_motion_cb(GtkEventControllerMotion *controller, double x, double y, gpointer data);
/* GTK4: driven by a GtkEventControllerKey on the main window (see main.c). */
extern gboolean receiver_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data);
extern void receiver_key_released(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data);
extern gboolean receiver_scroll_cb(GtkEventControllerScroll *controller, double dx, double dy, gpointer data);

extern void receiver_filter_changed(RECEIVER *rx,int filter);
extern void receiver_mode_changed(RECEIVER *rx,int mode);
#ifdef FT8
extern void receiver_ft8_waterfall_sync(RECEIVER *rx);
#endif
extern void receiver_band_changed(RECEIVER *rx,int band);
extern void receiver_xvtr_changed(RECEIVER *rx);
extern void set_filter(RECEIVER *rx,int low,int high);
extern void set_deviation(RECEIVER *rx);
extern void set_squelch(RECEIVER *rx);

extern void update_noise(RECEIVER *rx);

extern void receiver_save_state(RECEIVER *rx);
extern void receiver_change_sample_rate(RECEIVER *rx,int sample_rate);
extern void set_agc(RECEIVER *rx);
extern void calculate_display_average(RECEIVER *rx);
extern void receiver_fps_changed(RECEIVER *rx);
extern void receiver_refit_vpaned(RECEIVER *rx);
extern void receiver_change_zoom(RECEIVER *rx,int zoom);
extern void update_frequency(RECEIVER *rx);
extern void receiver_move(RECEIVER *rx,long long hz,gboolean round);
extern void receiver_move_b(RECEIVER *rx,long long hz,gboolean b_only,gboolean round);
extern void receiver_move_to(RECEIVER *rx,long long hz);
extern void receiver_set_volume(RECEIVER *rx);
extern void receiver_set_agc_gain(RECEIVER *rx);
extern void receiver_set_ctun(RECEIVER *rx);
extern void receiver_set_freetune(RECEIVER *rx, gboolean enable);
extern void set_band(RECEIVER *rx,int band,int entry);
#endif
