/* Copyright (C)
* 2019 - John Melton, G0ORX/N6LYT
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include "discovered.h"
#include "soapy_discovery.h"

static int rtlsdr_count=0;
static int sdrplay_count=0;

// found: the complete Kwargs the enumeration returned for this device, or a
// synthesised set for a device named explicitly (see soapy_discovery()).  It is
// carried into SoapySDRDevice_make() and remembered for every later re-open -
// passing just "driver=" (as this used to) is enough for a device the driver
// finds by itself, but says nothing about *which* one or *where* it is.
static void get_info(const char *driver, const SoapySDRKwargs *found) {
  size_t rx_rates_length, tx_rates_length, rx_gains_length, tx_gains_length, ranges_length, rx_antennas_length, tx_antennas_length, rx_bandwidth_length, tx_bandwidth_length;
  int i;
  SoapySDRKwargs args={};
  char *version;
  char *address=NULL;
  int rtlsdr_val=0;
  int sdrplay_val=0;
  char make_args[256];

  log_info("soapy_discovery: get_info: %s\n", driver);

  if(found!=NULL) {
    for(size_t k=0;k<found->size;k++) {
      SoapySDRKwargs_set(&args, found->keys[k], found->vals[k]);
    }
  }
  SoapySDRKwargs_set(&args, "driver", driver);
  if(strcmp(driver,"rtlsdr")==0) {
    rtlsdr_val=rtlsdr_count;
    rtlsdr_count++;
    // Only fall back to the positional "rtl=<n>" selector when the enumeration
    // gave us nothing better; a serial identifies the stick exactly.
    if(SoapySDRKwargs_get(&args,"serial")==NULL) {
      char count[16];
      sprintf(count,"%d",rtlsdr_val);
      SoapySDRKwargs_set(&args, "rtl", count);
    }
  } else if(strcmp(driver,"sdrplay")==0) {
    sdrplay_val=sdrplay_count;
    sdrplay_count++;
    if(SoapySDRKwargs_get(&args,"label")==NULL) {
      char label[16];
      sprintf(label,"SDRplay Dev%d",sdrplay_val);
      SoapySDRKwargs_set(&args, "label", label);
    }
  }

  char *markup=SoapySDRKwargs_toString(&args);
  g_strlcpy(make_args, markup==NULL?"":markup, sizeof(make_args));
  if(markup!=NULL) SoapySDR_free(markup);
  log_info("soapy_discovery: make args: %s\n",make_args);

  SoapySDRDevice *sdr = SoapySDRDevice_make(&args);
  if(sdr==NULL) {
    log_info("%s: SoapySdrDevice_make failed: %s\n",__FUNCTION__,SoapySDRDevice_lastError());
    SoapySDRKwargs_clear(&args);
    return;
  }
  SoapySDRKwargs_clear(&args);

  char *driverkey=SoapySDRDevice_getDriverKey(sdr);
  log_info("DriverKey=%s\n",driverkey);

  char *hardwarekey=SoapySDRDevice_getHardwareKey(sdr);
  log_info("HardwareKey=%s\n",hardwarekey);
  if(strcmp(driver,"sdrplay")==0) {
    address=hardwarekey;
  }

  SoapySDRKwargs info=SoapySDRDevice_getHardwareInfo(sdr);
  version="";
  for(i=0;i<info.size;i++) {
    log_info("soapy_discovery: hardware info key=%s val=%s\n",info.keys[i], info.vals[i]);
    if(strcmp(info.keys[i],"firmwareVersion")==0) {
      version=info.vals[i];
    }
    if(strcmp(info.keys[i],"fw_version")==0) {
      version=info.vals[i];
    }
    if(strcmp(info.keys[i],"sdrplay_api_api_version")==0) {
      /* take just the first 4 characters here */
      info.vals[i][4]='\0';
      version=info.vals[i];
    }
    if(strcmp(info.keys[i],"ip,ip-addr")==0) {
      address=info.vals[i];
    }
  }

  size_t rx_channels=SoapySDRDevice_getNumChannels(sdr, SOAPY_SDR_RX);
  log_info("Rx channels: %ld\n",rx_channels);
  for(int i=0;i<rx_channels;i++) {
    log_info("Rx channel full duplex: channel=%d fullduplex=%d\n",i,SoapySDRDevice_getFullDuplex(sdr, SOAPY_SDR_RX, i));
  }

  size_t tx_channels=SoapySDRDevice_getNumChannels(sdr, SOAPY_SDR_TX);
  log_info("Tx channels: %ld\n",tx_channels);
  for(int i=0;i<tx_channels;i++) {
    log_info("Tx channel full duplex: channel=%d fullduplex=%d\n",i,SoapySDRDevice_getFullDuplex(sdr, SOAPY_SDR_TX, i));
  }


  int sample_rate=0;
  SoapySDRRange *rx_rates=SoapySDRDevice_getSampleRateRange(sdr, SOAPY_SDR_RX, 0, &rx_rates_length);
  log_info("Rx sample rates: ");
  for (size_t i = 0; i < rx_rates_length; i++) {
    log_info("%f -> %f,", rx_rates[i].minimum, rx_rates[i].maximum);
  }
  log_info("\n");
  free(rx_rates);
  sample_rate=768000;
  if(strcmp(driver,"rtlsdr")==0) {
    sample_rate=1536000;
  }
  log_info("sample_rate selected %d\n",sample_rate);

  SoapySDRRange *tx_rates=SoapySDRDevice_getSampleRateRange(sdr, SOAPY_SDR_TX, 0, &tx_rates_length);
  log_info("Tx sample rates: ");
  for (size_t i = 0; i < tx_rates_length; i++) {
    log_info("%f -> %f (%f),", tx_rates[i].minimum, tx_rates[i].maximum, tx_rates[i].minimum/48000.0);
  }
  log_info("\n");
  free(tx_rates);

  double *bandwidths=SoapySDRDevice_listBandwidths(sdr, SOAPY_SDR_RX, 0, &rx_bandwidth_length);
  log_info("Rx bandwidths: ");
  for (size_t i = 0; i < rx_bandwidth_length; i++) {
    log_info("%f, ", bandwidths[i]);
  }
  log_info("\n");
  free(bandwidths);

  bandwidths=SoapySDRDevice_listBandwidths(sdr, SOAPY_SDR_TX, 0, &tx_bandwidth_length);
  log_info("Tx bandwidths: ");
  for (size_t i = 0; i < tx_bandwidth_length; i++) {
    log_info("%f, ", bandwidths[i]);
  }
  log_info("\n");
  free(bandwidths);

  double bandwidth=SoapySDRDevice_getBandwidth(sdr, SOAPY_SDR_RX, 0);
  log_info("RX0: bandwidth=%f\n",bandwidth);

  bandwidth=SoapySDRDevice_getBandwidth(sdr, SOAPY_SDR_TX, 0);
  log_info("TX0: bandwidth=%f\n",bandwidth);

  SoapySDRRange *ranges = SoapySDRDevice_getFrequencyRange(sdr, SOAPY_SDR_RX, 0, &ranges_length);
  log_info("Rx freq ranges: ");
  for (size_t i = 0; i < ranges_length; i++) log_info("[%f Hz -> %f Hz step=%f], ", ranges[i].minimum, ranges[i].maximum,ranges[i].step);
  log_info("\n");

  char** rx_antennas = SoapySDRDevice_listAntennas(sdr, SOAPY_SDR_RX, 0, &rx_antennas_length);
  log_info("Rx antennas: ");
  for (size_t i = 0; i < rx_antennas_length; i++) log_info("%s, ", rx_antennas[i]);
  log_info("\n");

  char** tx_antennas = SoapySDRDevice_listAntennas(sdr, SOAPY_SDR_TX, 0, &tx_antennas_length);
  log_info("Tx antennas: ");
  for (size_t i = 0; i < tx_antennas_length; i++) log_info("%s, ", tx_antennas[i]);
  log_info("\n");

  char **rx_gains = SoapySDRDevice_listGains(sdr, SOAPY_SDR_RX, 0, &rx_gains_length);

  gboolean has_automatic_gain=SoapySDRDevice_hasGainMode(sdr, SOAPY_SDR_RX, 0);
  log_info("has_automaic_gain=%d\n",has_automatic_gain);

  gboolean has_automatic_dc_offset_correction=SoapySDRDevice_hasDCOffsetMode(sdr, SOAPY_SDR_RX, 0);
  log_info("has_automaic_dc_offset_correction=%d\n",has_automatic_dc_offset_correction);

  char **tx_gains = SoapySDRDevice_listGains(sdr, SOAPY_SDR_TX, 0, &tx_gains_length);

  size_t formats_length;
  char **formats = SoapySDRDevice_getStreamFormats(sdr,SOAPY_SDR_RX,0,&formats_length);
  log_info("Rx formats: ");
  for (size_t i = 0; i < formats_length; i++) log_info("%s, ", formats[i]);
  log_info("\n");

  log_info("float=%lu double=%lu\n",sizeof(float),sizeof(double));

  size_t sensors;
  char **sensor = SoapySDRDevice_listSensors(sdr, &sensors);
  gboolean has_temp=FALSE;
  char *ptr;
  log_info("Sensors:\n");
  for (size_t i = 0; i < sensors; i++) {
  /*
    char *value=SoapySDRDevice_readSensor(sdr, sensor[i]);
    log_info("    %s=%s\n", sensor[i],value);
    if((ptr=strstr(sensor[i],"temp"))!=NULL) {
      has_temp=TRUE;
    }
   */
   log_info("    %s\n",sensor[i]);
  }

  if(devices<MAX_DEVICES) {
    discovered[devices].device=DEVICE_SOAPYSDR;
    discovered[devices].protocol=PROTOCOL_SOAPYSDR;
    strcpy(discovered[devices].name,driver);
    discovered[devices].supported_receivers=rx_channels;
    discovered[devices].supported_transmitters=tx_channels;
    discovered[devices].adcs=rx_channels;
    discovered[devices].status=STATE_AVAILABLE;
    strcpy(discovered[devices].software_version,version);
    discovered[devices].frequency_min=ranges[0].minimum;
    discovered[devices].frequency_max=ranges[0].maximum;
    discovered[devices].info.soapy.sample_rate=sample_rate;
    if(strcmp(driver,"rtlsdr")==0) {
      discovered[devices].info.soapy.rtlsdr_count=rtlsdr_val;
      discovered[devices].info.soapy.sdrplay_count=0;
    } else if(strcmp(driver,"sdrplay")==0) {
      discovered[devices].info.soapy.rtlsdr_count=0;
      discovered[devices].info.soapy.sdrplay_count=sdrplay_val;
    } else {
      discovered[devices].info.soapy.rtlsdr_count=0;
      discovered[devices].info.soapy.sdrplay_count=0;
    }
    discovered[devices].info.soapy.rx_channels=rx_channels;
    discovered[devices].info.soapy.rx_gains=rx_gains_length;
    discovered[devices].info.soapy.rx_gain=rx_gains;
    discovered[devices].info.soapy.rx_range=malloc(rx_gains_length*sizeof(SoapySDRRange));
log_info("Rx gains: \n");
    for (size_t i = 0; i < rx_gains_length; i++) {
      log_info("%s ", rx_gains[i]);
      SoapySDRRange rx_range=SoapySDRDevice_getGainElementRange(sdr, SOAPY_SDR_RX, 0, rx_gains[i]);
      log_info("%f -> %f step=%f\n",rx_range.minimum,rx_range.maximum,rx_range.step);
      discovered[devices].info.soapy.rx_range[i]=rx_range;
    }
    discovered[devices].info.soapy.rx_has_automatic_gain=has_automatic_gain;
    discovered[devices].info.soapy.rx_has_automatic_dc_offset_correction=has_automatic_dc_offset_correction;
    discovered[devices].info.soapy.rx_antennas=rx_antennas_length;
    discovered[devices].info.soapy.rx_antenna=rx_antennas;

    discovered[devices].info.soapy.tx_channels=tx_channels;
    discovered[devices].info.soapy.tx_gains=tx_gains_length;
    discovered[devices].info.soapy.tx_gain=tx_gains;
    discovered[devices].info.soapy.tx_range=malloc(tx_gains_length*sizeof(SoapySDRRange));
log_info("Tx gains: \n");
    for (size_t i = 0; i < tx_gains_length; i++) {
      log_info("%s ", tx_gains[i]);
      SoapySDRRange tx_range=SoapySDRDevice_getGainElementRange(sdr, SOAPY_SDR_TX, 0, tx_gains[i]);
      log_info("%f -> %f step=%f\n",tx_range.minimum,tx_range.maximum,tx_range.step);
      discovered[devices].info.soapy.tx_range[i]=tx_range;
    }
    discovered[devices].info.soapy.tx_antennas=tx_antennas_length;
    discovered[devices].info.soapy.tx_antenna=tx_antennas;
    discovered[devices].info.soapy.sensors=sensors;
    discovered[devices].info.soapy.sensor=sensor;
    discovered[devices].info.soapy.has_temp=has_temp;
    if(address==NULL && found!=NULL) {
      // A networked device knows where it is even when the hardware info does
      // not say so - show that instead of claiming "USB".
      const char *v=SoapySDRKwargs_get(found,"hostname");
      if(v==NULL) v=SoapySDRKwargs_get(found,"uri");
      if(v!=NULL && strcmp(v,"local:")!=0) address=(char *)v;
    }
    if(address!=NULL) {
      g_strlcpy(discovered[devices].info.soapy.address,address,sizeof(discovered[devices].info.soapy.address));
    } else {
      strcpy(discovered[devices].info.soapy.address,"USB");
    }
    g_strlcpy(discovered[devices].info.soapy.make_args,make_args,sizeof(discovered[devices].info.soapy.make_args));

    devices++;
  }

  // fv
  SoapySDRDevice_unmake(sdr);

  free(ranges);

}

// A PlutoSDR that is not on this subnet is invisible to both the USB scan and
// the mDNS one, so it has to be named explicitly.  Name it by URI and NEVER by
// hostname, however natural a hostname looks here:
//
//   - SoapyPlutoSDR's find_PlutoSDR() reuses a single Kwargs across its backend
//     loop and never clears it, so the "local" scan (which finds this PC's own
//     unrelated IIO devices) leaves uri="local:" behind, and the later
//     hostname branch hands that stale uri back as part of *our* device's
//     enumeration result;
//   - SoapySDR's Device::make() lets those enumerated args OVERRIDE the ones the
//     caller passes (Factory.cpp builds hybridArgs from the discovery result and
//     only fills the gaps from the caller), and SoapyPlutoSDR's constructor
//     tests "uri" before "hostname".
//
// So a hostname hint ends up opening iio context "local:" - "no device found in
// this context" - and no uri we pass can win, which is exactly the dead end this
// looks like from the outside.  With a uri and no hostname the Pluto find
// returns nothing at all, there is no discovery result to override us, and the
// uri is used verbatim.  Returns "" when no Pluto was named.
static void pluto_explicit_uri(char *uri, size_t len) {
  const char *env;

  uri[0]='\0';
  env=getenv("MACHPSDR_PLUTO_URI");
  if(env!=NULL && env[0]!='\0') {
    g_strlcpy(uri,env,len);
    return;
  }
  // MACHPSDR_PLUTO_HOST is the older spelling and stays supported, but it is
  // turned into a uri rather than passed on as a hostname.
  env=getenv("MACHPSDR_PLUTO_HOST");
  if(env!=NULL && env[0]!='\0') {
    if(strchr(env,':')!=NULL) g_strlcpy(uri,env,len);            // already a uri
    else g_snprintf(uri,len,"ip:%s",env);
  }
}

void soapy_discovery(void) {
  size_t length;
  int i;
  SoapySDRKwargs input_args={};
  char pluto_uri[128];

  log_info("%s\n",__FUNCTION__);

  pluto_explicit_uri(pluto_uri,sizeof(pluto_uri));
  if(pluto_uri[0]!='\0') {
    SoapySDRKwargs pluto={};
    log_info("%s: explicit PlutoSDR uri=%s\n",__FUNCTION__,pluto_uri);
    SoapySDRKwargs_set(&pluto, "driver", "plutosdr");
    SoapySDRKwargs_set(&pluto, "uri", pluto_uri);
    get_info("plutosdr", &pluto);
    SoapySDRKwargs_clear(&pluto);
  }

  // Enumerate with EMPTY args.  Anything else (a hostname in particular) makes
  // SoapySDR/libiio resolve a name on every startup, and with nothing to resolve
  // the Avahi lookup blocks until it times out (~30 s), hanging discovery.
  SoapySDRKwargs *results = SoapySDRDevice_enumerate(&input_args, &length);
  log_info("%s: length=%ld\n",__FUNCTION__,length);
  for (i = 0; i < length; i++) {
    const char *driver=NULL;
    log_info("%s: i=%d size=%ld\n",__FUNCTION__,i,results[i].size);
    for (size_t j = 0; j < results[i].size; j++) {
      log_info("%s key=%s value=%s\n",__FUNCTION__,results[i].keys[j],results[i].vals[j]);
      if(strcmp(results[i].keys[j],"driver")==0) driver=results[i].vals[j];
    }
    // get_info() runs after the whole result has been read, so it sees every
    // key.  (It used to be called from inside the key loop, on the "driver"
    // key, and could only ever see the keys sorting before it.)
    if(driver==NULL) continue;
    if(strcmp(driver,"audio")==0) continue;
    if(pluto_uri[0]!='\0' && strcmp(driver,"plutosdr")==0) {
      log_info("%s: skipping enumerated plutosdr, an explicit uri was given\n",__FUNCTION__);
      continue;
    }
    get_info(driver,&results[i]);
  }
  SoapySDRKwargsList_clear(results, length);
}
