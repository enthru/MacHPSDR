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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <wdsp.h>

#include "alex.h"
#include "button_text.h"
#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "receiver.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "diversity_mixer.h"
#include "radio.h"
#include "tx_panadapter.h"
#include "protocol1.h"
#include "protocol2.h"
#include "fake_protocol.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "main.h"
#include "configure_dialog.h"
#include "audio.h"
#include "vfo.h"
#include "mic_level.h"
#include "mic_gain.h"
#include "drive_level.h"
#include "frequency.h"
#include "property.h"
#include "css.h"
//#include "rigctl.h"
#include "receiver_dialog.h"
#include "subrx.h"
#include "hl2.h"

#include "cwdaemon.h"

#ifdef MIDI
#include "midi.h"
#include "midi_dialog.h"
// rather than including MIDI.h with all its internal stuff
// (e.g. enum components) we just declare the single bit thereof
// we need here to make a strict compiler happy.
int MIDIstartup(char *filename);
#endif

static GtkWidget *add_receiver_b;
static GtkWidget *add_wideband_b;

static void rxtx(RADIO *r);

int radio_restart(void *data) {
  RADIO *r=(RADIO *)data;
fprintf(stderr,"radio_restart\n");
  switch(r->discovered->protocol) {
    case PROTOCOL_1:
      protocol1_run();
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      soapy_protocol_change_sample_rate(r->receiver[0],r->sample_rate);
      break;
#endif
  }
  if(r->transmitter!=NULL) {
    update_tx_panadapter(r);
  }
  return 0;
}

int radio_start(void *data) {
  RADIO *r=(RADIO *)data;
fprintf(stderr,"radio_start\n");
  switch(r->discovered->protocol) {
    case PROTOCOL_1:
      protocol1_run(r);
      break;
    case PROTOCOL_2:
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      break;
#endif
    case PROTOCOL_FAKE:
      break;
  }
  if(r->transmitter!=NULL) {
    update_tx_panadapter(r);
  }
  return 0;
}

// Repaint the Cairo-drawn surfaces that cache their background and are NOT on a
// continuous refresh timer, so a live skin change shows up right away. The RX
// panadapter/waterfall/S-meter/mic-drive bars redraw every frame while running
// and pick up the new palette on their own; the TX "monitor" panadapter is idle
// unless transmitting, so it has to be repainted explicitly.
void radio_refresh_skin(RADIO *r) {
  if(r==NULL) return;
  if(r->transmitter!=NULL && r->transmitter->panadapter_surface!=NULL) {
    update_tx_panadapter(r);
  }
}

void radio_save_state(RADIO *radio) {
  char name[80];
  char value[80];
  int i;
  gint x,y;
  gint width,height;
  char filename[128];
  switch(radio->discovered->protocol) {

#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      sprintf(filename,"%s/.local/share/machpsdr/%s.props",
                        g_get_home_dir(),
                        radio->discovered->name);
      break;
#endif
    default:
      sprintf(filename,"%s/.local/share/machpsdr/%02X-%02X-%02X-%02X-%02X-%02X.props",
                        g_get_home_dir(),
                        radio->discovered->info.network.mac_address[0],
                        radio->discovered->info.network.mac_address[1],
                        radio->discovered->info.network.mac_address[2],
                        radio->discovered->info.network.mac_address[3],
                        radio->discovered->info.network.mac_address[4],
                        radio->discovered->info.network.mac_address[5]);
      break;
  }

g_print("radio_save_state: %s\n",filename);
  // radio_save_state is a full snapshot: initProperties() wipes everything and
  // only live receivers are re-serialized below. Retain the saved settings of
  // any inactive (user-closed) receiver slot so they are not discarded — they
  // are merged back in after the live state has been written.
  for(i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]==NULL) {
      char prefix[32];
      sprintf(prefix,"receiver[%d].",i);
      retainProperties(prefix);
    }
  }
  initProperties();

  sprintf(value,"%d",radio->model);
  setProperty("radio.model",value);
  sprintf(value,"%d",radio->filter_board);
  setProperty("radio.filter_board",value);
  sprintf(value,"%d",radio->sample_rate);
  setProperty("radio.sample_rate",value);
  sprintf(value,"%d",radio->buffer_size);
  setProperty("radio.buffer_size",value);
  sprintf(value,"%d",radio->receivers);
  setProperty("radio.receivers",value);
  sprintf(value,"%f",radio->meter_calibration);
  setProperty("radio.meter_calibration",value);
  sprintf(value,"%f",radio->panadapter_calibration);
  setProperty("radio.panadapter_calibration",value);
  sprintf(value,"%d",radio->cw_keyer_sidetone_frequency);
  setProperty("radio.cw_keyer_sidetone_frequency",value);
  sprintf(value,"%d",radio->cw_keyer_sidetone_volume);
  setProperty("radio.cw_keyer_sidetone_volume",value);
  sprintf(value,"%d",radio->cw_keys_reversed);
  setProperty("radio.cw_keys_reversed",value);
  sprintf(value,"%d",radio->cw_keyer_speed);
  setProperty("radio.cw_keyer_speed",value);
  sprintf(value,"%d",radio->cw_keyer_mode);
  setProperty("radio.cw_keyer_mode",value);
  sprintf(value,"%d",radio->cw_keyer_weight);
  setProperty("radio.cw_keyer_weight",value);
  sprintf(value,"%d",radio->cw_keyer_spacing);
  setProperty("radio.cw_keyer_internal",value);
  sprintf(value,"%d",radio->cw_keyer_internal);
  setProperty("radio.cw_keyer_internal",value);
  sprintf(value,"%d",radio->cw_keyer_ptt_delay);
  setProperty("radio.cw_keyer_ptt_delay",value);
  sprintf(value,"%d",radio->cw_keyer_hang_time);
  setProperty("radio.cw_keyer_hang_time",value);
  sprintf(value,"%d",radio->cw_breakin);
  setProperty("radio.cw_breakin",value);
  #ifdef CWDAEMON
  sprintf(value,"%d",radio->cwd_port);
  setProperty("radio.cwd_port",value);
  sprintf(value,"%d",radio->cwd_sidetone);
  setProperty("radio.cwd_sidetone",value);
  sprintf(value,"%d",radio->cw_generation_mode);
  setProperty("radio.cw_generation_mode",value);
  sprintf(value,"%d",radio->cwdaemon_running);
  setProperty("radio.cwdaemon_running",value);
  #endif
  sprintf(value,"%d",radio->local_microphone);
  setProperty("radio.local_microphone",value);
  sprintf(value,"%d",radio->mic_boost);
  setProperty("radio.mic_boost",value);
  if(radio->microphone_name!=NULL) {
    setProperty("radio.microphone_name",radio->microphone_name);
  }
  sprintf(value,"%d",radio->mic_ptt_enabled);
  setProperty("radio.mic_ptt_enabled",value);
  sprintf(value,"%d",radio->mic_bias_enabled);
  setProperty("radio.mic_bias_enabled",value);
  sprintf(value,"%d",radio->mic_ptt_tip_bias_ring);
  setProperty("radio.mic_ptt_tip_bias_ring",value);
  sprintf(value,"%d",radio->mic_linein);
  setProperty("radio.mic_linein",value);
  sprintf(value,"%d",radio->linein_gain);
  setProperty("radio.linein_gain",value);
  setProperty("radio.att10_label",radio->att10_label);
  setProperty("radio.att20_label",radio->att20_label);
  sprintf(value,"%d",radio->theme);
  setProperty("radio.theme",value);

  for(int i=0;i<radio->discovered->adcs;i++) {
    sprintf(name,"radio.adc[%d].filters",i);
    sprintf(value,"%d",radio->adc[i].filters);
    setProperty(name,value);
    sprintf(name,"radio.adc[%d].hpf",i);
    sprintf(value,"%d",radio->adc[i].hpf);
    setProperty(name,value);
    sprintf(name,"radio.adc[%d].lpf",i);
    sprintf(value,"%d",radio->adc[i].lpf);
    setProperty(name,value);
    sprintf(name,"radio.adc[%d].antenna",i);
    sprintf(value,"%d",radio->adc[i].antenna);
    setProperty(name,value);
    sprintf(name,"radio.adc[%d].dither",i);
    sprintf(value,"%d",radio->adc[i].dither);
    setProperty(name,value);
    sprintf(name,"radio.adc[%d].random",i);
    sprintf(value,"%d",radio->adc[i].random);
    setProperty(name,value);
    sprintf(name,"radio.adc[%d].preamp",i);
    sprintf(value,"%d",radio->adc[i].preamp);
    setProperty(name,value);

    sprintf(name,"radio.adc[%d].att10",i);
    sprintf(value,"%d",radio->adc[i].att10);
    setProperty(name,value);
    sprintf(name,"radio.adc[%d].att20",i);
    sprintf(value,"%d",radio->adc[i].att20);
    setProperty(name,value);

    sprintf(name,"radio.adc[%d].attenuation",i);
    sprintf(value,"%d",radio->adc[i].attenuation);
    setProperty(name,value);

#ifdef SOAPYSDR
    if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
      sprintf(name,"radio.adc[%d].gain",i);
      sprintf(value,"%f", radio->adc[0].gain);
      setProperty(name,value);
      sprintf(name,"radio.adc[%d].agc",i);
      sprintf(value,"%d", soapy_protocol_get_automatic_gain(&radio->adc[0]));
      setProperty(name,value);
      sprintf(name,"radio.dac[%d].gain",i);
      sprintf(value,"%f", radio->dac[0].gain);
      setProperty(name,value);
      sprintf(name,"radio.dac[%d].antenna",i);
      sprintf(value,"%d", radio->dac[0].antenna);
      setProperty(name,value);
    }
#endif
  }

  if (radio->hl2 != NULL) {
    sprintf(value,"%d",radio->hl2->hl2_tx_buffer_size);
    setProperty("radio.hl2.tx_buffer_size",value);
  }

  sprintf(value,"%d",radio->filter_board);
  setProperty("radio.filter_board",value);

  sprintf(value,"%d",radio->region);
  setProperty("radio.region",value);

  sprintf(value,"%d",radio->classE);
  setProperty("radio.classE",value);

  sprintf(value,"%d",radio->temperature_alarm_value);
  setProperty("radio.temp_alarm",value);
  sprintf(value,"%f",radio->swr_alarm_value);
  setProperty("radio.swr_alarm",value);
  sprintf(value,"%d",radio->ppm_correction_value);
  setProperty("radio.ppm_correction_value",value);
  sprintf(value,"%d",radio->wfm_deemphasis);
  setProperty("radio.wfm_deemphasis",value);
  sprintf(value,"%d",radio->rds_rbds);
  setProperty("radio.rds_rbds",value);

/*
  sprintf(value,"%d",rigctl_enable);
  setProperty("rigctl_enable",value);
  sprintf(value,"%d",rigctl_port_base);
  setProperty("rigctl_port_base",value);
*/
  sprintf(value,"%d",radio->iqswap);
  setProperty("radio.iqswap",value);

  sprintf(value,"%d",radio->which_audio);
  setProperty("radio.which_audio",value);

  sprintf(value,"%d",radio->which_audio_backend);
  setProperty("radio.which_audio_backend",value);

  filterSaveState();
  bandSaveState();

  for(i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]!=NULL) {
      receiver_save_state(radio->receiver[i]);
    }
  }

  if(radio->discovered->supported_transmitters!=0) {
    transmitter_save_state(radio->transmitter);
  }

#ifdef MIDI
  setProperty("radio.midi_filename",radio->midi_filename);
  sprintf(value,"%d",radio->midi_enabled);
  setProperty("radio.midi_enabled",value);

  midi_save_state();
#endif

  gtk_window_get_position(GTK_WINDOW(main_window),&x,&y);
  sprintf(value,"%d",x);
  setProperty("radio.x",value);
  sprintf(value,"%d",y);
  setProperty("radio.y",value);

  gtk_window_get_size(GTK_WINDOW(main_window),&width,&height);
  sprintf(value,"%d",width);
  setProperty("radio.width",value);
  sprintf(value,"%d",height);
  setProperty("radio.height",value);

  // Save inter-receiver GtkPaned divider positions as fractions of each pane's
  // height, so they restore sensibly even if the window is a different size.
  if(radio->rx_container!=NULL) {
    GList *children=gtk_container_get_children(GTK_CONTAINER(radio->rx_container));
    GtkWidget *w=(children!=NULL)?GTK_WIDGET(children->data):NULL;
    g_list_free(children);
    int k=0;
    while(w!=NULL && GTK_IS_PANED(w)) {
      int ph=gtk_widget_get_allocated_height(w);
      double frac=(ph>0)?((double)gtk_paned_get_position(GTK_PANED(w))/(double)ph):0.5;
      sprintf(name,"radio.rx_paned[%d]",k);
      sprintf(value,"%f",frac);
      setProperty(name,value);
      k++;
      w=gtk_paned_get_child2(GTK_PANED(w));
    }
  }

  // Merge back the retained settings of any inactive receiver slots.
  releaseRetainedProperties();

  saveProperties(filename);
}

void radio_restore_state(RADIO *radio) {
  char name[80];
  char *value;
  char filename[128];
  switch(radio->discovered->protocol) {
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      sprintf(filename,"%s/.local/share/machpsdr/%s.props",
                        g_get_home_dir(),
                        radio->discovered->name);
      break;
#endif
    default:
      sprintf(filename,"%s/.local/share/machpsdr/%02X-%02X-%02X-%02X-%02X-%02X.props",
                        g_get_home_dir(),
                        radio->discovered->info.network.mac_address[0],
                        radio->discovered->info.network.mac_address[1],
                        radio->discovered->info.network.mac_address[2],
                        radio->discovered->info.network.mac_address[3],
                        radio->discovered->info.network.mac_address[4],
                        radio->discovered->info.network.mac_address[5]);
      break;
  }

  loadProperties(filename);

  value=getProperty("radio.model");
  if(value!=NULL) radio->model=atoi(value);
  value=getProperty("radio.filter_board");
  if(value!=NULL) radio->filter_board=atoi(value);
  value=getProperty("radio.sample_rate");
  if(value!=NULL) radio->sample_rate=atoi(value);
  value=getProperty("radio.meter_calibration");
  if(value) radio->meter_calibration=atof(value);
  value=getProperty("radio.panadapter_calibration");
  if(value) radio->panadapter_calibration=atof(value);
  value=getProperty("radio.cw_keyer_sidetone_frequency");
  if(value!=NULL) radio->cw_keyer_sidetone_frequency=atoi(value);
  value=getProperty("radio.cw_keyer_sidetone_volume");
  if(value!=NULL) radio->cw_keyer_sidetone_volume=atoi(value);
  value=getProperty("radio.cw_keys_reversed");
  if(value!=NULL) radio->cw_keys_reversed=atoi(value);
  value=getProperty("radio.cw_keyer_speed");
  if(value!=NULL) radio->cw_keyer_speed=atoi(value);
  value=getProperty("radio.cw_keyer_mode");
  if(value!=NULL) radio->cw_keyer_mode=atoi(value);
  value=getProperty("radio.cw_keyer_weight");
  if(value!=NULL) radio->cw_keyer_weight=atoi(value);
  value=getProperty("radio.cw_keyer_internal");
  if(value!=NULL) radio->cw_keyer_internal=atoi(value);
  value=getProperty("radio.cw_keyer_ptt_delay");
  if(value!=NULL) radio->cw_keyer_ptt_delay=atoi(value);
  value=getProperty("radio.cw_keyer_hang_time");
  if(value!=NULL) radio->cw_keyer_hang_time=atoi(value);
  value=getProperty("radio.cw_breakin");
  if(value!=NULL) radio->cw_breakin=atoi(value);
  #ifdef CWDAEMON
  value=getProperty("radio.cwd_sidetone");
  if(value!=NULL) radio->cwd_sidetone=atoi(value);
  value=getProperty("radio.cwd_port");
  if(value!=NULL) radio->cwd_port=atoi(value);
  value=getProperty("radio.cw_generation_mode");
  if(value!=NULL) radio->cw_generation_mode = atoi(value);
  value=getProperty("radio.cwdaemon_running");
  if(value!=NULL) radio->cwdaemon_running = atoi(value);
  #endif

  for(int i=0;i<radio->discovered->adcs;i++) {
    sprintf(name,"radio.adc[%d].filters",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].filters=atoi(value);
    sprintf(name,"radio.adc[%d].hpf",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].hpf=atoi(value);
    sprintf(name,"radio.adc[%d].lpf",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].lpf=atoi(value);
    sprintf(name,"radio.adc[%d].antenna",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].antenna=atoi(value);
    sprintf(name,"radio.adc[%d].dither",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].dither=atoi(value);
    sprintf(name,"radio.adc[%d].random",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].random=atoi(value);
    sprintf(name,"radio.adc[%d].preamp",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].preamp=atoi(value);

    sprintf(name,"radio.adc[%d].att10",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].att10=atoi(value);
    sprintf(name,"radio.adc[%d].att20",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].att20=atoi(value);

    sprintf(name,"radio.adc[%d].attenuation",i);
    value=getProperty(name);
    if(value!=NULL) radio->adc[i].attenuation=atoi(value);

#ifdef SOAPYSDR
    if(radio->discovered->device==DEVICE_SOAPYSDR) {
      sprintf(name,"radio.adc[%d].gain",i);
      value=getProperty(name);
      if(value!=NULL) radio->adc[i].gain=atof(value);
      sprintf(name,"radio.adc[%d].agc",i);
      value=getProperty(name);
      if(value!=NULL) radio->adc[i].agc=atoi(value);
      if(radio->can_transmit) {
        sprintf(name,"radio.dac[%d].gain",i);
        value=getProperty(name);
        if(value!=NULL) radio->dac[i].gain=atof(value);
        sprintf(name,"radio.dac[%d].antenna",i);
        value=getProperty(name);
        if(value!=NULL) radio->dac[i].antenna=atoi(value);
      }
    }
#endif
  }

  if(radio->hl2 != NULL) {
    value=getProperty("radio.hl2.tx_buffer_size");
    if(value!=NULL) radio->hl2->hl2_tx_buffer_size = atoi(value);
  }

  value=getProperty("radio.local_microphone");
  if(value!=NULL) radio->local_microphone=atoi(value);
  value=getProperty("radio.microphone_name");
  if(value!=NULL) {
    radio->microphone_name=g_new0(gchar,strlen(value)+1);
    strcpy(radio->microphone_name,value);
  }
  value=getProperty("radio.mic_boost");
  if(value!=NULL) radio->mic_boost=atoi(value);
  value=getProperty("radio.mic_ptt_enabled");
  if(value!=NULL) radio->mic_ptt_enabled=atoi(value);
  value=getProperty("radio.mic_bias_enabled");
  if(value!=NULL) radio->mic_bias_enabled=atoi(value);
  value=getProperty("radio.mic_ptt_tip_bias_ring");
  if(value!=NULL) radio->mic_ptt_tip_bias_ring=atoi(value);
  value=getProperty("radio.mic_linein");
  if(value!=NULL) radio->mic_linein=atoi(value);
  value=getProperty("radio.linein_gain");
  if(value!=NULL) radio->linein_gain=atoi(value);
  value=getProperty("radio.att10_label");
  if(value!=NULL && value[0]!='\0') g_strlcpy(radio->att10_label,value,sizeof(radio->att10_label));
  value=getProperty("radio.att20_label");
  if(value!=NULL && value[0]!='\0') g_strlcpy(radio->att20_label,value,sizeof(radio->att20_label));
  value=getProperty("radio.theme");
  if(value!=NULL) radio->theme=atoi(value);
  css_set_theme(radio->theme);   // apply the saved skin to the (already-built) UI
  value=getProperty("radio.filter_board");
  if(value!=NULL) radio->filter_board=atoi(value);
  value=getProperty("radio.region");
  if(value!=NULL) radio->region=atoi(value);
  value=getProperty("radio.classE");
  if(value!=NULL) radio->classE=atoi(value);

  value=getProperty("radio.temp_alarm");
  if(value!=NULL) radio->temperature_alarm_value=atoi(value);
  value=getProperty("radio.swr_alarm");
  if(value!=NULL) radio->swr_alarm_value=atof(value);
  value=getProperty("radio.ppm_correction_value");
  if(value!=NULL) radio->ppm_correction_value=atoi(value);
  value=getProperty("radio.wfm_deemphasis");
  if(value!=NULL) radio->wfm_deemphasis=atoi(value);
  value=getProperty("radio.rds_rbds");
  if(value!=NULL) radio->rds_rbds=atoi(value);

  value=getProperty("radio.iqswap");
  if(value) radio->iqswap=atoi(value);

  value=getProperty("radio.which_audio");
  if(value) radio->which_audio=atoi(value);

  value=getProperty("radio.which_audio_backend");
  if(value) radio->which_audio_backend=atoi(value);

/*
  value=getProperty("rigctl_enable");
  if(value!=NULL) rigctl_enable=atoi(value);
  value=getProperty("rigctl_port_base");
  if(value!=NULL) rigctl_port_base=atoi(value);
*/

#ifdef MIDI
  midi_restore_state();
  value=getProperty("radio.midi_filename");
  if(value) strcpy(radio->midi_filename,value);
  value=getProperty("radio.midi_enabled");
  if(value) radio->midi_enabled=atoi(value);
#endif

  filterRestoreState();
  bandRestoreState();
}

gboolean radio_button_press_event_cb(GtkWidget *widget, GdkEventButton *event, gpointer data) {
  g_print("%s\n",__FUNCTION__);
  switch(event->button) {
    case 1: // left button
      break;
    case 3: // right button
      if(radio->dialog==NULL) {
        //radio->dialog=create_radio_dialog(radio);
        radio->dialog=create_configure_dialog(radio,0);
      }
      break;
  }
  return TRUE;
}

void radio_change_region(RADIO *r) {
  if(r->region==REGION_UK) {
    channel_entries=UK_CHANNEL_ENTRIES;
    band_channels_60m=&band_channels_60m_UK[0];
    bandstack60.entries=UK_CHANNEL_ENTRIES;
    bandstack60.current_entry=0;
    bandstack60.entry=bandstack_entries60_UK;
  } else {
    channel_entries=OTHER_CHANNEL_ENTRIES;
    band_channels_60m=&band_channels_60m_OTHER[0];
    bandstack60.entries=OTHER_CHANNEL_ENTRIES;
    bandstack60.current_entry=0;
    bandstack60.entry=bandstack_entries60_OTHER;
  }
}

#ifdef CWDAEMON
void radio_change_cwgeneration(RADIO *r) {
  g_print("radio_change_cwgeneration gen mode %d keyer %d\n", r->cw_generation_mode, r->cw_keyer_internal);
  if (r->cw_generation_mode == CWGEN_RADIO) {
    // Hermes Lite 2 does not have an internal keyer, but does have
    // cwx (key down command sent from PC), HL2 uses protocol 1
    // cw_keyer_internal bit to turn on/off cw. Safest for the HL2 to never
    // set this bit.
    if (r->discovered->device != DEVICE_HERMES_LITE2) r->cw_keyer_internal = TRUE;
  }
  else {
    // r->cw_generation_mode == CWGEN_PC
    // PC generated CW, disable internal keyer in the radio
    r->cw_keyer_internal = FALSE;
  }
}
#endif

void radio_change_audio(RADIO *r,int selected) {
  int i;
  g_print("%s: %dn",__FUNCTION__,selected);
  if(r->which_audio!=selected) {
    if(r->local_microphone) {
      radio->local_microphone=FALSE;
      audio_close_input(r);
    }
    for(i=0;i<radio->discovered->supported_receivers;i++) {
      if(radio->receiver[i]!=NULL) {
        if(radio->receiver[i]->local_audio) {
          radio->receiver[i]->local_audio=FALSE;
          audio_close_output(radio->receiver[i]);
        }
      }
    }
  }

  r->which_audio=selected;
  create_audio(r->which_audio_backend,r->which_audio==USE_SOUNDIO?audio_get_backend_name(r->which_audio_backend):NULL);
}

void radio_change_audio_backend(RADIO *r,int selected) {
  int i;
  g_print("%s: %d\n",__FUNCTION__,selected);
  if(r->which_audio_backend!=selected) {
    if(r->local_microphone) {
      radio->local_microphone=FALSE;
      audio_close_input(r);
    }
    for(i=0;i<radio->discovered->supported_receivers;i++) {
      if(radio->receiver[i]!=NULL) {
        if(radio->receiver[i]->local_audio) {
          radio->receiver[i]->local_audio=FALSE;
          audio_close_output(radio->receiver[i]);
        }
      }
    }
  }

  r->which_audio_backend=selected;
  create_audio(r->which_audio_backend,r->which_audio==USE_SOUNDIO?audio_get_backend_name(r->which_audio_backend):NULL);
}

void vox_changed(RADIO *r) {
  rxtx(radio);
}

// Apply the broadcast-FM de-emphasis choice (0 = 50 us, 1 = 75 us) to every
// receiver's WFM channel. Harmless on non-WFM channels (the WFM demod exists on
// all channels; only WFM mode runs it).
void radio_set_wfm_deemphasis(RADIO *r, int sel) {
  int i;
  r->wfm_deemphasis = sel ? 1 : 0;
  for(i=0;i<r->receivers;i++) {
    if(r->receiver[i]!=NULL)
      SetRXAWFMDeemphasisTau(r->receiver[i]->channel, sel ? 75.0e-6 : 50.0e-6);
  }
}

void frequency_changed(RECEIVER *rx) {

    // Diversity mixer hidden rx synced to the rx which is
    // visualised
    if (radio->divmixer[rx->dmix_id] != NULL) {
      if (radio->divmixer[rx->dmix_id]->rx_visual == rx) {
        radio->divmixer[rx->dmix_id]->rx_hidden->frequency_a = rx->frequency_a;
        radio->divmixer[rx->dmix_id]->rx_hidden->frequency_b = rx->frequency_b;
      }
    }

    if (radio->hl2 != NULL) {
      if (rx->lo_a != 0) {
        radio->hl2->xvtr = TRUE;
        gtk_widget_set_sensitive(add_receiver_b, FALSE);
        HL2clock2Status(radio->hl2, TRUE, &rx->lo_a);
      }
      else {
        gtk_widget_set_sensitive(add_receiver_b, TRUE);
        radio->hl2->xvtr = FALSE;
        HL2clock2Status(radio->hl2, FALSE, &rx->lo_a);
      }
    }

  if(rx->ctun || rx->freetune) {
    gint64 offset;
    rx->ctun_offset=rx->ctun_frequency-rx->frequency_a;
    offset=rx->ctun_offset;
    /*
    if(rx->mode_a==CWU) {
      offset+=(gint64)radio->cw_keyer_sidetone_frequency;
    } else if(rx->mode_a==CWL) {
      offset-=(gint64)radio->cw_keyer_sidetone_frequency;
    }
    */
    if(rx->rit_enabled) {
      offset+=rx->rit;
    }
    SetRXAShiftFreq(rx->channel, (double)offset);
    RXANBPSetShiftFrequency(rx->channel, (double)offset);


    // Diversity mixer hidden rx synced to the rx which is
    // visualised, this allow CTUN to work
    if (radio->divmixer[rx->dmix_id] != NULL) {
      if (radio->divmixer[rx->dmix_id]->rx_visual == rx) {
        int channel = radio->divmixer[rx->dmix_id]->rx_hidden->channel;

        SetRXAShiftFreq(channel, (double)offset);
        RXANBPSetShiftFrequency(channel, (double)offset);
      }
    }

    // Freetune (unlike plain CTUN) lets the span centre follow the cursor at the
    // span edges: frequency_a moves, so the hardware LO must be retuned to match.
    // Retune only when the centre actually changed, so in-span tuning stays a
    // click-free digital shift. Protocol 1 needs nothing here (its output thread
    // reads frequency_a continuously); Protocol 2 / SoapySDR must be told.
    if(rx->freetune && rx->frequency_a != rx->freetune_hw_frequency) {
      if(radio->discovered->protocol==PROTOCOL_2) {
        protocol2_high_priority();
#ifdef SOAPYSDR
      } else if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
        soapy_protocol_set_rx_frequency(rx);
#endif
      }
      rx->band_a=get_band_from_frequency(rx->frequency_a);
      rx->freetune_hw_frequency=rx->frequency_a;
    }
  } else {
    // Normal tuning: the VFO cursor sits at the centre, so there is no ctun
    // offset. Clear any stale value left over from ctun/freetune so the cursor
    // (drawn from ctun_offset in the panadapter) returns to the middle.
    rx->ctun_offset=0;
    if(radio->discovered->protocol==PROTOCOL_2) {
      protocol2_high_priority();
#ifdef SOAPYSDR
    } else if(radio->discovered->protocol==PROTOCOL_SOAPYSDR) {
      soapy_protocol_set_rx_frequency(rx);
#endif
    }
    rx->band_a=get_band_from_frequency(rx->frequency_a);
  }

  if(rx->subrx_enable) {
    // make sure VFO B frequency is in the passband
    gint64 min_frequency=rx->frequency_a-(gint64)(rx->sample_rate/2);
    gint64 max_frequency=rx->frequency_a+(gint64)(rx->sample_rate/2);

    gint64 filter_low_frequency=rx->frequency_b+(gint64)rx->filter_low_b;
    gint64 filter_high_frequency=rx->frequency_b+(gint64)rx->filter_high_b;

    if(filter_low_frequency<min_frequency) {
      rx->frequency_b=min_frequency+(gint64)rx->filter_low_b;
    } else if(filter_high_frequency>max_frequency) {
      rx->frequency_b=max_frequency-(gint64)rx->filter_high_b;
    }
    subrx_frequency_changed(rx);
  }
}

gboolean isTransmitting(RADIO *r) {
  return (r->ptt | r->mox | r->vox | r->tune);
}

void delete_wideband(WIDEBAND *w) {
  if(radio->wideband==w) {
    radio->wideband=NULL;
  }
  if(radio->wideband==NULL) {
    gtk_widget_set_sensitive(add_wideband_b,TRUE);
  }
  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_stop_wideband();
  }
  if(radio->dialog) {
    gtk_widget_destroy(radio->dialog);
    radio->dialog=NULL;
  }
  protocol1_stop();
  protocol1_run();
}

void delete_receiver(RECEIVER *rx) {

  g_mutex_lock(&radio->delete_rx_mutex);

  // Receiver may have a diveristy mixer connected,
  // this removes the mixer and hidden rx for that mixer
  if (radio->divmixer[rx->dmix_id] != NULL) {
    g_print("Not null, delete the hidden rx\n");
    delete_diversity_mixer(radio->divmixer[rx->dmix_id]);
  }

  int reopen_rx = 0;
#ifdef PURESIGNAL
  if (radio->transmitter->puresignal != NULL) {
    if (rx->show_rx == TRUE) reopen_rx = 1;
  }
#endif

  int i;
  for(i=0;i<radio->discovered->supported_receivers;i++) {
    if(radio->receiver[i]==rx) {
      if(radio->discovered->protocol==PROTOCOL_1) {
        protocol1_stop();
      }
      if(radio->transmitter!=NULL && radio->transmitter->rx==rx) {
        radio->transmitter->rx=NULL;
      }
      radio->receiver[i]=NULL;
      radio->receivers--;
      if(radio->discovered->protocol==PROTOCOL_1) {
        //protocol1_run();
        //metis_restart();
        protocol1_stop();

      }
g_print("delete_receiver: receivers now %d\n",radio->receivers);
      break;
    }
  }

  if(radio->transmitter!=NULL && radio->transmitter->rx==NULL) {
    if(radio->receivers>0) {
      for(i=0;i<radio->discovered->supported_receivers;i++) {
        if(radio->receiver[i]!=NULL) {
          radio->transmitter->rx=radio->receiver[i];
          update_vfo(radio->receiver[i]);
          break;
        }
      }
    } else {
      // no more receivers
    }
  }

  gtk_widget_set_sensitive(add_receiver_b,radio->receivers<radio->discovered->supported_receivers);
  if(radio->dialog) {
    gtk_widget_destroy(radio->dialog);
    radio->dialog=NULL;
  }
  // For PureSignal, need to reopen the receiver just deleted
  // as a hidden rx
  if (reopen_rx == 1) add_receiver(radio, 0);

  g_idle_add(radio_restart,(void *)radio);


  g_mutex_unlock(&radio->delete_rx_mutex);
}

void delete_diversity_mixer(DIVMIXER *dmix) {
  int hidden_channel;

  for (int i = 0; i < MAX_DIVERSITY_MIXERS; i++) {
    if(radio->divmixer[i] == dmix) {
      g_print("delete div mixer %d\n", i);
      radio->divmixer[i]->rx_visual->dmix_id = MAX_DIVERSITY_MIXERS+1;
      radio->divmixer[i]->rx_hidden->dmix_id = MAX_DIVERSITY_MIXERS+1;
      // Store the hidden channel before we delete the
      // mixer
      hidden_channel = radio->divmixer[i]->rx_hidden->channel;
      radio->divmixer[i]=NULL;
      radio->diversity_mixers--;

g_print("delete_diversity_mixer: dmixers now %d\n",radio->diversity_mixers);
      break;
    }
  }
  // Delete the hidden receiver
  if (radio->receiver[hidden_channel] != NULL) {
    g_print("delete_diversity_mixer: delete the hidden rx\n");
    delete_receiver(radio->receiver[hidden_channel]);
  }
}

static void rxtx(RADIO *r) {
  int i;
g_print("%s: isTransmitting=%d\n",__FUNCTION__,isTransmitting(r));
  if(isTransmitting(r)) {
    for(i=0;i<r->discovered->supported_receivers;i++) {
      if(r->receiver[i]!=NULL) {
        if(!r->receiver[i]->duplex) {
          SetChannelState(r->receiver[i]->channel,0,1);
        }
      }
    }
    SetChannelState(r->transmitter->channel,1,0);
    if (r->transmitter->puresignal != NULL) SetPSMox(r->transmitter->channel, 1);
    switch(r->discovered->protocol) {
      case PROTOCOL_1:
        break;
      case PROTOCOL_2:
        protocol2_high_priority();
        protocol2_receive_specific();
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        // Half-duplex: pause RX, point at the TX frequency/antenna, then bring
        // the TX stream up and start clocking samples out.
        soapy_protocol_rx_pause();
        soapy_protocol_set_tx_frequency(r->transmitter);
        soapy_protocol_set_tx_antenna(r->transmitter,radio->dac[0].antenna);
        soapy_protocol_activate_tx(r->transmitter);
        break;
#endif
    }
  } else {
    SetChannelState(r->transmitter->channel,0,1);
    if (r->transmitter->puresignal != NULL) SetPSMox(r->transmitter->channel, 0);
    for(i=0;i<r->discovered->supported_receivers;i++) {
      if(r->receiver[i]!=NULL) {
        if(!r->receiver[i]->duplex) {
          SetChannelState(r->receiver[i]->channel,1,0);
        }
      }
    }
    switch(r->discovered->protocol) {
      case PROTOCOL_1:
        break;
      case PROTOCOL_2:
        protocol2_high_priority();
        protocol2_receive_specific();
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        // Bring the TX stream down, then resume RX (half-duplex).
        soapy_protocol_deactivate_tx(r->transmitter);
        soapy_protocol_rx_resume();
        break;
#endif
    }
    update_tx_panadapter(r);
    /* TX just stopped: clear the stale mic-level frame (it only repaints
       while TX is running, so the last VOX-peak block would otherwise linger). */
    r->vox_peak=0.0;
    update_mic_level(r);
  }
  update_vfo(r->transmitter->rx);
}

void set_mox(RADIO *r,gboolean state) {
g_print("%s: state=%d\n",__FUNCTION__,state);
  if(r->tune) {
    r->tune=FALSE;
    SetTXAPostGenRun(radio->transmitter->channel,0);
    switch(radio->transmitter->rx->mode_a) {
      case CWL:
      case CWU:
        SetTXAMode(radio->transmitter->channel, radio->transmitter->rx->mode_a);
        break;
    }
  }
  r->mox=state;
  rxtx(r);
  update_radio(r);
}

void ptt_changed(RADIO *r) {
g_print("ptt_changed\n");
  set_mox(r,r->local_ptt);
  update_vfo(r->transmitter->rx);
}

static void mox_cb(GtkToggleButton *widget,gpointer data) {
  RADIO *r=(RADIO *)data;
g_print("mox_cb: mox=%d\n",r->mox);
  if(r->mox) {
    set_mox(r,FALSE);
  } else {
    set_mox(r,TRUE);
  }
}

static void vox_cb(GtkToggleButton *widget,gpointer data) {
  RADIO *r=(RADIO *)data;
  r->vox_enabled=!r->vox_enabled;
  update_radio(r);
}

void set_tune(RADIO *r,gboolean state) {
  if(r->mox) {
    r->mox=FALSE;
    set_button_text_color(r->mox_button,r->mox?"red":"black");
  }
  r->tune=state;
  if(r->tune) {
    //SM4VEY
    struct timeval te;
    gettimeofday(&te,NULL);
    long long now=te.tv_sec*1000LL+te.tv_usec/1000 + r->OCfull_tune_time;
    r->tune_timeout = now;

    switch(r->transmitter->rx->mode_a) {
      case CWL:
        SetTXAMode(r->transmitter->channel, LSB);
        SetTXAPostGenToneFreq(r->transmitter->channel, -(double)r->cw_keyer_sidetone_frequency);
        r->cw_keyer_internal=FALSE;
        break;
      case LSB:
      case DIGL:
        SetTXAPostGenToneFreq(r->transmitter->channel, (double)(-r->transmitter->filter_low-((r->transmitter->filter_high-r->transmitter->filter_low)/2)));
        break;
      case CWU:
        SetTXAMode(r->transmitter->channel, USB);
        SetTXAPostGenToneFreq(r->transmitter->channel, (double)r->cw_keyer_sidetone_frequency);
        r->cw_keyer_internal=FALSE;
        break;
      default:
        SetTXAPostGenToneFreq(r->transmitter->channel, (double)(r->transmitter->filter_low+((r->transmitter->filter_high-r->transmitter->filter_low)/2)));
        break;
    }
    SetTXAPostGenToneMag(r->transmitter->channel,0.99999);
    SetTXAPostGenMode(r->transmitter->channel,0);
    SetTXAPostGenRun(r->transmitter->channel,1);
    rxtx(r);
  } else {
    SetTXAPostGenRun(r->transmitter->channel,0);
    switch(r->transmitter->rx->mode_a) {
      case CWL:
      case CWU:
        SetTXAMode(r->transmitter->channel, r->transmitter->rx->mode_a);
        #ifdef CWDAEMON
        if (r->cw_generation_mode == CWGEN_RADIO) r->cw_keyer_internal=TRUE;
        #else
        r->cw_keyer_internal=TRUE;
        #endif
        break;
    }
    rxtx(r);
  }
  update_radio(r);
}

static void tune_cb(GtkToggleButton *widget,gpointer data) {
  RADIO *r=(RADIO *)data;
  if(r->tune) {
    set_tune(r,FALSE);
  } else {
    set_tune(r,TRUE);
  }
}

#ifdef SOAPYSDR
// One-shot, fired a short time after a SoapySDR RX stream starts.  HackRF only
// writes the RX gain (in particular the +14 dB RF preamp/AMP) into hardware
// when setGain is called while the stream is actually transferring samples.
// Gain applied during startup is cached but not physically engaged, so
// reception was weak until the user nudged the slider (any change re-ran
// setGain on the live stream and switched the preamp on).  Re-apply the stored
// gain here to reproduce that nudge automatically.
static gboolean soapy_reapply_rx_gain(gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(rx==NULL || soapy_protocol_is_running()==FALSE) return G_SOURCE_REMOVE;
  soapy_protocol_set_automatic_gain(rx,radio->adc[0].agc);
  if(!radio->adc[0].agc) {
    soapy_protocol_set_gain(&radio->adc[0]);
  }
  return G_SOURCE_REMOVE;
}
#endif

int add_receiver(void *data, gboolean show_rx) {
  RADIO *r=(RADIO *)data;
  int i;
  for(i=0;i<r->discovered->supported_receivers;i++) {
    if(r->receiver[i]==NULL) {
      break;
    }
  }
  if(i<r->discovered->supported_receivers) {
g_print("add_receiver: using receiver %d\n",i);

    if (!show_rx) {
      g_print("add_receiver: no visuals %d\n",i);
      r->receiver[i]=create_receiver(i,r->sample_rate, FALSE);
    } else {
      r->receiver[i]=create_receiver(i,r->sample_rate, TRUE);
    }
    r->receivers++;
g_print("add_receiver: receivers now %d\n",r->receivers);
    switch(r->discovered->protocol) {
      case PROTOCOL_2:
        protocol2_start_receiver(r->receiver[i]);
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        soapy_protocol_create_receiver(r->receiver[i]);
        RECEIVER *rx=r->receiver[i];
        int adc=rx->adc;
        soapy_protocol_set_rx_antenna(radio->receiver[i],radio->adc[adc].antenna);
        double f=(double)(rx->frequency_a-rx->lo_a+rx->error_a);
        soapy_protocol_set_rx_frequency(radio->receiver[i]);
        soapy_protocol_set_automatic_gain(radio->receiver[i],radio->adc[adc].agc);
        for(int i=0;i<radio->discovered->info.soapy.rx_gains;i++) {
          soapy_protocol_set_gain(&radio->adc[adc]);
        }
        soapy_protocol_start_receiver(rx);
        // HackRF only latches RX gain once the stream is actually transferring;
        // re-apply shortly after streaming starts (see soapy_reapply_rx_gain).
        g_timeout_add(500,soapy_reapply_rx_gain,rx);
        break;
#endif
      default:
        break;
    }
  } else {
g_print("add_receiver: no receivers available\n");
    i = -1;
  }

  if (radio->hl2 == NULL || radio->hl2->xvtr == FALSE) {
    gtk_widget_set_sensitive(add_receiver_b,r->receivers<r->discovered->supported_receivers);
  }

  if(radio->dialog) {
    gtk_widget_destroy(radio->dialog);
    radio->dialog=NULL;
  }
  if(i>=0) radio_rebuild_rx_stack(r);
  return i;
}

int add_diversity_mixer(void *data, RECEIVER *rx_visual, RECEIVER *rx_hidden) {
  RADIO *r=(RADIO *)data;
  int i = 0;

  for (i = 0; i < MAX_DIVERSITY_MIXERS; i++) {
    if(r->divmixer[i]==NULL) {
      break;
    }
  }

  if (i < MAX_DIVERSITY_MIXERS) {

g_print("add_diversity_mixer: using diversity mixer %d\n",i);

    r->divmixer[i] = create_diversity_mixer(i, rx_visual, rx_hidden);
    rx_visual->dmix_id = i;
    rx_hidden->dmix_id = i;
    radio->diversity_mixers++;
  } else {
g_print("add_diversity_mixer: no diversity mixers available\n");
    i = -1;
  }

  return i;
}

// One-shot balancer: split the receiver stack evenly. Runs on a timeout so the
// container has a real allocated height by the time it computes positions.
static gboolean rx_stack_balance(gpointer data) {
  RADIO *r=(RADIO *)data;
  if(r->rx_container==NULL) return FALSE;
  int total=gtk_widget_get_allocated_height(r->rx_container);
  if(total<=1) return TRUE;  // not allocated yet; retry on next timeout

  int n=0;
  for(int i=0;i<r->discovered->supported_receivers;i++) {
    if(r->receiver[i]!=NULL && r->receiver[i]->table!=NULL) n++;
  }
  if(n<2) { r->rx_paned_restore=FALSE; return FALSE; }

  GList *children=gtk_container_get_children(GTK_CONTAINER(r->rx_container));
  GtkWidget *w=(children!=NULL)?GTK_WIDGET(children->data):NULL;
  g_list_free(children);

  // On the first balance after startup, restore saved divider positions (stored
  // as fractions of each pane's height); afterwards, and for a fresh config,
  // fall back to an even split.
  gboolean restore=r->rx_paned_restore;
  r->rx_paned_restore=FALSE;

  int remaining=total;
  int k=0;
  while(w!=NULL && GTK_IS_PANED(w)) {
    double frac=-1.0;
    if(restore) {
      char pname[32];
      sprintf(pname,"radio.rx_paned[%d]",k);
      char *pvalue=getProperty(pname);
      if(pvalue!=NULL) frac=atof(pvalue);
    }
    int slot;
    if(frac>0.0 && frac<1.0) slot=(int)(frac*remaining);
    else slot=remaining/(n-k);
    gtk_paned_set_position(GTK_PANED(w),slot);
    remaining-=slot;
    k++;
    w=gtk_paned_get_child2(GTK_PANED(w));
  }
  return FALSE;
}

// Rebuild the vertical stack of receiver panels in radio->rx_container. Visible
// receivers are laid out top-to-bottom; when more than one is present they are
// separated by draggable GtkPaned dividers so the user can reapportion vertical
// space. A single receiver is packed directly. Call this whenever the set of
// visible receivers changes (add/remove).
void radio_rebuild_rx_stack(RADIO *r) {
  if(r==NULL || r->rx_container==NULL) return;

  // Collect the live panels in channel order, holding a temporary reference on
  // each and unparenting it so the teardown of the old layout below does not
  // destroy them.
  GtkWidget *tables[MAX_RECEIVERS];
  int n=0;
  for(int i=0;i<r->discovered->supported_receivers;i++) {
    RECEIVER *rx=r->receiver[i];
    if(rx!=NULL && rx->table!=NULL) {
      GtkWidget *t=rx->table;
      g_object_ref(t);
      GtkWidget *parent=gtk_widget_get_parent(t);
      if(parent!=NULL) gtk_container_remove(GTK_CONTAINER(parent),t);
      tables[n++]=t;
    }
  }

  // Destroy whatever remains in the container: the old paned skeleton plus any
  // orphaned panel (e.g. a receiver that was just closed). Live panels were
  // unparented above so they survive this.
  GList *children=gtk_container_get_children(GTK_CONTAINER(r->rx_container));
  for(GList *l=children;l!=NULL;l=l->next) {
    gtk_widget_destroy(GTK_WIDGET(l->data));
  }
  g_list_free(children);

  // Build the new layout.
  if(n==1) {
    gtk_box_pack_start(GTK_BOX(r->rx_container),tables[0],TRUE,TRUE,0);
  } else if(n>1) {
    // Right-leaning chain of vertical panes: P0(t0, P1(t1, P2(t2, ... t_{n-1}))).
    GtkWidget *paned=gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *top=paned;
    for(int k=0;k<n-1;k++) {
      gtk_paned_pack1(GTK_PANED(paned),tables[k],TRUE,TRUE);
      if(k==n-2) {
        gtk_paned_pack2(GTK_PANED(paned),tables[k+1],TRUE,TRUE);
      } else {
        GtkWidget *next=gtk_paned_new(GTK_ORIENTATION_VERTICAL);
        gtk_paned_pack2(GTK_PANED(paned),next,TRUE,TRUE);
        paned=next;
      }
    }
    gtk_box_pack_start(GTK_BOX(r->rx_container),top,TRUE,TRUE,0);
  }

  // Panels are now owned by their new parents; drop the temporary refs.
  for(int k=0;k<n;k++) g_object_unref(tables[k]);

  gtk_widget_show_all(r->rx_container);

  if(n>1) g_timeout_add(100,rx_stack_balance,r);
}

void add_receivers(RADIO *r) {
  char name[80];
  char *value;
  int receivers;
  int i;

  receivers=0;
  value=getProperty("radio.receivers");
  if(value!=NULL) receivers=atoi(value);

  // always add receiver 0
  if(receivers==0) {
    r->receiver[0]=create_receiver(0,r->sample_rate, TRUE);
    r->receivers++;
    switch(r->discovered->protocol) {
      case PROTOCOL_2:
        protocol2_start_receiver(r->receiver[0]);
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        soapy_protocol_create_receiver(r->receiver[0]);
        break;
#endif
      default:
        break;
    }
  } else {
    for(i=0;i<r->discovered->supported_receivers;i++) {
      sprintf(name,"receiver[%d].channel",i);
      value=getProperty(name);
      if(value!=NULL) {
        // A receiver closed by the user is marked inactive: keep its saved
        // settings but do not recreate it (channel 0 is always active).
        sprintf(name,"receiver[%d].active",i);
        value=getProperty(name);
        if(i!=0 && value!=NULL && atoi(value)==0) continue;
        r->receiver[i]=create_receiver(i,r->sample_rate, TRUE);
        r->receivers++;
        switch(r->discovered->protocol) {
          case PROTOCOL_2:
            protocol2_start_receiver(r->receiver[i]);
            break;
#ifdef SOAPYSDR
          case PROTOCOL_SOAPYSDR:
            soapy_protocol_create_receiver(r->receiver[i]);
            break;
#endif
          default:
            break;
        }
      }
    }
  }

  for(i=0;i<r->discovered->supported_receivers;i++) {
    if(r->receiver[i]!=NULL) {
      r->active_receiver=r->receiver[i];
      break;
    }
  }

  if(radio->discovered->protocol==PROTOCOL_2) {
    protocol2_general();
    protocol2_high_priority();
    protocol2_receive_specific();
  }

}

void add_transmitter(RADIO *r) {
  r->transmitter=create_transmitter(TRANSMITTER_CHANNEL);
  r->transmitter->rx=r->receiver[0];
  if(r->transmitter->rx->split) {
    transmitter_set_mode(r->transmitter,r->transmitter->rx->mode_b);
  } else {
    transmitter_set_mode(r->transmitter,r->transmitter->rx->mode_a);
  }
  update_vfo(r->transmitter->rx);
}

int add_wideband(void *data) {
  RADIO *r=(RADIO *)data;
  r->wideband=create_wideband(WIDEBAND_CHANNEL);

  protocol1_stop();
  protocol1_run();

  if(r->discovered->protocol==PROTOCOL_2) {
    protocol2_start_wideband(r->wideband);
  }
  if(radio->dialog) {
    gtk_widget_destroy(radio->dialog);
    radio->dialog=NULL;
  }
  return 0;
}

static gboolean add_receiver_cb(GtkWidget *widget,gpointer data) {
  RADIO *r=(RADIO *)data;
  add_receiver(r, TRUE);
  return TRUE;
}

static gboolean add_wideband_cb(GtkWidget *widget,gpointer data) {
  RADIO *r=(RADIO *)data;
  add_wideband(r);
  if(r->wideband !=NULL) {
    gtk_widget_set_sensitive(add_wideband_b,FALSE);
  }
  return TRUE;
}

static gboolean configure_cb(GtkWidget *widget,gpointer data) {
  RADIO *radio=(RADIO *)data;
  if(radio->dialog==NULL) {
    radio->dialog=create_configure_dialog(radio,0);
  }
  return TRUE;
}

// Bottom-bar RX front-end toggles (ADC 0). These just flip the ADC state; the
// protocol high-priority packet applies it to hardware (no-op on the fake device).
static void adc_preamp_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->preamp=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

static void adc_att10_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->att10=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

static void adc_att20_cb(GtkWidget *widget, gpointer data) {
  ADC *adc=(ADC *)data;
  adc->att20=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

// --- bottom-bar "console" helpers (Option A: labelled modules + hairline rails) ---

// Wrap a control column under a small uppercase section label -> one module.
static GtkWidget *bar_module(const char *title, GtkWidget *content) {
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,7);
  gtk_widget_set_valign(box,GTK_ALIGN_FILL);   // stretch to the bar height
  GtkWidget *lbl=gtk_label_new(title);
  gtk_widget_set_name(lbl,"section-label");
  gtk_widget_set_halign(lbl,GTK_ALIGN_START);
  gtk_widget_set_valign(lbl,GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box),lbl,FALSE,FALSE,0);
  // content takes the remaining height and is vertically centred within it,
  // so short columns (e.g. the transmit buttons) don't hug the top.
  gtk_widget_set_valign(content,GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(box),content,TRUE,FALSE,0);
  return box;
}

// Hairline vertical rail between modules (accent = teal boundary before Setup).
static GtkWidget *bar_rail(gboolean accent) {
  GtkWidget *s=gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_name(s,accent?"bar-rail-accent":"bar-rail");
  return s;
}

// RDS Programme Type names (European RDS table; the user's live FM is European).
// The 32 programme-type names differ between the European RDS table and the
// North-American RBDS table; the active one is chosen by radio->rds_rbds.
// Code 0 = "no programme type", shown as blank.
static const char *rds_pty_name[32] = {
  "",                 "News",             "Current Affairs",  "Information",
  "Sport",            "Education",        "Drama",            "Culture",
  "Science",          "Varied",           "Pop Music",        "Rock Music",
  "Easy Listening",   "Light Classical",  "Serious Classical","Other Music",
  "Weather",          "Finance",          "Children",         "Social Affairs",
  "Religion",         "Phone In",         "Travel",           "Leisure",
  "Jazz",             "Country",          "National Music",   "Oldies",
  "Folk Music",       "Documentary",      "Alarm Test",       "Alarm"
};
static const char *rbds_pty_name[32] = {
  "",                 "News",             "Information",       "Sports",
  "Talk",             "Rock",             "Classic Rock",     "Adult Hits",
  "Soft Rock",        "Top 40",           "Country",          "Oldies",
  "Soft",             "Nostalgia",        "Jazz",             "Classical",
  "R&B",              "Soft R&B",         "Language",         "Religious Music",
  "Religious Talk",   "Personality",      "Public",           "College",
  "Spanish Talk",     "Spanish Music",    "Hip-Hop",          "",
  "",                 "Weather",          "Emergency Test",   "Emergency"
};

// Country from the Extended Country Code + the PI country-code nibble (cc). A cc
// of 0 in the table means "any" (the Americas vary the nibble). Standard RDS ECC
// table; the European ECCs E0/E1 are the most complete, others partial — unknown
// (ecc,cc) pairs fall back to a raw hex code so nothing is ever shown wrong.
static const char *rds_country_name(int ecc, int cc) {
  static const struct { unsigned char ecc, cc; const char *name; } tab[] = {
    {0xE0,0x1,"Germany"},{0xE0,0x5,"Italy"},{0xE0,0x6,"Belgium"},
    {0xE0,0x7,"Russia"},{0xE0,0xA,"Austria"},{0xE0,0xB,"Hungary"},{0xE0,0xD,"Germany"},
    {0xE1,0x1,"Greece"},{0xE1,0x2,"Cyprus"},{0xE1,0x4,"Switzerland"},{0xE1,0x6,"Finland"},
    {0xE1,0x7,"Luxembourg"},{0xE1,0x9,"Denmark"},{0xE1,0xC,"United Kingdom"},
    {0xE1,0xE,"Romania"},{0xE1,0xF,"France"},
    {0xE2,0x2,"Czech Republic"},{0xE2,0x3,"Poland"},{0xE2,0x5,"Slovakia"},
    {0xE2,0xA,"Iceland"},{0xE2,0xC,"Lithuania"},{0xE2,0xE,"Spain"},
    {0xE3,0x2,"Ireland"},{0xE3,0x8,"Netherlands"},{0xE3,0xE,"Sweden"},{0xE3,0xF,"Norway"},
    {0xA0,0x0,"United States"},{0xA1,0x0,"Canada"},
  };
  size_t i;
  for(i=0;i<sizeof(tab)/sizeof(tab[0]);i++)
    if(tab[i].ecc==ecc && (tab[i].cc==cc || tab[i].cc==0)) return tab[i].name;
  return NULL;
}

// Append to a running (p,n) cursor without overrunning; truncation stops growth.
#define RDS_APP(...) do { int _w=snprintf(p,n,__VA_ARGS__); \
    if(_w<0) _w=0; else if((size_t)_w>=n) _w=(int)n-1; p+=_w; n-=(size_t)_w; } while(0)

// Periodically refresh the 3-line RDS module, but only while the active receiver
// is demodulating broadcast FM (WFM):
//   line 0 - identity : PS, PI, PTY, Music/Speech, Stereo/Mono, TP, TA
//   line 1 - RadioText
//   line 2 - now-playing (RT+), station clock (CT), alternative frequencies (AF)
static gboolean rds_update_cb(gpointer data) {
  RADIO *r=(RADIO *)data;
  RECEIVER *rx=r->active_receiver;
  char l0[256], l1[256], l2[512];
  l0[0]=l1[0]=l2[0]=0;
  if(rx!=NULL && rx->mode_a==WFM) {
    int chn=rx->channel;
    char ps[9], rt[65], title[65], artist[65];
    int pi=GetRXAWFMRDSPI(chn);
    int have_ps=GetRXAWFMRDSPS(chn,ps);
    int have_rt=GetRXAWFMRDSRT(chn,rt);
    int pty=0,tp=0,ta=0; int have_flags=GetRXAWFMRDSFlags(chn,&pty,&tp,&ta);
    int ms=0,di=0;       int have_msdi=GetRXAWFMRDSMSDI(chn,&ms,&di);
    int have_rtp=GetRXAWFMRDSRTPlus(chn,title,artist);
    const char **ptytab=r->rds_rbds?rbds_pty_name:rds_pty_name;

    // ---- line 0: station identity ----
    { char *p=l0; size_t n=sizeof(l0);
      if(have_ps && pi)      RDS_APP("%s    PI %04X",ps,pi);
      else if(have_ps)       RDS_APP("%s",ps);
      else if(pi)            RDS_APP("PI %04X",pi);
      else                   RDS_APP("searching…");
      // country from the Extended Country Code + PI's country-code nibble
      if(pi) { int ecc=GetRXAWFMRDSECC(chn);
        if(ecc) { const char *ctry=rds_country_name(ecc,(pi>>12)&0xF);
          if(ctry) RDS_APP("  (%s)",ctry); else RDS_APP("  (ECC %02X)",ecc); } }
      if(have_flags && pty>0 && ptytab[pty][0]) RDS_APP("    %s",ptytab[pty]);
      if(have_msdi) RDS_APP("    %s",ms?"Music":"Speech");
      // Real stereo state from the 19 kHz pilot lock (not the RDS DI bit): the
      // decoder auto-blends to mono without a pilot, so this reflects what you hear.
      RDS_APP("    %s", GetRXAWFMPilotLock(chn)>0.5 ? "Stereo" : "Mono");
      if(have_flags && tp) RDS_APP("  TP");
      if(have_flags && ta) RDS_APP("  TA");
    }

    // ---- line 1: RadioText ----
    if(have_rt) snprintf(l1,sizeof(l1),"%s",rt);

    // ---- line 2: now-playing (RT+), clock (CT), alternative frequencies (AF) ----
    { char *p=l2; size_t n=sizeof(l2); int first=1;
      if(have_rtp && (title[0]||artist[0])) {
        if(title[0] && artist[0]) RDS_APP("♪ %s — %s",artist,title);
        else                      RDS_APP("♪ %s",title[0]?title:artist);
        first=0;
      }
      int cy,cmo,cd,chh,cmi;
      if(GetRXAWFMRDSCT(chn,&cy,&cmo,&cd,&chh,&cmi)) {
        RDS_APP("%s%04d-%02d-%02d %02d:%02d",first?"":"    ",cy,cmo,cd,chh,cmi); first=0;
      }
      int af[25]; int naf=GetRXAWFMRDSAF(chn,af,25);
      if(naf>0) {
        RDS_APP("%sAF:",first?"":"    "); first=0;
        for(int i=0;i<naf && n>6;i++) RDS_APP(" %.1f",af[i]/10.0);
      }
    }
  }
  for(int i=0;i<3;i++) if(r->rds_label[i]!=NULL)
    gtk_label_set_text(GTK_LABEL(r->rds_label[i]),i==0?l0:i==1?l1:l2);
  return TRUE;   // keep the timer running
}
#undef RDS_APP

static void create_visual(RADIO *r) {
  // The top row (r->visual) is now an empty spacer: all TX controls and the
  // toolbar buttons live in a single horizontal bottom bar below the receivers.
  r->visual=gtk_grid_new();

  r->bottom_bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_name(r->bottom_bar,"bottom-bar");

  if(r->can_transmit) {
    // Module: TX MONITOR - the small transmit panadapter.
    gtk_box_pack_start(GTK_BOX(r->bottom_bar),
                       bar_module("TX MONITOR",r->transmitter->panadapter),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(r->bottom_bar),bar_rail(FALSE),FALSE,FALSE,0);

    // Module: MIC & DRIVE - three stacked meters.
    GtkWidget *slider_col=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
    r->mic_level=create_mic_level(radio->transmitter);
    gtk_box_pack_start(GTK_BOX(slider_col),r->mic_level,FALSE,FALSE,0);
    r->mic_gain=create_mic_gain(radio->transmitter);
    gtk_box_pack_start(GTK_BOX(slider_col),r->mic_gain,FALSE,FALSE,0);
    r->drive_level=create_drive_level(radio->transmitter);
    gtk_box_pack_start(GTK_BOX(slider_col),r->drive_level,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(r->bottom_bar),
                       bar_module("MIC & DRIVE",slider_col),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(r->bottom_bar),bar_rail(FALSE),FALSE,FALSE,0);

    // Module: TRANSMIT - MOX / VOX / Tune.
    GtkWidget *tx_btn_col=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
    r->mox_button=gtk_toggle_button_new_with_label("MOX");
    gtk_widget_set_name(r->mox_button,"transmit-warning");
    g_signal_connect(r->mox_button,"toggled",G_CALLBACK(mox_cb),(gpointer)r);
    gtk_box_pack_start(GTK_BOX(tx_btn_col),r->mox_button,FALSE,FALSE,0);

    r->vox_button=gtk_toggle_button_new_with_label("VOX");
    gtk_widget_set_name(r->vox_button,"transmit-warning");
    g_signal_connect(r->vox_button,"toggled",G_CALLBACK(vox_cb),(gpointer)r);
    gtk_box_pack_start(GTK_BOX(tx_btn_col),r->vox_button,FALSE,FALSE,0);

    r->tune_button=gtk_toggle_button_new_with_label("Tune");
    gtk_widget_set_name(r->tune_button,"transmit-warning");
    g_signal_connect(r->tune_button,"toggled",G_CALLBACK(tune_cb),(gpointer)r);
    gtk_box_pack_start(GTK_BOX(tx_btn_col),r->tune_button,FALSE,FALSE,0);

    gtk_box_pack_start(GTK_BOX(r->bottom_bar),
                       bar_module("TRANSMIT",tx_btn_col),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(r->bottom_bar),bar_rail(FALSE),FALSE,FALSE,0);
  }

  // Module: RX FRONT-END - Preamp / Att10 / Att20 (ADC 0).
  GtkWidget *adc_col=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);

  GtkWidget *preamp_button=gtk_toggle_button_new_with_label("Preamp");
  gtk_widget_set_name(preamp_button,"toolbar-button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(preamp_button),radio->adc[0].preamp);
  g_signal_connect(preamp_button,"toggled",G_CALLBACK(adc_preamp_cb),&radio->adc[0]);
  gtk_box_pack_start(GTK_BOX(adc_col),preamp_button,FALSE,FALSE,0);

  GtkWidget *att10_button=gtk_toggle_button_new_with_label(r->att10_label);
  gtk_widget_set_name(att10_button,"toolbar-button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(att10_button),radio->adc[0].att10);
  g_signal_connect(att10_button,"toggled",G_CALLBACK(adc_att10_cb),&radio->adc[0]);
  gtk_box_pack_start(GTK_BOX(adc_col),att10_button,FALSE,FALSE,0);
  r->att10_button=att10_button;

  GtkWidget *att20_button=gtk_toggle_button_new_with_label(r->att20_label);
  gtk_widget_set_name(att20_button,"toolbar-button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(att20_button),radio->adc[0].att20);
  g_signal_connect(att20_button,"toggled",G_CALLBACK(adc_att20_cb),&radio->adc[0]);
  gtk_box_pack_start(GTK_BOX(adc_col),att20_button,FALSE,FALSE,0);
  r->att20_button=att20_button;

  gtk_box_pack_start(GTK_BOX(r->bottom_bar),
                     bar_module("RX FRONT-END",adc_col),FALSE,FALSE,0);

  // Module: RDS - a 3-line decoder readout (identity / RadioText / now-playing +
  // clock + AF). Its own block, attached to the left cluster and separated by a
  // rail; the free bottom-bar space sits to its right and each line ellipsizes.
  gtk_box_pack_start(GTK_BOX(r->bottom_bar),bar_rail(FALSE),FALSE,FALSE,0);
  GtkWidget *rds_col=gtk_box_new(GTK_ORIENTATION_VERTICAL,1);
  gtk_widget_set_hexpand(rds_col,TRUE);
  for(int i=0;i<3;i++) {
    r->rds_label[i]=gtk_label_new("");
    // Per-line names give the 3 RDS rows a typographic hierarchy in CSS:
    // 0 = station identity (accent, bold), 1 = RadioText (muted), 2 = now-playing.
    gtk_widget_set_name(r->rds_label[i], i==0?"rds-text-0":i==1?"rds-text-1":"rds-text-2");
    gtk_widget_set_halign(r->rds_label[i],GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(r->rds_label[i],TRUE);
    gtk_label_set_xalign(GTK_LABEL(r->rds_label[i]),0.0);
    if(i==0) gtk_label_set_width_chars(GTK_LABEL(r->rds_label[i]),12); // modest min
    gtk_label_set_ellipsize(GTK_LABEL(r->rds_label[i]),PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(rds_col),r->rds_label[i],FALSE,FALSE,0);
  }
  gtk_box_pack_start(GTK_BOX(r->bottom_bar),
                     bar_module("RDS",rds_col),TRUE,TRUE,0);
  g_timeout_add(500,rds_update_cb,(gpointer)r);

  // Module: SETUP - Configure / Add Receiver / Add Wideband.
  GtkWidget *tool_col=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);

  GtkWidget *configure=gtk_button_new_with_label("Configure");
  gtk_widget_set_name(configure,"toolbar-button");
  g_signal_connect(configure,"clicked",G_CALLBACK(configure_cb),(gpointer)r);
  gtk_box_pack_start(GTK_BOX(tool_col),configure,FALSE,FALSE,0);

  if(r->discovered->supported_receivers>1) {
    add_receiver_b=gtk_button_new_with_label("Add Receiver");
    gtk_widget_set_name(add_receiver_b,"toolbar-button");
    g_signal_connect(add_receiver_b,"clicked",G_CALLBACK(add_receiver_cb),(gpointer)r);
    gtk_box_pack_start(GTK_BOX(tool_col),add_receiver_b,FALSE,FALSE,0);

    if (radio->hl2 != NULL) {
      if (radio->hl2->xvtr == FALSE) {
        gtk_widget_set_sensitive(add_receiver_b,r->receivers<r->discovered->supported_receivers);
      }
      else {
        gtk_widget_set_sensitive(add_receiver_b, FALSE);
      }
    }
  }

#ifdef SOAPYSDR
  if(r->discovered->protocol!=PROTOCOL_SOAPYSDR) {
#endif
    add_wideband_b=gtk_button_new_with_label("Add Wideband");
    gtk_widget_set_name(add_wideband_b,"toolbar-button");
    g_signal_connect(add_wideband_b,"clicked",G_CALLBACK(add_wideband_cb),(gpointer)r);
    gtk_box_pack_start(GTK_BOX(tool_col),add_wideband_b,FALSE,FALSE,0);
#ifdef SOAPYSDR
  }
#endif

  // Setup module pinned to the right edge, teal accent rail to its left.
  // pack_end order: Setup first -> far right; rail next -> immediately left of it.
  gtk_box_pack_end(GTK_BOX(r->bottom_bar),bar_module("SETUP",tool_col),FALSE,FALSE,0);
  gtk_box_pack_end(GTK_BOX(r->bottom_bar),bar_rail(TRUE),FALSE,FALSE,0);

  gtk_widget_show_all(r->visual);

}

RADIO *create_radio(DISCOVERED *d) {
  RADIO *r;
  int i;

g_print("create_radio for %s %d\n",d->name,d->device);

  radio=g_new0(RADIO,1);
  r=radio;
  r->discovered=d;
  switch(d->device) {
    case DEVICE_METIS:
      r->model=ATLAS;
      break;
    case DEVICE_HERMES:
      r->model=ANAN_100;
      break;
    case DEVICE_HERMES2:
      r->model=HERMES_2;
      break;
    case DEVICE_ANGELIA:
      r->model=ANAN_100D;
      break;
    case DEVICE_ORION:
      r->model=ANAN_200D;
      break;
    case DEVICE_ORION2:
      r->model=ANAN_8000DLE;
      break;
    case DEVICE_HERMES_LITE:
      r->model=HERMES_LITE;
      break;
    case DEVICE_HERMES_LITE2:
      r->model=HERMES_LITE_2;
      break;


#ifdef SOAPYSDR
    case DEVICE_SOAPYSDR:
      r->model=SOAPYSDR;
      break;
#endif
    default:
      r->model=HERMES;
      break;
  }

  switch(r->model) {
#ifdef SOAPYSDR
    case SOAPYSDR:
      r->sample_rate=r->discovered->info.soapy.sample_rate;
      r->sample_rate=768000;
      if(strcmp(r->discovered->name,"rtlsdr")==0) {
        // RTL2832U is stable up to ~2.4 MHz; 1.92 MHz (=40*48k) is a clean ADC
        // rate that also lets the receiver offer the widest 1920000 span.
        r->sample_rate=1920000;
      } else if(strcmp(r->discovered->name,"hackrf")==0) {
        // HackRF only supports integer-MHz sample rates (1..20 MHz; practical
        // minimum 2 MHz for its 1.75 MHz baseband filter).  768 kHz is not
        // achievable, so SoapyHackRF ran the hardware at a different real rate
        // than the app assumed -> garbage waterfall.  The per-receiver
        // resampler brings this ADC rate down to rx->sample_rate.
        r->sample_rate=2000000;
      }
      r->buffer_size=2048;
      if(strcmp(r->discovered->name,"lime")==0) {
        r->alex_rx_antenna=3; // LNAW
      } else {
        r->alex_rx_antenna=0; // ANT 0
      }
      r->alex_tx_antenna=0; // ANT 1
      break;
#endif
    default:
      r->sample_rate=192000;
      r->buffer_size=2048;
      r->alex_rx_antenna=0; // ANT 1
      r->alex_tx_antenna=0; // ANT 1
      break;
  }
  r->receivers=0;
  switch(d->device) {
#ifdef SOAPYSDR
    case DEVICE_SOAPYSDR:
      r->meter_calibration=-20.0;
      r->panadapter_calibration=-20.0;
      break;
#endif
    default:
      r->meter_calibration=0.0;
      r->panadapter_calibration=0.0;
      break;
  }

  for(i=0;i<r->receivers;i++) {
    r->receiver[i]=NULL;
  }
  r->active_receiver=NULL;
  r->transmitter=NULL;

  r->diversity_mixers = 0;
  for(i=0;i< r->diversity_mixers; i++) {
    r->divmixer[i] = NULL;
  }
  r->divmixer[MAX_DIVERSITY_MIXERS+1] = NULL;

  r->can_transmit=TRUE;
#ifdef SOAPYSDR
  if(r->discovered->protocol==PROTOCOL_SOAPYSDR) {
    r->can_transmit=r->discovered->info.soapy.tx_channels>0;
  }
#endif
  r->mox=FALSE;
  r->tune=FALSE;
  r->vox=FALSE;
  r->ptt=FALSE;
  r->dot=FALSE;
  r->dash=FALSE;

  r->atlas_clock_source_128mhz=FALSE;
  r->atlas_clock_source_10mhz=0;
  r->classE=FALSE;

  r->cw_keyer_internal=TRUE;
  r->cw_keyer_sidetone_frequency=650;
  r->cw_keyer_sidetone_volume=20;
  r->cw_keyer_speed=12;
  r->cw_keyer_mode=KEYER_STRAIGHT;
  r->cw_keyer_weight=30;
  r->cw_keyer_spacing=0;
  r->cw_keyer_ptt_delay=20;
  r->cw_keyer_hang_time=300;
  r->cw_keys_reversed=FALSE;
  r->cw_breakin=FALSE;

  r->protocol1_timer = 0;
  r->hang_time_ctr = 0;
  r->cwdaemon=FALSE;

  #ifdef CWDAEMON
  r->cw_generation_mode = CWGEN_RADIO;
  r->cwdaemon_running=FALSE;
  r->cwd_port = 51000;
  r->cwd_sidetone = FALSE;
  #endif

  r->display_filled=TRUE;

  r->mic_boost=FALSE;
  r->mic_ptt_enabled=FALSE;
  r->mic_bias_enabled=FALSE;
  r->mic_ptt_tip_bias_ring=FALSE;

  r->microphone_name=NULL;
  r->local_microphone=FALSE;
  r->local_microphone_buffer_size=256;
  r->local_microphone_buffer_offset=0;
  r->local_microphone_buffer=NULL;
#ifndef __APPLE__
  r->record_handle=NULL;
#endif

  g_mutex_init(&r->local_microphone_mutex);

  g_mutex_init(&r->ring_buffer_mutex);

  g_mutex_init(&r->delete_rx_mutex);

  g_cond_init(&r->ring_buffer_cond);

  r->filter_board=ALEX;

  r->oc_tx_signal_id = g_new0(gulong, BANDS * 8);
  r->oc_rx_signal_id = g_new0(gulong, BANDS * 8);

  r->adc[0].id=0;
  r->adc[0].antenna=ANTENNA_1;
  r->adc[0].filters=AUTOMATIC;
  r->adc[0].hpf=HPF_13;
  r->adc[0].lpf=LPF_30_20;
  r->adc[0].dither=FALSE;
  r->adc[0].random=FALSE;
  r->adc[0].preamp=FALSE;
  r->adc[0].att10=FALSE;
  r->adc[0].att20=FALSE;
  r->adc[0].attenuation=0;
  r->adc_overload = 0;

  strcpy(r->att10_label,"Att10");
  strcpy(r->att20_label,"Att20");
  r->theme=0;   // Charcoal

#ifdef SOAPYSDR
  if(r->discovered->device==DEVICE_SOAPYSDR) {
    r->adc[0].gain=20;
    r->adc[0].agc=FALSE;
    if(r->can_transmit) {
      r->dac[0].antenna=radio->discovered->info.soapy.tx_antennas>0?radio->discovered->info.soapy.tx_antennas-1:0;
      r->dac[0].gain=20;
    }
  }
#endif
  r->adc[1].id=1;
  r->adc[1].antenna=ANTENNA_1;
  r->adc[1].filters=AUTOMATIC;
  r->adc[1].hpf=HPF_9_5;
  r->adc[1].lpf=LPF_60_40;
  r->adc[1].dither=FALSE;
  r->adc[1].random=FALSE;
  r->adc[1].preamp=FALSE;
  r->adc[1].att10=FALSE;
  r->adc[1].att20=FALSE;
  r->adc[1].attenuation=0;
#ifdef SOAPYSDR
  if(r->discovered->device==DEVICE_SOAPYSDR) {
    r->adc[1].gain=20;
    r->adc[1].agc=FALSE;
    if(r->can_transmit) {
      r->dac[0].antenna=radio->discovered->info.soapy.tx_antennas>0?radio->discovered->info.soapy.tx_antennas-1:0;
      r->dac[1].gain=20;
    }
  }
#endif

  r->wideband=NULL;

  r->vox_enabled=FALSE;
  r->vox_threshold=0.10;
  r->vox_hang=250;

  r->region=REGION_OTHER;


  #ifdef SOAPYSDR
  if(r->discovered->device==DEVICE_SOAPYSDR) {
    r->iqswap=TRUE;
  } else {
#endif
    r->iqswap=FALSE;
#ifdef SOAPYSDR
  }
#endif

  r->which_audio=USE_SOUNDIO;
  r->which_audio_backend=0;

  r->swr_alarm_value = 2.0;
  r->ppm_correction_value = 0;
  r->temperature_alarm_value = 50;
  r->qos_flag = FALSE;

  r->midi_enabled = FALSE;
  sprintf(r->midi_filename,"%s/.local/share/machpsdr/midi.props", g_get_home_dir());

  r->dialog=NULL;

  if (radio->discovered->device==DEVICE_HERMES_LITE2) {
    r->hl2 = create_hl2();
  }

  radio_restore_state(r);

  // Fake test device: run at 384 kHz so a wideband-FM signal (~180 kHz) sits in
  // the middle of the span with noise margin around it instead of filling the
  // whole display. Forced (overrides any persisted rate) as it is a test device.
  if(r->discovered->protocol==PROTOCOL_FAKE) {
    r->sample_rate=384000;
  }

#ifdef SOAPYSDR
  // HackRF only supports integer-MHz ADC rates; force 2 MHz regardless of any
  // persisted radio.sample_rate (e.g. a stale 768000), so the hardware always
  // gets a valid rate and the per-receiver resampler provides the (narrower)
  // waterfall span.  Without this a restored 768000 hit the hardware directly.
  if(r->discovered->device==DEVICE_SOAPYSDR &&
     strcmp(r->discovered->name,"hackrf")==0) {
    r->sample_rate=2000000;
  }
#endif

  if (radio->hl2 != NULL) hl2_init(r->hl2);

  radio_change_region(r);

#ifdef CWDAEMON
  radio_change_cwgeneration(r);
  // Check if cwdaemon was running when machpsdr was last closed cleanly (and
  // thus wrote this to the props file)
  if (r->cwdaemon_running) {
    r->cwdaemon_running = FALSE;
    r->cwdaemon = cwdaemon_start();
  }
#endif

#ifdef SOAPYSDR
  if(r->discovered->protocol==PROTOCOL_SOAPYSDR) {
    soapy_protocol_init(r,0);
  }
#endif

  create_audio(r->which_audio_backend,r->which_audio==USE_SOUNDIO?audio_get_backend_name(r->which_audio_backend):NULL);

  r->rx_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_vexpand(r->rx_container, TRUE);
  gtk_widget_set_hexpand(r->rx_container, TRUE);
  r->rx_paned_restore=TRUE;  // first stack balance restores saved divider positions

  add_receivers(r);

  // Receivers are now loaded (possibly already at the max, e.g. restored from
  // saved properties), so set the Add Receiver button sensitivity to match.
  // The button only exists when supported_receivers>1; leave HL2-with-xvtr as-is.
  if(r->discovered->supported_receivers>1 && (r->hl2==NULL || r->hl2->xvtr==FALSE)) {
    gtk_widget_set_sensitive(add_receiver_b,r->receivers<r->discovered->supported_receivers);
  }

  radio_rebuild_rx_stack(r);

  switch(r->discovered->protocol) {
    case PROTOCOL_1:
    case PROTOCOL_2:
    case PROTOCOL_FAKE:
      add_transmitter(r);
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      if(r->discovered->info.soapy.tx_channels>0) {
        add_transmitter(r);
      }
      break;
#endif
  }


  create_visual(r);

  switch(r->discovered->protocol) {
    case PROTOCOL_1:
      protocol1_init(r);
      break;
    case PROTOCOL_2:
      protocol2_init(r);
      break;
    case PROTOCOL_FAKE:
      fake_protocol_init(r);
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      soapy_protocol_set_rx_antenna(radio->receiver[0],radio->adc[0].antenna);
      for(int i=0;i<radio->discovered->info.soapy.rx_gains;i++) {
        soapy_protocol_set_gain(&radio->adc[0]);
      }
      RECEIVER *rx=r->receiver[0];
      double f=(double)(rx->frequency_a-rx->lo_a+rx->error_a);
      soapy_protocol_set_rx_frequency(radio->receiver[0]);
      soapy_protocol_set_automatic_gain(radio->receiver[0],radio->adc[0].agc);
      for(int i=0;i<radio->discovered->info.soapy.rx_gains;i++) {
        soapy_protocol_set_gain(&radio->adc[0]);
      }
      soapy_protocol_start_receiver(rx);
      // HackRF only latches RX gain into hardware once the stream is actually
      // transferring samples: gain set before/at activation is cached but not
      // physically applied, so reception stayed weak until the user nudged the
      // slider (any change, up or down, made it take effect).  Re-apply the
      // stored gain a moment after streaming has begun, emulating that nudge,
      // so the saved gain is active from the first tune.
      g_timeout_add(500,soapy_reapply_rx_gain,rx);
      if(r->can_transmit) {
        if(r->transmitter!=NULL && r->transmitter->rx==rx) {
          soapy_protocol_set_tx_antenna(r->transmitter,radio->dac[0].antenna);
          soapy_protocol_set_tx_frequency(r->transmitter);
        }
      }
      if(radio->can_transmit) {
        soapy_protocol_set_tx_gain(&radio->dac[0]);
      }
      break;
#endif
  }

#ifdef SOAPYSDR
  soapy_protocol_set_mic_sample_rate(r->sample_rate);
#endif
  /*
  if(radio->local_microphone) {
    if(audio_open_input(r)!=0) {
      radio->local_microphone=FALSE;
    }
  }
  */

  //
  // MIDIstartup must not be called before the radio is completely set up, since
  // then MIDI can asynchronously trigger actions which require the radio already
  // running. So this is the last thing we do when starting the radio.
  //
#ifdef MIDI
//  if(r->midi_enabled) {
//    r->midi_enabled=(MIDIstartup(r->midi_filename)==0);
//  }
  if(r->midi_enabled && midi_device_name!=NULL) {
    if(register_midi_device(midi_device_name)<0) {
      r->midi_enabled=false;
    }
  } else {
    r->midi_enabled=false;
  }
#endif

  g_idle_add(radio_start,(gpointer)r);


  return r;
}

void update_radio(RADIO *r) {
  // update MOX button
  g_signal_handlers_block_by_func(r->mox_button,G_CALLBACK(mox_cb),r);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->mox_button),r->mox);
  g_signal_handlers_unblock_by_func(r->mox_button,G_CALLBACK(mox_cb),r);

  // update TUNE button
  g_signal_handlers_block_by_func(r->tune_button,G_CALLBACK(tune_cb),r);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->tune_button),r->tune);
  g_signal_handlers_unblock_by_func(r->tune_button,G_CALLBACK(tune_cb),r);

  // update VOX button
  g_signal_handlers_block_by_func(r->vox_button,G_CALLBACK(vox_cb),r);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->vox_button),r->vox_enabled);
  g_signal_handlers_unblock_by_func(r->vox_button,G_CALLBACK(vox_cb),r);
}
