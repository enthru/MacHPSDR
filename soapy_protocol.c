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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wdsp.h>

#include "SoapySDR/Constants.h"
#include "SoapySDR/Device.h"
#include "SoapySDR/Formats.h"
#include "SoapySDR/Version.h"

#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "protocol1.h"
#include "soapy_protocol.h"
#include "audio.h"
#include "signal.h"
#include "vfo.h"
#include "ext.h"
#include "error_handler.h"
#include "reconnect.h"

static double bandwidth=2000000.0;

#define MAX_CHANNELS 2
static SoapySDRDevice *soapy_device;
static SoapySDRStream *rx_stream[MAX_CHANNELS];
static SoapySDRStream *tx_stream;
static int soapy_rx_sample_rate;
static int soapy_tx_sample_rate;
static int max_samples;

static int samples=0;

static GThread *receive_thread_id;
static gpointer receive_thread(gpointer data);

static int actual_rate;

static gboolean running;

static int mic_sample_divisor=1;


static int max_tx_samples;
static float *output_buffer;
static int output_buffer_index;

// Half-duplex (HackRF): the RX stream must be deactivated before the TX stream
// is activated and vice-versa.  receive_thread keeps running for the lifetime
// of the app but only reads while rx_stream_active is TRUE.
static gboolean rx_stream_active=TRUE;
static int rx_channel=0;

// TX pump: WDSP's TX exchange is clocked by whatever feeds add_mic_sample().
// For voice that is the mic thread (audio.c).  For tune / no-microphone there
// is nothing to clock it, so we run a dedicated thread that feeds silence at
// the TX buffer boundary; writeStream() back-pressure paces the loop.
static GThread *tx_thread_id=NULL;
static gboolean tx_pump_running=FALSE;
static gboolean tx_stream_active=FALSE;

// TX gain range reported by the device, used to map the 0..100 drive slider
// onto the hardware TX gain (HackRF: raises VGA then turns the RF amp on).
static double tx_gain_min=0.0;
static double tx_gain_max=47.0;

SoapySDRDevice *get_soapy_device() {
  return soapy_device;
}

void soapy_protocol_set_mic_sample_rate(int rate) {
  mic_sample_divisor=rate/48000;
}

void soapy_protocol_change_sample_rate(RECEIVER *rx,int rate) {
  int rc;

  if(strcmp(radio->discovered->name,"sdrplay")==0) {
    soapy_rx_sample_rate=rx->sample_rate;
    g_print("%s: setting samplerate=%f resampled_buffer=%p resampler=%p\n",__FUNCTION__,(double)soapy_rx_sample_rate,rx->resampled_buffer,rx->resampler);
    rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_RX,rx->adc,(double)soapy_rx_sample_rate);
    if(rc!=0) {
      g_print("%s: SoapySDRDevice_setSampleRate(%f) failed: %s\n",__FUNCTION__,(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
    }
  } else if(rx->sample_rate==radio->sample_rate) {
    if(rx->resampled_buffer!=NULL) {
      rx->resampled_buffer_size=0;
    }
    if(rx->resampler!=NULL) {
      destroy_resample(rx->resampler);
      rx->resampler=NULL;
    }
  } else {
    if(rx->resampler!=NULL) {
      destroy_resample(rx->resampler);
      rx->resampler=NULL;
    }
    rx->resampled_buffer_size=2*max_samples/(radio->sample_rate/rx->sample_rate);
    rx->resampler=create_resample (1,max_samples,rx->buffer,rx->resampled_buffer,radio->sample_rate,rx->sample_rate,0.0,0,1.0);

g_print("%s: created resampler: buffer_size=%d resampled_buffer_size=%d radio->sample_rate=%d rx->sample_rate=%d\n",__FUNCTION__,max_samples*2,rx->resampled_buffer_size,radio->sample_rate,rx->sample_rate);
  }
}

void soapy_protocol_create_receiver(RECEIVER *rx) {
  int rc;

  // Idempotent: a reconnect re-runs this on an existing receiver, so release
  // any buffers/resampler from the previous session before re-allocating.
  if(rx->resampler!=NULL) {
    destroy_resample(rx->resampler);
    rx->resampler=NULL;
  }
  if(rx->buffer!=NULL) {
    g_free(rx->buffer);
    rx->buffer=NULL;
  }
  if(rx->resampled_buffer!=NULL) {
    g_free(rx->resampled_buffer);
    rx->resampled_buffer=NULL;
  }

  // Drive the hardware at the radio (ADC) sample rate — a rate the device
  // actually supports — and let the per-receiver resampler take it down to
  // rx->sample_rate.  Setting the hardware straight to rx->sample_rate asked
  // e.g. HackRF for an unsupported rate; it clamped internally while the app
  // still read the stream as rx->sample_rate -> garbage waterfall.
  soapy_rx_sample_rate=radio->sample_rate;

g_print("%s: setting bandwidth=%f\n",__FUNCTION__,bandwidth);
  rc=SoapySDRDevice_setBandwidth(soapy_device,SOAPY_SDR_RX,rx->adc,bandwidth);
  if(rc!=0) {
    g_print("%s: SoapySDRDevice_setBandwidth(%f) failed: %s\n",__FUNCTION__,(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

g_print("%s: setting samplerate=%f\n",__FUNCTION__,(double)soapy_rx_sample_rate);
  rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_RX,rx->adc,(double)soapy_rx_sample_rate);
  if(rc!=0) {
    g_print("%s: SoapySDRDevice_setSampleRate(%f) failed: %s\n",__FUNCTION__,(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

  size_t channel=rx->adc;
g_print("%s: SoapySDRDevice_setupStream: channel=%ld\n",__FUNCTION__,channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&rx_stream[channel],SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    g_print("%s: SoapySDRDevice_setupStream (RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
  rx_stream[channel]=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rx_stream[channel]==NULL) {
    g_print("%s: SoapySDRDevice_setupStream (RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    _exit(-1);
  }
#endif


  max_samples=SoapySDRDevice_getStreamMTU(soapy_device,rx_stream[channel]);
  if(max_samples>(2*rx->fft_size)) {
    max_samples=2*rx->fft_size;
  }
  rx->buffer=g_new(double,max_samples*2);
  rx->resampled_buffer=g_new(double,max_samples*2);

  if(rx->sample_rate==radio->sample_rate) {
    rx->resampler=NULL;
    rx->resampled_buffer_size=0;
  } else {
    rx->resampled_buffer_size=2*max_samples/(radio->sample_rate/rx->sample_rate);
    rx->resampler=create_resample (1,max_samples,rx->buffer,rx->resampled_buffer,radio->sample_rate,rx->sample_rate,0.0,0,1.0);
g_print("%s: created resampler: buffer_size=%d resampled_buffer_size=%d radio->sample_rate=%d rx->sample_rate=%d\n",__FUNCTION__,max_samples*2,rx->resampled_buffer_size,radio->sample_rate,rx->sample_rate);
  }

}

void soapy_protocol_start_receiver(RECEIVER *rx) {
  int rc;

g_print("%s: activate_stream\n",__FUNCTION__);

  double rate=SoapySDRDevice_getSampleRate(soapy_device,SOAPY_SDR_RX,rx->adc);
  g_print("%s: rate=%f\n",__FUNCTION__,rate);

  size_t channel=rx->adc;
  rx_channel=channel;
  rx_stream_active=TRUE;
  rc=SoapySDRDevice_activateStream(soapy_device, rx_stream[channel], 0, 0LL, 0);
  if(rc!=0) {
    g_print("%s: SoapySDRDevice_activateStream failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    _exit(-1);
  }

g_print("%s: create receive_thread\n",__FUNCTION__);
  receive_thread_id = g_thread_new( "rx_thread", receive_thread, rx);
  if( ! receive_thread_id )
  {
    g_print("%s: g_thread_new failed for receive_thread\n",__FUNCTION__);
    exit( -1 );
  }
  g_print( "%s: receive_thread: id=%p\n",__FUNCTION__,receive_thread_id);
}

void soapy_protocol_create_transmitter(TRANSMITTER *tx) {
  int rc;

g_print("soapy_protocol_create_transmitter: dac=%d\n",tx->dac);

  soapy_tx_sample_rate=tx->iq_output_rate;

g_print("soapy_protocol_create_transmitter: setting bandwidth=%f\n",bandwidth);

  rc=SoapySDRDevice_setBandwidth(soapy_device,SOAPY_SDR_TX,tx->dac,bandwidth);
  if(rc!=0) {
    g_print("soapy_protocol_create_receiver: SoapySDRDevice_setBandwidth(%f) failed: %s\n",(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

g_print("soapy_protocol_create_transmitter: setting samplerate=%f\n",(double)soapy_tx_sample_rate);
  rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_TX,tx->dac,(double)soapy_tx_sample_rate);
  if(rc!=0) {
    g_print("soapy_protocol_configure_transmitter: SoapySDRDevice_setSampleRate(%f) failed: %s\n",(double)soapy_tx_sample_rate,SoapySDR_errToStr(rc));
  }

  size_t channel=tx->dac;
g_print("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream: channel=%ld\n",channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&tx_stream,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    g_print("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (RX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
  tx_stream=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(tx_stream==NULL) {
    g_print("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (TX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#endif

  max_tx_samples=SoapySDRDevice_getStreamMTU(soapy_device,tx_stream);
  if(max_tx_samples>(2*tx->fft_size)) {
    max_tx_samples=2*tx->fft_size;
  }
g_print("soapy_protocol_create_transmitter: max_tx_samples=%d\n",max_tx_samples);
  // Idempotent: a reconnect re-runs this, so release the previous buffer first.
  if(output_buffer!=NULL) {
    free(output_buffer);
    output_buffer=NULL;
  }
  output_buffer=(float *)malloc(max_tx_samples*sizeof(float)*2);
  output_buffer_index=0;

  // Learn the hardware TX gain range so the drive slider (0..100) can be mapped
  // onto it.  On HackRF the overall TX gain spans VGA (0..47 dB) plus the RF amp
  // (+14 dB at the top), so raising drive raises gain and eventually the amp.
  SoapySDRRange grange=SoapySDRDevice_getGainRange(soapy_device,SOAPY_SDR_TX,tx->dac);
  tx_gain_min=grange.minimum;
  tx_gain_max=grange.maximum;
g_print("soapy_protocol_create_transmitter: tx gain range=%f..%f\n",tx_gain_min,tx_gain_max);

  if(radio->local_microphone) {
      if(audio_open_input(radio)!=0) {
        fprintf(stderr,"audio_open_input failed\n");
        radio->local_microphone=FALSE;
      }
    }

}

void soapy_protocol_start_transmitter(TRANSMITTER *tx) {
  int rc;

g_print("soapy_protocol_start_transmitter: activateStream\n");
  rc=SoapySDRDevice_activateStream(soapy_device, tx_stream, 0, 0LL, 0);
  if(rc!=0) {
    g_print("soapy_protocol_start_transmitter: SoapySDRDevice_activateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

void soapy_protocol_stop_transmitter(TRANSMITTER *tx) {
  int rc;

g_print("soapy_protocol_stop_transmitter: deactivateStream\n");
  rc=SoapySDRDevice_deactivateStream(soapy_device, tx_stream, 0, 0LL);
  if(rc!=0) {
    g_print("soapy_protocol_stop_transmitter: SoapySDRDevice_deactivateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

void soapy_protocol_init(RADIO *r,int rx) {
  SoapySDRKwargs args={};
  char temp[32];
  int rc;
  int i;

g_print("soapy_protocol_init\n");

  // initialize the radio
g_print("soapy_protocol_init: SoapySDRDevice_make\n");
  SoapySDRKwargs_set(&args, "driver", radio->discovered->name);
  if(strcmp(radio->discovered->name,"rtlsdr")==0) {
    sprintf(temp,"%d",radio->discovered->info.soapy.rtlsdr_count);
    SoapySDRKwargs_set(&args, "rtl", temp);
  } else if(strcmp(radio->discovered->name,"sdrplay")==0) {
    sprintf(temp,"SDRplay Dev%d",radio->discovered->info.soapy.sdrplay_count);
    SoapySDRKwargs_set(&args, "label", temp);
  }
  soapy_device=SoapySDRDevice_make(&args);
  if(soapy_device==NULL) {
    g_print("%s: SoapySDRDevice_make failed: %s\n",__FUNCTION__,SoapySDRDevice_lastError());
    _exit(-1);
  }
  SoapySDRKwargs_clear(&args);
}

static gpointer receive_thread(gpointer data) {
  double isample;
  double qsample;
  int elements;
  int flags=0;
  long long timeNs=0;
  long timeoutUs=100000L;
  int i;
  RECEIVER *rx=(RECEIVER *)data;
  float *buffer=g_new(float,max_samples*2);
  void *buffs[]={buffer};

  running=TRUE;
g_print("%s: running\n",__FUNCTION__);
  size_t channel=rx->adc;
  while(running) {
    // Paused while transmitting (half-duplex): the RX stream is deactivated,
    // so don't read from it - just idle until it is resumed.
    if(!rx_stream_active) {
      g_usleep(1000);
      continue;
    }
    elements=SoapySDRDevice_readStream(soapy_device,rx_stream[channel],buffs,max_samples,&flags,&timeNs,timeoutUs);
    if(elements<0) continue;
    if(elements>0) reconnect_note_data();   // fed the disconnect watchdog
    for(i=0;i<elements;i++) {
      rx->buffer[i*2]=(double)buffer[i*2];
      rx->buffer[(i*2)+1]=(double)buffer[(i*2)+1];
    }
    if(rx->resampler!=NULL) {
      int out_elements=xresample(rx->resampler);
      for(i=0;i<out_elements;i++) {
        if(radio->iqswap) {
          qsample=rx->resampled_buffer[i*2];
          isample=rx->resampled_buffer[(i*2)+1];
        } else {
          isample=rx->resampled_buffer[i*2];
          qsample=rx->resampled_buffer[(i*2)+1];
        }
        add_iq_samples(rx,isample,qsample);
      }
    } else {
      for(i=0;i<elements;i++) {
        isample=rx->buffer[i*2];
        qsample=rx->buffer[(i*2)+1];
        if(radio->iqswap) {
          add_iq_samples(rx,qsample,isample);
        } else {
          add_iq_samples(rx,isample,qsample);
        }
      }
    }
  }
g_print("%s: receive_thread: SoapySDRDevice_deactivateStream\n",__FUNCTION__);
  SoapySDRDevice_deactivateStream(soapy_device,rx_stream[channel],0,0LL);

  g_thread_exit((gpointer)0);
}

void soapy_protocol_process_local_mic(RADIO *r) {
  int b;
  int i;
  short sample;

// always 48000 samples per second
  b=0;
  for(i=0;i<r->local_microphone_buffer_size;i++) {
    add_mic_sample(r->transmitter,r->local_microphone_buffer[i]);
  }
}

void soapy_protocol_iq_samples(float isample,float qsample) {
  long timeoutUs=100000L;
  // Samples arrive as normalised floats (~+/-1.0) straight from WDSP - exactly
  // what the CF32 TX stream expects.  Only write while actually transmitting
  // and while the TX stream has been activated.
  if(isTransmitting(radio) && tx_stream_active) {
    output_buffer[output_buffer_index*2]=isample;
    output_buffer[(output_buffer_index*2)+1]=qsample;
    output_buffer_index++;
    if(output_buffer_index>=max_tx_samples) {
      int written=0;
      while(written<max_tx_samples) {
        int flags=0;
        long long timeNs=0;
        const void *tx_buffs[]={&output_buffer[written*2]};
        int elements=SoapySDRDevice_writeStream(soapy_device,tx_stream,tx_buffs,max_tx_samples-written,&flags,timeNs,timeoutUs);
        if(elements>0) {
          written+=elements;
        } else {
          // SOAPY_SDR_TIMEOUT / underflow / error: drop the rest of this block
          // rather than spin - the next block will keep the stream fed.
          break;
        }
      }
      output_buffer_index=0;
    }
  }
}

// ---- Half-duplex RX pause/resume ------------------------------------------

void soapy_protocol_rx_pause() {
  if(soapy_device==NULL) return;
  if(!rx_stream_active) return;
  rx_stream_active=FALSE;
  // let an in-flight readStream() return before we deactivate the stream
  g_usleep(2000);
  SoapySDRDevice_deactivateStream(soapy_device,rx_stream[rx_channel],0,0LL);
}

void soapy_protocol_rx_resume() {
  if(soapy_device==NULL) return;
  if(rx_stream_active) return;
  SoapySDRDevice_activateStream(soapy_device,rx_stream[rx_channel],0,0LL,0);
  rx_stream_active=TRUE;
}

// ---- TX pump + stream activation ------------------------------------------

static gpointer tx_thread(gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
g_print("soapy tx_thread: start\n");
  // Feed silence into the TX exchange; writeStream() back-pressure paces us.
  // For tune, WDSP's tone generator fills the output regardless of this input.
  while(tx_pump_running) {
    add_mic_sample(tx,0.0f);
  }
g_print("soapy tx_thread: exit\n");
  return NULL;
}

void soapy_protocol_activate_tx(TRANSMITTER *tx) {
  if(soapy_device==NULL) return;
  output_buffer_index=0;
  if(!tx_stream_active) {
    int rc=SoapySDRDevice_activateStream(soapy_device,tx_stream,0,0LL,0);
    if(rc!=0) {
      g_print("soapy_protocol_activate_tx: activateStream failed: %s\n",SoapySDR_errToStr(rc));
    }
    tx_stream_active=TRUE;
  }
  // Make sure the hardware TX gain matches the drive slider before we key up.
  soapy_protocol_set_tx_drive(tx->drive);
  // Only run our own pump when nothing else clocks the TX exchange.  With a
  // local microphone the mic thread already feeds add_mic_sample().
  if(!radio->local_microphone && tx_thread_id==NULL) {
    tx_pump_running=TRUE;
    tx_thread_id=g_thread_new("soapy_tx",tx_thread,tx);
  }
}

void soapy_protocol_deactivate_tx(TRANSMITTER *tx) {
  if(soapy_device==NULL) return;
  if(tx_thread_id!=NULL) {
    tx_pump_running=FALSE;
    g_thread_join(tx_thread_id);
    tx_thread_id=NULL;
  }
  if(tx_stream_active) {
    SoapySDRDevice_deactivateStream(soapy_device,tx_stream,0,0LL);
    tx_stream_active=FALSE;
  }
  output_buffer_index=0;
}

// Map the 0..100 drive slider onto the hardware TX gain range.
void soapy_protocol_set_tx_drive(double drive) {
  if(soapy_device==NULL) return;
  if(drive<0.0) drive=0.0;
  if(drive>100.0) drive=100.0;
  double g=tx_gain_min+((tx_gain_max-tx_gain_min)*(drive/100.0));
  radio->dac[0].gain=g;
  int rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,radio->dac[0].id,g);
  if(rc!=0) {
    g_print("%s: SoapySDRDevice_setGain failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
}



void soapy_protocol_stop() {
g_print("%s\n",__FUNCTION__);
  running=FALSE;
g_print("%s: g_thread_join\n",__FUNCTION__);
  g_thread_join(receive_thread_id);
}

// Re-apply the stored RX frequency and gain a moment after streaming resumes.
// HackRF only latches control settings into hardware once samples are actually
// flowing (see the note in radio.c); after a reconnect we emulate the same
// post-start "nudge" so a cold device tunes correctly instead of receiving
// garbage until the next manual re-tune.
static gboolean soapy_reconnect_reapply_gain(gpointer data) {
  if(soapy_device!=NULL) {
    soapy_protocol_set_rx_frequency(radio->receiver[0]);
    soapy_protocol_set_gain(&radio->adc[0]);
  }
  return FALSE;   // one-shot
}

// In-place hardware re-initialisation after a disconnect.  Tears the SoapySDR
// device down completely and re-makes it, then re-creates the RX stream and
// re-applies the stored settings.  Returns FALSE if the device could not be
// re-opened (e.g. still unplugged); the caller's watchdog then re-offers the
// dialog after the next timeout.  Runs on the GTK main thread.
gboolean soapy_protocol_reconnect(RECEIVER *rx) {
  SoapySDRKwargs args={};
  char temp[32];
  size_t channel=rx->adc;

g_print("%s: tearing down old device/streams\n",__FUNCTION__);

  // Stop the receive thread (it may be spinning on read errors from the dead
  // device).  receive_thread_id can be NULL if a previous reconnect failed.
  if(running) {
    running=FALSE;
    if(receive_thread_id!=NULL) {
      g_thread_join(receive_thread_id);
      receive_thread_id=NULL;
    }
  }

  // Best-effort teardown of streams and device.  These calls may fail on an
  // already-vanished device; that is fine, we discard it either way.
  if(soapy_device!=NULL) {
    if(rx_stream[channel]!=NULL) {
      SoapySDRDevice_deactivateStream(soapy_device,rx_stream[channel],0,0LL);
      SoapySDRDevice_closeStream(soapy_device,rx_stream[channel]);
      rx_stream[channel]=NULL;
    }
    if(tx_stream!=NULL) {
      SoapySDRDevice_deactivateStream(soapy_device,tx_stream,0,0LL);
      SoapySDRDevice_closeStream(soapy_device,tx_stream);
      tx_stream=NULL;
    }
    SoapySDRDevice_unmake(soapy_device);
    soapy_device=NULL;
  }
  tx_stream_active=FALSE;

  // Re-make the device (same key args as soapy_protocol_init).  Unlike init we
  // must NOT abort the whole app on failure - the user may simply not have
  // plugged the device back in yet.
  SoapySDRKwargs_set(&args, "driver", radio->discovered->name);
  if(strcmp(radio->discovered->name,"rtlsdr")==0) {
    sprintf(temp,"%d",radio->discovered->info.soapy.rtlsdr_count);
    SoapySDRKwargs_set(&args, "rtl", temp);
  } else if(strcmp(radio->discovered->name,"sdrplay")==0) {
    sprintf(temp,"SDRplay Dev%d",radio->discovered->info.soapy.sdrplay_count);
    SoapySDRKwargs_set(&args, "label", temp);
  }
  soapy_device=SoapySDRDevice_make(&args);
  SoapySDRKwargs_clear(&args);
  if(soapy_device==NULL) {
    g_print("%s: SoapySDRDevice_make failed: %s\n",__FUNCTION__,SoapySDRDevice_lastError());
    return FALSE;
  }

g_print("%s: re-making streams and re-applying settings\n",__FUNCTION__);

  // Re-create the TX stream FIRST, before the receiver.  On a half-duplex device
  // (HackRF) RX and TX share one hardware sample-rate clock, so whichever stream
  // is configured last wins.  The initial start path sets up TX (via
  // add_transmitter) before the receiver, leaving the RX rate applied last; we
  // must keep that order here or create_transmitter's TX rate would clobber the
  // live RX rate and the RX stream would stop delivering samples (gain still
  // sets, but no audio / no waterfall).
  if(radio->can_transmit && radio->transmitter!=NULL && radio->transmitter->rx==rx) {
    // create_transmitter re-opens the mic input; close the old handle first so
    // we don't leak/double-open it across a reconnect.
    if(radio->local_microphone) {
      audio_close_input(radio);
    }
    soapy_protocol_create_transmitter(radio->transmitter);
    soapy_protocol_set_tx_antenna(radio->transmitter,radio->dac[0].antenna);
    soapy_protocol_set_tx_frequency(radio->transmitter);
    soapy_protocol_set_tx_gain(&radio->dac[0]);
  }

  // Rebuild the RX stream (create_receiver is idempotent - it also re-applies the
  // RX sample rate, which must be the last rate set) and re-apply the stored
  // antenna / gain / frequency / AGC, mirroring the initial start path in radio.c.
  soapy_protocol_create_receiver(rx);
  soapy_protocol_set_rx_antenna(rx,radio->adc[0].antenna);
  for(int i=0;i<radio->discovered->info.soapy.rx_gains;i++) {
    soapy_protocol_set_gain(&radio->adc[0]);
  }
  soapy_protocol_set_rx_frequency(rx);
  soapy_protocol_set_automatic_gain(rx,radio->adc[0].agc);
  for(int i=0;i<radio->discovered->info.soapy.rx_gains;i++) {
    soapy_protocol_set_gain(&radio->adc[0]);
  }

  soapy_protocol_start_receiver(rx);
  g_timeout_add(500,soapy_reconnect_reapply_gain,NULL);

g_print("%s: done\n",__FUNCTION__);
  return TRUE;
}

void soapy_protocol_set_rx_frequency(RECEIVER *rx) {
  int rc;

  if(soapy_device!=NULL) {
    double f=(double)(rx->frequency_a-rx->lo_a+rx->error_a);
    if(!rx->ctun) {
      if(rx->rit_enabled) {
        f+=(double)rx->rit;
      }
    }
    //g_print("%s: %f\n",__FUNCTION__,f);
    rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_RX,rx->adc,f,NULL);
    if(rc!=0) {
      g_print("%s: SoapySDRDevice_setFrequency(RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_frequency(TRANSMITTER *tx) {
  int rc;
  double f;

  if(soapy_device!=NULL) {
    RECEIVER* rx=tx->rx;
    if(rx!=NULL) {
      if(rx->split) {
        f=rx->frequency_b-rx->lo_b+rx->error_b;
      } else {
        if(rx->ctun) {
          f=rx->ctun_frequency-rx->lo_a+rx->error_a;
        } else {
          f=rx->frequency_a-rx->lo_a+rx->error_a;
        }
      }
      if(tx->xit_enabled) {
        f+=(double)tx->xit;
      }
//g_print("%s: %f\n",__FUNCTION__,f);
      rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_TX,tx->dac,f,NULL);
      if(rc!=0) {
        g_print("%s: SoapySDRDevice_setFrequency(TX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
      }
    }
  }
}

void soapy_protocol_set_rx_antenna(RECEIVER *rx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    g_print("%s: set_rx_antenna: %s\n",__FUNCTION__,radio->discovered->info.soapy.rx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_RX,rx->adc,radio->discovered->info.soapy.rx_antenna[ant]);
    if(rc!=0) {
      g_print("%s: SoapySDRDevice_setAntenna RX failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_antenna(TRANSMITTER *tx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    g_print("%s: %s\n",__FUNCTION__,radio->discovered->info.soapy.tx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_TX,tx->dac,radio->discovered->info.soapy.tx_antenna[ant]);
    if(rc!=0) {
      g_print("%s: SoapySDRDevice_setAntenna TX failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_gain(ADC *adc) {
  int rc;
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_RX,adc->id,adc->gain);
  if(rc!=0) {
    g_print("%s: SoapySDRDevice_setGain failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_tx_gain(DAC *dac) {
  int rc;
g_print("%s: dac=%d gain=%f\n",__FUNCTION__,dac->id,dac->gain);
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,dac->id,dac->gain);
  if(rc!=0) {
    g_print("%s: SoapySDRDevice_setGain failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
}

int soapy_protocol_get_gain(ADC *adc) {
  double gain;
  gain=SoapySDRDevice_getGain(soapy_device,SOAPY_SDR_RX,adc->id);
  return (int)gain;
}

gboolean soapy_protocol_get_automatic_gain(ADC *adc) {
  gboolean mode=SoapySDRDevice_getGainMode(soapy_device, SOAPY_SDR_RX, adc->id);
  return mode;
}

void soapy_protocol_set_automatic_gain(RECEIVER *rx,gboolean mode) {
  int rc;
  rc=SoapySDRDevice_setGainMode(soapy_device, SOAPY_SDR_RX, rx->adc,mode);
  if(rc!=0) {

    g_print("%s: SoapySDRDevice_getGainMode failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
}

char *soapy_protocol_read_sensor(char *name) {
  return SoapySDRDevice_readSensor(soapy_device, name);
}

gboolean soapy_protocol_is_running() {
  return running;
}
