/* radio_state.c -- RADIO persistence (split out of radio.c).
 *
 * radio_save_state() / radio_restore_state() serialise every persisted RADIO
 * field to/from the property store. They were ~525 lines of the radio.c
 * god-file and touch only public APIs (get/setProperty, band/receiver save
 * helpers) and public RADIO struct fields, so they live in their own
 * translation unit with no shared internal header. Prototypes are in radio.h.
 */
#include <gtk/gtk.h>
#include "log.h"
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
#include "recorder.h"
#include "tx_panadapter.h"
#include "protocol1.h"
#include "protocol2.h"
#include "fake_protocol.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "reconnect.h"
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
#ifdef FT8
#include "ft8_decoder.h"
#include "ft8_panel.h"
#include "ft8_qso.h"
#endif
#ifdef SSTV
#include "sstv_decoder.h"
#include "sstv_panel.h"
#include "wefax_decoder.h"
#include "wefax_panel.h"
#include "cw_decoder.h"
#include "cw_panel.h"
#endif

#include "cwdaemon.h"
#include "dxcluster.h"
#include "tci.h"

#ifdef MIDI
#include "midi.h"
#include "midi_dialog.h"
// rather than including MIDI.h with all its internal stuff
// (e.g. enum components) we just declare the single bit thereof
// we need here to make a strict compiler happy.
int MIDIstartup(char *filename);
#endif

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

log_info("radio_save_state: %s\n",filename);
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
  setProperty("radio.station_call",radio->station_call);
  setProperty("radio.station_grid",radio->station_grid);
  sprintf(value,"%d",radio->ft8_proto);
  setProperty("radio.ft8_proto",value);
  sprintf(value,"%d",radio->decode_mode);
  setProperty("radio.decode_mode",value);
  sprintf(value,"%d",radio->hfdl_log);
  setProperty("radio.hfdl_log",value);
  sprintf(value,"%d",radio->hfdl_scan);
  setProperty("radio.hfdl_scan",value);
  sprintf(value,"%d",radio->wefax_lpm);
  setProperty("radio.wefax_lpm",value);
  sprintf(value,"%d",radio->wefax_ioc);
  setProperty("radio.wefax_ioc",value);
  sprintf(value,"%d",radio->wefax_autostart);
  setProperty("radio.wefax_autostart",value);
  sprintf(value,"%d",radio->wefax_autophase);
  setProperty("radio.wefax_autophase",value);
  sprintf(value,"%d",radio->wefax_denoise);
  setProperty("radio.wefax_denoise",value);
  sprintf(value,"%d",radio->wefax_invert);
  setProperty("radio.wefax_invert",value);
  sprintf(value,"%d",radio->apt_channel);
  setProperty("radio.apt_channel",value);
  sprintf(value,"%d",radio->apt_autosave);
  setProperty("radio.apt_autosave",value);
  setProperty("radio.apt_save_dir",radio->apt_save_dir);
  sprintf(value,"%f",radio->apt_contrast);
  setProperty("radio.apt_contrast",value);
  sprintf(value,"%f",radio->apt_brightness);
  setProperty("radio.apt_brightness",value);
  sprintf(value,"%d",radio->ft8_tx_offset);
  setProperty("radio.ft8_tx_offset",value);
  sprintf(value,"%d",radio->ft8_tx_even);
  setProperty("radio.ft8_tx_even",value);
  setProperty("radio.ft8_cq_dir",radio->ft8_cq_dir);
  sprintf(value,"%d",radio->ft8_log_udp);
  setProperty("radio.ft8_log_udp",value);
  setProperty("radio.ft8_log_udp_host",radio->ft8_log_udp_host);
  sprintf(value,"%d",radio->ft8_log_udp_port);
  setProperty("radio.ft8_log_udp_port",value);
  sprintf(value,"%d",radio->ft8_pskr);
  setProperty("radio.ft8_pskr",value);
  sprintf(value,"%d",radio->cluster_enable);
  setProperty("radio.cluster_enable",value);
  sprintf(value,"%d",radio->cluster_spots_show);
  setProperty("radio.cluster_spots_show",value);
  setProperty("radio.cluster_host",radio->cluster_host);
  sprintf(value,"%d",radio->cluster_port);
  setProperty("radio.cluster_port",value);
  setProperty("radio.cluster_login",radio->cluster_login);
  sprintf(value,"%d",radio->cluster_spots_font);
  setProperty("radio.cluster_spots_font",value);
  sprintf(value,"%d",radio->cluster_spots_on);
  setProperty("radio.cluster_spots_on",value);
  sprintf(value,"%f",radio->cluster_spots_bg_r);
  setProperty("radio.cluster_spots_bg_r",value);
  sprintf(value,"%f",radio->cluster_spots_bg_g);
  setProperty("radio.cluster_spots_bg_g",value);
  sprintf(value,"%f",radio->cluster_spots_bg_b);
  setProperty("radio.cluster_spots_bg_b",value);
  sprintf(value,"%f",radio->cluster_spots_bg_a);
  setProperty("radio.cluster_spots_bg_a",value);
  sprintf(value,"%d",radio->cluster_spots_fg_dxcc);
  setProperty("radio.cluster_spots_fg_dxcc",value);
  sprintf(value,"%f",radio->cluster_spots_fg_r);
  setProperty("radio.cluster_spots_fg_r",value);
  sprintf(value,"%f",radio->cluster_spots_fg_g);
  setProperty("radio.cluster_spots_fg_g",value);
  sprintf(value,"%f",radio->cluster_spots_fg_b);
  setProperty("radio.cluster_spots_fg_b",value);
  sprintf(value,"%f",radio->cluster_spots_fg_a);
  setProperty("radio.cluster_spots_fg_a",value);
  sprintf(value,"%d",radio->tci_enable);
  setProperty("radio.tci_enable",value);
  sprintf(value,"%d",radio->tci_port);
  setProperty("radio.tci_port",value);
  setProperty("radio.rec_dir",radio->rec_dir);
  sprintf(value,"%d",radio->rec_iq);
  setProperty("radio.rec_iq",value);
  sprintf(value,"%d",radio->rec_af);
  setProperty("radio.rec_af",value);
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
  for(i=0;i<CW_N_MEMORIES;i++) {
    sprintf(name,"radio.cw_memory[%d]",i);
    setProperty(name,radio->cw_memory[i]);
  }
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
  sprintf(value,"%f",radio->ppm_correction_value);
  setProperty("radio.ppm_correction_value",value);
  sprintf(value,"%d",radio->ppm_ref_station);
  setProperty("radio.ppm_ref_station",value);
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

  setProperty("radio.iq_player_file",radio->iq_player_file);
  sprintf(value,"%f",radio->iq_player_offset);
  setProperty("radio.iq_player_offset",value);

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

  // GTK4: no client-side window position; persist -1 (ignored on restore).
  x=-1; y=-1;
  sprintf(value,"%d",x);
  setProperty("radio.x",value);
  sprintf(value,"%d",y);
  setProperty("radio.y",value);

  width=gtk_widget_get_width(main_window);
  height=gtk_widget_get_height(main_window);
  sprintf(value,"%d",width);
  setProperty("radio.width",value);
  sprintf(value,"%d",height);
  setProperty("radio.height",value);

  // Save inter-receiver GtkPaned divider positions as fractions of each pane's
  // height, so they restore sensibly even if the window is a different size.
  if(radio->rx_container!=NULL) {
    // GTK4: iterate children directly (gtk_container_get_children is gone).
    GtkWidget *w=gtk_widget_get_first_child(radio->rx_container);
    int k=0;
    while(w!=NULL && GTK_IS_PANED(w)) {
      int ph=gtk_widget_get_height(w);
      double frac=(ph>0)?((double)gtk_paned_get_position(GTK_PANED(w))/(double)ph):0.5;
      sprintf(name,"radio.rx_paned[%d]",k);
      sprintf(value,"%f",frac);
      setProperty(name,value);
      k++;
      w=gtk_paned_get_end_child(GTK_PANED(w));
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
  gboolean cw_mem_found=FALSE;
  for(int i=0;i<CW_N_MEMORIES;i++) {
    sprintf(name,"radio.cw_memory[%d]",i);
    value=getProperty(name);
    if(value!=NULL) { cw_mem_found=TRUE; g_strlcpy(radio->cw_memory[i],value,sizeof(radio->cw_memory[i])); }
  }
  // Seed sensible defaults ONLY on a genuine first run (no cw_memory keys in the
  // props at all). Once the config has been saved, a memory the operator cleared
  // to "" persists as an empty key and is never re-seeded (memories 4-7 empty).
  if(!cw_mem_found) {
    g_strlcpy(radio->cw_memory[0],"CQ CQ CQ DE %C %C K",sizeof(radio->cw_memory[0]));
    g_strlcpy(radio->cw_memory[1],"%C",sizeof(radio->cw_memory[1]));
    g_strlcpy(radio->cw_memory[2],"73 TU",sizeof(radio->cw_memory[2]));
    g_strlcpy(radio->cw_memory[3],"R R73",sizeof(radio->cw_memory[3]));
  }
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
  if(value!=NULL) radio->ppm_correction_value=atof(value);
  value=getProperty("radio.ppm_ref_station");
  if(value!=NULL) radio->ppm_ref_station=atoi(value);
  value=getProperty("radio.wfm_deemphasis");
  if(value!=NULL) radio->wfm_deemphasis=atoi(value);
  value=getProperty("radio.rds_rbds");
  if(value!=NULL) radio->rds_rbds=atoi(value);

  value=getProperty("radio.iqswap");
  if(value) radio->iqswap=atoi(value);

  value=getProperty("radio.iq_player_file");
  if(value) g_strlcpy(radio->iq_player_file,value,sizeof(radio->iq_player_file));
  value=getProperty("radio.iq_player_offset");
  if(value) radio->iq_player_offset=atof(value);

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
