/* Copyright (C)
*
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
#ifdef FT8
#include "ft8_decoder.h"
#include "ft8_waterfall.h"
#endif
#ifdef SSTV
#include "sstv_decoder.h"
#include "wefax_decoder.h"
#endif
// Shared "a decoder is tapping this RX's audio" machinery (unity WDSP panel
// gain + software listen-volume) is used by both the FT8/FT4 and the SSTV
// decoders, so it is compiled whenever either is enabled.
#if defined(FT8) || defined(SSTV)
#define DECODERS 1
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
  for(i=0;i<4;i++) {
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

  sprintf(name,"receiver[%d].agc",rx->channel);
  value=getProperty(name);
  if(value) rx->agc=atoi(value);
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
  for(i=0;i<4;i++) {
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

void receiver_xvtr_changed(RECEIVER *rx) {
}

// WDSP iobuff ring depth as a function of the receive span (sample rate).
// A deeper ring keeps wide reception (WFM at high sample rates) glitch-free but
// adds a fixed audio latency, so scale it with the span instead of using one
// blanket value: narrow spans (<=384k, which covers every HPSDR rate) keep the
// original low-latency depth of 2, wider spans step up 4/8/16.  Must be applied
// via SetDSPMult() BEFORE any OpenChannel/SetAllRates so create_iobuffs captures
// it for the (re)built ring.
int rx_ring_depth(int sample_rate) {
  if(sample_rate <= 384000)  return 2;
  if(sample_rate <= 768000)  return 4;
  if(sample_rate <= 1536000) return 8;
  return 16;
}

// Position the zoomed panadapter/waterfall pan window so the cursor (the
// freetune/ctun listening frequency, i.e. frequency_a + ctun_offset) sits in
// the middle of the visible area instead of the span centre. At zoom==1 there
// is no pan headroom, so recentring there needs the span centre itself to move
// (done on a bandwidth change); this only slides the window when zoomed in.
// For plain tuning ctun_offset==0, so this reduces to centring on the span.
static void center_pan_on_cursor(RECEIVER *rx) {
  if(rx->zoom<=1) {
    rx->pan=0;
    return;
  }
  double hz_per_pixel=(double)rx->sample_rate/(double)rx->pixels;
  int cursor_px=(rx->pixels/2)+(int)((double)(rx->ctun_frequency-rx->frequency_a)/hz_per_pixel);
  rx->pan=cursor_px-(rx->panadapter_width/2);
  if(rx->pan<0) rx->pan=0;
  if(rx->pan>(rx->pixels-rx->panadapter_width)) rx->pan=rx->pixels-rx->panadapter_width;
}

void receiver_change_sample_rate(RECEIVER *rx,int sample_rate) {
log_info("receiver_change_sample_rate: from %d to %d radio=%d\n",rx->sample_rate,sample_rate,radio->sample_rate);
  g_mutex_lock(&rx->mutex);
  SetChannelState(rx->channel,0,1);
  g_free(rx->audio_output_buffer);
  rx->audio_output_buffer=NULL;
  rx->sample_rate=sample_rate;
  rx->output_samples=rx->buffer_size/(rx->sample_rate/48000);
  rx->audio_output_buffer=g_new0(gdouble,2*rx->output_samples);
  rx->hz_per_pixel=(double)rx->sample_rate/(double)rx->samples;
  //SetInputSamplerate(rx->channel, sample_rate);
  // Keep the wide DSP rate while in WFM (see set_mode); 48 kHz otherwise.
  rx->dsp_rate=(rx->mode_a==WFM)?rx->sample_rate:48000;
  // Re-match the iobuff ring depth to the new span before the rebuild below.
  SetDSPMult(rx_ring_depth(rx->sample_rate));
  SetAllRates(rx->channel,rx->sample_rate,rx->dsp_rate,48000);

  receiver_init_analyzer(rx);
  SetEXTANBSamplerate (rx->channel, sample_rate);
  SetEXTNOBSamplerate (rx->channel, sample_rate);
log_info("receiver_change_sample_rate: channel=%d rate=%d buffer_size=%d output_samples=%d\n",rx->channel, rx->sample_rate, rx->buffer_size, rx->output_samples);

  /* Recentre the freetune span on the current listening frequency (the cursor)
     so it stays in the middle when the bandwidth changes. Moving the span centre
     retunes the hardware LO to the cursor; frequency_changed() (called after the
     mutex is released, below) re-derives the now-zero digital shift and issues
     the retune. */
  if(rx->freetune) {
    rx->frequency_a = rx->ctun_frequency;
    rx->ctun_min = rx->frequency_a - (rx->sample_rate / 2);
    rx->ctun_max = rx->frequency_a + (rx->sample_rate / 2);
  }

#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    soapy_protocol_change_sample_rate(rx,sample_rate);
/*
    rx->resample_step=radio->sample_rate/rx->sample_rate;
log_info("receiver_change_sample_rate: resample_step=%d\n",rx->resample_step);
*/
  }
#endif

  SetChannelState(rx->channel,1,0);
  g_mutex_unlock(&rx->mutex);

  /* Freetune moved the span centre onto the cursor above: retune the LO and
     re-zero the digital shift (frequency_changed), then keep the cursor centred
     in the zoomed view. */
  if(rx->freetune) {
    frequency_changed(rx);
    center_pan_on_cursor(rx);
    update_vfo(rx);
  }

  receiver_update_title(rx);
}

void update_noise(RECEIVER *rx) {
  SetEXTANBRun(rx->channel, rx->nb);
  SetEXTNOBRun(rx->channel, rx->nb2);
  SetRXAANRRun(rx->channel, rx->nr);
  SetRXAEMNRRun(rx->channel, rx->nr2);
  SetRXAANFRun(rx->channel, rx->anf);
  SetRXASNBARun(rx->channel, rx->snb);
  update_vfo(rx);
}

void receiver_close(RECEIVER *rx) {
  // Keep at least one receiver — never leave the radio headless
  if(radio->receivers<=1) return;

  g_source_remove(rx->update_timer_id);
  if(radio->dialog!=NULL) {
    gtk_window_destroy(GTK_WINDOW(radio->dialog));
    radio->dialog=NULL;
  }
  if(rx->bookmark_dialog!=NULL) {
    gtk_window_destroy(GTK_WINDOW(rx->bookmark_dialog));
    rx->bookmark_dialog=NULL;
  }
  // Persist this receiver's current settings so they survive the close, then
  // mark the slot inactive: the settings are kept but the receiver is not
  // auto-recreated on the next start-up (and are restored if it is re-added).
  {
    char name[80];
    receiver_save_state(rx);
    sprintf(name,"receiver[%d].active",rx->channel);
    setProperty(name,"0");
  }
  delete_receiver(rx);
  // delete_receiver has set radio->receiver[i]=NULL for this rx; if it was the
  // active receiver, hand focus to the first remaining live receiver.
  if(radio->active_receiver==rx) {
    int i;
    for(i=0;i<radio->discovered->supported_receivers;i++) {
      if(radio->receiver[i]!=NULL) { radio->active_receiver=radio->receiver[i]; break; }
    }
  }
  // Rebuild the stack: re-lays out the surviving panels (with GtkPaned dividers)
  // and destroys the just-closed panel, now orphaned in the old layout tree.
  radio_rebuild_rx_stack(radio);
}

#ifdef FT8
// TRUE when a panadapter gesture should place the FT8 TX offset rather than
// tune the RX: Shift held while the active receiver is in DIGU.
static gboolean ft8_tx_offset_gesture(RECEIVER *rx, guint state) {
  return rx->mode_a==DIGU && (state & GDK_SHIFT_MASK);
}

// Set the FT8 TX audio offset from a panadapter x-coordinate, clamped to the
// FT8 passband.  Purely sets dial+offset inside the *static* display window — it
// never tunes/pans the RX, so the operator can drop TX on a clear frequency.
static void ft8_set_tx_offset_from_x(RECEIVER *rx, double ex) {
  long long half=(long long)rx->sample_rate/2LL;
  long long min_display=(rx->frequency_a - half)+(long long)((double)rx->pan*rx->hz_per_pixel);
  double clicked=(double)min_display + ex*rx->hz_per_pixel;
  int off=(int)(clicked - (double)rx->frequency_a);
  if(off<200) off=200;
  if(off>2800) off=2800;
  radio->ft8_tx_offset=off;
  if(rx->panadapter!=NULL) gtk_widget_queue_draw(rx->panadapter);
}
#endif

// GTK4: GtkGestureClick "pressed" handler (button/state from the gesture).
void receiver_pressed_cb(GtkGestureClick *gesture, int n_press, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  radio->active_receiver=rx;
  guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  switch(button) {
    case 1: // left button
#ifdef FT8
      // Shift+click in DIGU sets the FT8 TX audio offset (dial+offset) without
      // tuning the dial.  Mirrors WSJT-X double-click-to-set-Tx.  The matching
      // guards in the release/motion handlers keep the RX from retuning.
      if(ft8_tx_offset_gesture(rx,state)) {
        ft8_set_tx_offset_from_x(rx,ex);
        rx->last_x=(int)ex;
        rx->has_moved=FALSE;
        rx->is_panning=FALSE;
        return;
      }
#endif
      //if(!rx->locked) {
        rx->last_x=(int)ex;
        rx->has_moved=FALSE;
        if(rx->zoom>1 && ey>=rx->panadapter_height-20) {
          rx->is_panning=TRUE;
        }
      //}
      break;
    case 3: // right button
      if(radio->dialog==NULL) {
        int i;
        for(i=0;i<radio->discovered->supported_receivers;i++) {
          if(rx==radio->receiver[i]) {
            break;
          }
        }
        radio->dialog=create_configure_dialog(radio,i+rx_base);
        update_receiver_dialog(rx);
      }
      break;
  }
}

void update_frequency(RECEIVER *rx) {
  if(!rx->locked) {
    if(rx->vfo!=NULL) {
      update_vfo(rx);
    }
    if(radio->transmitter!=NULL) {
      if(radio->transmitter->rx==rx) {
        update_tx_panadapter(radio);
      }
    }
  }
}

long long receiver_move_a(RECEIVER *rx, long long hz, gboolean round) {
  long long delta = 0LL;
  if(!rx->locked) {
    /* Stop scroll to negative number */
    if(rx->frequency_a - hz < 0) return 0;

    /* freetune forces ctun-like behaviour but clamps to span */
    gboolean use_ctun = rx->ctun || rx->freetune;

    if(use_ctun) {
      delta = rx->ctun_frequency;
      rx->ctun_frequency = rx->ctun_frequency + hz;

      if(rx->freetune) {
        long long span_half = (long long)(rx->sample_rate / 2);
        long long span_min  = rx->frequency_a - span_half;
        long long span_max  = rx->frequency_a + span_half;

        long long threshold = span_half * 15 / 100;

        if(rx->ctun_frequency > span_max - threshold) {
          long long shift = rx->ctun_frequency - (span_max - threshold);
          rx->frequency_a += shift;
          rx->ctun_min = rx->frequency_a - span_half;
          rx->ctun_max = rx->frequency_a + span_half;
        } else if(rx->ctun_frequency < span_min + threshold) {
          long long shift = (span_min + threshold) - rx->ctun_frequency;
          rx->frequency_a -= shift;
          rx->ctun_min = rx->frequency_a - span_half;
          rx->ctun_max = rx->frequency_a + span_half;
        } else {
          if(rx->ctun_frequency < span_min) rx->ctun_frequency = span_min;
          if(rx->ctun_frequency > span_max) rx->ctun_frequency = span_max;
        }
      }

      if(round && (rx->mode_a != CWL && rx->mode_a != CWU)) {
        rx->ctun_frequency = (rx->ctun_frequency / rx->step) * rx->step;
      }
      delta = rx->ctun_frequency - delta;
    } else {
      delta = rx->frequency_a;
      rx->frequency_a = rx->frequency_a - hz;
      if(round && (rx->mode_a != CWL && rx->mode_a != CWU)) {
        rx->frequency_a = (rx->frequency_a / rx->step) * rx->step;
      }
      delta = rx->frequency_a - delta;
    }
  }
  return delta;
}

void receiver_move_b(RECEIVER *rx,long long hz,gboolean b_only,gboolean round) {
  if(!rx->locked) {
    // Stop scroll to negative number
    if ((rx->frequency_b + hz) <= 0) return;
    long long f=rx->frequency_b;
    switch(rx->split) {
      case SPLIT_OFF:
        if(round) {
          rx->frequency_b=(rx->frequency_b+hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b+hz;
        }
        update_frequency(rx);
        break;
      case SPLIT_ON:
        if(round) {
          rx->frequency_b=(rx->frequency_b+hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b+hz;
        }
        update_frequency(rx);
        break;
      case SPLIT_SAT:
        if(round) {
          rx->frequency_b=(rx->frequency_b+hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b+hz;
        }
	if(rx->subrx!=NULL) {
  	  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
          rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);
          if(rx->frequency_b<rx->ctun_min || rx->frequency_b>rx->ctun_max) {
            rx->frequency_b=f;
          }
	}
        if(!b_only) {
          receiver_move_a(rx,hz,round);
          frequency_changed(rx);
        }
        update_frequency(rx);
        break;
      case SPLIT_RSAT:
        if(round) {
          rx->frequency_b=(rx->frequency_b-hz)/rx->step*rx->step;
        } else {
          rx->frequency_b=rx->frequency_b-hz;
        }
	if(rx->subrx!=NULL) {
  	  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
          rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);
          if(rx->frequency_b<rx->ctun_min || rx->frequency_b>rx->ctun_max) {
            rx->frequency_b=f;
          }
	}
        if(!b_only) {
          receiver_move_a(rx,-hz,round);
          frequency_changed(rx);
          update_frequency(rx);
        }
        break;
    }

    if(rx->subrx_enable) {
      // dont allow frequency outside of passband
    }
  }
}

void receiver_move(RECEIVER *rx,long long hz,gboolean round) {
  if(!rx->locked) {
    long long delta=receiver_move_a(rx,hz,round);
    switch(rx->split) {
      case SPLIT_OFF:
        break;
      case SPLIT_ON:
        break;
      case SPLIT_SAT:
      case SPLIT_RSAT:
        receiver_move_b(rx,delta,TRUE,round);
        break;
    }

    frequency_changed(rx);
    update_frequency(rx);
  }
}

void receiver_move_to(RECEIVER *rx,long long hz) {
  long long delta;
  long long start=(long long)rx->frequency_a-(long long)(rx->sample_rate/2);
  long long offset=hz;
  long long f;

  if(!rx->locked) {
    offset=hz;

    f=start+offset+(long long)((double)rx->pan*rx->hz_per_pixel);
    f=f/rx->step*rx->step;

    double cw_offset = 0;
    if(rx->mode_a==CWL || rx->mode_a==CWU) {
      if(rx->mode_a==CWU) {
        cw_offset=-radio->cw_keyer_sidetone_frequency;
      } else {
        cw_offset=+radio->cw_keyer_sidetone_frequency;
      }
    }

    if(rx->ctun || rx->freetune) {
      delta=rx->ctun_frequency;
      rx->ctun_frequency=f + cw_offset;

      /* freetune: clamp to visible span, never move the waterfall centre */
      if(rx->freetune) {
        long long span_min = rx->frequency_a - (long long)(rx->sample_rate / 2);
        long long span_max = rx->frequency_a + (long long)(rx->sample_rate / 2);
        if(rx->ctun_frequency < span_min) rx->ctun_frequency = span_min;
        if(rx->ctun_frequency > span_max) rx->ctun_frequency = span_max;
      }

      delta=rx->ctun_frequency-delta;
    } else {
      if((rx->split==SPLIT_ON) &&
         (rx->mode_a==CWL || rx->mode_a==CWU ||
          rx->mode_a==USB || rx->mode_a==LSB)) {
        rx->frequency_b=f + cw_offset;
      } else {
        delta=rx->frequency_a;
        rx->frequency_a=f + cw_offset;
        delta=rx->frequency_a-delta;
      }
    }

    switch(rx->split) {
      case SPLIT_OFF:
        break;
      case SPLIT_ON:
        break;
      case SPLIT_SAT:
        receiver_move_b(rx,delta,TRUE,TRUE);
        break;
      case SPLIT_RSAT:
        receiver_move_b(rx,-delta,TRUE,TRUE);
        break;
    }

    frequency_changed(rx);
    update_frequency(rx);
  }
}

/*
 * receiver_set_freetune:
 *   Enable or disable freetune mode.
 *   When enabled the waterfall/panadapter centre frequency is frozen;
 *   only ctun_frequency (the VFO cursor) moves within the visible span.
 *   When disabled the radio returns to normal tuning behaviour.
 */

void receiver_set_freetune(RECEIVER *rx, gboolean enable) {
  rx->freetune = enable;
  log_info("receiver_set_freetune: channel=%d enable=%d\n", rx->channel, enable);

  if(enable) {
    if(!rx->ctun) {
      rx->ctun_frequency = rx->frequency_a;
    }
    rx->ctun_min = rx->frequency_a - (long long)(rx->sample_rate / 2);
    rx->ctun_max = rx->frequency_a + (long long)(rx->sample_rate / 2);
    if(rx->ctun_frequency < rx->ctun_min) rx->ctun_frequency = rx->ctun_min;
    if(rx->ctun_frequency > rx->ctun_max) rx->ctun_frequency = rx->ctun_max;
    /* The hardware LO is already on the current centre; remember it so we only
       retune when the span centre later moves (see frequency_changed). */
    rx->freetune_hw_frequency = rx->frequency_a;
    SetRXAShiftRun(rx->channel, 1);
  } else {
    if(!rx->ctun) {
      /* Leaving freetune: keep listening to whatever the cursor was on by
         folding the freetune offset into the real VFO, recentre the display
         (ctun_offset -> 0 so the cursor returns to the middle) and drop the
         WDSP shift. The retune to the new centre happens in frequency_changed. */
      rx->frequency_a = rx->ctun_frequency;
      rx->ctun_offset = 0;
      SetRXAShiftFreq(rx->channel, 0.0);
      SetRXAShiftRun(rx->channel, 0);
    }
  }

  /* Только если визуальная часть уже создана */
  if(rx->vfo != NULL) {
    frequency_changed(rx);
    update_frequency(rx);
  }
}

// Layout-independent test for the physical "Q" key. GTK reports event->keyval
// after the active keyboard layout, so on a Russian layout Cmd-Q arrives as the
// Cyrillic "й" and a plain keyval==GDK_KEY_q test misses it. Instead we look up
// every keyval the pressed hardware keycode produces across all layout groups
// (there is always a Latin group where the physical Q key is q) so Cmd-Q quits
// on any keyboard layout.
// GTK4: GdkKeymap is gone — map a hardware keycode to keyvals via the display.
static gboolean key_is_q(guint keyval, guint keycode) {
  if(keyval==GDK_KEY_q || keyval==GDK_KEY_Q) return TRUE;
  GdkDisplay *display=gdk_display_get_default();
  if(!display || keycode==0) return FALSE;
  GdkKeymapKey *keys=NULL;
  guint *keyvals=NULL;
  int n=0;
  gboolean found=FALSE;
  if(gdk_display_map_keycode(display,keycode,&keys,&keyvals,&n)) {
    for(int i=0;i<n;i++) {
      if(keyvals[i]==GDK_KEY_q || keyvals[i]==GDK_KEY_Q) { found=TRUE; break; }
    }
  }
  g_free(keys);
  g_free(keyvals);
  return found;
}

// GTK4: GtkEventControllerKey "key-pressed" handler (returns TRUE if handled).
gboolean receiver_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
  log_info("Pressed: %s\n", gdk_keyval_name(keyval));
  // Cmd-Q (macOS) / Ctrl-Q: clean shutdown. On the quartz backend the Command
  // key may show up as either GDK_META_MASK or GDK_MOD2_MASK, so accept both.
  // key_is_q() matches the physical Q key on any keyboard layout (e.g. Cmd-й on
  // a Russian layout).
  if(key_is_q(keyval,keycode) &&
     (state & (GDK_META_MASK|GDK_ALT_MASK|GDK_CONTROL_MASK))) {
    main_delete(NULL);
    return TRUE;
  }
  // SDR#-style digit entry: a number key while hovering a VFO digit overwrites
  // that digit in place. Only consumes the key when the cursor is over a digit.
  if(vfo_type_digit(keyval)) {
    return TRUE;
  }
  switch(keyval) {
    case GDK_KEY_space:
        set_mox(radio,TRUE);
      return TRUE;
  }
  return FALSE;
}

// GTK4: GtkEventControllerKey "key-released" handler (void).
void receiver_key_released(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
  log_info("Released: %s\n", gdk_keyval_name(keyval));
  switch(keyval) {
    case GDK_KEY_space:
      set_mox(radio,FALSE);
      break;
  }
}

// GTK4: GtkGestureClick "released" handler.
void receiver_released_cb(GtkGestureClick *gesture, int n_press, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  int x=(int)ex;
  int moved=x-rx->last_x;
  if(button==1) {
#ifdef FT8
      // FT8 TX-offset gesture (Shift+click/drag in DIGU): update the offset from
      // the release point and return — never tune/pan the RX.
      if(ft8_tx_offset_gesture(rx,state)) {
        ft8_set_tx_offset_from_x(rx,(double)x);
        rx->last_x=x;
        rx->has_moved=FALSE;
        return;
      }
#endif
      if(rx->is_panning) {
        int pan=rx->pan+(moved*rx->zoom);
        if(pan<0) {
          pan=0;
        } else if(pan>(rx->pixels-rx->panadapter_width)) {
          pan=rx->pixels-rx->panadapter_width;
        }
        rx->pan=pan;
        rx->last_x=x;
        rx->has_moved=FALSE;
        rx->is_panning=FALSE;
      } else if(!rx->locked) {
        if(rx->has_moved) {
          // drag
          /* In freetune mode, dragging only moves the VFO cursor,
             not the waterfall centre */
          receiver_move(rx,(long long)((double)(moved*rx->hz_per_pixel)),TRUE);
        } else {
          // move to this frequency
          receiver_move_to(rx,(long long)((float)x*rx->hz_per_pixel));
        }
        rx->last_x=x;
        rx->has_moved=FALSE;
    }
  }
}

// GTK4: GtkEventControllerMotion "motion" handler (state incl. button masks
// comes from the controller; cursor coords are stashed for the scroll handler).
void receiver_motion_cb(GtkEventControllerMotion *controller, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  GdkModifierType state=gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  int x=(int)ex;
  int y=(int)ey;
  rx->cursor_x=x;
  rx->cursor_y=y;
  int moved=x-rx->last_x;
#ifdef FT8
  // Shift+drag in DIGU slides the FT8 TX offset live, without tuning the RX.
  if(ft8_tx_offset_gesture(rx,state) && (state & GDK_BUTTON1_MASK)) {
    ft8_set_tx_offset_from_x(rx,(double)x);
    rx->last_x=x;
    return;
  }
#endif
  if(rx->is_panning) {
    int pan=rx->pan+(moved*rx->zoom);
    if(pan<0) {
      pan=0;
    } else if(pan>(rx->pixels-rx->panadapter_width)) {
      pan=rx->pixels-rx->panadapter_width;
    }
    rx->pan=pan;
    rx->last_x=x;
  } else if(!rx->locked) {
    if((state & GDK_BUTTON1_MASK) == GDK_BUTTON1_MASK) {
      //receiver_move(rx,(long long)((double)(moved*rx->hz_per_pixel)),FALSE);
      receiver_move(rx,(long long)((double)(moved*rx->hz_per_pixel)),TRUE);
      rx->last_x=x;
      if (moved > 1 || moved < -1) {
        rx->has_moved=TRUE;
      }
    } else {
      if(x>4 && x<35) {
        gtk_widget_set_cursor_from_name(widget,"ew-resize");
      } else {
        gtk_widget_set_cursor_from_name(widget,"crosshair");
      }
    }
  }
}

// GTK4: GtkEventControllerScroll "scroll" handler. The signal carries no pointer
// position, so use the coords the motion handler stashed; dy<0 == scroll up.
gboolean receiver_scroll_cb(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  int x=rx->cursor_x;
  int y=rx->cursor_y;
  int half=rx->panadapter_height/2;
  gboolean up=dy<0;

  if(rx->zoom>1 && y>=rx->panadapter_height-20) {
    int pan;
    if(up) {
      pan=rx->pan+rx->zoom;
    } else {
      pan=rx->pan-rx->zoom;
    }

    if(pan<0) {
      pan=0;
    } else if(pan>(rx->pixels-rx->panadapter_width)) {
      pan=rx->pixels-rx->panadapter_width;
    }
    rx->pan=pan;
  } else if(!rx->locked) {
    if((x>4 && x<35) && (widget==rx->panadapter)) {
      if(up) {
        if(y<half) {
          rx->panadapter_high=rx->panadapter_high-5;
        } else {
          rx->panadapter_low=rx->panadapter_low-5;
        }
      } else {
        if(y<half) {
          rx->panadapter_high=rx->panadapter_high+5;
        } else {
          rx->panadapter_low=rx->panadapter_low+5;
        }
      }
    } else if(up) {
      if(rx->ctun || rx->freetune) {
        receiver_move(rx,rx->step,TRUE);
      } else {
        receiver_move(rx,-rx->step,TRUE);
      }
    } else {
      if(rx->ctun || rx->freetune) {
        receiver_move(rx,-rx->step,TRUE);
      } else {
        receiver_move(rx,+rx->step,TRUE);
      }
    }
  }
  return TRUE;
}

static gboolean update_timer_cb(void *data) {
  int rc=0;
  int rc2;
  RECEIVER *rx=(RECEIVER *)data;
  gboolean running=FALSE;
  gboolean have_pixels=FALSE;   // panadapter/waterfall data fetched this tick?
  gboolean draw_display;        // not TX (or duplex) -> update pan/wf/meter
  gboolean tx_needs_update=FALSE;

  // -------------------------------------------------------------------------
  // Phase 1: read the WDSP display data under rx->mutex. This runs on the GTK
  // main thread (g_timeout at rx->fps). The mutex also guards the RX/audio
  // thread's fexchange0()/Spectrum0() in full_rx_buffer(), so we must hold it
  // for as SHORT a time as possible: only the WDSP reads, never the cairo
  // rendering. Holding it across the (pixel-area-scaled) panadapter draw used
  // to block the RX thread and stutter the audio on a large window.
  // -------------------------------------------------------------------------
  g_mutex_lock(&rx->mutex);
  switch(radio->discovered->protocol) {
    case PROTOCOL_1:
      running=protocol1_is_running();
      break;
    case PROTOCOL_2:
      running=protocol2_is_running();
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      running=soapy_protocol_is_running();
      break;
#endif
    case PROTOCOL_FAKE:
      running=fake_protocol_is_running();
      break;
  }
  draw_display = (!isTransmitting(radio) || (rx->duplex));
  if(draw_display) {
    if(rx->panadapter_resize_timer==-1 && rx->pixel_samples!=NULL) {
      GetPixels(rx->channel,0,rx->pixel_samples,&rc);

      if (radio->divmixer[rx->dmix_id] != NULL) {
        if (radio->divmixer[rx->dmix_id]->calibrate_gain) {
          GetPixels(radio->divmixer[rx->dmix_id]->rx_hidden->channel, 0,
                    radio->divmixer[rx->dmix_id]->rx_hidden->pixel_samples, &rc2);
        }
      }
      have_pixels=TRUE;
    }
    rx->meter_db=GetRXAMeter(rx->channel,rx->smeter) + radio->meter_calibration;
    // Cache the AGC hang/threshold here (under the lock) so the unlocked
    // panadapter render below never calls WDSP while the RX thread runs.
    if(rx->agc!=AGC_OFF) {
      GetRXAAGCHangLevel(rx->channel, &rx->agc_hang_level);
      GetRXAAGCThresh(rx->channel, &rx->agc_thresh_level, 4096.0, (double)rx->sample_rate);
    }
  }

#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    if(radio->transmitter!=NULL && radio->discovered->info.soapy.has_temp) {
      radio->transmitter->updated=FALSE;
    }
  }
#endif
  tx_needs_update = (radio->transmitter!=NULL && !radio->transmitter->updated);
  g_mutex_unlock(&rx->mutex);

  // -------------------------------------------------------------------------
  // Phase 2: render WITHOUT the lock. rx->pixel_samples is only written above
  // (under the lock) and only from this main-thread timer, so reading it here
  // is safe, and the RX/audio thread is never blocked waiting on the draw.
  // -------------------------------------------------------------------------
  if(draw_display) {
    if(have_pixels) {
      if(rc) {
        update_rx_panadapter(rx,running);
        update_waterfall(rx);
      } else if(!running) {
        update_rx_panadapter(rx,running);
      }
    }
    update_meter(rx);
  }
  update_radio_info(rx);

  if(tx_needs_update) {
    update_tx_panadapter(radio);
  }

  return TRUE;
}

static void set_mode(RECEIVER *rx,int m) {
  rx->mode_a=m;
  // Wideband (broadcast) FM must be demodulated at the full sample rate so the
  // ~180 kHz signal fits; the stereo pilot (19/38 kHz) and RDS (57 kHz)
  // subcarriers also need this wide baseband.  All other modes run the DSP at
  // 48 kHz.  The audio output stays at 48 kHz either way.
  //
  // Drive the DSP rate off the TARGET mode, not the transition: when WFM is
  // restored from props at startup there is no other-mode->WFM transition, yet
  // the channel is opened at 48 kHz (see OpenChannel) and must still be raised.
  // rx->dsp_rate tracks the channel's real DSP rate so we only rebuild when it
  // actually changes.
  int desired_dsp = (m==WFM) ? rx->sample_rate : 48000;
  if(rx->dsp_rate != desired_dsp) {
    SetChannelState(rx->channel,0,1);
    // WFM runs the DSP at the full span, so match the ring depth to that span
    // before rebuilding the iobuffs (create_iobuffs captures it).
    SetDSPMult(rx_ring_depth(rx->sample_rate));
    SetAllRates(rx->channel, rx->sample_rate, desired_dsp, 48000);
    SetChannelState(rx->channel,1,0);
    rx->dsp_rate = desired_dsp;
  }
  SetRXAMode(rx->channel, m);
}

void set_filter(RECEIVER *rx,int low,int high) {
//fprintf(stderr,"set_filter: %d %d\n",low,high);
  if(rx->mode_a==CWL) {
    rx->filter_low_a=-radio->cw_keyer_sidetone_frequency-low;
    rx->filter_high_a=-radio->cw_keyer_sidetone_frequency+high;
  } else if(rx->mode_a==CWU) {
    rx->filter_low_a=radio->cw_keyer_sidetone_frequency-low;
    rx->filter_high_a=radio->cw_keyer_sidetone_frequency+high;
  } else {
    rx->filter_low_a=low;
    rx->filter_high_a=high;
  }
  RXASetPassband(rx->channel,(double)rx->filter_low_a,(double)rx->filter_high_a);
  if(radio->transmitter!=NULL && radio->transmitter->rx==rx && radio->transmitter->use_rx_filter) {
    transmitter_set_filter(radio->transmitter,rx->filter_low_a,rx->filter_high_a);
    update_tx_panadapter(radio);
  }
}

void set_deviation(RECEIVER *rx) {
log_info("set_deviation: %d\n",rx->deviation);
  SetRXAFMDeviation(rx->channel, (double)rx->deviation);
  set_squelch(rx);
}

void set_squelch(RECEIVER *rx) {
  double fm_sq=pow(10.0, -2.0*rx->squelch);
  SetRXAFMSQThreshold(rx->channel, fm_sq);
  // The squelch bar is clamped to [0,1], so fm_sq = 10^(-2*squelch) is at most
  // 1.0 and the old "fm_sq > 1" disable test could never fire: the FM squelch
  // stayed armed even with the bar fully down, where fm_sq=1.0 leaves
  // unmute_thresh=0.9 (see SetRXAFMSQThreshold) still gating the audio.  On a
  // weak/fake signal avnoise never drops below that, so the channel sits muted
  // and there is no way to fully open the squelch -> "no sound with squelch off".
  // Treat the bar at minimum as squelch OFF: stop the FMSQ so audio always
  // passes (open FM = hiss on an empty channel, as expected).
  if(rx->squelch <= 0.0) {
    rx->squelch_enable = FALSE;
  }
  else {
    rx->squelch_enable = TRUE;
  }
  SetRXAFMSQRun(rx->channel, rx->squelch_enable);
  log_info("Set squelch %f %f\n", rx->squelch, fm_sq);
}

void calculate_display_average(RECEIVER *rx) {
  double display_avb;
  int display_average;

  double t=0.001 * rx->display_average_time;

  display_avb = exp(-1.0 / ((double)rx->fps * t));
  display_average = max(2, (int)fmin(60, (double)rx->fps * t));
  SetDisplayAvBackmult(rx->channel, 0, display_avb);
  SetDisplayNumAverage(rx->channel, 0, display_average);
}

void receiver_fps_changed(RECEIVER *rx) {
  g_source_remove(rx->update_timer_id);
  int poll_ms=1000/(3*rx->fps); if(poll_ms<8) poll_ms=8;   // oversample vs frame production
  rx->update_timer_id=g_timeout_add(poll_ms,update_timer_cb,(gpointer)rx);
  calculate_display_average(rx);
}

void receiver_filter_changed(RECEIVER *rx,int filter) {
//fprintf(stderr,"receiver_filter_changed: %d\n",filter);
  rx->filter_a=filter;
  // Keep this mode's remembered filter in sync so the selection is restored
  // when the operator returns to the mode later.
  if(rx->mode_a>=0 && rx->mode_a<MODES) rx->mode_filter[rx->mode_a]=filter;
  if(rx->mode_a==FMN) {
    if(filter == 0) {
      rx->deviation = 2500;
    }
    else {
      rx->deviation = 5000;
    }
    switch(rx->deviation) {
      case 2500:
        set_filter(rx,-4000,4000);
        break;
      case 5000:
        set_filter(rx,-8000,8000);
        break;
    }
    set_deviation(rx);
  } else {
    FILTER *mode_filters=filters[rx->mode_a];
    FILTER *f=&mode_filters[rx->filter_a];
    set_filter(rx,f->low,f->high);
  }
  update_vfo(rx);
}

#ifdef FT8
// Add/remove the dedicated FT8 band waterfall to the RIGHT of this receiver's RF
// spectrum (~1/3 width) while it is in DIGU *and* the FT8 panel is open.  The
// spectrum lives in a GtkBox (rx->wf_hpaned).  GTK thread only.
// GTK4 note: the box no longer re-proportions the 1/3 split live on window
// resize (the "size-allocate" signal is gone); the ft8 waterfall is sized to
// 1/3 when opened and rescales its own content to whatever width it gets.
void receiver_ft8_waterfall_sync(RECEIVER *rx) {
  if(rx==NULL || rx->wf_hpaned==NULL) return;
  gboolean want = (rx->mode_a==DIGU) && radio!=NULL && radio->ft8_panel_open;
  gboolean have = (rx->ft8_waterfall!=NULL);
  if(want==have) return;
  if(want) {
    rx->ft8_waterfall=ft8_waterfall_create();
    int w=gtk_widget_get_width(rx->wf_hpaned);
    if(w>1) gtk_widget_set_size_request(rx->ft8_waterfall,w/3,-1);
    gtk_widget_set_hexpand(rx->ft8_waterfall,FALSE);
    gtk_box_append(GTK_BOX(rx->wf_hpaned),rx->ft8_waterfall);
  } else {
    // Removing the last ref disposes it, emitting "destroy" (stops its timer).
    gtk_box_remove(GTK_BOX(rx->wf_hpaned),rx->ft8_waterfall);
    rx->ft8_waterfall=NULL;
  }
}
#endif

void receiver_mode_changed(RECEIVER *rx,int mode) {
  // Remember the filter selected for the mode we are leaving, then restore the
  // one this mode used last time, so each mode keeps its own bandwidth
  // independently (changing the AM filter must not move the SSB filter).
  if(rx->mode_a>=0 && rx->mode_a<MODES) rx->mode_filter[rx->mode_a]=rx->filter_a;
  set_mode(rx,mode);
  if(mode>=0 && mode<MODES) rx->filter_a=rx->mode_filter[mode];
  log_info("mode_changed: %d\n",mode);
  if(mode != 5) {
    rx->squelch_enable = FALSE;
    SetRXAFMSQRun(rx->channel, rx->squelch_enable);
  }
  receiver_filter_changed(rx,rx->filter_a);
#ifdef DECODERS
  // Re-apply the WDSP panel gain: entering DIGU/DIGL switches the channel to
  // unity (so the decoder taps full level regardless of volume/mute), leaving it
  // restores the listen gain. rx_panel_gain() depends on the just-set mode.
  // Guard against early calls before the WDSP channel exists (create_receiver
  // sets the gain itself).
  if(rx->channel>=0) receiver_set_volume(rx);
#endif
#ifdef FT8
  // Show/hide the embedded FT8 QSO panel (and gate a second receiver) when the
  // active receiver enters/leaves DIGU.  GTK-thread context here, unlike the
  // audio-thread decoder tap in process_rx_buffer().
  if(radio!=NULL && rx==radio->active_receiver) radio_ft8_panel_sync(radio);
  receiver_ft8_waterfall_sync(rx);
#endif
#ifdef SSTV
  // Same for the SSTV image panel (meaningful in DIGU/DIGL + SSTV).
  if(radio!=NULL && rx==radio->active_receiver) radio_sstv_panel_sync(radio);
#endif
}

void receiver_band_changed(RECEIVER *rx,int band) {
#ifdef OLD_BAND
  BANDSTACK *bandstack=bandstack_get_bandstack(band);
  if(rx->band_a==band) {
    // same band selected - step to the next band stack
    rx->bandstack++;
    if(rx->bandstack>=bandstack->entries) {
      rx->bandstack=0;
    }
  } else {
    // new band - get band stack entry
    rx->band_a=band;
    rx->bandstack=bandstack->current_entry;
  }

  BANDSTACK_ENTRY *entry=&bandstack->entry[rx->bandstack];
  rx->frequency_a=entry->frequency;
  receiver_mode_changed(rx,entry->mode);
  receiver_filter_changed(rx,entry->filter);
#else
  rx->band_a=band;
  receiver_mode_changed(rx,rx->mode_a);
  receiver_filter_changed(rx,rx->filter_a);
#endif

  /* When band changes, freetune span must be re-anchored */
  if(rx->freetune) {
    rx->ctun_frequency = rx->frequency_a;
    rx->ctun_min = rx->frequency_a - (long long)(rx->sample_rate / 2);
    rx->ctun_max = rx->frequency_a + (long long)(rx->sample_rate / 2);
  }

  frequency_changed(rx);
}

#ifdef DECODERS
// TRUE when a decoder (FT8/FT4/SSTV) is selected and this receiver is in a
// digital mode (DIGU/DIGL) — i.e. its demodulated audio is being tapped by a
// decoder rather than (only) listened to. The decoder is off by default, so
// plain DIGU/DIGL listening is unaffected; the operator opts in from the
// bottom-bar Decode block (radio->decode_mode). Used to force unity WDSP panel
// gain (so the decoder always sees full level) and to apply the listen
// volume/mute in software instead — see rx_panel_gain / process_rx_buffer.
static gboolean decoder_taps_audio(RECEIVER *rx) {
  if(radio==NULL || radio->decode_mode==DECODE_OFF) return FALSE;
  // SSTV also decodes narrowband FM (VHF, e.g. the ISS on 145.800); FT8/FT4 are
  // SSB-only (DIGU/DIGL).
  if(radio->decode_mode==DECODE_SSTV)
    return rx->mode_a==DIGU || rx->mode_a==DIGL || rx->mode_a==FMN;
  // WEFAX/FT8/FT4 are HF SSB-only (DIGU/DIGL).
  return rx->mode_a==DIGU || rx->mode_a==DIGL;
}
#endif

// WDSP audio-panel gain for a receiver's channel. Normally this is the listen
// gain (volume, or 0 when muted). When a decoder is active on this RX we run the
// channel at unity instead: the decoder taps rx->audio_output_buffer, which WDSP
// scales by this gain, and the operator does not listen to the decoded signal —
// so it must decode regardless of the volume slider or mute. The listen
// volume/mute is applied to the audible output in software in
// process_rx_buffer() for that case. See receiver_set_volume().
static gdouble rx_panel_gain(RECEIVER *rx) {
#ifdef DECODERS
  if(decoder_taps_audio(rx)) return 1.0;
#endif
  return rx->mute ? 0.0 : rx->volume;
}

static void process_rx_buffer(RECEIVER *rx) {
  gdouble left_sample,right_sample;
  short left_audio_sample, right_audio_sample;
  SUBRX *subrx=(SUBRX *)rx->subrx;

  for (int i=0;i<rx->output_samples;i++) {
    // if subrx is enabled left channel is main and right channel is sub
    if(rx->subrx_enable) {
      left_sample=rx->audio_output_buffer[i*2];
      right_sample=subrx->audio_output_buffer[i*2];
    } else {
      // Rx option for left channel only, right only, or both channels
      switch (rx->audio_channels) {
        case AUDIO_STEREO: {
          left_sample = rx->audio_output_buffer[i*2];
          right_sample = rx->audio_output_buffer[(i*2)+1];
          break;
        }
        case AUDIO_LEFT_ONLY: {
          left_sample = rx->audio_output_buffer[i*2];
          right_sample = 0;
          break;
        }
        case AUDIO_RIGHT_ONLY: {
          left_sample = 0;
          right_sample = rx->audio_output_buffer[(i*2)+1];
          break;
        }
      }
    }
#ifdef DECODERS
    // When a decoder is tapping this RX the WDSP channel runs at unity (see
    // rx_panel_gain) so the decoder always sees a full-level signal; apply the
    // listen volume/mute to the audible output here instead, keeping the speaker
    // behaviour unchanged.
    if(decoder_taps_audio(rx)) {
      gdouble lg = rx->mute ? 0.0 : rx->volume;
      left_sample  *= lg;
      right_sample *= lg;
    }
#endif
    // Clamp to full scale before the 16-bit conversion below.  FM has no audio
    // AGC and WDSP's NBFM de-emphasis boosts the low end hard (a 300 Hz tone at
    // rated deviation demodulates to ~3x full scale), so the demod output swings
    // well past +/-1.0 on loud/low modulation peaks.  An out-of-range
    // (short)(x*32767) is undefined and in practice wraps a positive peak to a
    // large negative value -> a loud click/pop on the HPSDR audio path; the raw
    // float fed to audio_write() likewise hard-clips into buzzy distortion on
    // the local sound card.  Bound both here so peaks flat-top cleanly instead.
    if(left_sample  >  1.0) left_sample  =  1.0; else if(left_sample  < -1.0) left_sample  = -1.0;
    if(right_sample >  1.0) right_sample =  1.0; else if(right_sample < -1.0) right_sample = -1.0;

    left_audio_sample=(short)(left_sample*32767.0);
    right_audio_sample=(short)(right_sample*32767.0);


    if (isTransmitting(radio) && (rx->mute_while_transmitting)) {
      left_sample=0;
      right_sample=0;
    }

    if(rx->local_audio) {
      audio_write(rx,(float)left_sample,(float)right_sample);
    }

    if(radio->active_receiver==rx) {

      if ((isTransmitting(radio) || rx->remote_audio==FALSE) &&  (!rx->duplex)) {
        left_audio_sample=0;
        right_audio_sample=0;
      }
      switch(radio->discovered->protocol) {
        case PROTOCOL_1:
          if(radio->discovered->device!=DEVICE_HERMES_LITE2) {
            protocol1_audio_samples(rx,left_audio_sample,right_audio_sample);
          }
          break;
        case PROTOCOL_2:
          protocol2_audio_samples(rx,left_audio_sample,right_audio_sample);
          break;
      }
    }
  }
  if(rx->local_audio && !rx->output_started) {
    audio_start_output(rx);
  }

  // Tap the clean demodulated audio (pre listen-volume) for recording.
  recorder_audio(rx, rx->audio_output_buffer, rx->output_samples);

#ifdef FT8
  // Decoder tap: feed the active receiver's demodulated audio to the selected
  // decoder while it is in a digital mode (DIGU/DIGL).  Which decoder — if any —
  // runs is radio->decode_mode (off by default; the operator picks FT8/FT4/SSTV
  // from the bottom-bar Decode block).  Driving enable/disable from the active
  // RX's own buffer covers mode changes, active-receiver switches and start-up
  // in one place (only the active receiver ever toggles the flag, so there is no
  // cross-RX thrash).
  if(radio->active_receiver==rx) {
    gboolean digimode = (rx->mode_a==DIGU || rx->mode_a==DIGL);
    gboolean ft8_on = digimode &&
                      (radio->decode_mode==DECODE_FT8 || radio->decode_mode==DECODE_FT4);
    ft8_decoder_set_enabled(ft8_on);
    if(ft8_on) {
      ft8_decoder_set_protocol(radio->decode_mode==DECODE_FT4 ? 1 : 0);  // FT8 vs FT4
      ft8_decoder_add_audio(rx->audio_output_buffer, rx->output_samples);
    }
  }
#endif
#ifdef SSTV
  // SSTV image decoder tap: same one-place enable/disable as FT8, but its own
  // mode selector (DECODE_SSTV).  Feeds the demodulated audio to the Hilbert
  // discriminator + line decoder in sstv_decoder.c.
  if(radio->active_receiver==rx) {
    gboolean sstv_on = (rx->mode_a==DIGU || rx->mode_a==DIGL || rx->mode_a==FMN) &&
                       radio->decode_mode==DECODE_SSTV;
    sstv_decoder_set_enabled(sstv_on);
    if(sstv_on) sstv_decoder_add_audio(rx->audio_output_buffer, rx->output_samples);

    // WEFAX / HF radiofax decoder tap (same one-place enable/disable). HF USB
    // only (DIGU/DIGL).
    gboolean wefax_on = (rx->mode_a==DIGU || rx->mode_a==DIGL) &&
                        radio->decode_mode==DECODE_WEFAX;
    wefax_decoder_set_enabled(wefax_on);
    if(wefax_on) wefax_decoder_add_audio(rx->audio_output_buffer, rx->output_samples);
  }
#endif
}

// Feed the spectrum analyzer in ANALYZER_FEED_BLOCK-sized chunks so the write
// never overruns WDSP's input ring (see the contract documented in receiver.h).
// SetAnalyzer is configured with bf_sz == ANALYZER_FEED_BLOCK, so each Spectrum0
// call consumes exactly one block from `iq`.
void analyzer_feed(int channel, double *iq, int nsamples) {
  int off = 0;
  for(; off + ANALYZER_FEED_BLOCK <= nsamples; off += ANALYZER_FEED_BLOCK) {
    Spectrum0(1, channel, 0, 0, iq + 2 * off);
  }
  if(off != nsamples) {
    static gboolean warned = FALSE;
    if(!warned) {
      warned = TRUE;
      g_warning("analyzer_feed: nsamples=%d not a multiple of %d; %d samples "
                "dropped from the spectrum feed",
                nsamples, ANALYZER_FEED_BLOCK, nsamples - off);
    }
  }
}

static void full_rx_buffer(RECEIVER *rx) {
  int error;

  if(isTransmitting(radio) && (!rx->duplex)) return;

  // Tap the genuine off-air I/Q before the noise blanker mutates it in place.
  recorder_iq(rx, rx->iq_input_buffer, rx->buffer_size);
  ppm_cal_iq_feed(rx, rx->iq_input_buffer, rx->buffer_size);

  // noise blanker works on origianl IQ samples
  if(rx->nb) {
     xanbEXT (rx->channel, rx->iq_input_buffer, rx->iq_input_buffer);
  }
  if(rx->nb2) {
     xnobEXT (rx->channel, rx->iq_input_buffer, rx->iq_input_buffer);
  }

  g_mutex_lock(&rx->mutex);
  fexchange0(rx->channel, rx->iq_input_buffer, rx->audio_output_buffer, &error);
  //if(error!=0 && error!=-2) {
  if(error!=0) {
    log_error("full_rx_buffer: channel=%d fexchange0: error=%d\n",rx->channel,error);
  }

  if(rx->subrx_enable) {
    subrx_iq_buffer(rx);
  }

  analyzer_feed(rx->channel, rx->iq_input_buffer, rx->buffer_size);

  process_rx_buffer(rx);
  g_mutex_unlock(&rx->mutex);

}

void full_diviqrx_buffer(RECEIVER *rx) {
  int error;

  if(isTransmitting(radio) && (!rx->duplex)) return;

  recorder_iq(rx, rx->diviq_input_buffer, rx->buffer_size);
  ppm_cal_iq_feed(rx, rx->diviq_input_buffer, rx->buffer_size);

  // noise blanker works on origianl IQ samples
  if(rx->nb) {
     xanbEXT (rx->channel, rx->diviq_input_buffer, rx->diviq_input_buffer);
  }
  if(rx->nb2) {
     xnobEXT (rx->channel, rx->diviq_input_buffer, rx->diviq_input_buffer);
  }

  g_mutex_lock(&rx->mutex);
  fexchange0(rx->channel, rx->diviq_input_buffer, rx->audio_output_buffer, &error);
  //if(error!=0 && error!=-2) {
  if(error!=0) {
    log_error("full_rx_buffer: channel=%d fexchange0: error=%d\n",rx->channel,error);
  }

  if(rx->subrx_enable) {
    subrx_iq_buffer(rx);
  }

  analyzer_feed(rx->channel, rx->diviq_input_buffer, rx->buffer_size);
  process_rx_buffer(rx);
  g_mutex_unlock(&rx->mutex);

}

void add_iq_samples(RECEIVER *rx,double i_sample,double q_sample) {
  rx->iq_input_buffer[rx->samples*2]=i_sample;
  rx->iq_input_buffer[(rx->samples*2)+1]=q_sample;
  rx->samples=rx->samples+1;
  if(rx->samples>=rx->buffer_size) {
    // If diversity mixer active, WDSP diversity mixer works on a
    // seperate channel to the rx and must all be done pre normal RX dsp
    if (radio->divmixer[rx->dmix_id] != NULL) {
      if (radio->divmixer[rx->dmix_id]->calibrate_gain) {
        full_rx_buffer(rx);
      }
      else {
        //g_print("Mixer %d\n", mixer);
        // Diversity uses 2 rx channels, if the order of these is processed incorrectly, WDSP
        // will be working with 1 packet ahead/behind.
        // Main RX # is less than hidden rx
        int mixer = rx->dmix_id;
        if (radio->divmixer[mixer]->rx_visual->channel < radio->divmixer[mixer]->rx_hidden->channel) {
          if (rx == radio->divmixer[mixer]->rx_hidden) {
            diversity_mix_full_buffers(radio->divmixer[mixer]);
          } else {
            diversity_add_buffer(radio->divmixer[mixer]);
          }
        } else {
          // Main rx number is greater than the hidden rx
          if (rx == radio->divmixer[mixer]->rx_visual) {
            diversity_mix_full_buffers(radio->divmixer[mixer]);
          } else {
            diversity_add_buffer(radio->divmixer[mixer]);
          }
        }
      }
    } else {
      // Non diversity, normal iq packet processing
      full_rx_buffer(rx);
    }
    rx->samples=0;
  }

  if(rx->bpsk_enable && rx->bpsk!=NULL) {
    bpsk_add_iq_samples(rx->bpsk,i_sample,q_sample);
  }
}

void set_agc(RECEIVER *rx) {

  SetRXAAGCMode(rx->channel, rx->agc);
  SetRXAAGCSlope(rx->channel,rx->agc_slope);
  SetRXAAGCTop(rx->channel,rx->agc_gain);
  switch(rx->agc) {
    case AGC_OFF:
      break;
    case AGC_LONG:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,2000);
      SetRXAAGCDecay(rx->channel,2000);
      SetRXAAGCHangThreshold(rx->channel,(int)rx->agc_hang_threshold);
      break;
    case AGC_SLOW:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,1000);
      SetRXAAGCDecay(rx->channel,500);
      SetRXAAGCHangThreshold(rx->channel,(int)rx->agc_hang_threshold);
      break;
    case AGC_MEDIUM:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,0);
      SetRXAAGCDecay(rx->channel,250);
      SetRXAAGCHangThreshold(rx->channel,100);
      break;
    case AGC_FAST:
      SetRXAAGCAttack(rx->channel,2);
      SetRXAAGCHang(rx->channel,0);
      SetRXAAGCDecay(rx->channel,50);
      SetRXAAGCHangThreshold(rx->channel,100);
      break;
  }
}

void receiver_update_title(RECEIVER *rx) {
  gchar title[128];
  g_snprintf((gchar *)&title,sizeof(title),"MacHPSDR: %s Rx-%d ADC-%d %d",radio->discovered->name,rx->channel,rx->adc,rx->sample_rate);
log_info("receiver_update_title: %s\n",title);
  if(rx->window!=NULL) {
    gtk_window_set_title(GTK_WINDOW(rx->window),title);
  }
}

// GTK4: GtkEventControllerMotion "enter" (x,y,data) / "leave" (data) handlers.
static void enter (GtkEventControllerMotion *controller, double ex, double ey, gpointer user_data) {
   RECEIVER *rx=(RECEIVER *)user_data;
   GtkWidget *ebox=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
   if((ex>4 && ex<35) && (ebox==rx->panadapter)) {
     gtk_widget_set_cursor_from_name(ebox,"ew-resize");
   } else {
     gtk_widget_set_cursor_from_name(ebox,"crosshair");
   }
}

static void leave (GtkEventControllerMotion *controller, gpointer user_data) {
   GtkWidget *ebox=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
   gtk_widget_set_cursor_from_name(ebox,"default");
}

static void close_button_cb(GtkWidget *widget, gpointer data) {
  receiver_close((RECEIVER *)data);
}

static void create_visual(RECEIVER *rx) {

  rx->dialog=NULL;

  rx->window=NULL;

  receiver_update_title(rx);

  rx->table=gtk_grid_new();

  rx->vfo=create_vfo(rx);
  gtk_widget_set_hexpand(rx->vfo,TRUE);
  gtk_grid_attach(GTK_GRID(rx->table), rx->vfo, 0, 0, 3, 1);

  rx->radio_info=create_radio_info_visual(rx);
  gtk_widget_set_size_request(rx->radio_info, 170, 60);
  gtk_grid_attach(GTK_GRID(rx->table), rx->radio_info, 3, 0, 1, 1);

  rx->meter=create_meter_visual(rx);
  gtk_widget_set_size_request(rx->meter,250,60);        // resize from 154 to 300 for custom s-meter
  gtk_grid_attach(GTK_GRID(rx->table), rx->meter, 4, 0, 2, 1);

  // Additional receivers (not the primary channel 0) get a close button in the
  // top-right corner of the panel. Placed as a real grid cell rather than a
  // GtkOverlay, because the panadapter's native window would hide an overlay.
  if(rx->channel!=0) {
    GtkWidget *close_b=gtk_button_new_with_label("✕");
    gtk_widget_set_name(close_b,"vfo-close");
    gtk_widget_set_tooltip_text(close_b,"Close this receiver");
    gtk_widget_set_halign(close_b,GTK_ALIGN_END);
    gtk_widget_set_valign(close_b,GTK_ALIGN_START);
    g_signal_connect(close_b,"clicked",G_CALLBACK(close_button_cb),rx);
    gtk_grid_attach(GTK_GRID(rx->table), close_b, 6, 0, 1, 1);
  }

  rx->vpaned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_name(rx->vpaned,"rx-spectrum");   // hairline inset frame (CSS)
  gtk_widget_set_hexpand(rx->vpaned, TRUE);
  gtk_widget_set_vexpand(rx->vpaned, TRUE);
  // Breathing room between the VFO mode/filter row and the spectrum below it.
  gtk_widget_set_margin_top(rx->vpaned, 8);

  rx->panadapter=create_rx_panadapter(rx);
  // Guarantee the spectroscope a minimum height and forbid the pane from
  // shrinking it below that (shrink=FALSE) so it can never open as a sliver.
  gtk_widget_set_size_request(rx->panadapter, -1, MIN_PANADAPTER_HEIGHT);
  gtk_paned_set_start_child (GTK_PANED(rx->vpaned), rx->panadapter);
  gtk_paned_set_resize_start_child (GTK_PANED(rx->vpaned), TRUE);
  gtk_paned_set_shrink_start_child (GTK_PANED(rx->vpaned), FALSE);
  // GTK4: enter/leave come from a motion controller (crossing events are gone).
  GtkEventController *pan_cross=gtk_event_controller_motion_new();
  g_signal_connect(pan_cross,"enter",G_CALLBACK(enter),rx);
  g_signal_connect(pan_cross,"leave",G_CALLBACK(leave),rx);
  gtk_widget_add_controller(rx->panadapter,pan_cross);

  rx->waterfall=create_waterfall(rx);
  gtk_paned_set_end_child (GTK_PANED(rx->vpaned), rx->waterfall);
  gtk_paned_set_resize_end_child (GTK_PANED(rx->vpaned), TRUE);
  gtk_paned_set_shrink_end_child (GTK_PANED(rx->vpaned), TRUE);
  GtkEventController *wf_cross=gtk_event_controller_motion_new();
  g_signal_connect(wf_cross,"enter",G_CALLBACK(enter),rx);
  g_signal_connect(wf_cross,"leave",G_CALLBACK(leave),rx);
  gtk_widget_add_controller(rx->waterfall,wf_cross);

  // Wrap the RF spectrum in a horizontal GtkBox so the FT8 band waterfall can be
  // added to its right (~1/3).  (The GTK3 "windowless box" GdkWindow workaround
  // is moot in GTK4 — widgets have no child GdkWindows — but the box layout is
  // kept.)  ft8_waterfall is added/removed by receiver_ft8_waterfall_sync().
  rx->wf_hpaned=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  rx->ft8_waterfall=NULL;
  gtk_box_append(GTK_BOX(rx->wf_hpaned), rx->vpaned);
  gtk_grid_attach(GTK_GRID(rx->table), rx->wf_hpaned, 0, 1, 7, 2);
  gtk_widget_set_hexpand(rx->wf_hpaned, TRUE);
  gtk_widget_set_vexpand(rx->wf_hpaned, TRUE);

  gtk_widget_set_size_request(rx->table, -1, 180);
  // Make sure the panel grows to fill the container on window resize (otherwise
  // the vpaned can stay at its minimum and leave a gap below the waterfall).
  gtk_widget_set_hexpand(rx->table, TRUE);
  gtk_widget_set_vexpand(rx->table, TRUE);
  gtk_widget_set_valign(rx->table, GTK_ALIGN_FILL);
}

void receiver_init_analyzer(RECEIVER *rx) {
    int flp[] = {0};
    double keep_time = 0.1;
    int n_pixout=1;
    int spur_elimination_ffts = 1;
    int data_type = 1;
    int fft_size = 8192;
    int window_type = 4;
    double kaiser_pi = 14.0;
    // WDSP advances (fft_size - overlap) samples per spectrum frame, so the
    // display frame rate is sample_rate/(fft_size-overlap). A hardcoded overlap
    // (was 2048) made that rate depend on the sample rate — e.g. only ~15 fps at
    // 96 kHz — which the fps-rate GetPixels poll then aliased into a jerky
    // waterfall. Tie overlap to sample_rate/fps (the canonical linhpsdr formula)
    // so a new frame lands every 1/fps s and the waterfall scrolls smoothly.
    int overlap = (int)fmax(0.0, ceil((double)fft_size - (double)rx->sample_rate / (double)rx->fps));
    if(overlap >= fft_size) overlap = fft_size - 1;
    int clip = 0;
    int span_clip_l = 0;
    int span_clip_h = 0;
    int pixels=rx->pixels;
    int stitches = 1;
    int calibration_data_set = 0;
    double span_min_freq = 0.0;
    double span_max_freq = 0.0;

  if(rx->pixel_samples!=NULL) {
    g_free(rx->pixel_samples);
    rx->pixel_samples=NULL;
  }
  if(rx->pixels>0) {
    rx->pixel_samples=g_new0(float,rx->pixels);
    rx->hz_per_pixel=(gdouble)rx->sample_rate/(gdouble)rx->pixels;

    int max_w = fft_size + (int) fmin(keep_time * (double) rx->fps, keep_time * (double) fft_size * (double) rx->fps);

    SetAnalyzer(rx->channel,
            n_pixout,
            spur_elimination_ffts,
            data_type,
            flp,
            fft_size,
            ANALYZER_FEED_BLOCK, // transfer block: must divide the input ring; fed via analyzer_feed()
            window_type,
            kaiser_pi,
            overlap,
            clip,
            span_clip_l,
            span_clip_h,
            pixels,
            stitches,
            calibration_data_set,
            span_min_freq,
            span_max_freq,
            max_w
    );
  }

}

// WDSP's analyzer keeps fixed-size internal buffers of dMAX_PIXELS (16384)
// entries; asking SetAnalyzer for more pixels overruns them and corrupts the
// heap (blank display + broken RX/decode).  rx->pixels == panadapter_width*zoom,
// so cap the zoom to whatever keeps pixels within that ceiling.
#define WDSP_MAX_PIXELS 16384

void receiver_change_zoom(RECEIVER *rx,int zoom) {
log_info("%s: %d\n",__FUNCTION__,zoom);
  if(rx->panadapter_width>0) {
    int max_zoom=WDSP_MAX_PIXELS/rx->panadapter_width;
    if(max_zoom<1) max_zoom=1;
    if(zoom>max_zoom) zoom=max_zoom;
  }
  if(zoom<1) zoom=1;
  rx->zoom=zoom;
  rx->pixels=rx->panadapter_width*rx->zoom;
  // Centre the zoomed view on the cursor (freetune/ctun listening frequency);
  // for plain tuning ctun_offset==0 so this centres on the span, as before.
  center_pan_on_cursor(rx);
  receiver_init_analyzer(rx);
}

// Restore the saved panadapter/waterfall split. Must run only once the vpaned
// has a real allocated height - at create_receiver time the panel is not yet in
// a sized window (it gets re-parented into the RX stack and the window resized
// afterwards), so gtk_widget_get_height() would return ~1 and the
// saved proportion would be lost. Poll on a timeout until allocated, then apply.
static gboolean restore_paned_position_cb(gpointer data);

// Re-fit the panadapter/waterfall split to the vpaned's CURRENT height. The
// position is absolute pixels, so when the RX area is resized (e.g. a decode
// panel opens/closes and the outer paned re-apportions the height) the old
// position no longer matches — leaving the panadapter collapsed. Re-run the
// restore (retries until allocated) so the split re-fits the new height.
// Show or hide the panadapter (spectroscope) per rx->show_panadapter. When
// hidden the widget is set invisible so the GtkPaned gives the waterfall the
// whole height; the waterfall's own resize then drives rx->pixels/analyzer (see
// waterfall.c resize_timeout) so the spectrum feed keeps working without the
// panadapter. When shown, re-fit the saved split.
void receiver_apply_panadapter_visibility(RECEIVER *rx) {
  if(rx==NULL || rx->panadapter==NULL) return;
  if(rx->show_panadapter) {
    gtk_widget_set_visible(rx->panadapter,TRUE);
    receiver_refit_vpaned(rx);
  } else {
    gtk_widget_set_visible(rx->panadapter,FALSE);
  }
}

void receiver_refit_vpaned(RECEIVER *rx) {
  if(rx==NULL || rx->vpaned==NULL) return;
  if(!rx->show_panadapter) return;   // waterfall owns the whole pane
  rx->paned_restore_tries=0;
  // 150 ms so this lands AFTER the outer rx_stack_balance (100 ms) has set the
  // RX area height; restore_paned_position_cb then retries until the vpaned is
  // allocated at its final size.
  g_timeout_add(150, restore_paned_position_cb, (gpointer)rx);
}

static gboolean restore_paned_position_cb(gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(rx->vpaned==NULL) return FALSE;
  gint paned_height=gtk_widget_get_height(rx->vpaned);
  // Keep waiting until the pane is allocated a usable height. A tiny but >1
  // height (mid-allocation) would otherwise compute a sliver position and stop
  // for good, opening the spectroscope collapsed — the bug this guards against.
  // Once it is at least the two minimums tall we commit; a cramped pane past
  // ~20 retries (~3 s) is committed anyway (clamped) so we never spin forever.
  gint min_total=MIN_PANADAPTER_HEIGHT+MIN_WATERFALL_HEIGHT;
  if(paned_height<=1) return TRUE;  // not allocated yet, keep waiting
  if(paned_height<min_total && ++rx->paned_restore_tries<20) return TRUE;
  rx->paned_restore_tries=0;
  // Guard a degenerate saved split: 0 collapses the panadapter (spectroscope
  // invisible), 1 collapses the waterfall, and NaN fails every comparison —
  // fall back to an even split so the spectrum is always visible.
  double pct=rx->paned_percent;
  if(!(pct>0.05 && pct<0.95)) pct=0.5;
  gint position=(gint)((double)paned_height*pct);
  // Clamp so both the spectroscope and the waterfall keep their minimum height
  // (skip when the pane is too short to satisfy both — clamp order would flip).
  if(paned_height>=min_total) {
    if(position < MIN_PANADAPTER_HEIGHT) position=MIN_PANADAPTER_HEIGHT;
    if(position > paned_height - MIN_WATERFALL_HEIGHT) position=paned_height - MIN_WATERFALL_HEIGHT;
  }
  gtk_paned_set_position(GTK_PANED(rx->vpaned),position);
  return FALSE;  // done, stop the timeout
}


RECEIVER *create_receiver(int channel,int sample_rate, gboolean show_rx) {



  RECEIVER *rx=g_new0(RECEIVER,1);
  char name [80];
  char *value;
  gint x=-1;
  gint y=-1;
  gint width;
  gint height;

log_info("create_receiver: channel=%d sample_rate=%d\n", channel, sample_rate);
  g_mutex_init(&rx->mutex);
  rx->channel=channel;
  rx->adc=0;

  rx->frequency_min=(gint64)radio->discovered->frequency_min;
  rx->frequency_max=(gint64)radio->discovered->frequency_max;
log_info("create_receiver: channel=%d frequency_min=%lld frequency_max=%lld\n", channel, rx->frequency_min, rx->frequency_max);

// DEBUG
  if(channel==0 ) {
    rx->adc=0;
  } else {
    switch(radio->discovered->protocol) {
      case PROTOCOL_1:
        switch(radio->discovered->device) {
          case DEVICE_ANGELIA:
          case DEVICE_ORION:
          case DEVICE_ORION2:
            rx->adc=1;
            break;
          default:
            rx->adc=0;
            break;
        }
        break;
      case PROTOCOL_2:
        switch(radio->discovered->device) {
          case NEW_DEVICE_ANGELIA:
          case NEW_DEVICE_ORION:
          case NEW_DEVICE_ORION2:
            rx->adc=1;
            break;
          default:
            rx->adc=0;
            break;

        }
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        if(radio->discovered->supported_receivers>1 &&
// fv - need to figure how to deal with the Lime Suite
           strcmp(radio->discovered->name,"lms7")==0) {
          rx->adc=2;
        } else {
          rx->adc=1;
        }
        break;
#endif
    }
  }

  rx->sample_rate=sample_rate;
  rx->dsp_rate=48000;
  rx->output_rate=48000;

  switch(radio->discovered->protocol) {
    default:
      rx->frequency_a=14200000;
      rx->band_a=band20;
      rx->mode_a=USB;
      rx->bandstack=1;
      break;
#ifdef IIO
    case PROTOCOL_IIO:
      rx->frequency_a=2400000000;
      rx->band_a=band2300;
      rx->mode_a=USB;
      rx->bandstack=1;
      break;
#endif
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      rx->frequency_a=145000000;
      rx->band_a=band144;
      rx->mode_a=USB;
      rx->bandstack=1;
      break;
#endif
  }

  rx->deviation=2500;
  rx->squelch_enable = FALSE;
  rx->squelch = 0.1;

  rx->filter_a=F5;
  for(int i=0;i<MODES;i++) rx->mode_filter[i]=F5;
  rx->lo_a=0;
  rx->error_a=0;
  rx->offset=0;

  rx->frequency_b=rx->frequency_a;
#ifdef USE_VFO_B_MODE_AND_FILTER
  rx->band_b=rx->band_a;
  rx->mode_b=rx->mode_a;
  rx->filter_b=rx->filter_a;
#endif
  rx->lo_b=rx->lo_a;
  rx->error_b=rx->error_a;


  rx->lo_tx=0;
  rx->tx_track_rx=FALSE;

  rx->ctun=FALSE;
  rx->ctun_offset=0;
  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
  rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);

  /* freetune defaults to off */
  rx->freetune=FALSE;

  rx->bpsk=NULL;
  rx->bpsk_enable=FALSE;

  rx->frequency_b=14300000;
#ifdef USE_VFO_B_MODE_AND_FILTER
  rx->band_b=band20;
  rx->mode_b=USB;
  rx->filter_b=F5;
#endif
  rx->lo_b=0;
  rx->error_b=0;

  rx->split=FALSE;

  rx->rit_enabled=FALSE;
  rx->rit=0;
  rx->rit_step=10;

  rx->step=100;
  rx->locked=FALSE;

  rx->volume=0.05;
  rx->mute=FALSE;
  rx->agc=AGC_OFF;
  rx->agc_gain=80.0;
  rx->agc_slope=35.0;
  rx->agc_hang_threshold=0.0;

  rx->smeter=RXA_S_AV;

  rx->pixels=0;
  rx->pixel_samples=NULL;
  rx->waterfall_pixbuf=NULL;
  rx->iq_sequence=0;
  // Wideband receivers use the large 5120-sample I/O block.  WFM runs the whole
  // DSP chain at the (wide) sample_rate with a large output resampler
  // (sample_rate->48k).  Two things must hold to avoid glitches in WDSP's async
  // double-buffered I/O ring:
  //   (1) in_size == dsp_insize, else the DSP thread fills the output ring only
  //       every N-th fexchange while fexchange drains it every call -> periodic
  //       boundary click.  For WFM dsp_rate==sample_rate so dsp_insize==fft_size,
  //       hence buffer_size must equal fft_size.
  //   (2) the output block out_size = buffer_size/(sample_rate/48000) must be an
  //       exact integer AND reasonably large; tiny blocks (=64 at 1536k with
  //       buffer_size=2048) still glitch.
  // 5120 = 2^10 * 5 satisfies both for every offered span, because it is an exact
  // multiple of (sample_rate/48000) for 192/384/768/1536/1920 k (ratios
  // 4/8/16/32/40) -> out_size = 1280/640/320/160/128, all integer and >=128.
  // (A pure power of two like 4096 cannot support the 1920k span, whose ratio 40
  // has a factor of 5.)  The fake test device shares this so it too can offer the
  // wide spans (its 1024-sample block breaks 1920k: 1024/40 is not an integer).
  gboolean wide_buffers = (radio->discovered->protocol==PROTOCOL_FAKE);
#ifdef SOAPYSDR
  if(radio->discovered->device==DEVICE_SOAPYSDR) wide_buffers=TRUE;
#endif
  rx->buffer_size = wide_buffers ? 5120 : 1024;
log_info("create_receiver: buffer_size=%d\n",rx->buffer_size);
  rx->iq_input_buffer=g_new0(gdouble,2*rx->buffer_size);
  rx->diviq_input_buffer=g_new0(gdouble,2*rx->buffer_size);


  rx->audio_buffer_size=480;
  rx->audio_buffer=g_new0(guchar,rx->audio_buffer_size);
  rx->audio_sequence=0;

  rx->mixed_audio=0;

  rx->output_started=FALSE;

  rx->fps=25;
  rx->meter_smoothing=50;   // half-strength S-meter needle ballistics by default
  rx->display_average_time=40.0;

  // Must equal buffer_size for wideband receivers so in_size==dsp_insize for the
  // WFM chain (see the buffer_size comment above): avoids the WDSP output-ring
  // boundary glitch.
  rx->fft_size = wide_buffers ? 5120 : 2048;
log_info("create_receiver: fft_size=%d\n",rx->fft_size);
  rx->low_latency=FALSE;

  rx->nb=FALSE;
  rx->nb2=FALSE;
  rx->nr=FALSE;
  rx->nr2=FALSE;
  rx->anf=FALSE;
  rx->snb=FALSE;

  rx->nr_agc=0;
  rx->nr2_gain_method=2;
  rx->nr2_npe_method=0;
  rx->nr2_ae=1;

  // set default location and sizes
  rx->window_x=channel*30;
  rx->window_y=channel*30;
  rx->window_width=820;
  rx->window_height=180;
  rx->window=NULL;
  rx->panadapter_low=-140;
  rx->panadapter_high=-60;
  rx->panadapter_step=20;
  rx->panadapter_surface=NULL;

  rx->panadapter_filled=TRUE;
  rx->panadapter_gradient=TRUE;
  rx->panadapter_agc_line=TRUE;

  rx->panadapter_single_color=TRUE;

  rx->waterfall_automatic=TRUE;
  rx->waterfall_ft8_marker=FALSE;
  rx->waterfall_color_theme=1;

  rx->vfo_surface=NULL;
  rx->meter_surface=NULL;
  rx->radio_info_surface=NULL;

#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    rx->remote_audio=FALSE;
  } else {
#endif
    rx->remote_audio=TRUE;        // on or off remote audio
#ifdef SOAPYSDR
  }
#endif
  // Default a freshly-added radio to Local Audio on the "System Default" output
  // so it makes sound out of the box.  receiver_restore_state only overrides
  // these when the property actually exists, so an existing device's saved audio
  // choice (including a deliberate "off") is preserved — only brand-new devices
  // get this default.
  rx->local_audio=TRUE;
  rx->local_audio_buffer_size=2048;
  rx->local_audio_buffer_offset=0;
  rx->local_audio_buffer=NULL;
  rx->local_audio_latency=50;
  rx->audio_channels = 0;
  g_mutex_init(&rx->local_audio_mutex);


  rx->audio_name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
  rx->mute_when_not_active=FALSE;

  rx->zoom=1;
  rx->pan=0;
  rx->is_panning=FALSE;
  rx->enable_equalizer=FALSE;
  rx->equalizer[0]=0;
  rx->equalizer[1]=0;
  rx->equalizer[2]=0;
  rx->equalizer[3]=0;

  rx->bookmark_dialog=NULL;

  rx->filter_frame=NULL;
  rx->filter_grid=NULL;

  rx->rigctl_port=19090+rx->channel;
  rx->rigctl_enable=FALSE;
  rx->cat_client_connected = FALSE;

  strcpy(rx->rigctl_serial_port,"/dev/ttyACM0");
  rx->rigctl_serial_baudrate=B9600;
  rx->rigctl_serial_enable=FALSE;
  rx->rigctl_debug=FALSE;
  rx->rigctl=NULL;

  rx->vpaned=NULL;
  rx->paned_position=-1;
  rx->paned_restore_tries=0;
  rx->paned_percent=0.5;
  rx->show_panadapter=TRUE;

  rx->split=SPLIT_OFF;
  rx->duplex=FALSE;
  rx->mute_while_transmitting=FALSE;

  rx->vfo=NULL;
  rx->subrx_enable=FALSE;
  rx->subrx=NULL;

  rx->diversity = FALSE;
  rx->diversity_hidden_rx = -1;
  rx->dmix_id = MAX_DIVERSITY_MIXERS+1;

  rx->show_rx = show_rx;

  receiver_restore_state(rx);

  if(radio->discovered->protocol==PROTOCOL_1) {
    if(rx->sample_rate!=sample_rate) {
      rx->sample_rate=sample_rate;
    }
  }

  // Fake test device runs at a fixed wide rate (see create_radio); don't let a
  // persisted narrower rate override it, so wideband FM has room on the display.
  if(radio->discovered->protocol==PROTOCOL_FAKE) {
    rx->sample_rate=sample_rate;
  }

  rx->output_samples=rx->buffer_size/(rx->sample_rate/48000);
  rx->audio_output_buffer=g_new0(gdouble,2*rx->output_samples);

log_info("create_receiver: OpenChannel: channel=%d buffer_size=%d sample_rate=%d fft_size=%d output_samples=%d\n", rx->channel, rx->buffer_size, rx->sample_rate, rx->fft_size,rx->output_samples);

  // Size the iobuff ring to this receiver's span (2/4/8/16 by width); captured
  // by create_iobuffs inside OpenChannel below.
  SetDSPMult(rx_ring_depth(rx->sample_rate));

  OpenChannel(rx->channel,
              rx->buffer_size,
              rx->fft_size,
              rx->sample_rate,
              48000, // dsp rate
              48000, // output rate
              0, // receive
              1, // run
              0.010, 0.025, 0.0, 0.010, 0);

  // The channel is opened at 48 kHz DSP rate regardless of any rate persisted
  // in props; keep rx->dsp_rate in sync so set_mode's guard reflects reality
  // (otherwise a restored WFM rate would make it skip the needed raise).
  rx->dsp_rate=48000;

  // Modified per pihpsdr commit d9af51206087959083feddcb325443d9368dad8c
  create_anbEXT(rx->channel, 1, rx->buffer_size, rx->sample_rate, 0.00001, 0.00001, 0.00001, 0.05, 4.95);
  create_nobEXT(rx->channel,1, 0, rx->buffer_size, rx->sample_rate, 0.00001, 0.00001, 0.00001, 0.05, 4.95);
  RXASetNC(rx->channel, rx->fft_size);
  RXASetMP(rx->channel, rx->low_latency);
#ifdef SOAPYSDR
  if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
    rx->resample_step=radio->sample_rate/rx->sample_rate;
log_info("receiver_change_sample_rate: resample_step=%d\n",rx->resample_step);
  }
#endif


  frequency_changed(rx);
  receiver_mode_changed(rx,rx->mode_a);

  SetRXAPanelGain1(rx->channel, rx_panel_gain(rx));
  SetRXAPanelSelect(rx->channel, 3);
  SetRXAPanelPan(rx->channel, 0.5);
  SetRXAPanelCopy(rx->channel, 0);
  SetRXAPanelBinaural(rx->channel, 0);
  SetRXAPanelRun(rx->channel, 1);

  if(rx->enable_equalizer) {
    SetRXAGrphEQ(rx->channel, rx->equalizer);
    SetRXAEQRun(rx->channel, 1);
  } else {
    SetRXAEQRun(rx->channel, 0);
  }

  set_agc(rx);

  SetEXTANBRun(rx->channel, rx->nb);
  SetEXTNOBRun(rx->channel, rx->nb2);

  SetRXAEMNRPosition(rx->channel, rx->nr_agc);
  SetRXAEMNRgainMethod(rx->channel, rx->nr2_gain_method);
  SetRXAEMNRnpeMethod(rx->channel, rx->nr2_npe_method);
  SetRXAEMNRRun(rx->channel, rx->nr2);
  SetRXAEMNRaeRun(rx->channel, rx->nr2_ae);

  SetRXAANRVals(rx->channel, 64, 16, 16e-4, 10e-7); // defaults
  SetRXAANRRun(rx->channel, rx->nr);
  SetRXAANFRun(rx->channel, rx->anf);
  SetRXASNBARun(rx->channel, rx->snb);

  /* Restore freetune WDSP shift state after channel open */
  if(rx->freetune || rx->ctun) {
    SetRXAShiftRun(rx->channel, 1);
  }
  /* The LO starts on the current centre; seed the freetune retune tracker. */
  rx->freetune_hw_frequency = rx->frequency_a;

  int result;
  XCreateAnalyzer(rx->channel, &result, WDSP_ANALYZER_MAX_SIZE, 1, 1, "");
  if(result != 0) {
    log_info("XCreateAnalyzer channel=%d failed: %d\n", rx->channel, result);
  } else {
    receiver_init_analyzer(rx);
  }

  SetDisplayDetectorMode(rx->channel, 0, DETECTOR_MODE_AVERAGE/*display_detector_mode*/);
  SetDisplayAverageMode(rx->channel, 0,  AVERAGE_MODE_LOG_RECURSIVE/*display_average_mode*/);
  calculate_display_average(rx);

  /* Apply the saved broadcast-FM de-emphasis (WDSP defaults to 50 us). */
  SetRXAWFMDeemphasisTau(rx->channel, radio->wfm_deemphasis ? 75.0e-6 : 50.0e-6);

  if (!rx->show_rx) return rx;

  create_visual(rx);
  // GTK4: children are visible by default; no gtk_widget_show_all.
  radio->active_receiver=rx;
#ifdef FT8
  // Mode was applied (receiver_mode_changed) before create_visual built the box,
  // so sync the FT8 waterfall now (it appears only if DIGU + panel already open).
  receiver_ft8_waterfall_sync(rx);
#endif
  if(rx->vpaned!=NULL) {
    // Always position the panadapter/waterfall split once the vpaned has a real
    // height (see restore_paned_position_cb): it uses the saved fraction, or an
    // even 0.5 fallback when that is missing/degenerate. Gating this on a
    // positive saved percent (as before) left a 0 / never-saved split at GTK4's
    // default, which collapsed the panadapter (spectroscope invisible).
    g_timeout_add(100,restore_paned_position_cb,(gpointer)rx);
  }
  // Honour a saved "spectroscope off": hide the panadapter so the waterfall
  // takes the whole pane (the restore above no-ops while hidden).
  receiver_apply_panadapter_visibility(rx);
  update_frequency(rx);

  // Poll the display faster than WDSP produces frames (production ~= fps, tied to
  // the analyzer overlap). Polling AT fps aliased against the ~fps production and
  // caught new frames at irregular 40/80 ms intervals -> the waterfall juddered
  // unevenly. Oversampling (3x) catches each frame promptly, so the scroll
  // advances at the production cadence (regular, since the RX feed is real-time).
  int poll_ms=1000/(3*rx->fps); if(poll_ms<8) poll_ms=8;
  rx->update_timer_id=g_timeout_add(poll_ms,update_timer_cb,(gpointer)rx);


  if(rx->local_audio) {
    if(audio_open_output(rx)<0) {
      rx->local_audio=FALSE;
    }
  }

  if(rx->bpsk_enable) {
    rx->bpsk=create_bpsk(BPSK_CHANNEL,rx->band_a);
  }


  if(rx->rigctl_enable) {
    launch_rigctl(rx);
  }

  if(rx->rigctl_serial_enable) {
    launch_serial(rx);
  }

  return rx;
}

void receiver_set_volume(RECEIVER *rx) {
  SetRXAPanelGain1(rx->channel, rx_panel_gain(rx));
  if(rx->subrx_enable) {
    subrx_volume_changed(rx);
  }
}

void receiver_set_agc_gain(RECEIVER *rx) {
  SetRXAAGCTop(rx->channel, rx->agc_gain);
}

void receiver_set_ctun(RECEIVER *rx) {
  rx->ctun_offset=0;
  rx->ctun_frequency=rx->frequency_a;
  rx->ctun_min=rx->frequency_a-(rx->sample_rate/2);
  rx->ctun_max=rx->frequency_a+(rx->sample_rate/2);
  if(!rx->ctun && !rx->freetune) {
    SetRXAShiftRun(rx->channel, 0);
  } else {
    SetRXAShiftRun(rx->channel, 1);
  }
  frequency_changed(rx);
  update_frequency(rx);
}
