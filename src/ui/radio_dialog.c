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
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "audio.h"
#include "settings_ui.h"
#include "receiver_dialog.h"
#include "fake_protocol.h"
//#include "rigctl.h"

static GtkWidget *filter_board_combo_box;
// Audio-related frames (Microphone / Audio backend / dBm calibration) used to
// crowd the Radio hardware page. They are now attached to their own grid,
// exposed via create_radio_audio_dialog() and composed onto the "Audio" tab
// (next to the recorder settings) in configure_dialog.c. Built during
// create_radio_dialog() — which must run first (it does: the Radio page is
// added before the Audio page).
static GtkWidget *radio_audio_page;
static GtkWidget *adc0_frame;
static GtkWidget *adc0_antenna_combo_box;
static GtkWidget *adc0_filters_combo_box;
static GtkWidget *adc0_hpf_combo_box;
static GtkWidget *adc0_lpf_combo_box;
static GtkWidget *duplex_b;
static GtkWidget *sat_combo;
static GtkWidget *mute_rx_b;
static GtkWidget *dither_b;
static GtkWidget *random_b;
static GtkWidget *preamp_b;
static GtkWidget *att10_b;
static GtkWidget *att20_b;
static GtkWidget *attenuation_label;
static GtkWidget *attenuation_b;
static GtkWidget *enable_attenuation_b;
static GtkWidget *disable_fpgaclk_b;
static GtkWidget *swr_alarm_b;
static GtkWidget *ppm_correction_b;
static GtkWidget *temperature_alarm_b;

static GtkWidget *adc1_frame;
static GtkWidget *adc1_antenna_combo_box;
static GtkWidget *adc1_filters_combo_box;
static GtkWidget *adc1_hpf_combo_box;

//static GtkWidget *rigctl_base;

#ifdef SOAPYSDR
static GtkWidget *dac0_frame;
static GtkWidget *dac0_antenna_combo_box;
#endif

static GtkWidget *audio_backend_combo_box;

// The ADC-0/ADC-1 combos are built per DISCOVERED DEVICE (only Hermes-2,
// Angelia, Orion and Orion-2 get an ADC-1 frame at all), while the sensitivity
// below is decided by the operator-selected MODEL — two independent things.  So
// any radio whose model implies a second ADC but whose device did not build one
// walked straight into gtk_widget_set_sensitive(NULL): two GTK-CRITICALs on the
// console every time the Configure dialog was opened, on ordinary hardware and
// on the I/Q Player alike.  Skipping a control that does not exist is the whole
// fix; there is nothing to grey out.
static void sui_set_sensitive(GtkWidget *w,gboolean on) {
  if(w!=NULL) gtk_widget_set_sensitive(w,on);
}

static void radio_dialog_update_controls(void) {
	log_info("%s: model=%d\n",__FUNCTION__,radio->model);
  switch(radio->model) {
    case ANAN_10:
    case ANAN_10E:
    case ANAN_100:
    case ANAN_100D:
    case ANAN_200D:
      radio->filter_board=ALEX;
      break;
    case ANAN_7000DLE:
    case ANAN_8000DLE:
      radio->filter_board=ALEX;
      break;
    case HERMES_LITE_2:
      break;
    case ATLAS:
      radio->filter_board=ALEX;
      break;
    case HERMES:
    case HERMES_2:
    case ANGELIA:
    case ORION_1:
    case ORION_2:
#ifdef SOAPYSDR
    case SOAPY_DEVICE:
#endif
      radio->filter_board=NONE;
      break;
  }

  switch(radio->model) {
    case ANAN_7000DLE:
    case ANAN_8000DLE:
      sui_set_sensitive(adc0_antenna_combo_box, TRUE);
      sui_set_sensitive(adc0_filters_combo_box, TRUE);
      if(radio->adc[0].filters==AUTOMATIC) {
        sui_set_sensitive(adc0_lpf_combo_box, FALSE);
        sui_set_sensitive(adc0_hpf_combo_box, FALSE);
      } else {
        sui_set_sensitive(adc0_hpf_combo_box, TRUE);
        sui_set_sensitive(adc0_lpf_combo_box, TRUE);
      }
      sui_set_sensitive(adc1_antenna_combo_box, TRUE);
      sui_set_sensitive(adc1_filters_combo_box, TRUE);
      if(radio->adc[1].filters==AUTOMATIC) {
        sui_set_sensitive(adc1_hpf_combo_box, FALSE);
      } else {
        sui_set_sensitive(adc1_hpf_combo_box, TRUE);
      }
      break;
    case ANAN_100:
    case ANAN_100D:
    case ANAN_200D:
      sui_set_sensitive(adc0_antenna_combo_box, TRUE);
      sui_set_sensitive(adc0_filters_combo_box, TRUE);
      if(radio->adc[0].filters==AUTOMATIC) {
        sui_set_sensitive(adc0_lpf_combo_box, FALSE);
        sui_set_sensitive(adc0_hpf_combo_box, FALSE);
      } else {
        sui_set_sensitive(adc0_hpf_combo_box, TRUE);
        sui_set_sensitive(adc0_lpf_combo_box, TRUE);
      }
      sui_set_sensitive(adc1_antenna_combo_box, FALSE);
      sui_set_sensitive(adc1_filters_combo_box, FALSE);
      sui_set_sensitive(adc0_lpf_combo_box, FALSE);
      sui_set_sensitive(adc0_hpf_combo_box, FALSE);
      break;
    case HERMES_LITE:
    case HERMES_LITE_2:
      break;
#ifdef SOAPYSDR
    case SOAPY_DEVICE:
      break;
#endif
    case ATLAS:
      sui_set_sensitive(adc0_antenna_combo_box, TRUE);
      sui_set_sensitive(adc0_filters_combo_box, TRUE);
      sui_set_sensitive(adc0_lpf_combo_box, TRUE);
      sui_set_sensitive(adc0_hpf_combo_box, TRUE);
      break;

    default:
      log_info("%s: defualt set_sensitive\n",__FUNCTION__);
      sui_set_sensitive(adc0_antenna_combo_box, FALSE);
      sui_set_sensitive(adc0_filters_combo_box, FALSE);
      sui_set_sensitive(adc0_hpf_combo_box, FALSE);
      sui_set_sensitive(adc0_lpf_combo_box, FALSE);

      sui_set_sensitive(adc1_antenna_combo_box, FALSE);
      sui_set_sensitive(adc1_filters_combo_box, FALSE);
      sui_set_sensitive(adc1_hpf_combo_box, FALSE);
      break;
  }

#ifdef SOAPYSDR
  if(radio->discovered->device!=DEVICE_SOAPYSDR) {
#endif
    gtk_drop_down_set_selected(GTK_DROP_DOWN(filter_board_combo_box),radio->filter_board);
#ifdef SOAPYSDR
  }
#endif
}

// Read the selected row's text from a GtkStringList-backed GtkDropDown.
static const char *dropdown_selected_text(GtkDropDown *dd) {
  GtkStringObject *o=GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(dd));
  return o ? gtk_string_object_get_string(o) : "";
}

static void model_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->model=gtk_drop_down_get_selected(widget);
  radio_dialog_update_controls();
}

static void sample_rate_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int rate;
  int i;

  rate=atoi(dropdown_selected_text(widget));

  switch(radio->discovered->protocol) {
    case PROTOCOL_1:
      protocol1_stop();
      break;
    case PROTOCOL_2:
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      break;
#endif
  }
  radio->sample_rate=rate;
  for(i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]!=NULL) {
      receiver_change_sample_rate(radio->receiver[i],rate);
    }
  }
  switch(radio->discovered->protocol) {
    case PROTOCOL_1:
      protocol1_set_mic_sample_rate(rate);
      break;
    case PROTOCOL_2:
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      break;
#endif
  }

#ifdef PURESIGNAL
  transmitter_set_ps_sample_rate(radio->transmitter, rate);
#endif

  g_idle_add(radio_restart,(void *)radio);
}

#ifdef SOAPYSDR
// SoapySDR (e.g. HackRF): the hardware/ADC stays at radio->sample_rate (a rate
// the device actually supports); this control sets only the PER-RECEIVER rate,
// i.e. the panadapter/waterfall span.  The receiver's resampler bridges the ADC
// rate down to it, so no hardware re-tuning and no full radio restart is needed.
static void soapy_rx_rate_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int rate=atoi(dropdown_selected_text(widget));
  if(rate>radio->sample_rate) rate=radio->sample_rate;
  for(int i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]!=NULL) {
      receiver_change_sample_rate(radio->receiver[i],rate);
    }
  }
}
#endif

static void filter_board_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->filter_board=(int)gtk_drop_down_get_selected(widget);

  change_filters();

  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void adc0_antenna_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->adc[0].antenna=(int)gtk_drop_down_get_selected(widget);
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
#ifdef SOAPYSDR
  } else if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    soapy_protocol_set_rx_antenna(radio->receiver[0],radio->adc[0].antenna);
#endif
  }
}

static void adc1_antenna_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->adc[1].antenna=(int)gtk_drop_down_get_selected(widget);
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

#ifdef SOAPYSDR
static void dac0_antenna_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->dac[0].antenna=(int)gtk_drop_down_get_selected(widget);
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  } else if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    soapy_protocol_set_tx_antenna(radio->transmitter,radio->dac[0].antenna);
  }
}
#endif

static void adc0_filters_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->adc[0].filters=(int)gtk_drop_down_get_selected(widget);
  if(radio->adc[0].filters==MANUAL) {
    sui_set_sensitive(adc0_hpf_combo_box, TRUE);
    sui_set_sensitive(adc0_lpf_combo_box, TRUE);
  } else {
    sui_set_sensitive(adc0_hpf_combo_box, FALSE);
    sui_set_sensitive(adc0_lpf_combo_box, FALSE);
  }
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void adc0_hpf_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->adc[0].hpf=(int)gtk_drop_down_get_selected(widget);
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void adc0_lpf_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->adc[0].lpf=(int)gtk_drop_down_get_selected(widget);
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void adc1_filters_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->adc[1].filters=(int)gtk_drop_down_get_selected(widget);
  if(radio->adc[1].filters==MANUAL) {
    sui_set_sensitive(adc1_hpf_combo_box, TRUE);
  } else {
    sui_set_sensitive(adc1_hpf_combo_box, FALSE);
  }
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void adc1_hpf_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->adc[1].hpf=(int)gtk_drop_down_get_selected(widget);
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void ptt_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->mic_ptt_enabled=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void ptt_ring_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  if(gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) {
    radio->mic_ptt_tip_bias_ring=0;
  }
}

static void ptt_tip_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  if(gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) {
    radio->mic_ptt_tip_bias_ring=1;
  }
}

static void bias_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->mic_bias_enabled=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void boost_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->mic_boost=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

// The menu no longer lists every soundio backend (Dummy is filtered out), so a
// row number is not a backend index any more.  radio->which_audio_backend stays
// the soundio index — it is persisted, and create_audio() resolves it — and
// this table maps between the two.
#define MAX_AUDIO_BACKENDS 16
static int backend_rows[MAX_AUDIO_BACKENDS];   // menu row -> soundio index
static int n_backend_rows=0;
static gulong audio_backend_signal_id=0;

static int backend_row_of(int soundio_index) {
  for(int i=0;i<n_backend_rows;i++) if(backend_rows[i]==soundio_index) return i;
  return -1;
}

// Point the menu at the backend that is really connected, without re-entering
// the change handler (which would tear the audio down and build it again).
//
// Two different things, and conflating them is what hid the message: the menu
// selection is the operator's REQUEST (radio->which_audio_backend, persisted),
// and audio_get_current_backend() is what libsoundio actually connected.  JACK
// is listed because libsoundio was built with it, but it only works with a
// server running, so on an ordinary desktop the request cannot be honoured and
// PulseAudio is used instead.  Show the reality in the menu and name the
// difference underneath; unexplained, a selection that reverts reads as a
// broken control.  Unlike Dummy, JACK cannot simply be dropped from the list —
// it is the right choice on a machine that runs it, and the only way to find
// out is to try, since probing means connecting and connecting means becoming a
// JACK client.
static void sync_audio_backend_selection(RADIO *radio) {
  int actual=audio_get_current_backend();
  if(actual<0) actual=radio->which_audio_backend;
  int row=backend_row_of(actual);
  if(audio_backend_combo_box!=NULL && row>=0) {
    if(audio_backend_signal_id!=0)
      g_signal_handler_block(audio_backend_combo_box,audio_backend_signal_id);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(audio_backend_combo_box),row);
    if(audio_backend_signal_id!=0)
      g_signal_handler_unblock(audio_backend_combo_box,audio_backend_signal_id);
  }

}

static void update_audio_backends(RADIO *radio) {
  int i;
  GtkStringList *ab_sl=GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(audio_backend_combo_box)));
  gtk_string_list_splice(ab_sl,0,g_list_model_get_n_items(G_LIST_MODEL(ab_sl)),NULL);
  n_backend_rows=0;
  if(radio->which_audio==USE_SOUNDIO) {
    for(i=0;i<audio_get_backends(radio) && n_backend_rows<MAX_AUDIO_BACKENDS;i++) {
      if(!audio_backend_is_usable(i)) continue;
      gtk_string_list_append(ab_sl,audio_get_backend_name(i));
      backend_rows[n_backend_rows++]=i;
    }
  }
  // Only on a real change.  This is called every time the Radio page is built,
  // i.e. on every Configure open, and radio_change_audio_backend() unconditionally
  // re-runs create_audio() -- which used to leak the whole SoundIo connection
  // (with its device-scan thread) and every device string, once per open.
  if(radio->which_audio_backend>=0 && audio_get_current_backend()<0) {
    radio_change_audio_backend(radio,radio->which_audio_backend);
  }
  // create_audio() may have connected something other than what was asked for
  // (a backend can be built into libsoundio and still fail — JACK with no
  // server is the everyday case), and it writes back what it got.
  sync_audio_backend_selection(radio);
}

static void audio_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int selected=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
log_info("radio_dialog: audio_cb: selected=%d\n",selected);
  radio_change_audio(radio,selected);
  update_audio_backends(radio);
}

static void audio_backend_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *radio=(RADIO *)data;
  int row=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  if(row<0 || row>=n_backend_rows) return;
log_info("radio_dialog: audio_backend_cb: row=%d backend=%d\n",row,backend_rows[row]);
  radio_change_audio_backend(radio,backend_rows[row]);
  sync_audio_backend_selection(radio);   // show what actually connected
}

static void smeter_calibrate_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->meter_calibration=gtk_range_get_value(GTK_RANGE(widget));
}

static void panadapter_calibrate_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->panadapter_calibration=gtk_range_get_value(GTK_RANGE(widget));
}

static void swr_alarm_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->swr_alarm_value=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
}

static void ppm_correction_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->ppm_correction_value=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
}

static void temperature_alarm_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  radio->temperature_alarm_value=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void psu_clk_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  if (radio->hl2 != NULL) radio->hl2->psu_clk = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void region_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  radio->region=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  radio_change_region(radio);
}

static void dither_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->dither=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void random_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->random=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void preamp_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->preamp=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void att10_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->att10=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void att20_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->att20=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

#ifdef SOAPYSDR
static void adc_gain_value_changed_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->gain=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  if(radio->discovered->device==DEVICE_SOAPYSDR) {
    soapy_protocol_set_gain(adc);
  }
}

static void agc_changed_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  gboolean agc=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  soapy_protocol_set_automatic_gain(radio->receiver[0],agc);
}

static void dac0_gain_value_changed_cb(GtkWidget *widget, gpointer data) {
  DAC *dac=(DAC *)data;
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    dac->gain=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    soapy_protocol_set_tx_gain(dac);
  }
}
#endif

static void iqswap_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->iqswap=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

// ---- I/Q Player (fake device) file chooser -------------------------------
// Shows the basename of the currently-selected recording, or a note that the
// synthetic generator is active.
static void iq_player_set_label(GtkWidget *lbl, RADIO *r) {
  if(lbl==NULL) return;
  if(r->iq_player_file[0]) {
    char *base=g_path_get_basename(r->iq_player_file);
    gtk_label_set_text(GTK_LABEL(lbl),base);
    g_free(base);
  } else {
    gtk_label_set_text(GTK_LABEL(lbl),"(synthetic noise + tones)");
  }
}

// The chooser is async and its native window can outlive a Configure dialog the
// user closes underneath it, so the status label is tracked via a weak pointer
// (nulled if the label widget is destroyed) rather than a bare cached pointer.
typedef struct { RADIO *r; GtkWidget *lbl; } IqPlayerCtx;

static void iq_player_file_done(GObject *src, GAsyncResult *res, gpointer data) {
  IqPlayerCtx *ctx=(IqPlayerCtx *)data;
  GFile *gf=gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),res,NULL);
  if(gf!=NULL) {
    char *path=g_file_get_path(gf);
    g_object_unref(gf);
    if(path!=NULL && fake_protocol_set_iq_file(ctx->r,path)) {
      g_strlcpy(ctx->r->iq_player_file,path,sizeof(ctx->r->iq_player_file));
      iq_player_set_label(ctx->lbl,ctx->r);   // no-op if the label was destroyed
    }
    g_free(path);
  }
  if(ctx->lbl!=NULL) g_object_remove_weak_pointer(G_OBJECT(ctx->lbl),(gpointer *)&ctx->lbl);
  g_free(ctx);
}

static void iq_player_choose_cb(GtkWidget *widget, gpointer data) {
  IqPlayerCtx *ctx=g_new0(IqPlayerCtx,1);
  ctx->r=(RADIO *)data;
  ctx->lbl=g_object_get_data(G_OBJECT(widget),"iq-player-label");
  if(ctx->lbl!=NULL) g_object_add_weak_pointer(G_OBJECT(ctx->lbl),(gpointer *)&ctx->lbl);

  GtkWidget *top=GTK_WIDGET(gtk_widget_get_root(widget));
  GtkFileDialog *dlg=gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg,"Choose I/Q recording (16-bit stereo WAV)");
  GtkFileFilter *filt=gtk_file_filter_new();
  gtk_file_filter_set_name(filt,"WAV I/Q recordings");
  gtk_file_filter_add_pattern(filt,"*.wav");
  gtk_file_filter_add_pattern(filt,"*.WAV");
  GListStore *filters=g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters,filt);
  g_object_unref(filt);
  gtk_file_dialog_set_filters(dlg,G_LIST_MODEL(filters));
  g_object_unref(filters);
  if(ctx->r->iq_player_file[0]) {
    GFile *cur=g_file_new_for_path(ctx->r->iq_player_file);
    gtk_file_dialog_set_initial_file(dlg,cur);
    g_object_unref(cur);
  }
  gtk_file_dialog_open(dlg,GTK_WINDOW(top),NULL,iq_player_file_done,ctx);
  g_object_unref(dlg);
}

static void iq_player_clear_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->iq_player_file[0]='\0';
  fake_protocol_set_iq_file(r,NULL);
  iq_player_set_label(g_object_get_data(G_OBJECT(widget),"iq-player-label"),r);
}

// Carrier de-rotation for the played recording. A capture whose signal is not
// at DC (an HFDL burst sitting ~15 kHz off the LO, say) cannot demodulate
// without this, and it used to be reachable only through MACHPSDR_FAKE_OFFSET —
// which is not a control an operator can find.
static void iq_player_offset_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->iq_player_offset=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  fake_protocol_set_iq_offset(r,r->iq_player_offset);
}

static void attenuation_value_changed_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->attenuation=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));

  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_high_priority();
  }
}

static void lna2_value_changed_cb(GtkWidget *widget, gpointer data) {
  RADIO *r=(RADIO *)data;
  if (r->hl2 != NULL) {
    r->hl2->adc2_lna_gain = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
    r->hl2->adc2_value_to_send = TRUE;
  }
}

static void freetune_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx = (RECEIVER *)data;
  gboolean enable = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  receiver_set_freetune(rx, enable);
  log_info("radio_dialog: freetune rx=%d enable=%d\n", rx->channel, enable);
}

GtkWidget *create_radio_dialog(RADIO *radio) {
  log_info("%s\n",__FUNCTION__);
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  sui_style_page(grid);

  int row=0;
  int col=0;

  // Separate grid for the audio-related frames, split off the Radio page onto
  // the "Audio" tab. arow is its own row counter (independent of the hardware
  // page's row) so the frames pack from the top with no gaps.
  radio_audio_page=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(radio_audio_page),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(radio_audio_page),FALSE);
  sui_style_page(radio_audio_page);
  int arow=0;

  GtkWidget *model_frame=gtk_frame_new("Radio Model");
  GtkWidget *model_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(model_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(model_grid),TRUE);
  sui_style_group(model_grid);
  gtk_frame_set_child(GTK_FRAME(model_frame),model_grid);
  gtk_grid_attach(GTK_GRID(grid),model_frame,col,row++,1,1);

  int x=0;
  int y=0;

  GtkWidget *model_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANAN_10");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANAN_10E");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANAN_100");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANAN_100D");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANAN_200D");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANAN_7000DLE");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANAN_8000DLE");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ATLAS");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"HERMES");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"HERMES 2");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ANGELIA");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ORION");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"ORION 2");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"HERMES LITE");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"HERMES LITE 2");
#ifdef SOAPYSDR
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(model_combo_box))),"SoapySDR");
#endif
  gtk_drop_down_set_selected(GTK_DROP_DOWN(model_combo_box),radio->model);
  g_signal_connect(model_combo_box,"notify::selected",G_CALLBACK(model_cb),radio);
#ifdef SOAPYSDR
  // "Radio model" is an HPSDR-only concept (which ANAN/Hermes variant). For a
  // non-HPSDR device (SoapySDR: HackRF/RTL/Lime) the model is fixed by the
  // discovered hardware and changing it is meaningless, so grey it out.
  if(radio->discovered->device==DEVICE_SOAPYSDR) {
    gtk_widget_set_sensitive(model_combo_box,FALSE);
  }
#endif
  gtk_grid_attach(GTK_GRID(model_grid),model_combo_box,x,0,1,1);
  x++;
  // "Swap I & Q" is only wired for the protocols that actually consult it —
  // SoapySDR (both directions) and the I/Q Player (an inverted-sideband
  // recording). protocol1/protocol2 ignore it on receive, so offering it there
  // was a switch that silently did nothing on RX while inverting TX.
  gboolean iqswap_applies =
#ifdef SOAPYSDR
      (radio->discovered->protocol == PROTOCOL_SOAPYSDR) ||
#endif
      (radio->discovered->protocol == PROTOCOL_FAKE);
  if ((radio->discovered->device == DEVICE_HERMES_LITE2) || (radio->discovered->device == DEVICE_HERMES_LITE)) {
    /* no IQ swap for Hermes Lite */
  }
  else if (iqswap_applies) {
    GtkWidget *iqswap=gtk_check_button_new_with_label("Swap I & Q");
    gtk_grid_attach(GTK_GRID(model_grid),iqswap,x,0,1,1);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(iqswap),radio->iqswap);
    g_signal_connect(iqswap,"toggled",G_CALLBACK(iqswap_changed_cb),radio);
  }
  x++;

#ifdef SOAPYSDR
  if(radio->discovered->device!=DEVICE_SOAPYSDR) {
#endif
    filter_board_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(filter_board_combo_box))),"NONE");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(filter_board_combo_box))),"ALEX FILTERS");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(filter_board_combo_box))),"APOLLO FILTERS");
    if(radio->discovered->device==DEVICE_HERMES_LITE2) {
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(filter_board_combo_box))),"N2ADR FILTERS");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(filter_board_combo_box))),"HL2-MRF101");
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(filter_board_combo_box),radio->filter_board);
    g_signal_connect(filter_board_combo_box,"notify::selected",G_CALLBACK(filter_board_cb),radio);
    gtk_grid_attach(GTK_GRID(model_grid),filter_board_combo_box,x,0,1,1);
    x++;
#ifdef SOAPYSDR
  }
#endif

  // The classic Protocol-1 radio-wide rate selector.  Also offered for the fake
  // test device (PROTOCOL_FAKE emulates a Hermes/P1) so its panadapter span — and
  // hence how a played I/Q file is shown — can be changed in the UI. The faker
  // resamples the file to whatever rate is chosen.
  if(radio->discovered->protocol==PROTOCOL_1 ||
     radio->discovered->protocol==PROTOCOL_FAKE) {
    GtkWidget *sample_rate_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"48000");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"96000");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"192000");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"384000");
    // The fake device runs the wideband I/O path, so it can also offer the wide
    // spans (real Protocol-1 hardware tops out at 384k).
    if(radio->discovered->protocol==PROTOCOL_FAKE) {
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"768000");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"1536000");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"1920000");
    }
    switch(radio->sample_rate) {
      case 48000:   gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),0); break;
      case 96000:   gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),1); break;
      case 192000:  gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),2); break;
      case 384000:  gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),3); break;
      case 768000:  gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),4); break;
      case 1536000: gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),5); break;
      case 1920000: gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),6); break;
    }
    g_signal_connect(sample_rate_combo_box,"notify::selected",G_CALLBACK(sample_rate_cb),radio);
    gtk_grid_attach(GTK_GRID(model_grid),sample_rate_combo_box,x,0,1,1);
  }

#ifdef SOAPYSDR
  if(radio->discovered->device==DEVICE_SOAPYSDR &&
     strcmp(radio->discovered->name,"sdrplay")==0) {
    GtkWidget *sample_rate_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"96000");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"192000");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"384000");
    gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),"768000");
    switch(radio->sample_rate) {
      case 96000:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),0);
        break;
      case 192000:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),1);
        break;
      case 384000:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),2);
        break;
      case 768000:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),3);
        break;
    }
    g_signal_connect(sample_rate_combo_box,"notify::selected",G_CALLBACK(sample_rate_cb),radio);
    gtk_grid_attach(GTK_GRID(model_grid),sample_rate_combo_box,x,0,1,1);
  }

  // SoapySDR devices other than sdrplay (HackRF, RTL-SDR, Lime): let the user
  // pick the per-receiver rate = waterfall span, up to the ADC rate.  Higher =
  // wider view but more DSP load.
  if(radio->discovered->device==DEVICE_SOAPYSDR &&
     strcmp(radio->discovered->name,"sdrplay")!=0) {
    GtkWidget *sample_rate_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    // All are exact multiples of 48000 by a factor that divides buffer_size(5120)
    // so the 48k audio output rate stays exact (no drift/clicks).  1920000 is the
    // widest; 2000000 (HackRF ADC) is deliberately NOT offered because 2000000/48000
    // is not an integer -> the audio pipeline would drift.
    const int rates[]={192000,384000,768000,1536000,1920000};
    int rxrate=(radio->receiver[0]!=NULL)?radio->receiver[0]->sample_rate:radio->sample_rate;
    int active=-1,idx=0;
    for(int r=0;r<5;r++) {
      if(rates[r]<=radio->sample_rate) {
        char buf[16];
        snprintf(buf,sizeof(buf),"%d",rates[r]);
        gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(sample_rate_combo_box))),buf);
        if(rates[r]==rxrate) active=idx;
        idx++;
      }
    }
    if(active>=0) gtk_drop_down_set_selected(GTK_DROP_DOWN(sample_rate_combo_box),active);
    g_signal_connect(sample_rate_combo_box,"notify::selected",G_CALLBACK(soapy_rx_rate_cb),radio);
    gtk_grid_attach(GTK_GRID(model_grid),sample_rate_combo_box,x,0,1,1);
  }
#endif

  // I/Q Player: the fake ("I/Q Player") device loops a WAV recording. Let the
  // operator pick which file to play — live-swappable while it runs — or fall
  // back to the synthetic noise+tones generator. Only shown for PROTOCOL_FAKE.
  if(radio->discovered->protocol==PROTOCOL_FAKE) {
    GtkWidget *iq_frame=gtk_frame_new("I/Q Player");
    GtkWidget *iq_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(iq_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(iq_grid),FALSE);
    sui_style_group(iq_grid);
    gtk_frame_set_child(GTK_FRAME(iq_frame),iq_grid);
    gtk_grid_attach(GTK_GRID(grid),iq_frame,col,row++,1,1);

    GtkWidget *iq_lbl=gtk_label_new(NULL);
    gtk_widget_set_halign(iq_lbl,GTK_ALIGN_START);
    iq_player_set_label(iq_lbl,radio);

    GtkWidget *iq_choose=gtk_button_new_with_label("Choose I/Q File…");
    g_object_set_data(G_OBJECT(iq_choose),"iq-player-label",iq_lbl);
    g_signal_connect(iq_choose,"clicked",G_CALLBACK(iq_player_choose_cb),radio);
    gtk_grid_attach(GTK_GRID(iq_grid),iq_choose,0,0,1,1);

    GtkWidget *iq_clear=gtk_button_new_with_label("Synthetic");
    gtk_widget_set_tooltip_text(iq_clear,"Play the built-in synthetic noise + tones instead of a file");
    g_object_set_data(G_OBJECT(iq_clear),"iq-player-label",iq_lbl);
    g_signal_connect(iq_clear,"clicked",G_CALLBACK(iq_player_clear_cb),radio);
    gtk_grid_attach(GTK_GRID(iq_grid),iq_clear,1,0,1,1);

    gtk_grid_attach(GTK_GRID(iq_grid),iq_lbl,2,0,1,1);

    GtkWidget *iq_off_lbl=gtk_label_new("Frequency offset (Hz):");
    gtk_label_set_xalign(GTK_LABEL(iq_off_lbl),0.0);
    gtk_grid_attach(GTK_GRID(iq_grid),iq_off_lbl,0,1,1,1);
    GtkWidget *iq_off=gtk_spin_button_new_with_range(-500000.0,500000.0,10.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(iq_off),radio->iq_player_offset);
    gtk_widget_set_tooltip_text(iq_off,
      "Shift the recording in frequency so the signal you want lands at the centre "
      "of the span. Needed when the capture was not tuned to that signal — e.g. an "
      "HFDL burst recorded 15 kHz off the receiver's LO. 0 = play as recorded.");
    gtk_grid_attach(GTK_GRID(iq_grid),iq_off,1,1,1,1);
    g_signal_connect(iq_off,"value_changed",G_CALLBACK(iq_player_offset_cb),radio);
  }


  adc0_frame=gtk_frame_new("ADC-0");
  GtkWidget *adc0_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(adc0_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(adc0_grid),TRUE);
  sui_style_group(adc0_grid);
  gtk_frame_set_child(GTK_FRAME(adc0_frame),adc0_grid);
  gtk_grid_attach(GTK_GRID(grid),adc0_frame,col,row++,1,1);

  switch(radio->discovered->device) {

#ifdef SOAPYSDR
    case DEVICE_SOAPYSDR:
      {
      GtkWidget *antenna_label=gtk_label_new("Antenna:");
      gtk_grid_attach(GTK_GRID(adc0_grid),antenna_label,0,0,1,1);
      adc0_antenna_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);

      for(int i=0;i<radio->discovered->info.soapy.rx_antennas;i++) {
        gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_antenna_combo_box))),radio->discovered->info.soapy.rx_antenna[i]);
      }

      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc0_antenna_combo_box),radio->adc[0].antenna);
      g_signal_connect(adc0_antenna_combo_box,"notify::selected",G_CALLBACK(adc0_antenna_cb),radio);
      gtk_grid_attach(GTK_GRID(adc0_grid),adc0_antenna_combo_box,1,0,1,1);

      GtkWidget *adc_gain_label=gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(adc_gain_label), "<b>Gain</b>");
      gtk_grid_attach(GTK_GRID(adc0_grid),adc_gain_label,0,1,1,1);

      double max=100;
      if(strcmp(radio->discovered->name,"lime")==0) {
        max=60.0;
      } else if(strcmp(radio->discovered->name,"plutosdr")==0) {
        max=73.0;
      }
      GtkWidget *adc_gain_b=gtk_spin_button_new_with_range(0.0,max,1.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(adc_gain_b),radio->adc[0].gain);
      gtk_grid_attach(GTK_GRID(adc0_grid),adc_gain_b,1,1,1,1);
      g_signal_connect(adc_gain_b,"value_changed",G_CALLBACK(adc_gain_value_changed_cb),&radio->adc[0]);

      if(radio->discovered->info.soapy.rx_has_automatic_gain) {
        GtkWidget *agc=gtk_check_button_new_with_label("Hardware AGC: ");
        gtk_grid_attach(GTK_GRID(adc0_grid),agc,1,2,1,1);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(agc),radio->adc[0].agc);
        g_signal_connect(agc,"toggled",G_CALLBACK(agc_changed_cb),&radio->adc[0]);
      }
      }
      break;
#endif
    case DEVICE_HERMES_LITE2:
      attenuation_label=gtk_label_new("LNA gain (dB):");
      gtk_grid_attach(GTK_GRID(adc0_grid),attenuation_label,0,0,1,1);
      attenuation_b=gtk_spin_button_new_with_range(-12.0,48.0,1.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(attenuation_b),(double)radio->adc[0].attenuation);
      gtk_grid_attach(GTK_GRID(adc0_grid),attenuation_b,2,0,1,1);
      g_signal_connect(attenuation_b,"value_changed",G_CALLBACK(attenuation_value_changed_cb),&radio->adc[0]);

      if (radio->hl2 != NULL) {
        disable_fpgaclk_b=gtk_check_button_new_with_label("FPGA PSU clock");
        gtk_check_button_set_active (GTK_CHECK_BUTTON (disable_fpgaclk_b), radio->hl2->psu_clk);
        gtk_grid_attach(GTK_GRID(adc0_grid),disable_fpgaclk_b,0,1,1,1);
        g_signal_connect(disable_fpgaclk_b,"toggled",G_CALLBACK(psu_clk_cb),radio);
      }
      break;

    case DEVICE_HERMES_LITE:
      attenuation_label=gtk_label_new("LNA gain (dB):");
      gtk_grid_attach(GTK_GRID(adc0_grid),attenuation_label,0,0,1,1);
      attenuation_b=gtk_spin_button_new_with_range(-12.0,48.0,1.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(attenuation_b),(double)radio->adc[0].attenuation);
      gtk_grid_attach(GTK_GRID(adc0_grid),attenuation_b,2,0,1,1);
      g_signal_connect(attenuation_b,"value_changed",G_CALLBACK(attenuation_value_changed_cb),&radio->adc[0]);

      enable_attenuation_b=gtk_check_button_new_with_label("Enable 20dB Attenuation");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (enable_attenuation_b), radio->adc[0].dither);
      gtk_grid_attach(GTK_GRID(adc0_grid),enable_attenuation_b,0,1,1,1);
      g_signal_connect(enable_attenuation_b,"toggled",G_CALLBACK(dither_cb),&radio->adc[0]);
      break;

    default:
      adc0_antenna_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_antenna_combo_box))),"ANT_1");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_antenna_combo_box))),"ANT_2");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_antenna_combo_box))),"ANT_3");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_antenna_combo_box))),"XVTR");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_antenna_combo_box))),"EXT1");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_antenna_combo_box))),"EXT2");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc0_antenna_combo_box),radio->adc[0].antenna);
      g_signal_connect(adc0_antenna_combo_box,"notify::selected",G_CALLBACK(adc0_antenna_cb),radio);
      gtk_grid_attach(GTK_GRID(adc0_grid),adc0_antenna_combo_box,0,0,1,1);

      adc0_filters_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_filters_combo_box))),"Automatic");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_filters_combo_box))),"Manual");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc0_filters_combo_box),radio->adc[0].filters);
      g_signal_connect(adc0_filters_combo_box,"notify::selected",G_CALLBACK(adc0_filters_cb),radio);
      gtk_grid_attach(GTK_GRID(adc0_grid),adc0_filters_combo_box,1,0,1,1);

      adc0_hpf_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_hpf_combo_box))),"BYPASS HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_hpf_combo_box))),"1.5 MHz HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_hpf_combo_box))),"6.5 MHZ HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_hpf_combo_box))),"9.5 MHz HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_hpf_combo_box))),"13 MHz HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_hpf_combo_box))),"20 MHz HPF");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc0_hpf_combo_box),radio->adc[0].hpf);
      g_signal_connect(adc0_hpf_combo_box,"notify::selected",G_CALLBACK(adc0_hpf_cb),radio);
      gtk_grid_attach(GTK_GRID(adc0_grid),adc0_hpf_combo_box,2,0,1,1);

      adc0_lpf_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_lpf_combo_box))),"160m LPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_lpf_combo_box))),"80m LPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_lpf_combo_box))),"60/40m LPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_lpf_combo_box))),"30/20m LPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_lpf_combo_box))),"17/15m LPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_lpf_combo_box))),"12/10m LPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc0_lpf_combo_box))),"6m LPF");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc0_lpf_combo_box),radio->adc[0].lpf);
      g_signal_connect(adc0_lpf_combo_box,"notify::selected",G_CALLBACK(adc0_lpf_cb),radio);
      gtk_grid_attach(GTK_GRID(adc0_grid),adc0_lpf_combo_box,3,0,1,1);

      dither_b=gtk_check_button_new_with_label("Dither");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (dither_b), radio->adc[0].dither);
      gtk_grid_attach(GTK_GRID(adc0_grid),dither_b,0,1,1,1);
      g_signal_connect(dither_b,"toggled",G_CALLBACK(dither_cb),&radio->adc[0]);

      random_b=gtk_check_button_new_with_label("Random");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (random_b), radio->adc[0].random);
      gtk_grid_attach(GTK_GRID(adc0_grid),random_b,1,1,1,1);
      g_signal_connect(random_b,"toggled",G_CALLBACK(random_cb),&radio->adc[0]);

      preamp_b=gtk_check_button_new_with_label("Preamp");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (preamp_b), radio->adc[0].preamp);
      gtk_grid_attach(GTK_GRID(adc0_grid),preamp_b,2,1,1,1);
      g_signal_connect(preamp_b,"toggled",G_CALLBACK(preamp_cb),&radio->adc[0]);

      att10_b=gtk_check_button_new_with_label(radio->att10_label);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (att10_b), radio->adc[0].att10);
      gtk_grid_attach(GTK_GRID(adc0_grid),att10_b,3,1,1,1);
      g_signal_connect(att10_b,"toggled",G_CALLBACK(att10_cb),&radio->adc[0]);
      radio->att10_check=att10_b;

      att20_b=gtk_check_button_new_with_label(radio->att20_label);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (att20_b), radio->adc[0].att20);
      gtk_grid_attach(GTK_GRID(adc0_grid),att20_b,4,1,1,1);
      g_signal_connect(att20_b,"toggled",G_CALLBACK(att20_cb),&radio->adc[0]);
      radio->att20_check=att20_b;

      attenuation_label=gtk_label_new("Attenuation (dB):");
      gtk_grid_attach(GTK_GRID(adc0_grid),attenuation_label,5,1,1,1);

      attenuation_b=gtk_spin_button_new_with_range(0.0,31.0,1.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(attenuation_b),(double)radio->adc[0].attenuation);
      gtk_grid_attach(GTK_GRID(adc0_grid),attenuation_b,6,1,1,1);
      g_signal_connect(attenuation_b,"value_changed",G_CALLBACK(attenuation_value_changed_cb),&radio->adc[0]);
      break;
  }

  switch(radio->discovered->device) {
    case DEVICE_HERMES2:
    case DEVICE_ANGELIA:
    case DEVICE_ORION:
    case DEVICE_ORION2:
      adc1_frame=gtk_frame_new("ADC-1");
      GtkWidget *adc1_grid=gtk_grid_new();
      gtk_grid_set_row_homogeneous(GTK_GRID(adc1_grid),TRUE);
      gtk_grid_set_column_homogeneous(GTK_GRID(adc1_grid),TRUE);
      sui_style_group(adc1_grid);
      gtk_frame_set_child(GTK_FRAME(adc1_frame),adc1_grid);
      gtk_grid_attach(GTK_GRID(grid),adc1_frame,col,row++,1,1);

      adc1_antenna_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_antenna_combo_box))),"RX2");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc1_antenna_combo_box),radio->adc[1].antenna);
      g_signal_connect(adc1_antenna_combo_box,"notify::selected",G_CALLBACK(adc1_antenna_cb),radio);
      gtk_grid_attach(GTK_GRID(adc1_grid),adc1_antenna_combo_box,0,0,1,1);

      adc1_filters_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_filters_combo_box))),"Automatic");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_filters_combo_box))),"Manual");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc1_filters_combo_box),radio->adc[1].filters);
      g_signal_connect(adc1_filters_combo_box,"notify::selected",G_CALLBACK(adc1_filters_cb),radio);
      gtk_grid_attach(GTK_GRID(adc1_grid),adc1_filters_combo_box,1,0,1,1);

      adc1_hpf_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_hpf_combo_box))),"BYPASS HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_hpf_combo_box))),"1.5 MHz HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_hpf_combo_box))),"6.5 MHZ HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_hpf_combo_box))),"9.5 MHz HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_hpf_combo_box))),"13 MHz HPF");
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(adc1_hpf_combo_box))),"20 MHz HPF");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(adc1_hpf_combo_box),radio->adc[1].hpf);
      g_signal_connect(adc1_hpf_combo_box,"notify::selected",G_CALLBACK(adc1_hpf_cb),radio);
      gtk_grid_attach(GTK_GRID(adc1_grid),adc1_hpf_combo_box,2,0,1,1);

      dither_b=gtk_check_button_new_with_label("Dither");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (dither_b), radio->adc[1].dither);
      gtk_grid_attach(GTK_GRID(adc1_grid),dither_b,0,1,1,1);
      g_signal_connect(dither_b,"toggled",G_CALLBACK(dither_cb),&radio->adc[1]);

      random_b=gtk_check_button_new_with_label("Random");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (random_b), radio->adc[1].random);
      gtk_grid_attach(GTK_GRID(adc1_grid),random_b,1,1,1,1);
      g_signal_connect(random_b,"toggled",G_CALLBACK(random_cb),&radio->adc[1]);

      preamp_b=gtk_check_button_new_with_label("Preamp");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (preamp_b), radio->adc[1].preamp);
      gtk_grid_attach(GTK_GRID(adc1_grid),preamp_b,2,1,1,1);
      g_signal_connect(preamp_b,"toggled",G_CALLBACK(preamp_cb),&radio->adc[1]);

      att10_b=gtk_check_button_new_with_label(radio->att10_label);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (att10_b), radio->adc[1].att10);
      gtk_grid_attach(GTK_GRID(adc1_grid),att10_b,3,1,1,1);
      g_signal_connect(att10_b,"toggled",G_CALLBACK(att10_cb),&radio->adc[1]);

      att20_b=gtk_check_button_new_with_label(radio->att20_label);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (att20_b), radio->adc[1].att20);
      gtk_grid_attach(GTK_GRID(adc1_grid),att20_b,4,1,1,1);
      g_signal_connect(att20_b,"toggled",G_CALLBACK(att20_cb),&radio->adc[1]);

      attenuation_label=gtk_label_new("Attenuation (dB):");
      gtk_grid_attach(GTK_GRID(adc1_grid),attenuation_label,5,1,1,1);

      attenuation_b=gtk_spin_button_new_with_range(0.0,31.0,1.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(attenuation_b),(double)radio->adc[1].attenuation);
      gtk_grid_attach(GTK_GRID(adc1_grid),attenuation_b,6,1,1,1);
      g_signal_connect(attenuation_b,"value_changed",G_CALLBACK(attenuation_value_changed_cb),&radio->adc[1]);
      break;

    case DEVICE_HERMES_LITE2:
      if ((radio->hl2 != NULL) && (radio->diversity_mixers > 0)) {
        adc1_frame = gtk_frame_new("ADC-1");
        GtkWidget *adc1_grid = gtk_grid_new();
        gtk_grid_set_row_homogeneous(GTK_GRID(adc1_grid), TRUE);
        gtk_grid_set_column_homogeneous(GTK_GRID(adc1_grid), TRUE);
        sui_style_group(adc1_grid);
        gtk_frame_set_child(GTK_FRAME(adc1_frame), adc1_grid);
        gtk_grid_attach(GTK_GRID(grid), adc1_frame, col, row++, 1, 1);

        attenuation_label = gtk_label_new("LNA gain (dB):");
        gtk_grid_attach(GTK_GRID(adc1_grid), attenuation_label, 0, 0, 1, 1);
        attenuation_b = gtk_spin_button_new_with_range(-12.0, 48.0, 1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(attenuation_b), (double)radio->hl2->adc2_lna_gain);
        gtk_grid_attach(GTK_GRID(adc1_grid), attenuation_b, 2, 0, 1, 1);
        g_signal_connect(attenuation_b, "value_changed", G_CALLBACK(lna2_value_changed_cb), radio);
      }
      break;

    default:
      break;
  }

  radio_dialog_update_controls();

  switch(radio->discovered->device) {
#ifdef SOAPYSDR
    case DEVICE_SOAPYSDR:
      if(radio->can_transmit) {
        int r=0;
        dac0_frame=gtk_frame_new("DAC-0");
        GtkWidget *dac0_grid=gtk_grid_new();
        gtk_grid_set_row_homogeneous(GTK_GRID(dac0_grid),TRUE);
        gtk_grid_set_column_homogeneous(GTK_GRID(dac0_grid),TRUE);
        sui_style_group(dac0_grid);
        gtk_frame_set_child(GTK_FRAME(dac0_frame),dac0_grid);
        gtk_grid_attach(GTK_GRID(grid),dac0_frame,col,row++,1,1);

        GtkWidget *antenna_label=gtk_label_new("Antenna:");
        gtk_grid_attach(GTK_GRID(dac0_grid),antenna_label,0,r,1,1);

        dac0_antenna_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
        for(int i=0;i<radio->discovered->info.soapy.tx_antennas;i++) {
          gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(dac0_antenna_combo_box))),radio->discovered->info.soapy.tx_antenna[i]);
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dac0_antenna_combo_box),radio->dac[0].antenna);
        g_signal_connect(dac0_antenna_combo_box,"notify::selected",G_CALLBACK(dac0_antenna_cb),radio);
        gtk_grid_attach(GTK_GRID(dac0_grid),dac0_antenna_combo_box,1,r,1,1);
        r++;

        GtkWidget *gain_label=gtk_label_new("Gain");
        gtk_grid_attach(GTK_GRID(dac0_grid),gain_label,0,r,1,1);

        double max=100.0;
        if(strcmp(radio->discovered->name,"lime")==0) {
          max=64.0;
        } else if(strcmp(radio->discovered->name,"plutosdr")==0) {
          max=89.0;
        }

        GtkWidget *gain_b=gtk_spin_button_new_with_range(0.0,max,1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gain_b),radio->dac[0].gain);
        gtk_grid_attach(GTK_GRID(dac0_grid),gain_b,1,r,1,1);
        g_signal_connect(gain_b,"value_changed",G_CALLBACK(dac0_gain_value_changed_cb),&radio->dac[0]);
        r++;
      }
      break;
#endif
    default:
      break;
  }


  if(radio->discovered->device==DEVICE_ANGELIA ||
     radio->discovered->device==DEVICE_ORION ||
     radio->discovered->device==DEVICE_ORION2) {
    GtkWidget *mic_frame=gtk_frame_new("Microphone");
    GtkWidget *mic_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(mic_grid),TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(mic_grid),FALSE);
    sui_style_group(mic_grid);
    gtk_frame_set_child(GTK_FRAME(mic_frame),mic_grid);
    gtk_grid_attach(GTK_GRID(radio_audio_page),mic_frame,0,arow++,1,1);

    x=0;
    y=0;

    // GTK4: GtkRadioButton is gone — grouped GtkCheckButtons behave as radios.
    GtkWidget *ptt_ring_b=gtk_check_button_new_with_label("PTT On Ring, Mic and Bias on Tip");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (ptt_ring_b), radio->mic_ptt_tip_bias_ring==0);
    gtk_grid_attach(GTK_GRID(mic_grid),ptt_ring_b,x,y++,1,1);
    g_signal_connect(ptt_ring_b,"toggled",G_CALLBACK(ptt_ring_cb),radio);

    GtkWidget *ptt_tip_b=gtk_check_button_new_with_label("PTT On Tip, Mic and Bias on Ring");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(ptt_tip_b),GTK_CHECK_BUTTON(ptt_ring_b));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (ptt_tip_b), radio->mic_ptt_tip_bias_ring==1);
    gtk_grid_attach(GTK_GRID(mic_grid),ptt_tip_b,x,y++,1,1);
    g_signal_connect(ptt_tip_b,"toggled",G_CALLBACK(ptt_tip_cb),radio);

    GtkWidget *ptt_b=gtk_check_button_new_with_label("PTT Enabled");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (ptt_b), radio->mic_ptt_enabled);
    gtk_grid_attach(GTK_GRID(mic_grid),ptt_b,x,y++,1,1);
    g_signal_connect(ptt_b,"toggled",G_CALLBACK(ptt_cb),radio);

    GtkWidget *bias_b=gtk_check_button_new_with_label("BIAS Enabled");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (bias_b), radio->mic_bias_enabled);
    gtk_grid_attach(GTK_GRID(mic_grid),bias_b,x,y++,1,1);
    g_signal_connect(bias_b,"toggled",G_CALLBACK(bias_cb),radio);

    GtkWidget *boost_b=gtk_check_button_new_with_label("20dB boost");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (boost_b), radio->mic_boost);
    gtk_grid_attach(GTK_GRID(mic_grid),boost_b,x,y++,1,1);
    g_signal_connect(boost_b,"toggled",G_CALLBACK(boost_cb),radio);
  }

  GtkWidget *config_frame=gtk_frame_new("Configuration");
  GtkWidget *config_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(config_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(config_grid),FALSE);
  sui_style_group(config_grid);
  gtk_frame_set_child(GTK_FRAME(config_frame),config_grid);
  gtk_grid_attach(GTK_GRID(grid),config_frame,col,row++,1,1);

  /* Row 0: SWR alarm + PPM correction */
  GtkWidget *swr_alarm_label=gtk_label_new("SWR alarm at ");
  gtk_widget_set_visible(swr_alarm_label, TRUE);
  gtk_grid_attach(GTK_GRID(config_grid),swr_alarm_label,0,0,1,1);

  swr_alarm_b=gtk_spin_button_new_with_range(1.0, 5.0, 0.1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(swr_alarm_b), radio->swr_alarm_value);
  gtk_widget_set_visible(swr_alarm_b, TRUE);
  gtk_grid_attach(GTK_GRID(config_grid),swr_alarm_b,1,0,1,1);
  g_signal_connect(swr_alarm_b,"value_changed",G_CALLBACK(swr_alarm_changed_cb),radio);

  GtkWidget *ppm_correction_label=gtk_label_new("PPM correction ");
  gtk_widget_set_visible(ppm_correction_label, TRUE);
  gtk_grid_attach(GTK_GRID(config_grid),ppm_correction_label,2,0,1,1);

  ppm_correction_b=gtk_spin_button_new_with_range(-500, 500, 0.01);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ppm_correction_b), 2);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ppm_correction_b), radio->ppm_correction_value);
  gtk_widget_set_visible(ppm_correction_b, TRUE);
  gtk_grid_attach(GTK_GRID(config_grid),ppm_correction_b,3,0,1,1);
  g_signal_connect(ppm_correction_b,"value_changed",G_CALLBACK(ppm_correction_changed_cb),radio);

  if (radio->discovered->device == DEVICE_HERMES_LITE2) {
    GtkWidget *temp_alarm_label=gtk_label_new("Temp alarm at ");
    gtk_widget_set_visible(temp_alarm_label, TRUE);
    gtk_grid_attach(GTK_GRID(config_grid),temp_alarm_label,4,0,1,1);

    temperature_alarm_b=gtk_spin_button_new_with_range(30.0, 60.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(temperature_alarm_b), (double)radio->temperature_alarm_value);
    gtk_widget_set_visible(temperature_alarm_b, TRUE);
    gtk_grid_attach(GTK_GRID(config_grid),temperature_alarm_b,5,0,1,1);
    g_signal_connect(temperature_alarm_b,"value_changed",G_CALLBACK(temperature_alarm_changed_cb),radio);
  }

  {
    int ft_col = 0;

    GtkWidget *freetune_label = gtk_label_new("Freetune:");
    gtk_widget_set_visible(freetune_label, TRUE);
    gtk_grid_attach(GTK_GRID(config_grid), freetune_label, ft_col++, 1, 1, 1);

    for (int i = 0; i < radio->discovered->supported_receivers; i++) {
      if (radio->receiver[i] == NULL) continue;

      gchar label_text[32];
      g_snprintf(label_text, sizeof(label_text), "RX-%d", i);

      GtkWidget *freetune_b = gtk_check_button_new_with_label(label_text);
      gtk_check_button_set_active(GTK_CHECK_BUTTON(freetune_b),
                                   radio->receiver[i]->freetune);
      gtk_widget_set_visible(freetune_b, TRUE);
      gtk_widget_set_tooltip_text(freetune_b,
        "Freeze the waterfall/panadapter centre frequency.\n"
        "Tuning moves only within the visible span.");
      gtk_grid_attach(GTK_GRID(config_grid), freetune_b, ft_col++, 1, 1, 1);
      g_signal_connect(freetune_b, "toggled",
                       G_CALLBACK(freetune_cb), radio->receiver[i]);
    }
  }
  /* -------------------------------------------------------------- */

  GtkWidget *audio_frame=gtk_frame_new("Audio");
  GtkWidget *audio_grid=gtk_grid_new();
  // NOT row-homogeneous: this grid gained a second row (the backend-fallback
  // message), and equal-height rows then split the frame between them and
  // stretched the two drop-downs into tall boxes.  Homogeneous rows are only
  // ever right for a grid whose rows really are the same kind of thing.
  gtk_grid_set_row_homogeneous(GTK_GRID(audio_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(audio_grid),FALSE);
  sui_style_group(audio_grid);
  gtk_frame_set_child(GTK_FRAME(audio_frame),audio_grid);
  gtk_grid_attach(GTK_GRID(radio_audio_page),audio_frame,0,arow++,1,1);

  GtkWidget *audio_combo=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(audio_combo))),"SOUNDIO");
#ifndef __APPLE__
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(audio_combo))),"PULSEAUDIO");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(audio_combo))),"ALSA");
#endif
  gtk_drop_down_set_selected(GTK_DROP_DOWN(audio_combo),radio->which_audio);
  gtk_grid_attach(GTK_GRID(audio_grid),audio_combo,0,0,1,1);
  g_signal_connect(audio_combo,"notify::selected",G_CALLBACK(audio_cb),radio);

  GtkWidget *backend_label=gtk_label_new(" Backend:");
  gtk_grid_attach(GTK_GRID(audio_grid),backend_label,1,0,1,1);

  audio_backend_combo_box=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
  gtk_grid_attach(GTK_GRID(audio_grid),audio_backend_combo_box,2,0,1,1);
  audio_backend_signal_id=
    g_signal_connect(audio_backend_combo_box,"notify::selected",G_CALLBACK(audio_backend_cb),radio);
  // After the handler is connected, so the selection sync inside can block it.
  // The row is NOT which_audio_backend any more — Dummy is filtered out of the
  // menu, so the two only coincide by accident.
  update_audio_backends(radio);


  GtkWidget *calibration_frame=gtk_frame_new("Calibration [dBm]");
  GtkWidget *calibration_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(calibration_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(calibration_grid),FALSE);
  sui_style_group(calibration_grid);
  gtk_frame_set_child(GTK_FRAME(calibration_frame),calibration_grid);
  // Calibration [dBm] stays on the Radio page (not the Audio tab) — it belongs
  // with the radio hardware settings.
  gtk_grid_attach(GTK_GRID(grid),calibration_frame,col,row++,1,1);

  GtkWidget *smeter_label=gtk_label_new(" S-Meter:");
  gtk_grid_attach(GTK_GRID(calibration_grid),smeter_label,0,1,1,1);

  GtkWidget *smeter_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(radio->meter_calibration, -100.0, 100.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(smeter_scale,200,30);
  sui_scale_show_value(smeter_scale,0);
  gtk_widget_set_visible(smeter_scale, TRUE);
  g_signal_connect(G_OBJECT(smeter_scale),"value_changed",G_CALLBACK(smeter_calibrate_changed_cb),radio);
  gtk_grid_attach(GTK_GRID(calibration_grid),smeter_scale,1,1,1,1);

  GtkWidget *panadapter_label=gtk_label_new(" Panadapter:");
  gtk_grid_attach(GTK_GRID(calibration_grid),panadapter_label,0,2,1,1);

  GtkWidget *panadapter_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(radio->panadapter_calibration, -100.0, 100.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_scale,200,30);
  gtk_range_set_value (GTK_RANGE(panadapter_scale),radio->panadapter_calibration);
  sui_scale_show_value(panadapter_scale,0);
  gtk_widget_set_visible(panadapter_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_scale),"value_changed",G_CALLBACK(panadapter_calibrate_changed_cb),radio);
  gtk_grid_attach(GTK_GRID(calibration_grid),panadapter_scale,1,2,1,1);

  x=0;
  y=0;

  GtkWidget *region_frame=gtk_frame_new("Region");
  GtkWidget *region_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(region_grid),TRUE);
  gtk_grid_set_column_homogeneous(GTK_GRID(region_grid),FALSE);
  sui_style_group(region_grid);
  gtk_frame_set_child(GTK_FRAME(region_frame),region_grid);
  gtk_grid_attach(GTK_GRID(grid),region_frame,col,row++,1,1);

  GtkWidget *region_combo=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(region_combo))),"Other");
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(region_combo))),"UK");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(region_combo),radio->region);
  gtk_grid_attach(GTK_GRID(region_grid),region_combo,0,0,1,1);
  g_signal_connect(region_combo,"notify::selected",G_CALLBACK(region_cb),radio);

  return grid;
}

// The audio-related frames (Microphone / Audio backend / dBm calibration) are
// built by create_radio_dialog() into radio_audio_page; this returns that grid
// for the "Audio" configuration tab. Depends on create_radio_dialog() having
// run first — guaranteed by the page order in create_configure_dialog().
GtkWidget *create_radio_audio_dialog(RADIO *radio) {
  (void)radio;
  return radio_audio_page;
}
