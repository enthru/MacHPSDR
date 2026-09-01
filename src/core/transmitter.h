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

#ifndef TRANSMITTER_H
#define TRANSMITTER_H

#define NUM_TX_METERS 4 // PA current, PA temp, HL2 temp, PWR out
#define PEAK_DETECT_BUF_SIZE 40

#include "ringbuffer.h"
#include "peak_detect.h"
#include "tx_info_meter.h"
#include "puresignal.h"

#define CTCSS_FREQUENCIES 38
extern double ctcss_frequencies[CTCSS_FREQUENCIES];

/* Fixed TX-monitor span (Hz), centred on the carrier. The analyzer is zoomed
   onto this window (see transmitter_init_analyzer) and the panadapter draws it,
   so the view is the same width on every protocol regardless of iq_output_rate.
   This tap is post-DSP TX I/Q — a clean modulated signal — so the span is only
   for context around the carrier, not for viewing PA linearity. */
#define TX_MONITOR_SPAN_HZ 24000.0

/* Ceiling for the SoapySDR TX DAC rate (Hz).

   tx->iq_output_rate used to be radio->sample_rate outright, back when that
   number was the device's ADC rate for every SoapySDR radio.  It is not any
   more: for the devices driven at the receiver's own span (soapy_hw_rate_for)
   it is the WIDEST SPAN OFFERED — 9 600 000 on a HackRF — and a transmitter
   that follows it opens its WDSP channel to interpolate the 48 kHz mic by 200,
   sizes output_samples at 204 800 and allocates ~6.5 MB of TX buffers for a
   signal a few kHz wide.  Nothing on the transmit side wants the rate the
   PANADAPTER is drawing at.

   1 920 000 because that is the rate this path was actually keyed at on real
   hardware (HackRF ran 2 000 000, rounded down to a multiple of mic_dsp_rate)
   before the widest-span change moved the number under it. */
#define TX_SOAPY_MAX_IQ_RATE 1920000

typedef struct _transmitter {
  gint channel; // WDSP channel

  gint dac;
  GMutex queue_mutex;  

  gint alex_antenna;
  gdouble mic_gain;
  gint linein_gain;

  gint exciter_power;
  gint alex_forward_power;
  gint alex_reverse_power;

  gint alc_meter;
  gdouble fwd;
  gdouble exciter;
  gdouble rev;
  gdouble alc;
  gdouble lvlr_gain;   // leveler gain-reduction meter (dB)
  gdouble cfc_gain;    // CFC gain-reduction meter (dB)
  gdouble comp_pk;     // compressor peak meter (dB)
  gdouble swr;
  PEAKDETECTOR *fwd_peak_buf;    
  gdouble fwd_peak;
  

  GtkWidget *window;
  gint window_width;
  gint window_height;

  RECEIVER *rx;
  
  TXMETER *tx_info_meter[NUM_TX_METERS+1];
  GtkWidget *tx_info;
  gint num_tx_info_meters;

  gdouble drive;
  gdouble tune_percent;
  gboolean tune_use_drive;
  gint attenuation;

  gboolean eer;
  gint eer_amiq;
  gdouble eer_pgain;
  gdouble eer_pdelay;
  gdouble eer_mgain;
  gdouble eer_mdelay;
  gboolean eer_enable_delays;
  gint eer_pwm_min;
  gint eer_pwm_max;

  gboolean use_rx_filter;
  gint filter_low;
  gint filter_high;

  gint actual_filter_low;
  gint actual_filter_high;

  gint fft_size;
  gboolean low_latency;

  gint buffer_size;
  gint mic_samples;
  gdouble *mic_input_buffer;
  gdouble *iq_output_buffer;
  // Protocol 1 packet scheduling
  guint packet_counter;

  
  RINGBUFFERL *p1_ringbuf;  
  

  
  #ifdef CWDAEMON
  // PC generated cw
  glong cw_waveform_idx;
  RINGBUFFERL *cw_iq_delay_buf;  
  gboolean last_key_state;
  #endif
  
  gfloat *inI, *inQ, *outMI, *outMQ; // for EER
  gint mic_sample_rate;
  gint mic_dsp_rate;
  gint iq_output_rate;
  gint p1_packet_size;
  gint output_samples;
  
  gdouble temperature;

  gboolean pre_emphasize;
  gboolean enable_equalizer;
  gint equalizer[11];
  gboolean leveler;
  gboolean cessb;

  gboolean cfc_run;       // Continuous Frequency Compressor on/off
  gboolean cfc_peq_run;   // CFC post-equalizer on/off
  gdouble  cfc_precomp;   // pre-compression (dB)
  gdouble  cfc_prepeq;    // pre-post-eq (dB)
  gdouble  cfc_freq[5];   // band centre freqs (Hz)
  gdouble  cfc_comp[5];   // per-band compression (dB)
  gdouble  cfc_eq[5];     // per-band post-eq (dB)

  gboolean phrot_run;      // phase rotator on/off
  gdouble  phrot_corner;   // phase-rotator corner freq (Hz)
  gint     phrot_nstages;  // phase-rotator all-pass stages

  gboolean ctcss_enabled;
  gint ctcss;
  gdouble tone_level;

  gdouble deviation;
  gboolean compressor;
  gdouble compressor_level;

  gdouble am_carrier_level;

  gint fps;
  gint pixels;
  gfloat *pixel_samples;
  gint update_timer_id;

  gint panadapter_low;
  gint panadapter_high;

  gint panadapter_width;
  gint panadapter_height;
  GtkWidget *panadapter;
  cairo_surface_t *panadapter_surface;

  GtkWidget *dialog;

  PSIGNAL *puresignal;
  gboolean puresignal_enabled;
  gboolean ps_twotone;
  gboolean ps_feedback;
  gboolean ps_auto;
  gboolean ps_single;
#ifdef PURESIGNAL
  RECEIVER *rx_puresignal_txfbk;
  RECEIVER *rx_puresignal_rxfbk;
#endif 
  GtkWidget *ps;
  cairo_surface_t *ps_surface;
  gint ps_timer_id;

  GtkWidget *local_audio_b;
  GtkWidget *audio_choice_b;
  GtkWidget *tx_control_b;
  gulong audio_choice_signal_id;
  gulong local_audio_signal_id;
  gulong tx_control_signal_id;

  GtkWidget *local_microphone_b;
  GtkWidget *microphone_choice_b;
  gulong microphone_choice_signal_id;
  gulong local_microphone_signal_id;

  gboolean xit_enabled;
  gint64 xit;
  gint64 xit_step;

  gboolean updated;

} TRANSMITTER;

extern TRANSMITTER *create_transmitter(int channel);
extern void transmitter_init_analyzer(TRANSMITTER *tx);
extern void transmitter_save_state(TRANSMITTER *tx);
extern void transmitter_restore_state(TRANSMITTER *tx);
extern void add_mic_sample(TRANSMITTER *tx,float sample);
extern void add_ps_iq_samples(TRANSMITTER *tx, double i_sample_tx,double q_sample_tx, double i_sample_rx, double q_sample_rx);
extern void transmitter_set_filter(TRANSMITTER *tx,int low,int high);
extern void transmitter_set_pre_emphasize(TRANSMITTER *tx,int state);
extern void transmitter_set_mode(TRANSMITTER *tx,int mode);
extern void transmitter_set_deviation(TRANSMITTER *tx);
extern void transmitter_set_am_carrier_level(TRANSMITTER *tx);
extern void transmitter_fps_changed(TRANSMITTER *tx);
extern void transmitter_set_ctcss(TRANSMITTER *tx,gboolean run,int f);

extern void transmitter_set_ps(TRANSMITTER *tx,gboolean state);
extern void transmitter_set_twotone(TRANSMITTER *tx,gboolean state);
extern void transmitter_set_ps_sample_rate(TRANSMITTER *tx,int rate);

extern int transmitter_get_mode(TRANSMITTER *tx);
extern long long transmitter_get_frequency(TRANSMITTER *tx);
extern void full_tx_buffer(TRANSMITTER *tx, gboolean force_send);

extern void transmitter_enable_eer(TRANSMITTER *tx,gboolean state);
extern void transmitter_set_eer_mode_amiq(TRANSMITTER *tx,gboolean state);
extern void transmitter_enable_eer_delays(TRANSMITTER *tx,gboolean state);
extern void transmitter_set_eer_pgain(TRANSMITTER *tx,gdouble gain);
extern void transmitter_set_eer_pdelay(TRANSMITTER *tx,gdouble delay);
extern void transmitter_set_eer_mgain(TRANSMITTER *tx,gdouble gain);
extern void transmitter_set_eer_mdelay(TRANSMITTER *tx,gdouble delay);

#endif
