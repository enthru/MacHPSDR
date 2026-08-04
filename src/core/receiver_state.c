/* receiver_state.c -- RECEIVER persistence (split out of receiver.c).
 *
 * receiver_save_state() / receiver_restore_state() serialise every persisted
 * RECEIVER field to/from the property store. They were ~890 lines of the
 * receiver.c god-file and touch only public APIs (get/setProperty) and public
 * RECEIVER struct fields, so they live cleanly in their own translation unit
 * with no shared internal header. Prototypes are in receiver.h.
 */
#include <gtk/gtk.h>
#include "log.h"
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <wdsp.h>

#include "agc.h"
#include "mode.h"
#include "filter.h"
#include "bandstack.h"
#include "band.h"
#include "discovered.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "recorder.h"
#include "ppm_cal.h"
#include "main.h"
#include "vfo.h"
#include "meter.h"
#include "radio_info.h"
#include "rx_panadapter.h"
#include "tx_panadapter.h"
#include "waterfall.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#include "fake_protocol.h"
#endif
#include "audio.h"
#include "receiver_dialog.h"
#include "configure_dialog.h"
#include "property.h"
#include "rigctl.h"
#include "subrx.h"
#include "dxcluster.h"
#include "tci.h"
#ifdef FT8
#include "ft8_decoder.h"
#include "ft8_waterfall.h"
#endif
#ifdef SSTV
#include "sstv_decoder.h"
#include "wefax_decoder.h"
#include "cw_decoder.h"
#include "cw_keyer.h"
#endif
// Shared "a decoder is tapping this RX's audio" machinery (unity WDSP panel
// gain + software listen-volume) is used by both the FT8/FT4 and the SSTV
// decoders, so it is compiled whenever either is enabled.
#if defined(FT8) || defined(SSTV)
#define DECODERS 1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Minimum on-screen height (px) for the panadapter (the "spectroscope") and the
// waterfall in the vertical split, so neither can open collapsed to a sliver.
// The panadapter's size-request + shrink=FALSE enforce this against the pane's
// own apportioning; restore_paned_position_cb also clamps the restored split so
// both children keep at least this much when the RX area is short.
#define MIN_PANADAPTER_HEIGHT 90
#define MIN_WATERFALL_HEIGHT  60

void receiver_save_state(RECEIVER *rx) {
  if (rx->show_rx == FALSE) return;
  char name[80];
  char value[80];
  int i;
  gint x;
  gint y;
  gint width;
  gint height;

  sprintf(name,"receiver[%d].channel",rx->channel);
  sprintf(value,"%d",rx->channel);
  setProperty(name,value);
  sprintf(name,"receiver[%d].adc",rx->channel);
  sprintf(value,"%d",rx->adc);
  setProperty(name,value);
  sprintf(name,"receiver[%d].sample_rate",rx->channel);
  sprintf(value,"%d",rx->sample_rate);
  setProperty(name,value);
  sprintf(name,"receiver[%d].dsp_rate",rx->channel);
  sprintf(value,"%d",rx->dsp_rate);
  setProperty(name,value);
  sprintf(name,"receiver[%d].output_rate",rx->channel);
  sprintf(value,"%d",rx->output_rate);
  setProperty(name,value);
  sprintf(name,"receiver[%d].buffer_size",rx->channel);
  sprintf(value,"%d",rx->buffer_size);
  setProperty(name,value);
  sprintf(name,"receiver[%d].fft_size",rx->channel);
  sprintf(value,"%d",rx->fft_size);
  setProperty(name,value);
  sprintf(name,"receiver[%d].low_latency",rx->channel);
  sprintf(value,"%d",rx->low_latency);
  setProperty(name,value);
  sprintf(name,"receiver[%d].low_latency",rx->channel);
  sprintf(value,"%d",rx->low_latency);
  setProperty(name,value);
  sprintf(name,"receiver[%d].fps",rx->channel);
  sprintf(value,"%d",rx->fps);
  setProperty(name,value);
  sprintf(name,"receiver[%d].meter_smoothing",rx->channel);
  sprintf(value,"%d",rx->meter_smoothing);
  setProperty(name,value);

  sprintf(name,"receiver[%d].display_average_time",rx->channel);
  sprintf(value,"%f",rx->display_average_time);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_low",rx->channel);
  sprintf(value,"%d",rx->panadapter_low);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_high",rx->channel);
  sprintf(value,"%d",rx->panadapter_high);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_step",rx->channel);
  sprintf(value,"%d",rx->panadapter_step);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_filled",rx->channel);
  sprintf(value,"%d",rx->panadapter_filled);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_gradient",rx->channel);
  sprintf(value,"%d",rx->panadapter_gradient);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_agc_line",rx->channel);
  sprintf(value,"%d",rx->panadapter_agc_line);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_single_color",rx->channel);
  sprintf(value,"%d",rx->panadapter_single_color);
  setProperty(name,value);
  sprintf(name,"receiver[%d].display_detector_mode",rx->channel);
  sprintf(value,"%d",rx->display_detector_mode);
  setProperty(name,value);
  sprintf(name,"receiver[%d].display_average_mode",rx->channel);
  sprintf(value,"%d",rx->display_average_mode);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_peak_hold",rx->channel);
  sprintf(value,"%d",rx->panadapter_peak_hold);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_peak_decay",rx->channel);
  sprintf(value,"%d",rx->panadapter_peak_decay);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_histogram",rx->channel);
  sprintf(value,"%d",rx->panadapter_histogram);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_histogram_decay",rx->channel);
  sprintf(value,"%d",rx->panadapter_histogram_decay);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_phase",rx->channel);
  sprintf(value,"%d",rx->panadapter_phase);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_phase_mode",rx->channel);
  sprintf(value,"%d",rx->panadapter_phase_mode);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_phase_gain",rx->channel);
  sprintf(value,"%d",rx->panadapter_phase_gain);
  setProperty(name,value);
  sprintf(name,"receiver[%d].panadapter_phase_source",rx->channel);
  sprintf(value,"%d",rx->panadapter_phase_source);
  setProperty(name,value);

  if(rx->waterfall_automatic == FALSE) {
      sprintf(name,"receiver[%d].waterfall_low",rx->channel);
      sprintf(value,"%d",rx->waterfall_low);
      setProperty(name,value);
      sprintf(name,"receiver[%d].waterfall_high",rx->channel);
      sprintf(value,"%d",rx->waterfall_high);
      setProperty(name,value);
  }
  sprintf(name,"receiver[%d].waterfall_automatic",rx->channel);
  sprintf(value,"%d",rx->waterfall_automatic);
  setProperty(name,value);
  sprintf(name,"receiver[%d].waterfall_ft8_marker",rx->channel);
  sprintf(value,"%d",rx->waterfall_ft8_marker);
  setProperty(name,value);

  sprintf(name,"receiver[%d].waterfall_color_theme",rx->channel);
  sprintf(value,"%d",rx->waterfall_color_theme);
  setProperty(name,value);

  sprintf(name,"receiver[%d].frequency_a",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->frequency_a);
  setProperty(name,value);
  sprintf(name,"receiver[%d].lo_a",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->lo_a);
  setProperty(name,value);
  sprintf(name,"receiver[%d].error_a",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->error_a);
  setProperty(name,value);
  sprintf(name,"receiver[%d].lo_tx",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->lo_tx);
  setProperty(name,value);
  sprintf(name,"receiver[%d].error_tx",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->error_tx);
  setProperty(name,value);
  sprintf(name,"receiver[%d].tx_track_rx",rx->channel);
  sprintf(value,"%d",rx->tx_track_rx);
  setProperty(name,value);
  sprintf(name,"receiver[%d].band_a",rx->channel);
  sprintf(value,"%d",rx->band_a);
  setProperty(name,value);
  sprintf(name,"receiver[%d].mode_a",rx->channel);
  sprintf(value,"%d",rx->mode_a);
  setProperty(name,value);
  sprintf(name,"receiver[%d].filter_a",rx->channel);
  sprintf(value,"%d",rx->filter_a);
  setProperty(name,value);
  for(int i=0;i<MODES;i++) {
    sprintf(name,"receiver[%d].mode_filter[%d]",rx->channel,i);
    sprintf(value,"%d",rx->mode_filter[i]);
    setProperty(name,value);
  }
  for(int i=0;i<MODES;i++) {
    sprintf(name,"receiver[%d].mode_agc[%d]",rx->channel,i);
    sprintf(value,"%d",rx->mode_agc[i]);
    setProperty(name,value);
  }
  sprintf(name,"receiver[%d].filter_low_a",rx->channel);
  sprintf(value,"%d",rx->filter_low_a);
  setProperty(name,value);
  sprintf(name,"receiver[%d].filter_high_a",rx->channel);
  sprintf(value,"%d",rx->filter_high_a);
  setProperty(name,value);


  sprintf(name,"receiver[%d].frequency_b",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->frequency_b);
  setProperty(name,value);
  sprintf(name,"receiver[%d].lo_b",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->lo_b);
  setProperty(name,value);
  sprintf(name,"receiver[%d].error_b",rx->channel);
  sprintf(value,"%" G_GINT64_FORMAT,rx->error_b);
  setProperty(name,value);
#ifdef USE_VFO_B_MODE_AND_FILTER
  sprintf(name,"receiver[%d].band_b",rx->channel);
  sprintf(value,"%d",rx->band_b);
  setProperty(name,value);
  sprintf(name,"receiver[%d].mode_b",rx->channel);
  sprintf(value,"%d",rx->mode_b);
  setProperty(name,value);
  sprintf(name,"receiver[%d].filter_b",rx->channel);
  sprintf(value,"%d",rx->filter_b);
  setProperty(name,value);
  sprintf(name,"receiver[%d].filter_low_b",rx->channel);
  sprintf(value,"%d",rx->filter_low_b);
  setProperty(name,value);
  sprintf(name,"receiver[%d].filter_high_b",rx->channel);
  sprintf(value,"%d",rx->filter_high_b);
  setProperty(name,value);
#endif

  sprintf(name,"receiver[%d].ctun",rx->channel);
  sprintf(value,"%d",rx->ctun);
  setProperty(name,value);
  sprintf(name,"receiver[%d].ctun_offset",rx->channel);
  sprintf(value,"%lld",rx->ctun_offset);
  setProperty(name,value);
  sprintf(name,"receiver[%d].ctun_frequency",rx->channel);
  sprintf(value,"%lld",rx->ctun_frequency);
  setProperty(name,value);
  sprintf(name,"receiver[%d].ctun_min",rx->channel);
  sprintf(value,"%lld",rx->ctun_min);
  setProperty(name,value);
  sprintf(name,"receiver[%d].ctun_max",rx->channel);
  sprintf(value,"%lld",rx->ctun_max);
  setProperty(name,value);

  /* freetune state */
  sprintf(name,"receiver[%d].freetune",rx->channel);
  sprintf(value,"%d",rx->freetune);
  setProperty(name,value);

  sprintf(name,"receiver[%d].qo100_beacon",rx->channel);
  sprintf(value,"%d",rx->qo100_beacon);
  setProperty(name,value);

  sprintf(name,"receiver[%d].split",rx->channel);
  sprintf(value,"%d",rx->split);
  setProperty(name,value);

  sprintf(name,"receiver[%d].offset",rx->channel);
  sprintf(value,"%lld",rx->offset);
  setProperty(name,value);
  sprintf(name,"receiver[%d].bandstack",rx->channel);
  sprintf(value,"%d",rx->bandstack);
  setProperty(name,value);

  sprintf(name,"receiver[%d].remote_audio",rx->channel);
  sprintf(value,"%d",rx->remote_audio);
  setProperty(name,value);

  sprintf(name,"receiver[%d].local_audio",rx->channel);
  sprintf(value,"%d",rx->local_audio);
  setProperty(name,value);
  if(rx->audio_name!=NULL) {
    sprintf(name,"receiver[%d].audio_name",rx->channel);
    setProperty(name,rx->audio_name);
  }
  sprintf(name,"receiver[%d].output_index",rx->channel);
  sprintf(value,"%d",rx->output_index);
  setProperty(name,value);
  sprintf(name,"receiver[%d].mute_when_not_active",rx->channel);
  sprintf(value,"%d",rx->mute_when_not_active);
  setProperty(name,value);
  sprintf(name,"receiver[%d].local_audio_buffer_size",rx->channel);
  sprintf(value,"%d",rx->local_audio_buffer_size);
  setProperty(name,value);
  sprintf(name,"receiver[%d].local_audio_latency",rx->channel);
  sprintf(value,"%d",rx->local_audio_latency);
  setProperty(name,value);
  sprintf(name,"receiver[%d].audio_channels",rx->channel);
  sprintf(value,"%d",rx->audio_channels);
  setProperty(name,value);
  sprintf(name,"receiver[%d].subrx_enable",rx->channel);
  sprintf(value,"%d",rx->subrx_enable);
  setProperty(name,value);
  sprintf(name,"receiver[%d].subrx_mix",rx->channel);
  sprintf(value,"%d",rx->subrx_mix);
  setProperty(name,value);


  sprintf(name,"receiver[%d].step",rx->channel);
  sprintf(value,"%lld",rx->step);
  setProperty(name,value);
  sprintf(name,"receiver[%d].zoom",rx->channel);
  sprintf(value,"%d",rx->zoom);
  setProperty(name,value);
  sprintf(name,"receiver[%d].pan",rx->channel);
  sprintf(value,"%d",rx->pan);
  setProperty(name,value);

  sprintf(name,"receiver[%d].agc",rx->channel);
  sprintf(value,"%d",rx->agc);
  setProperty(name,value);
  sprintf(name,"receiver[%d].agc_gain",rx->channel);
  sprintf(value,"%f",rx->agc_gain);
  setProperty(name,value);
  sprintf(name,"receiver[%d].agc_slope",rx->channel);
  sprintf(value,"%f",rx->agc_slope);
  setProperty(name,value);
  sprintf(name,"receiver[%d].agc_hang_threshold",rx->channel);
  sprintf(value,"%f",rx->agc_hang_threshold);
  setProperty(name,value);

  sprintf(name,"receiver[%d].enable_equalizer",rx->channel);
  sprintf(value,"%d",rx->enable_equalizer);
  setProperty(name,value);
  for(i=0;i<11;i++) {
    sprintf(name,"receiver[%d].equalizer[%d]",rx->channel,i);
    sprintf(value,"%d",rx->equalizer[i]);
    setProperty(name,value);
  }

  sprintf(name,"receiver[%d].volume",rx->channel);
  sprintf(value,"%f",rx->volume);
  setProperty(name,value);

  sprintf(name,"receiver[%d].mute",rx->channel);
  sprintf(value,"%d",rx->mute);
  setProperty(name,value);

  sprintf(name,"receiver[%d].nr",rx->channel);
  sprintf(value,"%d",rx->nr);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr2",rx->channel);
  sprintf(value,"%d",rx->nr2);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr3",rx->channel);
  sprintf(value,"%d",rx->nr3);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr4",rx->channel);
  sprintf(value,"%d",rx->nr4);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr4_reduction",rx->channel);
  sprintf(value,"%f",rx->nr4_reduction);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr4_smoothing",rx->channel);
  sprintf(value,"%f",rx->nr4_smoothing);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr4_whitening",rx->channel);
  sprintf(value,"%f",rx->nr4_whitening);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr4_rescale",rx->channel);
  sprintf(value,"%f",rx->nr4_rescale);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nr4_postfilter",rx->channel);
  sprintf(value,"%f",rx->nr4_postfilter);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nb",rx->channel);
  sprintf(value,"%d",rx->nb);
  setProperty(name,value);
  sprintf(name,"receiver[%d].nb2",rx->channel);
  sprintf(value,"%d",rx->nb2);
  setProperty(name,value);
  sprintf(name,"receiver[%d].anf",rx->channel);
  sprintf(value,"%d",rx->anf);
  setProperty(name,value);
  sprintf(name,"receiver[%d].snb",rx->channel);
  sprintf(value,"%d",rx->snb);
  setProperty(name,value);

  // Squelch (previously not persisted) + APF (CW peak filter).
  sprintf(name,"receiver[%d].squelch",rx->channel);
  sprintf(value,"%f",rx->squelch);
  setProperty(name,value);
  for(i=0;i<MODES;i++) {
    sprintf(name,"receiver[%d].mode_squelch[%d]",rx->channel,i);
    sprintf(value,"%f",rx->mode_squelch[i]);
    setProperty(name,value);
  }
  sprintf(name,"receiver[%d].amsq_min_db",rx->channel);
  sprintf(value,"%f",rx->amsq_min_db);
  setProperty(name,value);
  sprintf(name,"receiver[%d].amsq_max_db",rx->channel);
  sprintf(value,"%f",rx->amsq_max_db);
  setProperty(name,value);
  sprintf(name,"receiver[%d].amsq_tail",rx->channel);
  sprintf(value,"%f",rx->amsq_tail);
  setProperty(name,value);
  sprintf(name,"receiver[%d].apf_enable",rx->channel);
  sprintf(value,"%d",rx->apf_enable);
  setProperty(name,value);
  sprintf(name,"receiver[%d].apf_bw",rx->channel);
  sprintf(value,"%f",rx->apf_bw);
  setProperty(name,value);
  sprintf(name,"receiver[%d].apf_gain",rx->channel);
  sprintf(value,"%f",rx->apf_gain);
  setProperty(name,value);

  sprintf(name,"receiver[%d].notches",rx->channel);
  sprintf(value,"%d",rx->notches);
  setProperty(name,value);
  sprintf(name,"receiver[%d].notch_default_width",rx->channel);
  sprintf(value,"%f",rx->notch_default_width);
  setProperty(name,value);
  for(i=0;i<rx->notches;i++) {
    sprintf(name,"receiver[%d].notch[%d].fcenter",rx->channel,i);
    sprintf(value,"%f",rx->notch[i].fcenter);
    setProperty(name,value);
    sprintf(name,"receiver[%d].notch[%d].fwidth",rx->channel,i);
    sprintf(value,"%f",rx->notch[i].fwidth);
    setProperty(name,value);
    sprintf(name,"receiver[%d].notch[%d].active",rx->channel,i);
    sprintf(value,"%d",rx->notch[i].active);
    setProperty(name,value);
    sprintf(name,"receiver[%d].notch[%d].af",rx->channel,i);
    sprintf(value,"%d",rx->notch[i].af);
    setProperty(name,value);
    sprintf(name,"receiver[%d].notch[%d].af_offset",rx->channel,i);
    sprintf(value,"%f",rx->notch[i].af_offset);
    setProperty(name,value);
  }

  sprintf(name,"receiver[%d].rit_enabled",rx->channel);
  sprintf(value,"%d",rx->rit_enabled);
  setProperty(name,value);
  sprintf(name,"receiver[%d].rit",rx->channel);
  sprintf(value,"%lld",rx->rit);
  setProperty(name,value);
  sprintf(name,"receiver[%d].rit_step",rx->channel);
  sprintf(value,"%lld",rx->rit_step);
  setProperty(name,value);

  sprintf(name,"receiver[%d].bpsk_enable",rx->channel);
  sprintf(value,"%d",rx->bpsk_enable);
  setProperty(name,value);

  sprintf(name,"receiver[%d].split",rx->channel);
  sprintf(value,"%d",rx->split);
  setProperty(name,value);
  sprintf(name,"receiver[%d].duplex",rx->channel);
  sprintf(value,"%d",rx->duplex);
  setProperty(name,value);
  sprintf(name,"receiver[%d].mute_while_transmitting",rx->channel);
  sprintf(value,"%d",rx->mute_while_transmitting);
  setProperty(name,value);

  sprintf(name,"receiver[%d].rigctl_port",rx->channel);
  sprintf(value,"%d",rx->rigctl_port);
  setProperty(name,value);
  sprintf(name,"receiver[%d].rigctl_enable",rx->channel);
  sprintf(value,"%d",rx->rigctl_enable);
  setProperty(name,value);
  sprintf(name,"receiver[%d].rigctl_serial_enable",rx->channel);
  sprintf(value,"%d",rx->rigctl_serial_enable);
  setProperty(name,value);
  sprintf(name,"receiver[%d].rigctl_serial_port",rx->channel);
  setProperty(name,rx->rigctl_serial_port);
  sprintf(name,"receiver[%d].rigctl_debug",rx->channel);
  sprintf(value,"%d",rx->rigctl_debug);
  setProperty(name,value);


  if(rx->window!=NULL) {
    // GTK4: client-side window position is not available; persist -1 (ignored
    // on restore, matching main.c's dropped gtk_window_move).
    x=-1; y=-1;
    sprintf(name,"receiver[%d].x",rx->channel);
    sprintf(value,"%d",x);
    setProperty(name,value);
    sprintf(name,"receiver[%d].y",rx->channel);
    sprintf(value,"%d",y);
    setProperty(name,value);

    // GTK4: gtk_window_get_size is gone; use the current allocation.
    width=gtk_widget_get_width(rx->window);
    height=gtk_widget_get_height(rx->window);
    sprintf(name,"receiver[%d].width",rx->channel);
    sprintf(value,"%d",width);
    setProperty(name,value);
    sprintf(name,"receiver[%d].height",rx->channel);
    sprintf(value,"%d",height);
    setProperty(name,value);
  }

  {
    gint position=gtk_paned_get_position(GTK_PANED(rx->vpaned));
    gint paned_height=gtk_widget_get_height(rx->vpaned);
    // Persist the panadapter/waterfall split only when it is sane. Saving a 0
    // (panadapter collapsed, or the vpaned not yet allocated so height<=1)
    // poisons paned_percent and collapses the spectroscope on the next launch
    // — and, once saved, self-perpetuates. Leave the previous good value be.
    if(paned_height>1 && position>0) {
      rx->paned_position=position;
      double paned_percent=(double)position/(double)paned_height;
      sprintf(name,"receiver[%d].paned_position",rx->channel);
      sprintf(value,"%d",rx->paned_position);
      setProperty(name,value);
      sprintf(name,"receiver[%d].paned_percent",rx->channel);
      sprintf(value,"%f",paned_percent);
      setProperty(name,value);
      log_info("receiver_save_sate: paned_position=%d paned_height=%d paned_percent=%f\n",rx->paned_position, paned_height, paned_percent);
    } else {
      log_info("receiver_save_sate: skip degenerate paned (pos=%d height=%d)\n",position,paned_height);
    }
  }

  sprintf(name,"receiver[%d].show_panadapter",rx->channel);
  sprintf(value,"%i", rx->show_panadapter);
  setProperty(name,value);

  sprintf(name,"receiver[%d].show_rx",rx->channel);
  sprintf(value,"%i", rx->show_rx);
  setProperty(name,value);

  sprintf(name,"receiver[%d].waterfall_color_theme",rx->channel);
  sprintf(value,"%d",rx->waterfall_color_theme);
  setProperty(name,value);

  // A live receiver is active; receiver_close overrides this with 0 so the
  // settings are retained but the receiver is not auto-recreated on start-up.
  sprintf(name,"receiver[%d].active",rx->channel);
  setProperty(name,"1");
}

void receiver_restore_state(RECEIVER *rx) {
  char name[80];
  char *value;
  int i;

  sprintf(name,"receiver[%d].adc",rx->channel);
  value=getProperty(name);
  if(value) rx->adc=atol(value);

  sprintf(name,"receiver[%d].sample_rate",rx->channel);
  value=getProperty(name);
  if(value) rx->sample_rate=atol(value);
  sprintf(name,"receiver[%d].dsp_rate",rx->channel);
  value=getProperty(name);
  if(value) rx->dsp_rate=atol(value);
  sprintf(name,"receiver[%d].output_rate",rx->channel);
  value=getProperty(name);
  if(value) rx->output_rate=atol(value);

  sprintf(name,"receiver[%d].frequency_a",rx->channel);
  value=getProperty(name);
  if(value) rx->frequency_a=atol(value);
  sprintf(name,"receiver[%d].lo_a",rx->channel);
  value=getProperty(name);
  if(value) rx->lo_a=atol(value);
  sprintf(name,"receiver[%d].lo_tx",rx->channel);
  value=getProperty(name);
  if(value) rx->lo_tx=atol(value);
  sprintf(name,"receiver[%d].error_tx",rx->channel);
  value=getProperty(name);
  if(value) rx->error_tx=atol(value);
  sprintf(name,"receiver[%d].tx_track_rx",rx->channel);
  value=getProperty(name);
  if(value) rx->tx_track_rx=atoi(value);
  sprintf(name,"receiver[%d].error_a",rx->channel);
  value=getProperty(name);
  if(value) rx->error_a=atol(value);
  sprintf(name,"receiver[%d].band_a",rx->channel);
  value=getProperty(name);
  if(value) rx->band_a=atoi(value);
  sprintf(name,"receiver[%d].mode_a",rx->channel);
  value=getProperty(name);
  if(value) rx->mode_a=atoi(value);
  sprintf(name,"receiver[%d].filter_a",rx->channel);
  value=getProperty(name);
  if(value) rx->filter_a=atoi(value);
  for(int i=0;i<MODES;i++) {
    sprintf(name,"receiver[%d].mode_filter[%d]",rx->channel,i);
    value=getProperty(name);
    if(value) rx->mode_filter[i]=atoi(value);
  }
  // Older configs saved only filter_a; make sure the current mode's remembered
  // filter agrees with it so the first mode switch doesn't reset the bandwidth.
  if(rx->mode_a>=0 && rx->mode_a<MODES) rx->mode_filter[rx->mode_a]=rx->filter_a;
  for(int i=0;i<MODES;i++) {
    sprintf(name,"receiver[%d].mode_agc[%d]",rx->channel,i);
    value=getProperty(name);
    if(value) rx->mode_agc[i]=atoi(value);
  }
  sprintf(name,"receiver[%d].filter_low_a",rx->channel);
  value=getProperty(name);
  if(value) rx->filter_low_a=atoi(value);
  sprintf(name,"receiver[%d].filter_high_a",rx->channel);
  value=getProperty(name);
  if(value) rx->filter_high_a=atoi(value);
  sprintf(name,"receiver[%d].frequency_b",rx->channel);
  value=getProperty(name);
  if(value) rx->frequency_b=atol(value);
  sprintf(name,"receiver[%d].lo_b",rx->channel);
  value=getProperty(name);
  if(value) rx->lo_b=atol(value);
  sprintf(name,"receiver[%d].error_b",rx->channel);
  value=getProperty(name);
  if(value) rx->error_b=atol(value);
#ifdef USE_VFO_B_MODE_AND_FILTER
  sprintf(name,"receiver[%d].band_b",rx->channel);
  value=getProperty(name);
  if(value) rx->band_b=atoi(value);
  sprintf(name,"receiver[%d].mode_b",rx->channel);
  value=getProperty(name);
  if(value) rx->mode_b=atoi(value);
  sprintf(name,"receiver[%d].filter_b",rx->channel);
  value=getProperty(name);
  if(value) rx->filter_b=atoi(value);
  sprintf(name,"receiver[%d].filter_low_b",rx->channel);
  value=getProperty(name);
  if(value) rx->filter_low_b=atoi(value);
  sprintf(name,"receiver[%d].filter_high_b",rx->channel);
  value=getProperty(name);
  if(value) rx->filter_high_b=atoi(value);
#endif

  sprintf(name,"receiver[%d].ctun",rx->channel);
  value=getProperty(name);
  if(value) rx->ctun=atoi(value);
  sprintf(name,"receiver[%d].ctun_offset",rx->channel);
  value=getProperty(name);
  if(value) rx->ctun_offset=atol(value);
  sprintf(name,"receiver[%d].ctun_frequency",rx->channel);
  value=getProperty(name);
  if(value) rx->ctun_frequency=atol(value);
  sprintf(name,"receiver[%d].ctun_min",rx->channel);
  value=getProperty(name);
  if(value) rx->ctun_min=atol(value);
  sprintf(name,"receiver[%d].ctun_max",rx->channel);
  value=getProperty(name);
  if(value) rx->ctun_max=atol(value);

  /* freetune restore */
  sprintf(name,"receiver[%d].freetune",rx->channel);
  value=getProperty(name);
  if(value) rx->freetune=atoi(value);

  sprintf(name,"receiver[%d].qo100_beacon",rx->channel);
  value=getProperty(name);
  if(value) rx->qo100_beacon=atoi(value);

  sprintf(name,"receiver[%d].split",rx->channel);
  value=getProperty(name);
  if(value) rx->split=atoi(value);

  sprintf(name,"receiver[%d].remote_audio",rx->channel);
  value=getProperty(name);
  if(value) rx->remote_audio=atoi(value);

  sprintf(name,"receiver[%d].local_audio",rx->channel);
  value=getProperty(name);
  if(value) rx->local_audio=atoi(value);
  sprintf(name,"receiver[%d].audio_name",rx->channel);
  value=getProperty(name);
  if(value) {
    rx->audio_name=g_new0(gchar,strlen(value)+1);
    strcpy(rx->audio_name,value);
  }
  sprintf(name,"receiver[%d].output_index",rx->channel);
  value=getProperty(name);
  if(value) rx->output_index=atoi(value);
  sprintf(name,"receiver[%d].mute_when_not_active",rx->channel);
  value=getProperty(name);
  if(value) rx->mute_when_not_active=atoi(value);
  sprintf(name,"receiver[%d].local_audio_buffer_size",rx->channel);
  value=getProperty(name);
  if(value) rx->local_audio_buffer_size=atoi(value);
  sprintf(name,"receiver[%d].local_audio_latency",rx->channel);
  value=getProperty(name);
  if(value) rx->local_audio_latency=atoi(value);
  sprintf(name,"receiver[%d].audio_channels",rx->channel);
  value=getProperty(name);
  if(value) rx->audio_channels=atoi(value);
  // Sub-RX: remember whether it was on and its L/R-mix. The WDSP sub-channel is
  // (re)created at the end of create_receiver (it needs the main channel open),
  // so the restored "on" state is parked in subrx_restore_pending — NOT in
  // subrx_enable, which frequency_changed()/process_rx_buffer() act on and which
  // would dereference the still-NULL rx->subrx during the rest of startup.
  sprintf(name,"receiver[%d].subrx_enable",rx->channel);
  value=getProperty(name);
  if(value) rx->subrx_restore_pending=atoi(value);
  sprintf(name,"receiver[%d].subrx_mix",rx->channel);
  value=getProperty(name);
  if(value) rx->subrx_mix=atoi(value);

  sprintf(name,"receiver[%d].step",rx->channel);
  value=getProperty(name);
  if(value) rx->step=atol(value);
  sprintf(name,"receiver[%d].zoom",rx->channel);
  value=getProperty(name);
  if(value) rx->zoom=atoi(value);
  sprintf(name,"receiver[%d].pan",rx->channel);
  value=getProperty(name);
  if(value) rx->pan=atoi(value);

  sprintf(name,"receiver[%d].volume",rx->channel);
  value=getProperty(name);
  if(value) rx->volume=atof(value);

  sprintf(name,"receiver[%d].mute",rx->channel);
  value=getProperty(name);
  if(value) rx->mute=atoi(value);

  sprintf(name,"receiver[%d].nr",rx->channel);
  value=getProperty(name);
  if(value) rx->nr=atoi(value);
  sprintf(name,"receiver[%d].nr2",rx->channel);
  value=getProperty(name);
  if(value) rx->nr2=atoi(value);
  sprintf(name,"receiver[%d].nr3",rx->channel);
  value=getProperty(name);
  if(value) rx->nr3=atoi(value);
  sprintf(name,"receiver[%d].nr4",rx->channel);
  value=getProperty(name);
  if(value) rx->nr4=atoi(value);
  sprintf(name,"receiver[%d].nr4_reduction",rx->channel);
  value=getProperty(name);
  if(value) rx->nr4_reduction=atof(value);
  sprintf(name,"receiver[%d].nr4_smoothing",rx->channel);
  value=getProperty(name);
  if(value) rx->nr4_smoothing=atof(value);
  sprintf(name,"receiver[%d].nr4_whitening",rx->channel);
  value=getProperty(name);
  if(value) rx->nr4_whitening=atof(value);
  sprintf(name,"receiver[%d].nr4_rescale",rx->channel);
  value=getProperty(name);
  if(value) rx->nr4_rescale=atof(value);
  sprintf(name,"receiver[%d].nr4_postfilter",rx->channel);
  value=getProperty(name);
  if(value) rx->nr4_postfilter=atof(value);
  sprintf(name,"receiver[%d].nb",rx->channel);
  value=getProperty(name);
  if(value) rx->nb=atoi(value);
  sprintf(name,"receiver[%d].nb2",rx->channel);
  value=getProperty(name);
  if(value) rx->nb2=atoi(value);
  sprintf(name,"receiver[%d].anf",rx->channel);
  value=getProperty(name);
  if(value) rx->anf=atoi(value);
  sprintf(name,"receiver[%d].snb",rx->channel);
  value=getProperty(name);
  if(value) rx->snb=atoi(value);

  sprintf(name,"receiver[%d].squelch",rx->channel);
  value=getProperty(name);
  if(value) rx->squelch=atof(value);
  if(rx->squelch<0.0) rx->squelch=0.0;
  if(rx->squelch>1.0) rx->squelch=1.0;
  for(i=0;i<MODES;i++) {
    sprintf(name,"receiver[%d].mode_squelch[%d]",rx->channel,i);
    value=getProperty(name);
    if(value) rx->mode_squelch[i]=atof(value);
    if(rx->mode_squelch[i]<0.0) rx->mode_squelch[i]=0.0;
    if(rx->mode_squelch[i]>1.0) rx->mode_squelch[i]=1.0;
  }
  // A config written before per-mode squelch existed has only the scalar: seed
  // the current mode from it so the first mode change doesn't clobber it (same
  // migration as mode_filter/mode_agc below).
  if(rx->mode_a>=0 && rx->mode_a<MODES) rx->mode_squelch[rx->mode_a]=rx->squelch;
  sprintf(name,"receiver[%d].amsq_min_db",rx->channel);
  value=getProperty(name);
  if(value) rx->amsq_min_db=atof(value);
  sprintf(name,"receiver[%d].amsq_max_db",rx->channel);
  value=getProperty(name);
  if(value) rx->amsq_max_db=atof(value);
  sprintf(name,"receiver[%d].amsq_tail",rx->channel);
  value=getProperty(name);
  if(value) rx->amsq_tail=atof(value);
  sprintf(name,"receiver[%d].apf_enable",rx->channel);
  value=getProperty(name);
  if(value) rx->apf_enable=atoi(value);
  sprintf(name,"receiver[%d].apf_bw",rx->channel);
  value=getProperty(name);
  if(value) rx->apf_bw=atof(value);
  sprintf(name,"receiver[%d].apf_gain",rx->channel);
  value=getProperty(name);
  if(value) rx->apf_gain=atof(value);

  sprintf(name,"receiver[%d].notches",rx->channel);
  value=getProperty(name);
  if(value) rx->notches=atoi(value);
  if(rx->notches<0) rx->notches=0;
  if(rx->notches>MAX_NOTCHES) rx->notches=MAX_NOTCHES;
  sprintf(name,"receiver[%d].notch_default_width",rx->channel);
  value=getProperty(name);
  if(value) rx->notch_default_width=atof(value);
  if(rx->notch_default_width<NOTCH_MIN_WIDTH) rx->notch_default_width=NOTCH_MIN_WIDTH;
  if(rx->notch_default_width>NOTCH_MAX_WIDTH) rx->notch_default_width=NOTCH_MAX_WIDTH;
  for(i=0;i<rx->notches;i++) {
    sprintf(name,"receiver[%d].notch[%d].fcenter",rx->channel,i);
    value=getProperty(name);
    if(value) rx->notch[i].fcenter=atof(value);
    sprintf(name,"receiver[%d].notch[%d].fwidth",rx->channel,i);
    value=getProperty(name);
    if(value) rx->notch[i].fwidth=atof(value);
    sprintf(name,"receiver[%d].notch[%d].active",rx->channel,i);
    value=getProperty(name);
    if(value) rx->notch[i].active=atoi(value);
    sprintf(name,"receiver[%d].notch[%d].af",rx->channel,i);
    value=getProperty(name);
    rx->notch[i].af=value?atoi(value):FALSE;   // pre-AF configs: RF-anchored
    sprintf(name,"receiver[%d].notch[%d].af_offset",rx->channel,i);
    value=getProperty(name);
    rx->notch[i].af_offset=value?atof(value):0.0;
  }

  sprintf(name,"receiver[%d].agc",rx->channel);
  value=getProperty(name);
  if(value) rx->agc=atoi(value);
  // Older configs saved only agc; keep the current mode's remembered AGC in step
  // so the first mode switch doesn't clobber it.
  if(rx->mode_a>=0 && rx->mode_a<MODES) rx->mode_agc[rx->mode_a]=rx->agc;
  sprintf(name,"receiver[%d].agc_gain",rx->channel);
  value=getProperty(name);
  if(value) rx->agc_gain=atof(value);
  sprintf(name,"receiver[%d].agc_slope",rx->channel);
  value=getProperty(name);
  if(value) rx->agc_slope=atof(value);
  sprintf(name,"receiver[%d].agc_hang_threshold",rx->channel);
  value=getProperty(name);
  if(value) rx->agc_hang_threshold=atof(value);

  sprintf(name,"receiver[%d].enable_equalizer",rx->channel);
  value=getProperty(name);
  if(value) rx->enable_equalizer=atoi(value);
  for(i=0;i<11;i++) {
    sprintf(name,"receiver[%d].equalizer[%d]",rx->channel,i);
    value=getProperty(name);
    if(value) rx->equalizer[i]=atoi(value);
  }

  sprintf(name,"receiver[%d].rit_enabled",rx->channel);
  value=getProperty(name);
  if(value) rx->rit_enabled=atoi(value);
  sprintf(name,"receiver[%d].rit",rx->channel);
  value=getProperty(name);
  if(value) rx->rit=atol(value);
  sprintf(name,"receiver[%d].rit_step",rx->channel);
  value=getProperty(name);
  if(value) rx->rit_step=atol(value);

  sprintf(name,"receiver[%d].bpsk_enable",rx->channel);
  value=getProperty(name);
  if(value) rx->bpsk_enable=atoi(value);

  sprintf(name,"receiver[%d].fps",rx->channel);
  value=getProperty(name);
  if(value) rx->fps=atoi(value);

  sprintf(name,"receiver[%d].meter_smoothing",rx->channel);
  value=getProperty(name);
  if(value) rx->meter_smoothing=atoi(value);

  sprintf(name,"receiver[%d].display_average_time",rx->channel);
  value=getProperty(name);
  if(value) rx->display_average_time=atof(value);

  sprintf(name,"receiver[%d].panadapter_low",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_low=atoi(value);
  sprintf(name,"receiver[%d].panadapter_high",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_high=atoi(value);
  sprintf(name,"receiver[%d].panadapter_step",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_step=atoi(value);
  sprintf(name,"receiver[%d].panadapter_filled",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_filled=atoi(value);
  sprintf(name,"receiver[%d].panadapter_gradient",rx->channel);

  sprintf(name,"receiver[%d].panadapter_single_color",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_single_color=atoi(value);

  value=getProperty(name);
  if(value) rx->panadapter_gradient=atoi(value);
  sprintf(name,"receiver[%d].panadapter_agc_line",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_agc_line=atoi(value);

  sprintf(name,"receiver[%d].display_detector_mode",rx->channel);
  value=getProperty(name);
  if(value) rx->display_detector_mode=atoi(value);
  sprintf(name,"receiver[%d].display_average_mode",rx->channel);
  value=getProperty(name);
  if(value) rx->display_average_mode=atoi(value);
  sprintf(name,"receiver[%d].panadapter_peak_hold",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_peak_hold=atoi(value);
  sprintf(name,"receiver[%d].panadapter_peak_decay",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_peak_decay=atoi(value);
  sprintf(name,"receiver[%d].panadapter_histogram",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_histogram=atoi(value);
  sprintf(name,"receiver[%d].panadapter_histogram_decay",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_histogram_decay=atoi(value);
  sprintf(name,"receiver[%d].panadapter_phase",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_phase=atoi(value);
  sprintf(name,"receiver[%d].panadapter_phase_mode",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_phase_mode=atoi(value);
  sprintf(name,"receiver[%d].panadapter_phase_gain",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_phase_gain=atoi(value);
  sprintf(name,"receiver[%d].panadapter_phase_source",rx->channel);
  value=getProperty(name);
  if(value) rx->panadapter_phase_source=atoi(value);

  sprintf(name,"receiver[%d].waterfall_low",rx->channel);
  value=getProperty(name);
  if(value) rx->waterfall_low=atoi(value);
  sprintf(name,"receiver[%d].waterfall_high",rx->channel);
  value=getProperty(name);
  if(value) rx->waterfall_high=atoi(value);
  sprintf(name,"receiver[%d].waterfall_automatic",rx->channel);
  value=getProperty(name);
  if(value) rx->waterfall_automatic=atoi(value);
  sprintf(name,"receiver[%d].waterfall_ft8_marker",rx->channel);
  value=getProperty(name);
  if(value) rx->waterfall_ft8_marker=atoi(value);

  sprintf(name,"receiver[%d].waterfall_color_theme",rx->channel);
  value=getProperty(name);
  if(value) rx->waterfall_color_theme=atoi(value);

  sprintf(name,"receiver[%d].split",rx->channel);
  value=getProperty(name);
  if(value) rx->split=atoi(value);
  sprintf(name,"receiver[%d].duplex",rx->channel);
  value=getProperty(name);
  if(value) rx->duplex=atoi(value);
  sprintf(name,"receiver[%d].mute_while_transmitting",rx->channel);
  value=getProperty(name);
  if(value) rx->mute_while_transmitting=atoi(value);

  sprintf(name,"receiver[%d].rigctl_port",rx->channel);
  value=getProperty(name);
  if(value) rx->rigctl_port=atoi(value);
  sprintf(name,"receiver[%d].rigctl_enable",rx->channel);
  value=getProperty(name);
  if(value) rx->rigctl_enable=atoi(value);
  sprintf(name,"receiver[%d].rigctl_serial_enable",rx->channel);
  value=getProperty(name);
  if(value) rx->rigctl_serial_enable=atoi(value);
  sprintf(name,"receiver[%d].rigctl_serial_port",rx->channel);
  value=getProperty(name);
  if(value) strcpy(rx->rigctl_serial_port,value);
  sprintf(name,"receiver[%d].rigctl_debug",rx->channel);
  value=getProperty(name);
  if(value) rx->rigctl_debug=atoi(value);

  sprintf(name,"receiver[%d].paned_position",rx->channel);
  value=getProperty(name);
  if(value) rx->paned_position=atoi(value);

  sprintf(name,"receiver[%d].paned_percent",rx->channel);
  value=getProperty(name);
  if(value) rx->paned_percent=atof(value);

  sprintf(name,"receiver[%d].show_panadapter",rx->channel);
  value=getProperty(name);
  if(value) rx->show_panadapter=atoi(value);

  sprintf(name,"receiver[%d].show_rx",rx->channel);
  value=getProperty(name);
  if(value) rx->show_rx=atoi(value);

  sprintf(name,"receiver[%d].waterfall_color_theme",rx->channel);
  value=getProperty(name);
  if(value) rx->waterfall_color_theme=atoi(value);
}
