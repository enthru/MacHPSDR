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
#include <SoapySDR/Version.h>
#include "discovered.h"
#include "resource_path.h"
#include "soapy_discovery.h"

static int rtlsdr_count=0;
static int sdrplay_count=0;

// found: the complete Kwargs the enumeration returned for this device, or a
// synthesised set for a device named explicitly (see soapy_discovery()).  It is
// carried into SoapySDRDevice_make() and remembered for every later re-open -
// passing just "driver=" (as this used to) is enough for a device the driver
// finds by itself, but says nothing about *which* one or *where* it is.
// Returns TRUE if the device answered and was appended to discovered[].
static gboolean get_info(const char *driver, const SoapySDRKwargs *found) {
  size_t rx_rates_length, tx_rates_length, rx_gains_length, tx_gains_length, ranges_length, rx_antennas_length, tx_antennas_length, rx_bandwidth_length, tx_bandwidth_length;
  int i;
  SoapySDRKwargs args={};
  char *version;
  char *address=NULL;
  int rtlsdr_val=0;
  int sdrplay_val=0;
  char make_args[256];
  gboolean added=FALSE;

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
    return FALSE;
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
      /* take just the first 4 characters here -- but only if there ARE more
         than four: the driver owns this string, and a shorter one ("3.7") means
         writing past the end of its allocation. */
      if(strlen(info.vals[i])>4) info.vals[i][4]='\0';
      version=info.vals[i];
    }
    if(strcmp(info.keys[i],"ip,ip-addr")==0) {
      address=info.vals[i];
    }
  }

  int full_duplex=1;   // cleared below if the device says otherwise
  size_t rx_channels=SoapySDRDevice_getNumChannels(sdr, SOAPY_SDR_RX);
  log_info("Rx channels: %ld\n",(long)rx_channels);
  for(int i=0;i<rx_channels;i++) {
    log_info("Rx channel full duplex: channel=%d fullduplex=%d\n",i,SoapySDRDevice_getFullDuplex(sdr, SOAPY_SDR_RX, i));
    if(i==0 && !SoapySDRDevice_getFullDuplex(sdr, SOAPY_SDR_RX, i)) full_duplex=0;
  }

  size_t tx_channels=SoapySDRDevice_getNumChannels(sdr, SOAPY_SDR_TX);
  log_info("Tx channels: %ld\n",(long)tx_channels);
  for(int i=0;i<tx_channels;i++) {
    log_info("Tx channel full duplex: channel=%d fullduplex=%d\n",i,SoapySDRDevice_getFullDuplex(sdr, SOAPY_SDR_TX, i));
    if(i==0 && !SoapySDRDevice_getFullDuplex(sdr, SOAPY_SDR_TX, i)) full_duplex=0;
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

  log_info("float=%lu double=%lu\n",(unsigned long)sizeof(float),(unsigned long)sizeof(double));

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
    // g_strlcpy, as the make_args copy below already does: both strings come
    // from a plugin (or, for a networked device, off the network) and the
    // fields are char[64] and char[128].
    g_strlcpy(discovered[devices].name,driver,sizeof(discovered[devices].name));
    discovered[devices].supported_receivers=rx_channels;
    discovered[devices].supported_transmitters=tx_channels;
    discovered[devices].adcs=rx_channels;
    discovered[devices].status=STATE_AVAILABLE;
    g_strlcpy(discovered[devices].software_version,version,sizeof(discovered[devices].software_version));
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
    discovered[devices].info.soapy.full_duplex=full_duplex;
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
    added=TRUE;
  }

  // fv
  SoapySDRDevice_unmake(sdr);

  // Everything SoapySDR handed back that is NOT kept in discovered[].  The
  // gain, antenna and sensor lists above are stored there and deliberately
  // outlive this function; these five do not, and were leaked once per device
  // per discovery pass -- LeakSanitizer named them in CI, ~128 bytes a device.
  //
  // The order matters more than the amount: this has to happen HERE, after the
  // discovered[] entry is filled, because `version` points into info.vals[] and
  // `address` may alias hardwarekey (the sdrplay branch assigns it), so freeing
  // either earlier would copy freed memory into the device list.
  SoapySDR_free(driverkey);
  SoapySDR_free(hardwarekey);
  SoapySDRKwargs_clear(&info);
  SoapySDRStrings_clear(&formats, formats_length);

  free(ranges);

  return added;
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
// uri is used verbatim.
//
// Adding another kind of network device is one row here.  uri_fmt turns what the
// operator typed into the Soapy uri; the address is passed through unchanged if
// it already looks like one (contains ':'), so "ip:1.2.3.4" and "usb:1.4.5" can
// be typed in full when needed.
const SOAPY_NETDEV_TYPE soapy_netdev_types[] = {
  { "PlutoSDR", "plutosdr", "ip:%s", "192.168.2.1" },
};

int soapy_netdev_types_count(void) {
  return (int)(sizeof(soapy_netdev_types)/sizeof(soapy_netdev_types[0]));
}

static SOAPY_NETDEV netdev[SOAPY_NETDEV_MAX];
static int netdev_n=0;
// discovered[] index each saved device landed on this discovery run, -1 if it
// did not answer.  Rebuilt every soapy_discovery(); it is what maps a row the
// operator selected back to the entry to forget.
static int netdev_dev[SOAPY_NETDEV_MAX];
static gboolean netdev_loaded=FALSE;

static void netdev_path(char *path, size_t len) {
  g_snprintf(path,len,"%s/.local/share/machpsdr/devices.props",g_get_home_dir());
}

// Own file and own tiny parser rather than property.c: the property store is a
// single global that loadProperties() wipes, and the radio's own properties are
// loaded into it later - saving this list through it would eventually write a
// radio config into devices.props.
static void netdev_load(void) {
  char path[512];
  char line[256];
  FILE *f;

  if(netdev_loaded) return;
  netdev_loaded=TRUE;
  netdev_n=0;
  // -1 = "not in discovered[]".  Zero-initialised static storage would read as
  // "this one is discovered[0]" and make a saved device that never answered
  // look live (and discovered[0] look like ours) before discovery has run.
  for(int i=0;i<SOAPY_NETDEV_MAX;i++) netdev_dev[i]=-1;
  netdev_path(path,sizeof(path));
  f=fopen(path,"r");
  if(f==NULL) return;
  while(fgets(line,sizeof(line),f)!=NULL && netdev_n<SOAPY_NETDEV_MAX) {
    char drv[32], addr[96];
    if(line[0]=='#' || line[0]=='\n') continue;
    if(sscanf(line,"%31s %95s",drv,addr)!=2) continue;
    g_strlcpy(netdev[netdev_n].driver,drv,sizeof(netdev[netdev_n].driver));
    g_strlcpy(netdev[netdev_n].address,addr,sizeof(netdev[netdev_n].address));
    netdev_n++;
  }
  fclose(f);
  log_info("%s: %d saved network device(s)\n",__FUNCTION__,netdev_n);
}

static void netdev_save(void) {
  char path[512];
  FILE *f;

  netdev_path(path,sizeof(path));
  f=fopen(path,"w");
  if(f==NULL) {
    log_error("%s: cannot write %s\n",__FUNCTION__,path);
    return;
  }
  fprintf(f,"# MacHPSDR network devices: <driver> <address>, one per line.\n");
  fprintf(f,"# Devices no scan can find (another subnet); added in the device dialog.\n");
  for(int i=0;i<netdev_n;i++) {
    fprintf(f,"%s %s\n",netdev[i].driver,netdev[i].address);
  }
  fclose(f);
}

static const SOAPY_NETDEV_TYPE *netdev_type_for(const char *driver) {
  for(int t=0;t<soapy_netdev_types_count();t++) {
    if(strcmp(soapy_netdev_types[t].driver,driver)==0) return &soapy_netdev_types[t];
  }
  return NULL;
}

static void netdev_uri(const char *driver, const char *address, char *uri, size_t len) {
  const SOAPY_NETDEV_TYPE *type=netdev_type_for(driver);
  if(strchr(address,':')!=NULL || type==NULL) {
    g_strlcpy(uri,address,len);            // already a uri, or an unknown kind
  } else {
    g_snprintf(uri,len,type->uri_fmt,address);
  }
}

// Probe one network device.  TRUE if it answered and is now in discovered[].
static gboolean netdev_probe(const char *driver, const char *address) {
  SoapySDRKwargs args={};
  char uri[128];
  gboolean ok;

  netdev_uri(driver,address,uri,sizeof(uri));
  log_info("%s: probing %s at %s\n",__FUNCTION__,driver,uri);
  SoapySDRKwargs_set(&args, "driver", driver);
  SoapySDRKwargs_set(&args, "uri", uri);
  ok=get_info(driver,&args);
  SoapySDRKwargs_clear(&args);
  return ok;
}

int soapy_netdev_count(void) {
  netdev_load();
  return netdev_n;
}

const SOAPY_NETDEV *soapy_netdev_at(int i) {
  netdev_load();
  if(i<0 || i>=netdev_n) return NULL;
  return &netdev[i];
}

gboolean soapy_netdev_is_saved(int index) {
  netdev_load();
  for(int i=0;i<netdev_n;i++) {
    if(netdev_dev[i]==index) return TRUE;
  }
  return FALSE;
}

gboolean soapy_netdev_add(const char *driver, const char *address) {
  netdev_load();

  for(int i=0;i<netdev_n;i++) {
    if(strcmp(netdev[i].driver,driver)==0 && strcmp(netdev[i].address,address)==0) {
      // Already saved.  If it is live in this session's list there is nothing
      // to do; if it was not answering at startup, give it another go.
      if(netdev_dev[i]>=0) return TRUE;
      if(!netdev_probe(driver,address)) return FALSE;
      netdev_dev[i]=devices-1;
      return TRUE;
    }
  }

  if(netdev_n>=SOAPY_NETDEV_MAX) {
    log_error("%s: no room for another network device (max %d)\n",__FUNCTION__,SOAPY_NETDEV_MAX);
    return FALSE;
  }
  // Probe before saving: a typo should not become a permanent list entry that
  // stalls every future startup waiting for an address that answers nothing.
  if(!netdev_probe(driver,address)) return FALSE;

  g_strlcpy(netdev[netdev_n].driver,driver,sizeof(netdev[netdev_n].driver));
  g_strlcpy(netdev[netdev_n].address,address,sizeof(netdev[netdev_n].address));
  netdev_dev[netdev_n]=devices-1;
  netdev_n++;
  netdev_save();
  return TRUE;
}

gboolean soapy_netdev_forget_discovered(int index) {
  netdev_load();
  for(int i=0;i<netdev_n;i++) {
    if(netdev_dev[i]!=index) continue;
    log_info("%s: forgetting %s %s\n",__FUNCTION__,netdev[i].driver,netdev[i].address);
    for(int j=i;j<netdev_n-1;j++) {
      netdev[j]=netdev[j+1];
      netdev_dev[j]=netdev_dev[j+1];
    }
    netdev_n--;
    netdev_save();
    return TRUE;
  }
  return FALSE;
}

// The older MACHPSDR_PLUTO_URI / MACHPSDR_PLUTO_HOST escape hatch: a one-shot
// address that is used but not added to the saved list.  Returns "" if unset.
static void pluto_env_address(char *addr, size_t len) {
  const char *env;

  addr[0]='\0';
  env=getenv("MACHPSDR_PLUTO_URI");
  if(env==NULL || env[0]=='\0') env=getenv("MACHPSDR_PLUTO_HOST");
  if(env!=NULL && env[0]!='\0') g_strlcpy(addr,env,len);
}

// A build straight out of the repository loads only the SYSTEM's SoapySDR
// modules, and no distribution packages the PlutoSDR driver: Homebrew has no
// soapyplutosdr at all and Ubuntu's soapysdr-module-all carries twelve drivers
// without it.  That is why tools/build-soapy-plutosdr.sh exists and why `make
// app` and the AppImage copy what it builds into the bundle -- but nothing
// pointed a plain ./machpsdr at it, so the device this fork has done the most
// receive-path work for was the one a developer build could not see, while the
// released packages could.  Measured on this tree: "Available factories...
// hackrf, rtlsdr".
//
// So the script's own output directory joins SOAPY_SDR_PLUGIN_PATH when it is
// there, found beside the BINARY (the rule resource_path.h exists for) and
// APPENDED, so an explicit setting from the operator still comes first.  In an
// installed bundle there is no build/ next to the executable and this finds
// nothing, which is correct -- the bundle's launcher has already named its own.
//
// Deliberately not a general "load whatever is under build/": tools/soapy_null.cpp
// is a FAKE radio and must stay opt-in, or an ordinary run grows a device that
// does not exist.  It is named here so that the next person to add a module
// directory has to decide which of the two kinds it is.
static void soapy_add_local_plugin_path(void) {
  const char *exe=machpsdr_exe_dir();
  if(exe==NULL) return;                       // Windows, or unknowable
  char dir[1024];
  g_snprintf(dir,sizeof(dir),"%s/build/soapy-plutosdr/lib/SoapySDR/modules%s",
             exe,SOAPY_SDR_ABI_VERSION);
  if(!g_file_test(dir,G_FILE_TEST_IS_DIR)) return;
  const char *cur=getenv("SOAPY_SDR_PLUGIN_PATH");
  if(cur!=NULL && strstr(cur,dir)!=NULL) return;
  char *val=(cur!=NULL && cur[0]!='\0')
              ? g_strdup_printf("%s%c%s",cur,G_SEARCHPATH_SEPARATOR,dir)
              : g_strdup(dir);
  g_setenv("SOAPY_SDR_PLUGIN_PATH",val,TRUE);
  log_info("soapy_discovery: local driver build in %s\n",dir);
  g_free(val);
}

void soapy_discovery(void) {
  size_t length;
  int i;
  SoapySDRKwargs input_args={};
  char env_addr[128];
  char added_uri[SOAPY_NETDEV_MAX+1][128];
  int added_uris=0;

  log_info("%s\n",__FUNCTION__);

  // Before anything that can reach SoapySDR: modules are loaded once, on the
  // first call that needs them, and netdev_probe() below is such a call.
  soapy_add_local_plugin_path();

  // Saved network devices first: they cannot be found by any scan, and probing
  // them first keeps their discovered[] indices stable for the Forget button.
  netdev_load();
  for(i=0;i<netdev_n;i++) {
    netdev_dev[i]=-1;
    if(netdev_probe(netdev[i].driver,netdev[i].address)) {
      netdev_dev[i]=devices-1;
      netdev_uri(netdev[i].driver,netdev[i].address,added_uri[added_uris++],sizeof(added_uri[0]));
    } else {
      log_info("%s: saved device %s %s did not answer\n",__FUNCTION__,netdev[i].driver,netdev[i].address);
    }
  }

  pluto_env_address(env_addr,sizeof(env_addr));
  if(env_addr[0]!='\0') {
    log_info("%s: PlutoSDR from the environment: %s\n",__FUNCTION__,env_addr);
    if(netdev_probe("plutosdr",env_addr) && added_uris<=SOAPY_NETDEV_MAX) {
      netdev_uri("plutosdr",env_addr,added_uri[added_uris++],sizeof(added_uri[0]));
    }
  }

  // Enumerate with EMPTY args.  Anything else (a hostname in particular) makes
  // SoapySDR/libiio resolve a name on every startup, and with nothing to resolve
  // the Avahi lookup blocks until it times out (~30 s), hanging discovery.
  SoapySDRKwargs *results = SoapySDRDevice_enumerate(&input_args, &length);
  log_info("%s: length=%ld\n",__FUNCTION__,(long)length);
  for (i = 0; i < length; i++) {
    const char *driver=NULL;
    const char *uri=NULL;
    gboolean dup=FALSE;
    log_info("%s: i=%d size=%ld\n",__FUNCTION__,i,(long)results[i].size);
    for (size_t j = 0; j < results[i].size; j++) {
      log_info("%s key=%s value=%s\n",__FUNCTION__,results[i].keys[j],results[i].vals[j]);
      if(strcmp(results[i].keys[j],"driver")==0) driver=results[i].vals[j];
      if(strcmp(results[i].keys[j],"uri")==0)    uri=results[i].vals[j];
    }
    // get_info() runs after the whole result has been read, so it sees every
    // key.  (It used to be called from inside the key loop, on the "driver"
    // key, and could only ever see the keys sorting before it.)
    if(driver==NULL) continue;
    if(strcmp(driver,"audio")==0) continue;
    // A device we were told about may also be reachable by a scan (same subnet):
    // list it once.  Matching on the uri, not the driver, so a USB Pluto is
    // still offered alongside a networked one.
    for(int a=0;uri!=NULL && a<added_uris;a++) {
      if(strcmp(uri,added_uri[a])==0) dup=TRUE;
    }
    if(dup) {
      log_info("%s: %s at %s already added by hand\n",__FUNCTION__,driver,uri);
      continue;
    }
    get_info(driver,&results[i]);
  }
  SoapySDRKwargsList_clear(results, length);
}
