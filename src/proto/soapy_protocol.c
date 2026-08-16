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
#include "log.h"
#include <stdlib.h>
#include <unistd.h>
#include <wdsp.h>

#include "SoapySDR/Constants.h"
#include "SoapySDR/Device.h"
#include "SoapySDR/Errors.h"
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
// The block size each receiver's buffers and resampler were built for.
// max_samples above is a GLOBAL that the next receiver's create_receiver
// rewrites from its own MTU and fft_size, so anything that rebuilds one
// receiver's resampler later must use ITS size, not whatever the last receiver
// left behind (the same trap the receive thread's `const int block` avoids).
static int rx_block[MAX_CHANNELS];
// Set by each receive thread while it is parked on the rx_stream_active flag,
// cleared while it may be inside readStream().  soapy_protocol_rx_pause() waits
// for it rather than sleeping a fixed 2 ms: readStream's timeout is 100 ms, so
// the sleep was fifty times too short and the stream could be deactivated --
// and, on the next resume, CLOSED -- under a reader still inside the driver.
static volatile gint rx_parked[MAX_CHANNELS];

static int samples=0;

// One receive thread and one RX stream PER RECEIVER, indexed by rx->adc (which
// create_receiver hands out as 0 for receiver 0 and 1 for the second one on
// PROTOCOL_SOAPYSDR).  These used to be single globals, so adding a second
// receiver overwrote the thread handle and closing one stopped nothing: the
// thread ran for the rest of the process, its stream was never closed and the
// buffer it read into was never freed.  Ten add/close cycles under the null
// driver leaked ten of each, all still calling readStream().
static GThread *receive_thread_id[MAX_CHANNELS];
static gboolean rx_thread_running[MAX_CHANNELS];
static gpointer receive_thread(gpointer data);

// The rate the RX stream is REALLY running at, as the device reports it after
// being asked for radio->sample_rate.  Those are not always the same number and
// a driver owes us no error when it substitutes: SoapyPlutoSDR answers a request
// for 768 kHz by running the AD9361 at 6.144 MHz (8x -- the part cannot deliver
// a stream below ~2.08 MHz), returns success, and only says so through
// getSampleRate().  Every earlier version of this file built the resampler from
// the REQUESTED rate and believed it, so the stream arrived eight times faster
// than the DSP consumed it: the driver dropped most of it, and samples either
// side of a dropped block are spliced, which randomises the phase of every
// signal on the band.  Measured on a PlutoSDR: FT8's 15 s slots landed 53 s
// apart in an I/Q recording, carriers smeared into ~35 Hz humps, FM demodulated
// to something that was not speech, and the audio sink discarded 72 %.
// Whatever the device answers is the truth -- the resampler is built from THIS.
static int soapy_rx_actual_rate;

// TRUE while any receive thread is alive; what soapy_protocol_is_running()
// answers, and what the receiver/radio update timers gate their display on.
static gboolean running;

static int mic_sample_divisor=1;


static int max_tx_samples;
// TX samples the device refused to take (see soapy_protocol_iq_samples) and TX
// underflows the driver reported, both drained by the pump thread's 5 s report.
static long long tx_dropped;
static long long tx_underflows;
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

SoapySDRDevice *get_soapy_device(void) {
  return soapy_device;
}

void soapy_protocol_set_mic_sample_rate(int rate) {
  mic_sample_divisor=rate/48000;
}

/* Asks the device for `requested` and returns what it is ACTUALLY running at.
   A driver may substitute silently and still return success (see
   soapy_rx_actual_rate), so the readback -- never the request -- is what the
   rest of this file may rely on. */
static int soapy_set_rx_rate(size_t adc,int requested) {
  int rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_RX,adc,(double)requested);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setSampleRate(%d) failed: %s\n",__FUNCTION__,requested,SoapySDR_errToStr(rc));
  }
  double got=SoapySDRDevice_getSampleRate(soapy_device,SOAPY_SDR_RX,adc);
  int actual=(int)(got+0.5);
  if(actual<=0) {
    log_error("%s: device reports an RX rate of %f; falling back to the requested %d\n",__FUNCTION__,got,requested);
    actual=requested;
  } else if(actual!=requested) {
    // Not a warning to be tidied away: at this point the operator's band is
    // being resampled by a ratio nothing else in the app knows about.
    log_error("%s: asked the device for %d Hz, it is running at %d Hz (x%.4f) -- resampling from the rate it reports\n",
              __FUNCTION__,requested,actual,(double)actual/(double)requested);
  }
  soapy_rx_actual_rate=actual;
  return actual;
}

/* (Re)builds rx->resampled_buffer and rx->resampler to take `block` frames at
   the stream's real rate down to rx->sample_rate.  One implementation for both
   callers -- create_receiver and the sample-rate change had drifted into two
   copies of it, and only one of them was ever fixed.
   Callers hold delete_rx_mutex whenever a receive thread is alive. */
static void soapy_build_resampler(RECEIVER *rx,int block) {
  if(rx->resampler!=NULL) {
    destroy_resample(rx->resampler);
    rx->resampler=NULL;
  }
  int in_rate=(soapy_rx_actual_rate>0)?soapy_rx_actual_rate:radio->sample_rate;

  /* A device that lands a hair off the rate it was asked for is not substituting
     -- it is the same rate with a rounding error, and treating the two as
     different numbers is catastrophic.  A Pluto asked for 2304000 reports
     2303999: one hertz, 0.43 ppm -- and gcd(2303999,192000) is 1, so
     create_resample() interpolates by 192000 and stops resampling in any
     meaningful sense.  Measured: a single 10 kHz tone comes out as TWO peaks
     0.26 dB apart, at 8.80 and 11.20 kHz, and at the 768k span at 5.20 and
     14.80 kHz.  That is what "duplicated signals everywhere" looks like from
     the operator's chair.  So snap to the nearest exact integer ratio when the
     reported rate is within 100 ppm of one: the timebase error that leaves is
     three orders of magnitude below the ppm correction the operator already
     dials by hand, while the ratio becomes exact.  Integer arithmetic only --
     this is a rounding fix and has no business doing its own rounding. */
  const int mult=(in_rate+rx->sample_rate/2)/rx->sample_rate;
  if(mult>=1) {
    const long long snapped=(long long)mult*(long long)rx->sample_rate;
    const long long diff=(snapped>in_rate)?snapped-in_rate:(long long)in_rate-snapped;
    if(diff!=0 && diff*10000LL<(long long)in_rate) {     // within 100 ppm
      log_info("%s: stream reports %d Hz; snapping to %lld Hz -- exactly %dx the receiver's %d "
               "(%lld Hz out, %.2f ppm), because an inexact ratio is not resampled but split\n",
               __FUNCTION__,in_rate,snapped,mult,rx->sample_rate,diff,
               1.0e6*(double)diff/(double)in_rate);
      in_rate=(int)snapped;
    }
  }

  /* Size the output for the WORST case rather than for the ratio we expect: a
     device reporting a rate BELOW rx->sample_rate makes this an upsampler, and
     the old `2*block/(in/out)` both divided by zero there and under-sized the
     buffer xresampleV writes into.  resampled_buffer_size is the allocated
     capacity in doubles. */
  long long out_frames=((long long)block*(long long)rx->sample_rate+in_rate-1)/in_rate;
  if(out_frames<block) out_frames=block;
  const int need=(int)(out_frames*2)+16;
  if(rx->resampled_buffer==NULL || rx->resampled_buffer_size<need) {
    if(rx->resampled_buffer!=NULL) g_free(rx->resampled_buffer);
    rx->resampled_buffer=g_new(double,need);
    rx->resampled_buffer_size=need;
  }

  if(in_rate==rx->sample_rate) {
    log_info("%s: no resampler needed: stream and receiver are both at %d\n",__FUNCTION__,in_rate);
    return;
  }

  /* create_resample() reduces in:out by their gcd and interpolates by the L that
     falls out, so a substituted rate that shares no factor with rx->sample_rate
     asks it for an L of six figures -- and it does not refuse, it quietly gets
     the ratio wrong.  Measured: 2083333 -> 192000 (gcd 1) reads a 10.000 kHz
     tone at 10.155 kHz, a 1.55 % error on every frequency the operator sees,
     while 2100000 -> 192000 (gcd 12000) is exact.  Nothing downstream can detect
     that, so say it here. */
  int a=in_rate,b=rx->sample_rate;
  while(b!=0) { int t=a%b; a=b; b=t; }
  const int interp=rx->sample_rate/a;
  if(interp>1024) {
    log_error("%s: the stream's %d Hz and the receiver's %d Hz share no useful factor "
              "(gcd %d, interpolation %d) -- the resampler cannot hold this ratio and the "
              "frequency scale will be off by a percent or so.  Pick another receiver rate.\n",
              __FUNCTION__,in_rate,rx->sample_rate,a,interp);
  }
  rx->resampler=create_resample(1,block,rx->buffer,rx->resampled_buffer,in_rate,rx->sample_rate,0.0,0,1.0);
log_info("%s: created resampler: block=%d stream=%d -> rx=%d resampled_buffer=%d doubles\n",__FUNCTION__,block,in_rate,rx->sample_rate,rx->resampled_buffer_size);
}

/* Rebuilds rx->resampler, which the receive thread is using inside
   delete_rx_mutex -- so the caller must hold that lock.  The public wrapper
   below takes it; receiver_change_sample_rate takes it itself, BEFORE
   rx->mutex, because the receive thread takes them in that order
   (delete_rx_mutex around the block, then rx->mutex inside full_rx_buffer) and
   the other order is a deadlock. */
void soapy_protocol_change_sample_rate_locked(RECEIVER *rx,int rate) {
  const int block=rx_block[rx->adc<MAX_CHANNELS?rx->adc:0];

  // sdrplay is the one device driven straight at the receiver's rate instead of
  // at the ADC rate.  It still goes through the readback: if the hardware lands
  // somewhere else, the builder below puts a resampler in rather than pretending.
  if(strcmp(radio->discovered->name,"sdrplay")==0) {
    soapy_rx_sample_rate=rx->sample_rate;
    soapy_set_rx_rate(rx->adc,soapy_rx_sample_rate);
  }
  soapy_build_resampler(rx,block);
}

void soapy_protocol_change_sample_rate(RECEIVER *rx,int rate) {
  g_mutex_lock(&radio->delete_rx_mutex);
  soapy_protocol_change_sample_rate_locked(rx,rate);
  g_mutex_unlock(&radio->delete_rx_mutex);
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

log_info("%s: setting bandwidth=%f\n",__FUNCTION__,bandwidth);
  rc=SoapySDRDevice_setBandwidth(soapy_device,SOAPY_SDR_RX,rx->adc,bandwidth);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setBandwidth(%f) failed: %s\n",__FUNCTION__,(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

log_info("%s: setting samplerate=%f\n",__FUNCTION__,(double)soapy_rx_sample_rate);
  soapy_set_rx_rate(rx->adc,soapy_rx_sample_rate);

  size_t channel=rx->adc;
  if(channel>=MAX_CHANNELS) {
    log_error("%s: adc %ld has no stream slot (MAX_CHANNELS=%d)\n",__FUNCTION__,(long)channel,MAX_CHANNELS);
    return;
  }
  // Defensive: never set a second stream up over a live one.  The normal paths
  // (delete_receiver -> soapy_protocol_stop_receiver, and reconnect) have
  // already closed it and NULLed the slot; this is the belt for anything that
  // has not.  Overwriting the pointer instead leaked the old stream AND left
  // the old thread reading the new one.
  if(soapy_device!=NULL && rx_stream[channel]!=NULL && receive_thread_id[channel]==NULL) {
    SoapySDRDevice_deactivateStream(soapy_device,rx_stream[channel],0,0LL);
    SoapySDRDevice_closeStream(soapy_device,rx_stream[channel]);
    rx_stream[channel]=NULL;
  }
log_info("%s: SoapySDRDevice_setupStream: channel=%ld\n",__FUNCTION__,(long)channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&rx_stream[channel],SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setupStream (RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
  rx_stream[channel]=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rx_stream[channel]==NULL) {
    log_info("%s: SoapySDRDevice_setupStream (RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    _exit(-1);
  }
#endif


  max_samples=SoapySDRDevice_getStreamMTU(soapy_device,rx_stream[channel]);
  if(max_samples>(2*rx->fft_size)) {
    max_samples=2*rx->fft_size;
  }
  rx_block[channel]=max_samples;
  rx->buffer=g_new(double,max_samples*2);
  // The freeing above cleared it; the builder allocates it to the size the real
  // stream rate calls for.
  rx->resampled_buffer_size=0;
  soapy_build_resampler(rx,max_samples);
}

void soapy_protocol_start_receiver(RECEIVER *rx) {
  int rc;

log_info("%s: activate_stream\n",__FUNCTION__);

  // Re-assert the RX sample rate right before activating the stream.  HackRF is
  // half-duplex with a single shared hardware clock, so whichever of RX/TX ran
  // setSampleRate last wins.  On the cold-start path add_transmitter ->
  // create_transmitter sets the TX rate AFTER create_receiver set the RX rate,
  // leaving the hardware clock at the TX rate while the app's resampler/DSP still
  // expect the RX rate -> artefacts (a live reconnect happened to set RX last and
  // sounded clean, which is why toggling the device "fixed" it).  Setting it here
  // makes the RX rate authoritative at activation, independent of call order.
  const int rate_before=soapy_rx_actual_rate;
  const int rate=soapy_set_rx_rate(rx->adc,soapy_rx_sample_rate);
  log_info("%s: rate=%d\n",__FUNCTION__,rate);

  size_t channel=rx->adc;
  if(channel>=MAX_CHANNELS) {
    log_error("%s: adc %ld has no stream slot (MAX_CHANNELS=%d)\n",__FUNCTION__,(long)channel,MAX_CHANNELS);
    return;
  }
  if(rate!=rate_before) {
    // The re-assert landed somewhere else than create_receiver's resampler was
    // built for.  This channel's receive thread is started further down, so
    // nothing is reading the buffer and the rebuild needs no lock.
    soapy_build_resampler(rx,rx_block[channel]);
  }
  rx_channel=channel;
  rx_stream_active=TRUE;
  rc=SoapySDRDevice_activateStream(soapy_device, rx_stream[channel], 0, 0LL, 0);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_activateStream failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    _exit(-1);
  }

log_info("%s: create receive_thread\n",__FUNCTION__);
  rx_thread_running[channel]=TRUE;
  running=TRUE;
  receive_thread_id[channel] = g_thread_new( "rx_thread", receive_thread, rx);
  if( ! receive_thread_id[channel] )
  {
    log_info("%s: g_thread_new failed for receive_thread\n",__FUNCTION__);
    exit( -1 );
  }
  log_info( "%s: receive_thread: id=%p\n",__FUNCTION__,receive_thread_id[channel]);
}

void soapy_protocol_create_transmitter(TRANSMITTER *tx) {
  int rc;

log_info("soapy_protocol_create_transmitter: dac=%d\n",tx->dac);

  soapy_tx_sample_rate=tx->iq_output_rate;

log_info("soapy_protocol_create_transmitter: setting bandwidth=%f\n",bandwidth);

  rc=SoapySDRDevice_setBandwidth(soapy_device,SOAPY_SDR_TX,tx->dac,bandwidth);
  if(rc!=0) {
    log_info("soapy_protocol_create_receiver: SoapySDRDevice_setBandwidth(%f) failed: %s\n",(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

log_info("soapy_protocol_create_transmitter: setting samplerate=%f\n",(double)soapy_tx_sample_rate);
  rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_TX,tx->dac,(double)soapy_tx_sample_rate);
  if(rc!=0) {
    log_info("soapy_protocol_configure_transmitter: SoapySDRDevice_setSampleRate(%f) failed: %s\n",(double)soapy_tx_sample_rate,SoapySDR_errToStr(rc));
  }

  // Read the TX rate back for the same reason the RX one is (soapy_rx_actual_rate
  // above): a driver may substitute silently and return success.  There is no TX
  // resampler in this path -- WDSP is opened to produce exactly
  // tx->iq_output_rate and those samples go straight to the DAC -- so a
  // substitution here is not something to adapt to, it is something that puts
  // the whole transmitted signal on the wrong clock.  Nothing can be done about
  // it here; what can be done is to say so instead of transmitting into the dark.
  {
    double got=SoapySDRDevice_getSampleRate(soapy_device,SOAPY_SDR_TX,tx->dac);
    int actual=(int)(got+0.5);
    // A hair off is the same rate with a rounding error (a Pluto answers 2303999
    // for 2304000, 0.43 ppm) and means nothing here: there is no ratio to keep
    // exact, only a DAC clock.  Anything further out is a real substitution.
    long long diff=(actual>soapy_tx_sample_rate)?actual-soapy_tx_sample_rate
                                                :(long long)soapy_tx_sample_rate-actual;
    if(actual>0 && diff*10000LL>=(long long)soapy_tx_sample_rate) {
      log_error("soapy_protocol_create_transmitter: asked the device for %d Hz on TX, it is running at "
                "%d Hz (x%.4f) -- the transmitted signal will be off by that factor\n",
                soapy_tx_sample_rate,actual,(double)actual/(double)soapy_tx_sample_rate);
    } else {
      log_info("soapy_protocol_create_transmitter: TX rate readback %d Hz\n",actual);
    }
  }

  size_t channel=tx->dac;
log_info("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream: channel=%ld\n",(long)channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&tx_stream,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    log_info("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (RX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
  tx_stream=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(tx_stream==NULL) {
    log_info("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (TX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#endif

  max_tx_samples=SoapySDRDevice_getStreamMTU(soapy_device,tx_stream);
  if(max_tx_samples>(2*tx->fft_size)) {
    max_tx_samples=2*tx->fft_size;
  }
log_info("soapy_protocol_create_transmitter: max_tx_samples=%d\n",max_tx_samples);
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
log_info("soapy_protocol_create_transmitter: tx gain range=%f..%f\n",tx_gain_min,tx_gain_max);

  if(radio->local_microphone) {
      if(audio_open_input(radio)!=0) {
        log_error("audio_open_input failed\n");
        radio->local_microphone=FALSE;
      }
    }

}

void soapy_protocol_start_transmitter(TRANSMITTER *tx) {
  int rc;

log_info("soapy_protocol_start_transmitter: activateStream\n");
  rc=SoapySDRDevice_activateStream(soapy_device, tx_stream, 0, 0LL, 0);
  if(rc!=0) {
    log_info("soapy_protocol_start_transmitter: SoapySDRDevice_activateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

void soapy_protocol_stop_transmitter(TRANSMITTER *tx) {
  int rc;

log_info("soapy_protocol_stop_transmitter: deactivateStream\n");
  rc=SoapySDRDevice_deactivateStream(soapy_device, tx_stream, 0, 0LL);
  if(rc!=0) {
    log_info("soapy_protocol_stop_transmitter: SoapySDRDevice_deactivateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

// Rebuild the device arguments discovery opened this device with.  They carry
// the uri/hostname/serial that says WHICH device and WHERE it is; a networked
// device cannot be re-opened from the driver name alone.  Falls back to the old
// name-plus-index args for a config discovered before those were recorded.
static void soapy_device_args(SoapySDRKwargs *args) {
  char temp[32];

  if(radio->discovered->info.soapy.make_args[0]!='\0') {
    *args=SoapySDRKwargs_fromString(radio->discovered->info.soapy.make_args);
  }
  if(SoapySDRKwargs_get(args,"driver")==NULL) {
    SoapySDRKwargs_set(args, "driver", radio->discovered->name);
  }
  if(strcmp(radio->discovered->name,"rtlsdr")==0) {
    if(SoapySDRKwargs_get(args,"serial")==NULL && SoapySDRKwargs_get(args,"rtl")==NULL) {
      sprintf(temp,"%d",radio->discovered->info.soapy.rtlsdr_count);
      SoapySDRKwargs_set(args, "rtl", temp);
    }
  } else if(strcmp(radio->discovered->name,"sdrplay")==0) {
    if(SoapySDRKwargs_get(args,"label")==NULL) {
      sprintf(temp,"SDRplay Dev%d",radio->discovered->info.soapy.sdrplay_count);
      SoapySDRKwargs_set(args, "label", temp);
    }
  }
}

void soapy_protocol_init(RADIO *r,int rx) {
  SoapySDRKwargs args={};
  int rc;
  int i;

log_info("soapy_protocol_init\n");

  // initialize the radio
log_info("soapy_protocol_init: SoapySDRDevice_make\n");
  soapy_device_args(&args);
  soapy_device=SoapySDRDevice_make(&args);
  if(soapy_device==NULL) {
    log_info("%s: SoapySDRDevice_make failed: %s\n",__FUNCTION__,SoapySDRDevice_lastError());
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
  // Capture max_samples once: it is a global that the NEXT receiver's
  // create_receiver rewrites, and readStream() must never be asked for more
  // than this thread's own buffer holds.
  const int block=max_samples;
  float *buffer=g_new(float,block*2);
  void *buffs[]={buffer};
  int overruns=0;
  gint64 overrun_reported=g_get_monotonic_time();

log_info("%s: running\n",__FUNCTION__);
  size_t channel=rx->adc;
  while(rx_thread_running[channel]) {
    // Paused while transmitting (half-duplex): the RX stream is deactivated,
    // so don't read from it - just idle until it is resumed.  The stream is
    // torn down and rebuilt fresh in soapy_protocol_rx_resume() (see there),
    // so no backlog reaches us here.
    if(!rx_stream_active) {
      g_atomic_int_set(&rx_parked[channel],1);
      g_usleep(1000);
      continue;
    }
    g_atomic_int_set(&rx_parked[channel],0);
    elements=SoapySDRDevice_readStream(soapy_device,rx_stream[channel],buffs,block,&flags,&timeNs,timeoutUs);
    // A dropped block is not a silence the DSP can see: the samples either side
    // of it are spliced, so every signal on the band has its phase randomised at
    // the seam and the receiver's clock quietly runs fast.  Nothing looked at
    // this before, which is why a stream arriving faster than the app consumes
    // it presented as smearing rather than as loss.
    if(elements==SOAPY_SDR_OVERFLOW || (flags&SOAPY_SDR_END_ABRUPT)) {
      overruns++;
      const gint64 now=g_get_monotonic_time();
      if(now-overrun_reported>=5000000) {
        log_error("%s: %d overrun(s) in %.0f s on adc %ld -- the stream is arriving faster than it is consumed; signals will be smeared\n",
                  __FUNCTION__,overruns,(double)(now-overrun_reported)/1.0e6,(long)channel);
        overrun_reported=now;
        overruns=0;
      }
    }
    if(elements<0) continue;
    if(elements>0) reconnect_note_data();   // fed the disconnect watchdog
    // This thread was handed its RECEIVER at start-up and holds it for its whole
    // life, so unlike the protocol1/2 paths there is no slot to re-read -- the
    // check is whether that receiver is still one of the radio's.  Everything
    // that touches it (rx->buffer, the resampler, add_iq_samples) is inside the
    // lock, because delete_receiver frees all three.  The lock is held for the
    // whole block rather than per sample; protocol1 takes and drops it once per
    // sample, so this is the cheaper end of what the codebase already does.
    g_mutex_lock(&radio->delete_rx_mutex);
    if(!receiver_is_live(rx)) {
      g_mutex_unlock(&radio->delete_rx_mutex);
      continue;
    }
    for(i=0;i<elements;i++) {
      rx->buffer[i*2]=(double)buffer[i*2];
      rx->buffer[(i*2)+1]=(double)buffer[(i*2)+1];
    }
    if(rx->resampler!=NULL) {
      // xresampleV, not xresample: the latter always consumes the count the
      // resampler was CREATED with, while readStream returns "up to" block --
      // so a short read re-resampled the tail of the previous one, and the
      // very first one resampled uninitialised heap (rx->buffer is g_new).
      int out_elements=0;
      xresampleV(rx->buffer,rx->resampled_buffer,elements,&out_elements,rx->resampler);
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
    g_mutex_unlock(&radio->delete_rx_mutex);
  }
  // The stream is NOT touched here.  Whoever stopped this thread joins it and
  // then deactivates/closes the stream; doing it from both ends would race a
  // close against a readStream that has not returned yet.
log_info("%s: exit (channel=%ld)\n",__FUNCTION__,(long)channel);
  g_free(buffer);
  return NULL;
}

// Stop one receiver's thread and let go of its stream.
//
// MUST be called with radio->delete_rx_mutex NOT held: this thread takes that
// same mutex around every block it delivers, so joining it while holding the
// lock would block for ever on ourselves -- the shape that wedged protocol 1's
// output path.  delete_receiver() therefore calls this before it locks.
void soapy_protocol_stop_receiver(RECEIVER *rx) {
  if(rx==NULL) return;
  // Only a receiver that is one of the radio's owns a stream slot.  A hidden
  // PureSignal/diversity receiver shares its adc number with a real one, so
  // without this it would stop that receiver's thread on its way out.
  if(!receiver_is_live(rx)) return;
  size_t channel=rx->adc;
  if(channel>=MAX_CHANNELS) return;
  if(receive_thread_id[channel]==NULL) return;

log_info("%s: stopping receive thread for channel %ld\n",__FUNCTION__,(long)channel);
  rx_thread_running[channel]=FALSE;
  g_thread_join(receive_thread_id[channel]);
  receive_thread_id[channel]=NULL;

  if(soapy_device!=NULL && rx_stream[channel]!=NULL) {
    SoapySDRDevice_deactivateStream(soapy_device,rx_stream[channel],0,0LL);
    SoapySDRDevice_closeStream(soapy_device,rx_stream[channel]);
    rx_stream[channel]=NULL;
  }

  running=FALSE;
  for(int i=0;i<MAX_CHANNELS;i++) {
    if(receive_thread_id[i]!=NULL) running=TRUE;
  }
}

void soapy_protocol_process_local_mic(RADIO *r) {
  int i;
  short sample;

// always 48000 samples per second
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
          //
          // Dropping is a hole in the transmitted waveform, not a late block, so
          // it must never be silent: what comes out of the PA is a carrier
          // chopped at the block rate, which on the air is not a weak signal but
          // a wrong one -- hash where a tone should be.  Counted and reported so
          // "Tune sounds like noise" can be answered with a number instead of a
          // guess.
          tx_dropped+=(max_tx_samples-written);
          break;
        }
      }
      output_buffer_index=0;
    }
  }
}

// ---- Half-duplex RX pause/resume ------------------------------------------

// Wait (bounded) for a receive thread to acknowledge the pause.  Typically
// returns at once -- readStream comes back as soon as a block is ready, which
// at any usable sample rate is well under a millisecond -- and the ceiling is
// just over readStream's own 100 ms timeout for a stream that has gone quiet.
static void wait_rx_parked(size_t channel) {
  if(receive_thread_id[channel]==NULL) return;
  for(int i=0;i<150 && !g_atomic_int_get(&rx_parked[channel]);i++) {
    g_usleep(1000);
  }
}

void soapy_protocol_rx_pause(void) {
  if(soapy_device==NULL) return;
  if(!rx_stream_active) return;
  rx_stream_active=FALSE;
  // EVERY stream, not rx_channel's: that global holds whichever receiver was
  // started last, so with two receivers this deactivated one and left the other
  // streaming straight through a half-duplex transmit.
  for(size_t ch=0;ch<MAX_CHANNELS;ch++) {
    if(rx_stream[ch]==NULL) continue;
    wait_rx_parked(ch);
    SoapySDRDevice_deactivateStream(soapy_device,rx_stream[ch],0,0LL);
  }
}

static void rx_resume_channel(size_t channel) {
  // HackRF leaves the RX stream in a runaway overflow after a
  // deactivate/reactivate across a TX over: readStream then keeps returning
  // full buffers forever (observed 200M+ samples, far faster than real time),
  // which floods WDSP into an endless "fexchange0: error=-2" and dead RX.  A
  // plain activateStream does NOT clear it, and the backlog cannot be drained
  // (it arrives as fast as we read).  So tear the stream all the way down and
  // set it up fresh, which resumes RX at real time with an empty buffer.
  // Safe to rebuild the stream here: the receive thread only touches it while
  // rx_stream_active is TRUE, which we set last.
  SoapySDRDevice_deactivateStream(soapy_device,rx_stream[channel],0,0LL);
  SoapySDRDevice_closeStream(soapy_device,rx_stream[channel]);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  SoapySDRDevice_setupStream(soapy_device,&rx_stream[channel],SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
#else
  rx_stream[channel]=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
#endif
  SoapySDRDevice_activateStream(soapy_device,rx_stream[channel],0,0LL,0);
}

void soapy_protocol_rx_resume(void) {
  if(soapy_device==NULL) return;
  if(rx_stream_active) return;
  for(size_t ch=0;ch<MAX_CHANNELS;ch++) {
    if(rx_stream[ch]!=NULL) rx_resume_channel(ch);
  }
  rx_stream_active=TRUE;   // last, so no thread reads a stream mid-rebuild
}

// ---- TX pump + stream activation ------------------------------------------

static gpointer tx_thread(gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  gboolean status_supported=TRUE;
  int status_tick=0;
  gint64 reported=g_get_monotonic_time();
log_info("soapy tx_thread: start\n");
  // Feed silence into the TX exchange; writeStream() back-pressure paces us.
  // For tune, WDSP's tone generator fills the output regardless of this input.
  while(tx_pump_running) {
    // ...but that back-pressure is the ONLY thing pacing this loop, and it only
    // exists while soapy_protocol_iq_samples() is actually writing -- which it
    // refuses to do unless we are keyed with a live TX stream.  Unkeying
    // therefore leaves this spinning at whatever the CPU will give until
    // deactivate_tx joins us, feeding WDSP far faster than real time; WDSP
    // answers with "fexchange0: error=-2" (its output ring is empty, so it
    // zeroes the block) once per iteration.  A user's PlutoSDR log is a wall of
    // exactly that, every time between set_mox 0 and tx_thread exit.
    if(!isTransmitting(radio) || !tx_stream_active) {
      g_usleep(1000);
      continue;
    }
    add_mic_sample(tx,0.0f);

    // Ask the driver whether the DAC ran dry.  This is the only way to tell a
    // transmitter that is merely quiet from one whose waveform has holes in it,
    // and holes are what turn a Tune carrier into hash.  Polled every 256 mic
    // samples (~190/s) with a zero timeout so it cannot pace the loop, and
    // switched off for good if the driver does not implement it.
    if(status_supported && ++status_tick>=256) {
      status_tick=0;
      size_t chan_mask=0;
      int sflags=0;
      long long stime=0;
      int rc=SoapySDRDevice_readStreamStatus(soapy_device,tx_stream,&chan_mask,&sflags,&stime,0);
      if(rc==SOAPY_SDR_NOT_SUPPORTED) {
        status_supported=FALSE;
      } else if(rc==SOAPY_SDR_UNDERFLOW) {
        tx_underflows++;
      }
    }
    const gint64 now=g_get_monotonic_time();
    if(now-reported>=5000000) {
      if(tx_dropped>0 || tx_underflows>0) {
        log_error("%s: TX starved in the last %.0f s -- %lld sample(s) the device would not take, "
                  "%lld underflow(s).  The transmitted waveform has holes in it.\n",
                  __FUNCTION__,(double)(now-reported)/1.0e6,tx_dropped,tx_underflows);
      }
      tx_dropped=0;
      tx_underflows=0;
      reported=now;
    }
  }
log_info("soapy tx_thread: exit\n");
  return NULL;
}

void soapy_protocol_activate_tx(TRANSMITTER *tx) {
  if(soapy_device==NULL) return;
  output_buffer_index=0;
  if(!tx_stream_active) {
    int rc=SoapySDRDevice_activateStream(soapy_device,tx_stream,0,0LL,0);
    if(rc!=0) {
      log_info("soapy_protocol_activate_tx: activateStream failed: %s\n",SoapySDR_errToStr(rc));
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
    log_info("%s: SoapySDRDevice_setGain failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
}



void soapy_protocol_stop(void) {
log_info("%s\n",__FUNCTION__);
  // Every receiver's thread, not just the last one started.
  for(int i=0;i<MAX_CHANNELS;i++) {
    if(receive_thread_id[i]==NULL) continue;
    rx_thread_running[i]=FALSE;
    g_thread_join(receive_thread_id[i]);
    receive_thread_id[i]=NULL;
    if(soapy_device!=NULL && rx_stream[i]!=NULL) {
      SoapySDRDevice_deactivateStream(soapy_device,rx_stream[i],0,0LL);
    }
  }
  running=FALSE;
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
  size_t channel=rx->adc;

log_info("%s: tearing down old device/streams\n",__FUNCTION__);

  // Stop this receiver's thread (it may be spinning on read errors from the
  // dead device).  The slot can be NULL if a previous reconnect failed.
  if(channel<MAX_CHANNELS && receive_thread_id[channel]!=NULL) {
    rx_thread_running[channel]=FALSE;
    g_thread_join(receive_thread_id[channel]);
    receive_thread_id[channel]=NULL;
  }
  running=FALSE;
  for(int i=0;i<MAX_CHANNELS;i++) {
    if(receive_thread_id[i]!=NULL) running=TRUE;
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
  soapy_device_args(&args);
  soapy_device=SoapySDRDevice_make(&args);
  SoapySDRKwargs_clear(&args);
  if(soapy_device==NULL) {
    log_info("%s: SoapySDRDevice_make failed: %s\n",__FUNCTION__,SoapySDRDevice_lastError());
    return FALSE;
  }

log_info("%s: re-making streams and re-applying settings\n",__FUNCTION__);

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

log_info("%s: done\n",__FUNCTION__);
  return TRUE;
}

void soapy_protocol_set_rx_frequency(RECEIVER *rx) {
  int rc;

  if(soapy_device!=NULL) {
    double f=(double)(rx->frequency_a-rx->lo_a+rx->error_a);
    f+=(double)radio_ppm_correction(rx->frequency_a-rx->lo_a);
    if(!rx->ctun) {
      if(rx->rit_enabled) {
        f+=(double)rx->rit;
      }
    }
    //g_print("%s: %f\n",__FUNCTION__,f);
    rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_RX,rx->adc,f,NULL);
    if(rc!=0) {
      log_info("%s: SoapySDRDevice_setFrequency(RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
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
        f+=(double)radio_ppm_correction(rx->frequency_b-rx->lo_b);
      } else {
        if(rx->ctun) {
          f=rx->ctun_frequency-rx->lo_a+rx->error_a;
          f+=(double)radio_ppm_correction(rx->ctun_frequency-rx->lo_a);
        } else {
          f=rx->frequency_a-rx->lo_a+rx->error_a;
          f+=(double)radio_ppm_correction(rx->frequency_a-rx->lo_a);
        }
      }
      if(tx->xit_enabled) {
        f+=(double)tx->xit;
      }
//g_print("%s: %f\n",__FUNCTION__,f);
      rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_TX,tx->dac,f,NULL);
      if(rc!=0) {
        log_info("%s: SoapySDRDevice_setFrequency(TX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
      }
    }
  }
}

void soapy_protocol_set_rx_antenna(RECEIVER *rx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    log_info("%s: set_rx_antenna: %s\n",__FUNCTION__,radio->discovered->info.soapy.rx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_RX,rx->adc,radio->discovered->info.soapy.rx_antenna[ant]);
    if(rc!=0) {
      log_info("%s: SoapySDRDevice_setAntenna RX failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_antenna(TRANSMITTER *tx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    log_info("%s: %s\n",__FUNCTION__,radio->discovered->info.soapy.tx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_TX,tx->dac,radio->discovered->info.soapy.tx_antenna[ant]);
    if(rc!=0) {
      log_info("%s: SoapySDRDevice_setAntenna TX failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_gain(ADC *adc) {
  int rc;
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_RX,adc->id,adc->gain);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setGain failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_tx_gain(DAC *dac) {
  int rc;
log_info("%s: dac=%d gain=%f\n",__FUNCTION__,dac->id,dac->gain);
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,dac->id,dac->gain);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setGain failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
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

    log_info("%s: SoapySDRDevice_getGainMode failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
}

char *soapy_protocol_read_sensor(char *name) {
  return SoapySDRDevice_readSensor(soapy_device, name);
}

gboolean soapy_protocol_is_running(void) {
  return running;
}
