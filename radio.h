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

#ifndef RADIO_H
#define RADIO_H

#define MAX_RECEIVERS 8
#define MAX_DIVERSITY_MIXERS 2

#define MAX_BUFFER_SIZE 2048

#define TRANSMITTER_CHANNEL 8
#define WIDEBAND_CHANNEL 9
#define BPSK_CHANNEL 10

#include "diversity_mixer.h"
#include "hl2.h"
#include "band.h"

enum {
  ANAN_10=0,
  ANAN_10E,
  ANAN_100,
  ANAN_100D,
  ANAN_200D,
  ANAN_7000DLE,
  ANAN_8000DLE,
  ATLAS,
  HERMES,
  HERMES_2,
  ANGELIA,
  ORION_1,
  ORION_2,
  HERMES_LITE,
  HERMES_LITE_2
#ifdef SOAPYSDR
  ,SOAPY_DEVICE
#endif
};

enum {
  NONE=0,
  ALEX,
  APOLLO,
  N2ADR,
  HL2_MRF101
};

enum {
  KEYER_STRAIGHT=0,
  KEYER_MODE_A,
  KEYER_MODE_B
};


enum {
  REGION_OTHER=0,
  REGION_UK
};

#ifdef CWDAEMON
enum {
  CWGEN_RADIO = 0,
  CWGEN_PC
};
#endif

typedef struct _radio {
  DISCOVERED *discovered;
  gboolean can_transmit;
  gint model;
  gint sample_rate;
  gint buffer_size;
  gint receivers;
  gint diversity_mixers;
  RECEIVER *receiver[MAX_RECEIVERS];
  TRANSMITTER *transmitter;
  HERMESLITE2 *hl2;
  RECEIVER *active_receiver;
  // FT8 (Phase 2): station identity + TX state. Persisted in radio_save_state.
  char station_call[16];   // operator callsign, used for FT8 TX/QSO
  char station_grid[8];    // 4/6-char Maidenhead locator
  gint ft8_tx_offset;      // FT8 audio TX offset (Hz)
  gboolean ft8_tx_even;    // TX in even (TRUE) vs odd (FALSE) UTC 15 s slots
  GtkWidget *ft8_panel;    // embedded FT8 QSO panel (NULL unless open in DIGU)
  gboolean ft8_panel_open; // user toggled the big FT8 panel on (in place of RX2)
  gboolean ft8_log_udp;    // also send completed QSOs to a logger over the network
  char ft8_log_udp_host[64]; // UDP destination host/IP (WSJT-X-compatible logger)
  gint ft8_log_udp_port;   // UDP destination port (WSJT-X default 2237)
  gboolean ft8_pskr;       // report received FT8 spots to pskreporter.info
  DIVMIXER *divmixer[MAX_DIVERSITY_MIXERS+1];
  gint alex_rx_antenna;
  gint alex_tx_antenna;
  gdouble meter_calibration;
  gdouble panadapter_calibration;
  gdouble swr_alarm_value;
  gint temperature_alarm_value;
  int ppm_correction_value;

  gint cw_keyer_sidetone_frequency;
  gint cw_keyer_sidetone_volume;
  gboolean cw_keys_reversed;
  gint cw_keyer_speed;
  gint cw_keyer_mode;
  gint cw_keyer_weight;
  gint cw_keyer_spacing;
  gint cw_keyer_internal;
  gint cw_keyer_ptt_delay;
  gint cw_keyer_hang_time;
  gboolean cw_breakin;
  gboolean cwdaemon;
  
  gdouble protocol1_timer;
  gdouble hang_time_ctr;
  
  #ifdef CWDAEMON
  gboolean cw_generation_mode;
  
  gint cwdaemon_running;
  gint cwd_port;
  gboolean cwd_sidetone;

  struct sockaddr_in request_addr;
  socklen_t request_addrlen;

  struct sockaddr_in reply_addr;
  socklen_t reply_addrlen;
  int socket_descriptor;  
  #endif


  gboolean local_microphone;
  gchar *microphone_name;

  struct SoundIoDevice *input_device;
  struct SoundIoInStream *input_stream;
  struct SoundIoRingBuffer *ring_buffer;
  gboolean input_started;
  GMutex ring_buffer_mutex;
  GCond ring_buffer_cond;
  // device-rate -> 48 kHz resampler state for the local microphone input, used
  // when the capture device does not run at 48 kHz (e.g. a Bluetooth headset
  // mic at 16 kHz). mic_resample_rate==0 means "native 48 kHz, no resampling".
  gint    mic_resample_rate;
  gdouble mic_resample_phase;
  gfloat  mic_resample_prev;
  
  GMutex delete_rx_mutex;  
  
#ifndef __APPLE__
  pa_simple* microphone_stream;
  snd_pcm_t *record_handle;
#endif

  gint local_microphone_buffer_size;
  gint local_microphone_buffer_offset;
  float *local_microphone_buffer;
  GMutex local_microphone_mutex;

  gboolean mic_boost;
  gboolean mic_ptt_enabled;
  gboolean mic_bias_enabled;
  gboolean mic_ptt_tip_bias_ring;

  gboolean mic_linein;
  gint linein_gain;

  gboolean mox;
  gboolean tune;
  gboolean vox;
  gboolean ptt;
  gboolean dot;
  gboolean dash;
  gboolean local_ptt;

  gboolean adc_overload;
  gboolean IO1;
  gboolean IO2;
  gboolean IO3;
  gint AIN3;
  gint AIN4;
  gint AIN6;

  gboolean pll_locked;
  gint supply_volts;

  gint mercury_software_version;
  gint penelope_software_version;
  gint ozy_software_version;

  gboolean atlas_mic_source;
  gint atlas_clock_source_10mhz;
  gboolean atlas_clock_source_128mhz;

  gboolean classE;

  guchar oc_tune;

  gint OCfull_tune_time;
  gint OCmemory_tune_time;
  long long tune_timeout;

  gint filter_board;

  gboolean display_filled;

  GtkWidget *visual;
  GtkWidget *rx_container;
  gboolean rx_paned_restore;
  GtkWidget *bottom_bar;
  GtkWidget *mox_button;
  GtkWidget *vox_button;
  GtkWidget *tune_button;
  GtkWidget *mic_level;
  cairo_surface_t *mic_level_surface;
  GtkWidget *mic_gain;
  GtkWidget *drive_level;

  GtkWidget *att10_button;
  GtkWidget *att20_button;
  GtkWidget *att10_check;   // ADC-0 "Att10" check button on the Radio config page
  GtkWidget *att20_check;   // ADC-0 "Att20" check button on the Radio config page
  char att10_label[32];
  char att20_label[32];

  int theme;   // main-window skin index (see css.c); 0 = Charcoal (default)

  GtkWidget *dialog;
  
  GtkWidget *txmeter_info;
  cairo_surface_t *meter_surface;

  GtkWidget *oc_rx_b[BANDS * 8];
  GtkWidget *oc_tx_b[BANDS * 8];  
  gulong *oc_tx_signal_id;
  gulong *oc_rx_signal_id;
  
  ADC adc[2];
  DAC dac[2];

  WIDEBAND *wideband;

  gboolean vox_enabled;
  double vox_threshold;
  double vox_hang;
  double vox_peak;
  guint vox_timeout;

  int region;

  gboolean iqswap;

  gint which_audio;
  gint which_audio_backend;

  gboolean midi_enabled;
  char midi_filename[128];
  
  gboolean qos_flag;

  GtkWidget *rds_label[3];   // bottom-bar decoder readout (3-line RDS in WFM)
  GtkWidget *rds_title;      // bottom-bar decoder module title ("RDS" in WFM, else "Decode")
  GtkWidget *ft8_label;      // bottom-bar FT8 readout (up to 6 decode lines in DIGU)
  GtkWidget *ft8_expand_btn; // bottom-bar toggle: open/close the big FT8 panel (DIGU only)

  int wfm_deemphasis;        // broadcast-FM de-emphasis: 0 = 50 us, 1 = 75 us
  int rds_rbds;              // RDS PTY names: 0 = RDS (Europe), 1 = RBDS (N. America)

} RADIO;

extern int radio_restart(void *data);
extern int radio_start(void *data);
extern gboolean isTransmitting(RADIO *r);
extern RADIO *create_radio(DISCOVERED *d);
extern void delete_receiver(RECEIVER *rx);
extern void delete_diversity_mixer(DIVMIXER *dmix);
extern void frequency_changed(RECEIVER *rx);
extern void radio_set_wfm_deemphasis(RADIO *radio, int sel);
extern int add_receiver(void *data, gboolean show_rx);
extern int add_diversity_mixer(void *data, RECEIVER *rx_visual, RECEIVER *rx_hidden); // TODO - does this *need* a prototype?
extern void add_receivers(RADIO *r);
extern void radio_rebuild_rx_stack(RADIO *r);
extern void add_transmitter(RADIO *r);
extern void radio_save_state(RADIO *radio);
extern void radio_restore_state(RADIO *radio);
extern void radio_refresh_skin(RADIO *radio);
extern void delete_wideband(WIDEBAND *w);
extern void vox_changed(RADIO *r);
extern void ptt_changed(RADIO *r);
extern gboolean radio_button_press_event_cb(GtkWidget *widget, GdkEventButton *event, gpointer data);
extern void set_mox(RADIO *r,gboolean state);
#ifdef FT8
extern void radio_ft8_panel_sync(RADIO *r);
#endif
extern void set_tune(RADIO *r,gboolean state);
extern void radio_change_region(RADIO *r);
extern void radio_change_audio(RADIO *r,int selected);
extern void radio_change_audio_backend(RADIO *r,int selected);
extern void update_radio(RADIO *radio);
#ifdef CWDAEMON
extern void radio_change_cwgeneration(RADIO *r);
#endif
#endif
