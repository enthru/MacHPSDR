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
#ifdef LIQUID
// The SoapySDR front-end mixer and decimator (see the slot table below).
// Defined by the same
// Makefile switch that links liquid-dsp; without it the WDSP resampler is used.
#include <complex.h>
#include <liquid/liquid.h>
#endif

#include "SoapySDR/Constants.h"
#include "SoapySDR/Device.h"
#include "SoapySDR/Errors.h"
#include "SoapySDR/Formats.h"
#include "SoapySDR/Version.h"

#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "bpsk.h"
#include "dc_block.h"
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
static gboolean changing_device_rate=FALSE;
// Keep device reads and DSP work as separate geometries.  Some drivers need a
// whole hardware transfer read at once, while handing that entire transfer to
// DSP would make its output arrive in visible/audible bursts.
static int rx_read_block[MAX_CHANNELS];
// The smaller block each receiver's buffers and resampler were built for.
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

/* ---- the raw-stream FIFO -------------------------------------------------
   The receive thread used to do everything: read the device, decimate, run the
   whole DSP chain, feed the panadapter and the decoders, and hand the audio on.
   So any hesitation anywhere in that chain -- a span change rebuilding the
   channel, a decoder, the GUI -- was time not spent draining the device, and
   the driver answered with an overrun, which is not late data but SPLICED data:
   the samples either side of the gap are joined and every signal on the band has
   its phase stepped.  Measured on a HackRF: one or two per span change.

   Now the reader does nothing but read and push, and a per-ADC DSP thread pops
   and does the rest.  The FIFO absorbs a whole delivery burst (the device's real
   MTU, before it is clamped for the read size) several times over, so a stall in
   the DSP costs latency instead of samples.  Keyed per ADC, like the stream and
   the receive thread. */
static float *fifo_buf[MAX_CHANNELS];
static int fifo_cap[MAX_CHANNELS];      /* in SAMPLES */
static int fifo_rd[MAX_CHANNELS];
static int fifo_n[MAX_CHANNELS];
static long long fifo_dropped[MAX_CHANNELS];
static GMutex fifo_mutex[MAX_CHANNELS];
static GCond fifo_cond[MAX_CHANNELS];
static gboolean dsp_thread_running[MAX_CHANNELS];
/* "The stream underneath you is a different one now": raised by fifo_clear,
   consumed by the DSP thread, which is the only owner of the DC estimate --
   that state lives on its stack for exactly this reason, so that a restart on
   the GTK thread can ask for a reset without writing the filter's memory while
   it is being run. */
static volatile gint dc_block_reset_req[MAX_CHANNELS];
static GThread *dsp_thread_id[MAX_CHANNELS];
static gpointer dsp_thread(gpointer data);

/* Defined further down with the half-duplex helpers: waits (bounded) until the
   receive thread is out of readStream, so the stream it is reading can be torn
   down without racing it. */
static void wait_rx_parked(size_t channel);

static void fifo_alloc(size_t channel,int samples) {
  g_mutex_lock(&fifo_mutex[channel]);
  g_free(fifo_buf[channel]);
  fifo_buf[channel]=g_new(float,2*samples);
  fifo_cap[channel]=samples;
  fifo_rd[channel]=0;
  fifo_n[channel]=0;
  fifo_dropped[channel]=0;
  g_mutex_unlock(&fifo_mutex[channel]);
  log_info("fifo_alloc: adc %ld: %d samples of raw stream\n",(long)channel,samples);
}

/* Drop whatever is queued.  Used when the stream underneath is rebuilt: after a
   TX over the samples in here are from before the transmission and belong to a
   stream that no longer exists. */
static void fifo_clear(size_t channel) {
  g_mutex_lock(&fifo_mutex[channel]);
  fifo_rd[channel]=0;
  fifo_n[channel]=0;
  g_mutex_unlock(&fifo_mutex[channel]);
  /* The next block comes off a stream that was set up again, at a rate or a
     gain or a centre that may have moved, so the offset measured off the old
     one is not this one's.  Re-converging from zero takes a few milliseconds;
     walking there from a stale estimate takes the same few milliseconds and
     puts a step into the audio on the way. */
  if(channel<MAX_CHANNELS) g_atomic_int_set(&dc_block_reset_req[channel],1);
}

/* Both teardown paths (a receiver going away, and a reconnect) must stop this
   thread: it holds the RECEIVER, its buffers and its WDSP channel.  Call it
   AFTER the reader is joined, so nothing is still pushing. */
static void stop_dsp_thread(size_t channel) {
  if(channel>=MAX_CHANNELS || dsp_thread_id[channel]==NULL) return;
  g_mutex_lock(&fifo_mutex[channel]);
  dsp_thread_running[channel]=FALSE;
  g_cond_broadcast(&fifo_cond[channel]);
  g_mutex_unlock(&fifo_mutex[channel]);
  g_thread_join(dsp_thread_id[channel]);
  dsp_thread_id[channel]=NULL;
}

static void fifo_free(size_t channel) {
  g_mutex_lock(&fifo_mutex[channel]);
  g_free(fifo_buf[channel]);
  fifo_buf[channel]=NULL;
  fifo_cap[channel]=0;
  fifo_rd[channel]=0;
  fifo_n[channel]=0;
  g_mutex_unlock(&fifo_mutex[channel]);
}

/* Reader side.  A full FIFO means the DSP has been behind for a whole burst, so
   the block is dropped and counted -- the alternative, blocking here, is exactly
   the coupling this FIFO exists to remove. */
static void fifo_push(size_t channel,const float *iq,int n) {
  g_mutex_lock(&fifo_mutex[channel]);
  if(fifo_buf[channel]==NULL || n<=0) { g_mutex_unlock(&fifo_mutex[channel]); return; }
  if(n>fifo_cap[channel]-fifo_n[channel]) {
    fifo_dropped[channel]+=n;
    g_mutex_unlock(&fifo_mutex[channel]);
    return;
  }
  int wr=(fifo_rd[channel]+fifo_n[channel])%fifo_cap[channel];
  int first=fifo_cap[channel]-wr; if(first>n) first=n;
  memcpy(fifo_buf[channel]+2*wr,iq,(size_t)(2*first)*sizeof(float));
  if(n>first) memcpy(fifo_buf[channel],iq+2*first,(size_t)(2*(n-first))*sizeof(float));
  fifo_n[channel]+=n;
  g_cond_signal(&fifo_cond[channel]);
  g_mutex_unlock(&fifo_mutex[channel]);
}

/* DSP side.  Blocks until there is something or the thread is being stopped;
   returns a CONTIGUOUS run, so the caller sees the stream in order without the
   ring's wrap ever showing. */
static int fifo_pop(size_t channel,float *out,int want) {
  g_mutex_lock(&fifo_mutex[channel]);
  while(dsp_thread_running[channel] && fifo_n[channel]==0) {
    gint64 until=g_get_monotonic_time()+100000;   /* 100 ms, so a stop is prompt */
    g_cond_wait_until(&fifo_cond[channel],&fifo_mutex[channel],until);
  }
  int n=fifo_n[channel]; if(n>want) n=want;
  if(n>0) {
    int first=fifo_cap[channel]-fifo_rd[channel]; if(first>n) first=n;
    memcpy(out,fifo_buf[channel]+2*fifo_rd[channel],(size_t)(2*first)*sizeof(float));
    if(n>first) memcpy(out+2*first,fifo_buf[channel],(size_t)(2*(n-first))*sizeof(float));
    fifo_rd[channel]=(fifo_rd[channel]+n)%fifo_cap[channel];
    fifo_n[channel]-=n;
  }
  g_mutex_unlock(&fifo_mutex[channel]);
  return n;
}

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
// Per-transmission accounting, reported once at unkey (see deactivate_tx).
// "Nothing comes out" has four quite different causes that look identical from
// the operator's chair -- WDSP producing nothing (a dead mic/tune path), the
// device refusing the samples, the samples arriving at the wrong clock, or a
// full-scale waveform going into an attenuator turned all the way down -- and
// one line naming samples/s, peak level and the hardware gain separates them.
// One transmission, one log line: it cannot pace the TX loop.
static long long tx_written;
static float tx_peak;
// SoapyPlutoSDR converts CF32 to its native signed 16-bit stream with a plain
// cast after multiplying by 32767.999.  It does not saturate first, so any WDSP
// overshoot outside +/-1 can wrap across the signed range and turn into a
// discontinuity rather than a clipped peak.  Count and clamp those samples
// before they reach the driver.
static long long tx_clipped;
static gint64 tx_key_us;
// Linear form of tx->dac_backoff_db, applied to every sample on its way to the
// DAC.  Kept as a scalar the sample loop can multiply by: recomputing pow()
// per sample at 2.3 MS/s is not a thing to do on the TX path.  Written on the
// GTK thread (the control, and activate_tx), read by whichever thread clocks
// the TX exchange -- a benign single-word race in the same class as the level
// meters, since a torn read cannot happen for a float on any target here and
// the worst case is one block at the previous level.
static float tx_backoff_scale=1.0f;
static double tx_backoff_db=0.0;
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

/* Put the analogue TX chain at its quietest setting whenever it is not keyed.
   This is a second safety barrier behind stream/LO deactivation.  In
   particular, unpatched/system SoapyPlutoSDR releases turn the TX LO on from
   setupStream(), and a Pluto remembers its previous attenuation while a new
   network context is being opened.  Never leave that LO sitting at the
   driver's default (0 dB, i.e. full output) while the rest of the radio is
   still being constructed. */
static void soapy_tx_hold_safe(void) {
  if(soapy_device==NULL || radio==NULL || !radio->can_transmit) return;
  SoapySDRRange grange=SoapySDRDevice_getGainRange(soapy_device,SOAPY_SDR_TX,0);
  tx_gain_min=grange.minimum;
  tx_gain_max=grange.maximum;
  int rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,0,tx_gain_min);
  if(rc!=0) {
    log_error("%s: could not set the idle TX gain to %.1f dB: %s\n",
              __FUNCTION__,tx_gain_min,SoapySDR_errToStr(rc));
  } else {
    log_info("%s: TX held at minimum gain %.1f dB until key-up (range %.1f..%.1f)\n",
             __FUNCTION__,tx_gain_min,tx_gain_min,tx_gain_max);
  }
}

SoapySDRDevice *get_soapy_device(void) {
  return soapy_device;
}

void soapy_protocol_set_mic_sample_rate(int rate) {
  mic_sample_divisor=rate/48000;
}

/* ---- more than one receiver on one hardware RX channel --------------------
   A Pluto has a single RX channel, so "one receiver per ADC" -- which is what
   the stream, its block size and its receive thread are keyed by -- meant one
   receiver, full stop, and the Add Receiver button was greyed on the device
   this fork is most used with.  It cannot be cured by opening a second stream:
   an AD9361 has ONE RX synthesiser feeding both halves, so even a 2R2T Pluto
   gives two receivers that share a local oscillator.  The shared part is
   therefore the honest model, and it is what this table implements.

   Slot 0 of an ADC OWNS the hardware: the device is tuned to its centre, and it
   alone sets the sample rate, the analogue bandwidth and the stream up.  Every
   other slot is a FOLLOWER: it is handed the same raw block and brings its own
   centre to DC with an NCO before decimating to its own span, so it has a
   frequency, a span, a mode, filters, AGC, an audio device and a decoder panel
   of its own -- but its centre must stay inside the window the owner's LO and
   the hardware rate leave it (soapy_protocol_rx_window_error).

   The table is mutated only under radio->delete_rx_mutex, which is the lock the
   DSP thread already holds around the whole block it is distributing. */
#define MAX_ADC_RECEIVERS 4

typedef struct {
  RECEIVER *rx;                 /* NULL: free slot */
  gboolean ready;               /* its mixer/decimator are built for the live stream */
#ifdef LIQUID
  nco_crcf mixer;               /* this receiver's centre -> DC, before decimating */
  msresamp_crcf resamp;         /* ...then the stream's rate down to its span */
  liquid_float_complex *out;
  int out_cap;
  float ratio;                  /* span / stream rate: what out_cap is sized by */
  liquid_float_complex *mix;    /* the mixed block; NULL while offset==0 */
  int mix_cap;
#endif
  long long offset;             /* Hz between this receiver's centre and the device LO */
} RXSLOT;

static RXSLOT adc_slot[MAX_CHANNELS][MAX_ADC_RECEIVERS];
/* What the device is really tuned to, per ADC -- the READBACK, not the request,
   because every follower's offset is measured from it. */
static double adc_lo[MAX_CHANNELS];

#ifdef LIQUID
/* ---- front-end decimator (liquid-dsp) -------------------------------------
   WDSP's create_resample() does the whole ratio in ONE FIR stage, sized at
   140 taps per unit of ratio (wdsp/resample.c, calc_resample): taking 9.6 MS/s
   down to a 1 920 000 span is 700 taps per OUTPUT sample -- 1.34 G complex
   multiply-accumulates per second, in scalar double, on the receive thread,
   which also has to drain the device.  Measured against tools/soapy_null.cpp
   that thread could not keep up: the driver discarded its backlog and a quarter
   of the audio came out as zero-fill, while the same span taken directly (no
   resampler at all) ran clean.  liquid's msresamp does the same job as a
   CASCADE -- halfband stages first, an arbitrary resampler last -- in float,
   which is the shape every SDR of this class uses.

   One per RECEIVER, not per ADC: receivers sharing an ADC have their own spans
   and their own centres, so they cannot share a decimator.  Freed with the slot
   -- by soapy_protocol_stop_receiver() and by a re-entry into
   soapy_protocol_create_receiver(). */

/* Stop-band suppression of the cascade.  The panadapter draws about 100 dB of
   range, so an alias folded in at -60 dB would be a visible carrier that is not
   on the band; 100 dB puts it under the noise floor and still costs an order of
   magnitude less than the single-stage filter it replaces. */
#define MS_RESAMP_STOPBAND_DB 100.0f

static void slot_dsp_free(RXSLOT *sl) {
  sl->ready=FALSE;
  if(sl->resamp!=NULL) { msresamp_crcf_destroy(sl->resamp); sl->resamp=NULL; }
  if(sl->out!=NULL)    { g_free(sl->out); sl->out=NULL; }
  sl->out_cap=0;
  if(sl->mixer!=NULL)  { nco_crcf_destroy(sl->mixer); sl->mixer=NULL; }
  if(sl->mix!=NULL)    { g_free(sl->mix); sl->mix=NULL; }
  sl->mix_cap=0;
}
#else
static void slot_dsp_free(RXSLOT *sl) { sl->ready=FALSE; }
#endif

/* The slot this receiver already has, or NULL. */
static RXSLOT *slot_find(RECEIVER *rx) {
  if(rx==NULL) return NULL;
  size_t ch=(size_t)rx->adc;
  if(ch>=MAX_CHANNELS) return NULL;
  for(int i=0;i<MAX_ADC_RECEIVERS;i++) {
    if(adc_slot[ch][i].rx==rx) return &adc_slot[ch][i];
  }
  return NULL;
}

/* Its slot, taking the first free one if it has none.  Idempotent, because it
   is reached both from the create path and from the first frequency push --
   which, for a receiver added while the radio runs, happens FIRST (frequency_
   changed() is called at the end of create_receiver, before radio.c gets to
   soapy_protocol_create_receiver).  Registering there rather than only in the
   create path is what stops a not-yet-known second receiver being taken for the
   owner and retuning the LO out from under the first one. */
static RXSLOT *slot_add(RECEIVER *rx) {
  RXSLOT *sl=slot_find(rx);
  if(sl!=NULL) return sl;
  size_t ch=(size_t)rx->adc;
  if(ch>=MAX_CHANNELS) return NULL;
  for(int i=0;i<MAX_ADC_RECEIVERS;i++) {
    if(adc_slot[ch][i].rx==NULL) {
      // rx is written LAST.  This runs on the GTK thread while the DSP thread
      // may be walking the row, and rx!=NULL is what makes a slot visible to
      // it: everything it would then read has to say "not ready" already.
      adc_slot[ch][i].offset=0;
      adc_slot[ch][i].ready=FALSE;
      adc_slot[ch][i].rx=rx;
      if(i>0) log_info("%s: receiver %d shares adc %ld with receiver %d\n",
                       __FUNCTION__,rx->channel,(long)ch,adc_slot[ch][0].rx->channel);
      return &adc_slot[ch][i];
    }
  }
  log_error("%s: adc %ld already carries %d receivers\n",__FUNCTION__,(long)ch,MAX_ADC_RECEIVERS);
  return NULL;
}

/* Is this receiver the one the hardware belongs to?  A receiver with no slot
   yet is the owner only when nothing else holds the ADC -- see slot_add. */
static gboolean slot_is_owner(RECEIVER *rx) {
  if(rx==NULL) return FALSE;
  size_t ch=(size_t)rx->adc;
  if(ch>=MAX_CHANNELS) return FALSE;
  return (adc_slot[ch][0].rx==rx || adc_slot[ch][0].rx==NULL);
}

gboolean soapy_protocol_rx_owns_hardware(RECEIVER *rx) {
  return slot_is_owner(rx);
}

/* Drop a receiver from its ADC and close the gap, so slot 0 is always the
   owner.  Returns how many receivers are left on that ADC.  Called with
   delete_rx_mutex held: the DSP thread walks this row inside it. */
static int slot_remove(RECEIVER *rx) {
  size_t ch=(size_t)rx->adc;
  int n=0;
  if(ch>=MAX_CHANNELS) return 0;
  for(int i=0;i<MAX_ADC_RECEIVERS;i++) {
    if(adc_slot[ch][i].rx==rx) {
      slot_dsp_free(&adc_slot[ch][i]);
      adc_slot[ch][i].rx=NULL;
      adc_slot[ch][i].offset=0;
    }
  }
  /* Compact: a follower promoted to slot 0 becomes the owner, and the caller
     retunes the hardware onto it. */
  for(int i=0,j=0;i<MAX_ADC_RECEIVERS;i++) {
    if(adc_slot[ch][i].rx!=NULL) {
      if(i!=j) { adc_slot[ch][j]=adc_slot[ch][i]; memset(&adc_slot[ch][i],0,sizeof(RXSLOT)); }
      j++; n=j;
    }
  }
  return n;
}

/* HackRF's practical floor: below ~2 MHz its 1.75 MHz baseband filter is the
   limit, so a narrower span is taken at 2 MHz and resampled, exactly as it was
   before the wide spans existed. */
#define HACKRF_MIN_HW_RATE 2000000

/* The rate to program the HARDWARE with for this receiver.
   The default is radio->sample_rate: a fixed ADC rate the device is known to
   support, with the per-receiver resampler bridging it down to the span (asking
   the hardware straight for the span is what once handed HackRF a rate it does
   not have, which it clamped internally while the app went on reading the
   stream as the span -> garbage waterfall).  Two devices are driven at the
   receiver's own span instead:
     - sdrplay, as it always has been;
     - hackrf, but only while the span is above its 2 MHz floor.  Its widest
       spans (4800000, 9600000) are rates the hardware really has, and taking
       them directly means no resampler at all -- where the alternative, a fixed
       9.6 MHz ADC rate, would run the resampler at 9.6 MS/s for someone
       listening to SSB in a 192 kHz window.  The readback in soapy_set_rx_rate
       still has the last word if the device substitutes. */
gboolean soapy_span_driven(void) {
  if(radio==NULL || radio->discovered==NULL) return FALSE;
  return strcmp(radio->discovered->name,"sdrplay")==0 ||
         strcmp(radio->discovered->name,"hackrf")==0;
}

static int soapy_hw_rate_for(RECEIVER *rx) {
  if(strcmp(radio->discovered->name,"sdrplay")==0) return rx->sample_rate;
  if(strcmp(radio->discovered->name,"hackrf")==0)
    return (rx->sample_rate>HACKRF_MIN_HW_RATE)?rx->sample_rate:HACKRF_MIN_HW_RATE;
  return radio->sample_rate;
}

/* The rate to run the TX DAC at, which is NOT simply radio->sample_rate any
   more.  For a device with a real fixed ADC rate that number still is the rate
   the hardware runs at, and the DAC must use it too -- on an AD9361 in
   particular RX and TX are one clock (ad9361_set_bb_rate programmes both), so
   giving the transmitter a different number silently retunes the receiver.
   For a span-driven device it is the widest span OFFERED, which has nothing to
   do with transmitting: a HackRF would open the TX chain at 9 600 000 and
   interpolate a 48 kHz microphone by 200.  Those get the ceiling instead. */
/* ---- the rate the DEVICE is clocked at -----------------------------------
   On a device whose radio->sample_rate is a real ADC rate (everything except
   the span-driven ones -- see soapy_span_driven), that number is the width of
   the window every receiver on the channel shares: a second receiver can sit
   (rate - its own span)/2 either side of the first one's centre and no further.
   2 304 000 was chosen for a Pluto when a receiver was alone in that window and
   nothing wider than 1 920 000 was ever asked for, and it is the floor of what
   an AD9361 will do, not the ceiling -- the part runs to 20 MHz.

   Why it is a SETTING and not a bigger constant: what carries the stream is not
   the radio.  Measured on the development Pluto, reached over a LAN
   (tools/soapy_bench.c, 8 s per row, no application in the way): 99.9 % of
   2 304 000, 99.8 % of 9 216 000, 98.4 % of 11 520 000 -- and then 67.5 % of
   15 360 000 and 56.8 % of 20 000 000, with the DELIVERED rate flat at about
   11.3 MS/s, which is that link's ceiling and nothing to do with the AD9361.
   Another operator's Pluto, on USB or on a worse wireless link, has a different
   ceiling; a number picked here to suit one of them silently splices the
   stream for the others (a shortfall is not a silence -- the samples either
   side of it are joined). So the default stays where it was and the operator
   raises it against their own link, with receive_thread's own delivery count
   as the check.

   Every entry is a multiple of 192 kHz, so the ratio to every span the app
   offers stays exact, and of 96 kHz, which is what the TX chain's geometry is
   rounded to (create_transmitter).  4 800 000 and 9 600 000 are in it because
   they are also SPANS: the offered spans are filtered by this rate, so without
   them a device clocked at 9 216 000 stopped offering spans at 4 800 000, and a
   receiver could never run at the device's own rate -- which is the one
   configuration that needs no resampler at all.  9 600 000 is the widest span
   the block geometry permits (rx_span_is_exact: the audio block is 25600/(rate/
   48000) and must not fall below 128), so above it the extra width buys
   receivers room to sit apart from each other, not a wider view for any one of
   them. The list is bounded at use by what the
   device said it could be clocked at (info.soapy.rx_rate_max) -- an AD9361
   answers 61 440 000 there and a USRP more, so the top of this list is not
   meant to be the limit; the limit is meant to be the hardware and the link.
   It stops where it does because nothing this fork has met carries more (a
   Pluto has only USB 2.0 High Speed and the network gadget on it), and because
   an entry that no link can deliver is not a choice, it is a way to break the
   stream in silence. Add to it when a device and a link that want more turn
   up -- and measure the delivery, do not assume it.

   BELOW the old 2 304 000 the list has exactly three entries, and arithmetic is
   what picks them rather than taste.  The point of going down there is the
   wire: I/Q crosses it as CS16, four bytes a sample, so 2 304 000 is
   73.7 Mbit/s and 768 000 is 24.6 -- which is the whole answer for a Pluto on
   the far end of a network that a wider stream does not fit through.
     - The floor is 25e6/48 = 520 833 Hz, not the 25e6/12 = 2 083 333 the
       2 304 000 hardcode was chosen above.  That higher floor belongs to a
       SoapyPlutoSDR built WITHOUT libad9361, which tools/build-soapy-plutosdr.sh
       now guarantees.  Measured with it, on the development Pluto over the
       network, 8 s a row (soapy_bench): 768 000 answered 767 999 and delivered
       99.9 % at 24.6 Mbit/s, 1 536 000 and 1 920 000 answered exactly and
       delivered 99.9 % at 49.2 and 61.4, all three with 0 overruns and no gap
       past 2x the read interval.  The 1.3 ppm on the first is snapped away by
       soapy_build_resampler.
     - It stops at 768 000 and not at 192 000 because under that floor the
       driver runs the phy at 8x and decimates on the HOST, while
       getSampleRate() reports the rate that was asked for -- so nothing here
       can see it, and 384 000 would put 3 072 000 on the wire, MORE than the
       2 304 000 it was picked to undercut.  A rate below the floor saves
       nothing; it just moves the decimation to the wrong end of the link.
     - An entry at or below 1 920 000 must be one of soapy_rx_spans as well.
       Above that ceiling radio_default_rx_span() hands a fresh receiver
       1 920 000 whatever the rate is; at or below it, it hands the RATE, which
       therefore has to be a span a receiver can actually run and the span
       selector can actually show.  That is the test 1 152 000 and 576 000 fail
       even though both are multiples of 192 kHz -- rx_span_is_exact wants
       5120/(rate/48000) to be a whole number and both leave a remainder of 8 --
       and it is also why 960 000 is absent: the arithmetic passes (5120/20 =
       256) and it is still not a span this app offers.
   Two things the operator is buying them with, both in the tooltip: on an
   AD9361 this is the DAC clock as well (soapy_tx_dac_rate), so picking one
   re-opens the transmit chain at that rate; and the shared window IS this rate,
   so at 768 000 with a 768 000 span a second receiver has nowhere left to sit. */
static const int soapy_adc_rate_list[]={
  768000,1536000,1920000,
  2304000,3072000,3840000,4608000,4800000,5760000,7680000,9216000,9600000,
  11520000,15360000,19200000,23040000,30720000
};

int soapy_adc_rate_count(void) { return (int)G_N_ELEMENTS(soapy_adc_rate_list); }

// Shared by both settings pages and by the clamp on a device-rate decrease.
static const int soapy_rx_spans[]={192000,384000,768000,1536000,1920000,4800000,9600000};
int soapy_rx_span_count(void) { return (int)G_N_ELEMENTS(soapy_rx_spans); }
int soapy_rx_span_at(int i) {
  return i>=0 && i<soapy_rx_span_count()?soapy_rx_spans[i]:0;
}
int soapy_rx_span_max(int device_rate) {
  int span=0;
  for(int i=0;i<soapy_rx_span_count();i++) {
    if(soapy_rx_spans[i]<=device_rate) span=soapy_rx_spans[i];
  }
  return span;
}

int soapy_adc_rate_at(int i) {
  if(i<0 || i>=(int)G_N_ELEMENTS(soapy_adc_rate_list)) return 0;
  return soapy_adc_rate_list[i];
}

gboolean soapy_adc_rate_valid(DISCOVERED *d,int rate) {
  if(d==NULL || d->device!=DEVICE_SOAPYSDR) return FALSE;
  for(int i=0;i<(int)G_N_ELEMENTS(soapy_adc_rate_list);i++) {
    if(soapy_adc_rate_list[i]!=rate) continue;
    // A rate the device itself does not claim is not offered, whatever the list
    // says: an RTL dongle stops around 3.2 MS/s and must not be asked for 19.2.
    if(d->info.soapy.rx_rate_max>0 && rate>d->info.soapy.rx_rate_max) return FALSE;
    // Since the list reaches below 2 304 000 that ceiling is no longer the whole
    // test: the same RTL dongle claims 225001..300000 and 900001..3200000, and
    // 768 000 lies in the hole between them -- under the ceiling and not
    // runnable.  Where the device described its ranges, a rate has to be inside
    // one of them; where it described none, the ceiling is all there is.
    if(d->info.soapy.rx_rate_ranges>0) {
      for(int j=0;j<d->info.soapy.rx_rate_ranges;j++) {
        if(rate>=d->info.soapy.rx_rate_lo[j] && rate<=d->info.soapy.rx_rate_hi[j]) return TRUE;
      }
      return FALSE;
    }
    return TRUE;
  }
  return FALSE;
}

int soapy_tx_dac_rate(void) {
  if(radio==NULL) return TX_SOAPY_MAX_IQ_RATE;
  if(soapy_span_driven() && radio->sample_rate>TX_SOAPY_MAX_IQ_RATE)
    return TX_SOAPY_MAX_IQ_RATE;
  return radio->sample_rate;
}

/* The analog baseband filter has to follow the rate the hardware is running at,
   or the widest spans are a picture of a 2 MHz hole in the middle of the
   screen: this was a hardcoded 2000000 for every device and every rate.

   And asking for exactly the rate is not the same as getting it.  A device
   whose filter is a ladder of fixed steps rounds DOWN to its nearest one, so a
   HackRF told "9600000" runs its MAX2837 at 7 MHz: the outer 1.3 MHz of each
   side of the span is attenuated in the analogue domain, before anything here
   can see it (4800000 -> 3.5 MHz and 2000000 -> 1.75 MHz are the same story,
   and an sdrplay asked for 384000 gets 300 kHz).  A complex baseband stream at
   Fs occupies the whole +-Fs/2, so the step wanted is the first one AT OR ABOVE
   the rate, which is what is picked here out of what the driver offers.

   The cost is that the extra passband beyond Nyquist folds in: with a 10 MHz
   filter on a 9.6 MHz stream, 4.8..5 MHz lands on the top 200 kHz of the span.
   That is the trade -- a little aliasing at the very edge instead of a real
   analogue roll-off across a fifth of the picture.

   A driver that offers no list at all (or is too old to be asked) gets the
   request it always got. */
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION >= 0x00060000)
/* The lowest filter setting that still covers `want`, out of what the driver
   offers.  A fixed step comes back as minimum==maximum; a continuously tunable
   filter as a real span, where `want` itself is available if it lies inside it.
   With every range below `want` there is nothing to pick but the widest, which
   is what the device would have clamped to anyway.  Returns 0 when the driver
   offers nothing, i.e. "ask for what you were going to ask for". */
static double soapy_bandwidth_pick(const SoapySDRRange *r,size_t n,double want) {
  if(r==NULL || n==0) return 0.0;
  double pick=0.0,widest=0.0;
  for(size_t i=0;i<n;i++) {
    const double cand=(want<=r[i].minimum)?r[i].minimum:
                      ((want<=r[i].maximum)?want:0.0);
    if(cand>0.0 && (pick<=0.0 || cand<pick)) pick=cand;
    if(r[i].maximum>widest) widest=r[i].maximum;
  }
  return (pick>0.0)?pick:widest;
}
#endif

static void soapy_set_rx_bandwidth(size_t adc,int hw_rate) {
  double want=(double)hw_rate;
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION >= 0x00060000)
  size_t n=0;
  SoapySDRRange *r=SoapySDRDevice_getBandwidthRange(soapy_device,SOAPY_SDR_RX,adc,&n);
  const double pick=soapy_bandwidth_pick(r,n,want);
  if(pick>0.0) want=pick;
  if(r!=NULL) SoapySDR_free(r);
#endif
  int rc=SoapySDRDevice_setBandwidth(soapy_device,SOAPY_SDR_RX,adc,want);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setBandwidth(%.0f) failed: %s\n",__FUNCTION__,want,SoapySDR_errToStr(rc));
  }
  /* Say what the filter ENDED UP at, not what it was asked for: the round-down
     this function exists to defeat is invisible in the request. */
  const double got=SoapySDRDevice_getBandwidth(soapy_device,SOAPY_SDR_RX,adc);
  log_info("%s: hardware rate %d: asked for %.0f Hz of baseband filter, running %.0f\n",
           __FUNCTION__,hw_rate,want,got);
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
  RXSLOT *sl=slot_add(rx);
  if(sl==NULL) return;
  if(rx->resampler!=NULL) {
    destroy_resample(rx->resampler);
    rx->resampler=NULL;
  }
  slot_dsp_free(sl);
  const gboolean owner=(sl==&adc_slot[rx->adc][0]);
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

#ifdef LIQUID
  /* A follower is centred by an NCO of its own, so it needs the mixer even when
     the stream is already at its span (offset 0 costs nothing: the mixer is only
     run when the offset is non-zero). */
  if(!owner) {
    sl->mixer=nco_crcf_create(LIQUID_VCO);
    if(sl->mixer==NULL) {
      log_error("%s: nco_crcf_create failed; receiver %d cannot be tuned inside the shared stream\n",
                __FUNCTION__,rx->channel);
    }
  }
#else
  if(!owner) {
    /* Without liquid-dsp there is no mixer and no per-receiver decimator, so a
       shared-ADC receiver could only ever listen at the owner's centre.  It is
       not offered in that build (see soapy_discovery.c); say so if one turns up
       anyway rather than quietly receiving the wrong frequency. */
    log_error("%s: this build has no liquid-dsp: receiver %d cannot share adc %d\n",
              __FUNCTION__,rx->channel,rx->adc);
    return;
  }
#endif

  if(in_rate==rx->sample_rate) {
    log_info("%s: no resampler needed: stream and receiver are both at %d\n",__FUNCTION__,in_rate);
    sl->ready=TRUE;
    return;
  }

#ifdef LIQUID
  /* MACHPSDR_FRONTEND=wdsp puts the old single-stage resampler back without a
     rebuild.  It exists so a report of "the audio is wrong at some spans" can be
     split in one session: same signal, same span, one variable.  Only for the
     receiver that owns the ADC: WDSP's resampler cannot take the mixed block a
     follower needs, so honouring it there would silently move that receiver back
     onto the owner's frequency. */
  static int frontend_wdsp=-1;
  if(frontend_wdsp<0) {
    const char *e=g_getenv("MACHPSDR_FRONTEND");
    frontend_wdsp=(e!=NULL && strcmp(e,"wdsp")==0)?1:0;
    if(frontend_wdsp) log_info("%s: MACHPSDR_FRONTEND=wdsp: using WDSP's single-stage resampler\n",__FUNCTION__);
  }
  /* The multistage cascade takes the ratio as a float, so the gcd of the two
     rates does not enter into it -- the arithmetic trap the WDSP path below has
     to warn about simply is not there. */
  if(!frontend_wdsp || !owner) {
    const float rate=(float)((double)rx->sample_rate/(double)in_rate);
    sl->ratio=rate;
    sl->out_cap=(int)((double)block*(double)rate)+16;
    sl->out=g_new(liquid_float_complex,sl->out_cap);
    sl->resamp=msresamp_crcf_create(rate,MS_RESAMP_STOPBAND_DB);
    if(sl->resamp!=NULL) {
      log_info("%s: multistage decimator: block=%d stream=%d -> rx%d=%d (rate %.6f, %.0f dB, delay %.1f samples)\n",
               __FUNCTION__,block,in_rate,rx->channel,rx->sample_rate,(double)rate,
               (double)MS_RESAMP_STOPBAND_DB,(double)msresamp_crcf_get_delay(sl->resamp));
      sl->ready=TRUE;
      return;
    }
    /* Never silently: falling through here means the expensive path, and the
       operator is entitled to know why their wide span costs what it does. */
    log_error("%s: msresamp_crcf_create(%.6f) failed; falling back to WDSP's single-stage resampler\n",
              __FUNCTION__,(double)rate);
    if(sl->out!=NULL) { g_free(sl->out); sl->out=NULL; }
    sl->out_cap=0;
    if(!owner) return;   /* a follower has no usable fallback */
  }
#endif

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
  sl->ready=TRUE;
log_info("%s: created resampler: block=%d stream=%d -> rx=%d resampled_buffer=%d doubles\n",__FUNCTION__,block,in_rate,rx->sample_rate,rx->resampled_buffer_size);
}

/* The frequency this receiver wants the device tuned to.  The owner gets it;
   every follower measures its NCO offset against what the device answered. */
static double soapy_rx_target_freq(RECEIVER *rx) {
  double f=(double)(rx->frequency_a-rx->lo_a+rx->error_a);
  f+=(double)radio_ppm_correction(rx->frequency_a-rx->lo_a);
  if(!rx->ctun) {
    if(rx->rit_enabled) {
      f+=(double)rx->rit;
    }
  }
  return f;
}

/* How far a follower's centre sits from the device LO.  Only the number is
   stored: the NCO's step depends on the I/Q order the operator has selected,
   which is a per-block decision (see slot_feed). */
static void slot_set_offset(RXSLOT *sl,long long off) {
  if(sl->offset==off) return;
  sl->offset=off;
  log_debug_area(LOG_RX, "%s: rx%d now %+lld Hz from the device LO\n",
            __FUNCTION__,sl->rx!=NULL?sl->rx->channel:-1,off);
}

/* Re-derive every follower's offset on this ADC.  Runs whenever the thing they
   are measured against moves: the owner retuning, or the hardware rate changing
   under them. */
static void soapy_refresh_offsets(size_t ch) {
  if(ch>=MAX_CHANNELS) return;
  for(int i=1;i<MAX_ADC_RECEIVERS;i++) {
    RECEIVER *f=adc_slot[ch][i].rx;
    if(f==NULL) continue;
    slot_set_offset(&adc_slot[ch][i],(long long)(soapy_rx_target_freq(f)-adc_lo[ch]));
  }
}

/* Hz this receiver's centre has to move to sit inside the window its ADC's
   owner leaves it: the device covers hw_rate about the owner's LO, and this
   receiver's own span has to fit whole inside that.  0 when it fits, and 0 for
   a receiver that owns its ADC -- that one moves the LO instead.  The answer is
   a DELTA rather than a pair of limits on purpose: the dial, the converter LO,
   its measured error and the ppm correction all sit between the two frequencies,
   and a delta needs none of them inverted. */
long long soapy_protocol_rx_window_error(RECEIVER *rx) {
  if(rx==NULL) return 0;
  size_t ch=(size_t)rx->adc;
  if(ch>=MAX_CHANNELS) return 0;
  if(slot_is_owner(rx)) return 0;
  if(adc_lo[ch]==0.0) return 0;            /* the owner has not tuned yet */
  const int hw=(soapy_rx_actual_rate>0)?soapy_rx_actual_rate:radio->sample_rate;
  long long half=((long long)hw-(long long)rx->sample_rate)/2;
  if(half<0) half=0;                       /* a span wider than the stream: pin it to the LO */
  const long long want=(long long)soapy_rx_target_freq(rx);
  const long long lo=(long long)adc_lo[ch];
  if(want>lo+half) return (lo+half)-want;
  if(want<lo-half) return (lo-half)-want;
  return 0;
}

/* One raw block into one receiver: its own centre, its own span.  The caller
   holds delete_rx_mutex and has checked that the receiver is still live. */
static void slot_feed(RXSLOT *sl,const float *buffer,int elements,gboolean iqswap) {
  RECEIVER *rx=sl->rx;
  const float *src=buffer;
  double isample,qsample;
  int i;

#ifdef LIQUID
  /* Bring this receiver's centre to DC before the decimator, or the signal it
     is listening to is outside the band the decimator keeps.  The stream is
     CF32 and liquid_float_complex is (re,im) floats, so it goes in as it
     arrived.
     The shift is in the DEVICE's domain -- the offset is measured from the
     frequency the hardware is tuned to, and this runs before anything the
     application does to the pair -- so `iqswap` has no business in it, however
     much it looks as though it should.  That flag is not a mirrored device: on
     a SoapySDR device it is ON by default and cancels WDSP's own (Q, I) order,
     and flipping the mixer with it put the second receiver the right distance
     on the WRONG SIDE of the LO.  Measured against tools/soapy_null.cpp with a
     tone 40 kHz above centre and a second receiver 10 kHz above the first: the
     tone must land at +30 kHz in its stream, and read +50 kHz with the flip
     in. */
  if(sl->mixer!=NULL && sl->offset!=0) {
    const int rate=(soapy_rx_actual_rate>0)?soapy_rx_actual_rate:radio->sample_rate;
    if(sl->mix_cap<elements) {
      g_free(sl->mix);
      sl->mix=g_new(liquid_float_complex,elements);
      sl->mix_cap=elements;
    }
    nco_crcf_set_frequency(sl->mixer,(float)(2.0*M_PI*(double)sl->offset/(double)rate));
    nco_crcf_mix_block_down(sl->mixer,(liquid_float_complex *)(uintptr_t)src,sl->mix,(unsigned int)elements);
    src=(const float *)sl->mix;
  }
  if(sl->resamp!=NULL) {
    unsigned int ny=0;
    /* msresamp writes ahead of any count this function returns, so the room has
       to be there BEFORE the call -- clamping the answer afterwards is a check
       that runs after the overflow.  The block a receiver is fed is the one its
       ADC's owner drains the FIFO in, and that is decided by the owner: a
       follower built for a smaller one would otherwise be a heap overflow the
       first time the owner's stream came up with a bigger MTU. */
    const int need=(int)((double)elements*(double)sl->ratio)+16;
    if(sl->out_cap<need) {
      g_free(sl->out);
      sl->out=g_new(liquid_float_complex,need);
      sl->out_cap=need;
    }
    msresamp_crcf_execute(sl->resamp,(liquid_float_complex *)(uintptr_t)src,
                          (unsigned int)elements,sl->out,&ny);
    if((int)ny>sl->out_cap) ny=(unsigned int)sl->out_cap;  // cannot happen; not a thing to find out the hard way
    for(i=0;i<(int)ny;i++) {
      isample=(double)crealf(sl->out[i]);
      qsample=(double)cimagf(sl->out[i]);
      if(iqswap) add_iq_samples(rx,qsample,isample);
      else       add_iq_samples(rx,isample,qsample);
    }
    return;
  }
#else
  (void)sl;
#endif

  if(rx->resampler!=NULL) {
    for(i=0;i<elements;i++) {
      rx->buffer[i*2]=(double)src[i*2];
      rx->buffer[(i*2)+1]=(double)src[(i*2)+1];
    }
    // xresampleV, not xresample: the latter always consumes the count the
    // resampler was CREATED with, while readStream returns "up to" block --
    // so a short read re-resampled the tail of the previous one, and the
    // very first one resampled uninitialised heap (rx->buffer is g_new).
    int out_elements=0;
    xresampleV(rx->buffer,rx->resampled_buffer,elements,&out_elements,rx->resampler);
    for(i=0;i<out_elements;i++) {
      if(iqswap) {
        qsample=rx->resampled_buffer[i*2];
        isample=rx->resampled_buffer[(i*2)+1];
      } else {
        isample=rx->resampled_buffer[i*2];
        qsample=rx->resampled_buffer[(i*2)+1];
      }
      add_iq_samples(rx,isample,qsample);
    }
  } else {
    /* No resampler: the stream is already at the span, so it goes straight
       from the CF32 block the driver filled. */
    for(i=0;i<elements;i++) {
      isample=(double)src[i*2];
      qsample=(double)src[(i*2)+1];
      if(iqswap) {
        add_iq_samples(rx,qsample,isample);
      } else {
        add_iq_samples(rx,isample,qsample);
      }
    }
  }
}

/* Rebuilds rx->resampler, which the receive thread is using inside
   delete_rx_mutex -- so the caller must hold that lock.  The public wrapper
   below takes it; receiver_change_sample_rate takes it itself, BEFORE
   rx->mutex, because the receive thread takes them in that order
   (delete_rx_mutex around the block, then rx->mutex inside full_rx_buffer) and
   the other order is a deadlock. */
void soapy_protocol_change_sample_rate_locked(RECEIVER *rx,int rate) {
  // The device-rate transaction rebuilds all streams/resamplers together after
  // receiver_change_sample_rate has resized the DSP and clamped wide spans.
  if(changing_device_rate) return;
  const size_t slot_ch=(size_t)(rx->adc<MAX_CHANNELS?rx->adc:0);
  const int block=rx_block[slot_ch];

  if(!slot_is_owner(rx)) {
    // A follower's span is a decimation ratio of its own applied to the shared
    // block: rebuild that, re-derive its offset (the window it must fit in has
    // just changed with its span) and leave the device and the FIFO alone --
    // clearing the queue here would cost the OWNER a quarter of a second of
    // stream for a change that is none of its business.
    soapy_build_resampler(rx,block);
    soapy_protocol_set_rx_frequency(rx);
    return;
  }

  // Devices driven at the receiver's own rate (see soapy_hw_rate_for) have to be
  // re-programmed here; for the rest the hardware rate does not move and only
  // the resampler is rebuilt.  Either way it goes through the readback: if the
  // hardware lands somewhere else, the builder below puts a resampler in rather
  // than pretending.
  const int hw_rate=soapy_hw_rate_for(rx);
  if(hw_rate!=soapy_rx_sample_rate) {
    const size_t ch=rx->adc;
    // A sample rate set UNDER A LIVE STREAM is not a clean change of clock: the
    // transfers already in flight belong to the old rate, the driver's ring is
    // full of them, and what comes out the other side is spliced.  Measured on a
    // HackRF over twelve span changes: hundreds of dropped DSP blocks and up to
    // 400 ms of chopped audio per change.  So take the stream all the way down
    // around it, exactly as the TX over does (rx_resume_channel) -- park the
    // reader first, so nothing is inside readStream while the stream it is
    // reading is closed.
    const gboolean restart=(rx_stream_active && ch<MAX_CHANNELS && rx_stream[ch]!=NULL);
    if(restart) {
      log_info("%s: hardware rate %d -> %d: restarting the RX stream around it\n",
               __FUNCTION__,soapy_rx_sample_rate,hw_rate);
      rx_stream_active=FALSE;
      wait_rx_parked(ch);
      SoapySDRDevice_deactivateStream(soapy_device,rx_stream[ch],0,0LL);
      SoapySDRDevice_closeStream(soapy_device,rx_stream[ch]);
      rx_stream[ch]=NULL;
    }

    soapy_rx_sample_rate=hw_rate;
    soapy_set_rx_bandwidth(rx->adc,hw_rate);
    soapy_set_rx_rate(rx->adc,hw_rate);

    if(restart) {
      size_t setup_ch=ch;
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
      SoapySDRDevice_setupStream(soapy_device,&rx_stream[ch],SOAPY_SDR_RX,SOAPY_SDR_CF32,&setup_ch,1,NULL);
#else
      rx_stream[ch]=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_RX,SOAPY_SDR_CF32,&setup_ch,1,NULL);
#endif
      if(rx_stream[ch]==NULL) {
        log_error("%s: could not set the RX stream up again at %d Hz: %s\n",
                  __FUNCTION__,hw_rate,SoapySDRDevice_lastError());
      } else {
        SoapySDRDevice_activateStream(soapy_device,rx_stream[ch],0,0LL,0);
      }
      // Whatever is queued came off the old stream at the old rate.
      fifo_clear(ch);
      rx_stream_active=TRUE;    // last, so the reader never sees a half-built stream
    }
  }
  soapy_build_resampler(rx,block);
  // The stream's rate is what every follower's decimator ratio and NCO step are
  // built from, so a change here rebuilds theirs too -- on the devices whose
  // hardware rate follows the owner's span (soapy_hw_rate_for) this is the whole
  // window moving under them.
  for(int i=1;i<MAX_ADC_RECEIVERS;i++) {
    if(adc_slot[slot_ch][i].rx!=NULL) soapy_build_resampler(adc_slot[slot_ch][i].rx,block);
  }
  soapy_refresh_offsets(slot_ch);
  // Whatever queued up while the channel was being rebuilt is LATE -- the GTK
  // thread holds the lock for the whole rebuild, and at 2 MS/s a quarter of a
  // second of stream can pile up behind it.  Handing that to WDSP in one go is
  // what fills its input ring and costs a burst of dropped blocks (fexchange0
  // -2) plus the audio gap that follows; dropping it costs the same quarter
  // second, once, and the receiver comes back in step.
  fifo_clear(rx->adc);
}

void soapy_protocol_change_sample_rate(RECEIVER *rx,int rate) {
  g_mutex_lock(&radio->delete_rx_mutex);
  soapy_protocol_change_sample_rate_locked(rx,rate);
  g_mutex_unlock(&radio->delete_rx_mutex);
}

/* SoapySDR stream arguments.  One device needs them, and needs them badly.
 *
 * A networked PlutoSDR is reached through libiio's NETWORK backend, which is
 * strictly request/response: iio_buffer_refill() writes "READBUF <dev> <len>"
 * and waits for the whole reply, so nothing is draining the device while that
 * request is in flight.  A stall on the link is therefore not late data -- it is
 * samples the Pluto's DMA ring dropped, spliced into the stream, and NOTHING
 * reports it: SoapyPlutoSDR's readStreamStatus() is SOAPY_SDR_NOT_SUPPORTED, so
 * readStream never returns SOAPY_SDR_OVERFLOW and never sets END_ABRUPT.  The
 * operator sees a stuttering waterfall and chopped audio, which reads as a
 * broken receiver rather than as a link that stalls.
 *
 * The driver sizes its buffer at about a sixtieth of the sample rate (~17 ms),
 * so a link whose latency spikes past that loses samples on every spike.  The
 * cure is a buffer long enough to cover a spike: fewer round trips per second,
 * and each one covers more time.  Measured over Wi-Fi with 94 ms latency
 * outliers (tools/soapy_bench.c, 12 s per row):
 *
 *     rate       buffer   delivered   signal lost per second
 *     768 000    16384      94.6 %        53.6 ms
 *     768 000    65536      99.5 %         4.8 ms
 *     2 304 000  65536      97.8 %        22.4 ms
 *     2 304 000  262144     99.2 %         8.4 ms
 *
 * ~36 ms of stream is the best of it -- and it is not a monotonic trade where
 * more buffer buys more stream.  Each row twice, 10 s, at the 2 304 000 a Pluto
 * runs here, and the gap column is the one that matters to anything watching:
 *
 *     buffer                 delivered        p95 gap   longest
 *     65536   (28 ms)        96.9 %, 97.3 %    96 ms    119 ms
 *     81920   (36 ms)        99.9 %, 99.8 %    38 ms    124 ms
 *     131072  (57 ms)        99.6 %, 99.6 %   101 ms    151 ms
 *     262144  (114 ms)       99.0 %, 99.0 %   121 ms    199 ms
 *
 * A longer request is not merely wasteful: it occupies the socket for longer,
 * so a stall inside it costs more and the delivery is WORSE as well as later.
 * Rounded to a multiple of 8192 rather than to a power of two (the driver's own
 * preference, which would overshoot 36 ms to 57 here).
 *
 * The gap column also decides how the display moves, though nothing here is
 * tuned for it: update_timer_cb advances the waterfall one line per analyzer
 * frame, and a frame is one 1/fps of STREAM, so the waterfall inherits the
 * delivery cadence with no buffer of its own to hide it -- unlike the audio,
 * which rides over a 160 ms WDSP ring and a 50 ms sink.  A 38 ms p95 sits under
 * the 40 ms frame at the default 25 fps; a 101 ms one does not.
 *
 * It is paid for in latency and in nothing else, and only on the network
 * backend -- the same device over `usb:` has none of this and is left alone, as
 * is every other driver.  MACHPSDR_SOAPY_BUFFLEN overrides for experiment; 0
 * hands the driver back its own choice.
 *
 * Note the driver keeps its MTU at the default when bufflen is given
 * explicitly, so this does NOT change the size of the app's reads -- only how
 * much the device buffers behind them.
 */
static void soapy_rx_stream_args(SoapySDRKwargs *args, int rate) {
  long bufflen=0;
  const char *env=getenv("MACHPSDR_SOAPY_BUFFLEN");

  if(env!=NULL) {
    bufflen=atol(env);
    /* Bounded like every other number that reaches an allocation: this one is
       a buffer the DEVICE has to find, and a silly value fails the setup
       rather than being ignored. */
    if(bufflen<0) bufflen=0;
    if(bufflen>(1L<<22)) bufflen=(1L<<22);
  } else if(radio!=NULL && radio->discovered!=NULL &&
            strstr(radio->discovered->info.soapy.make_args,"uri=ip:")!=NULL &&
            rate>0) {
    const long target=(long)(((long long)rate*36)/1000);   /* ~36 ms; see above */
    bufflen=((target+8191)/8192)*8192;
    if(bufflen<8192) bufflen=8192;
    if(bufflen>(1L<<20)) bufflen=(1L<<20);
  }
  if(bufflen>0) {
    char temp[32];
    snprintf(temp,sizeof(temp),"%ld",bufflen);
    SoapySDRKwargs_set(args,"bufflen",temp);
    log_info("%s: network device: asking for a %ld-sample stream buffer (%.0f ms at %d)\n",
             __FUNCTION__,bufflen,1000.0*(double)bufflen/(double)(rate>0?rate:1),rate);
  }
}

static gboolean soapy_create_receiver(RECEIVER *rx) {
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

  size_t slot_ch=(size_t)rx->adc;
  if(slot_ch>=MAX_CHANNELS) {
    log_error("%s: adc %ld has no stream slot (MAX_CHANNELS=%d)\n",__FUNCTION__,(long)slot_ch,MAX_CHANNELS);
    return FALSE;
  }
  if(slot_add(rx)==NULL) return FALSE;
  if(!slot_is_owner(rx)) {
    // A receiver sharing another one's ADC gets no stream, no threads and no
    // say over the hardware: it is handed the same raw block and does its own
    // mixing and decimation (slot_feed).  Its block is therefore the one the
    // owner's stream is drained in.
    const int shared_block=rx_block[slot_ch]>0?rx_block[slot_ch]:2048;
    // Under the lock: the DSP thread for this ADC is already running and reads
    // the slot table inside it.  Nothing is joined in here, so there is nobody
    // to deadlock against -- unlike the owner's path below, which is only ever
    // reached with the threads not yet started or already stopped.
    g_mutex_lock(&radio->delete_rx_mutex);
    rx->buffer=g_new(double,shared_block*2);
    rx->resampled_buffer_size=0;
    soapy_build_resampler(rx,shared_block);
    soapy_protocol_set_rx_frequency(rx);      // its offset from the owner's LO
    g_mutex_unlock(&radio->delete_rx_mutex);
    log_info("%s: receiver %d shares adc %ld (%d-sample blocks, span %d)\n",
             __FUNCTION__,rx->channel,(long)slot_ch,shared_block,rx->sample_rate);
    return TRUE;
  }

  // What the hardware is told to run at -- the ADC rate for most devices, this
  // receiver's own span for the two that are driven directly (soapy_hw_rate_for).
  soapy_rx_sample_rate=soapy_hw_rate_for(rx);

  soapy_set_rx_bandwidth(rx->adc,soapy_rx_sample_rate);   /* logs what the filter ended up at */

log_info("%s: setting samplerate=%f\n",__FUNCTION__,(double)soapy_rx_sample_rate);
  soapy_set_rx_rate(rx->adc,soapy_rx_sample_rate);

  size_t channel=slot_ch;
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
  SoapySDRKwargs stream_args={0};
  soapy_rx_stream_args(&stream_args,soapy_rx_actual_rate>0?soapy_rx_actual_rate:soapy_rx_sample_rate);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&rx_stream[channel],SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,&stream_args);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setupStream (RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
    SoapySDRKwargs_clear(&stream_args);
    return FALSE;
  }
#else
  rx_stream[channel]=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,&stream_args);
  if(rx_stream[channel]==NULL) {
    // This branch has no return code to report -- setupStream answers with the
    // pointer -- so the reason comes from the device.  It used to print
    // SoapySDR_errToStr(rc) on whatever rc the previous call happened to leave.
    log_info("%s: SoapySDRDevice_setupStream (RX) failed: %s\n",__FUNCTION__,SoapySDRDevice_lastError());
    SoapySDRKwargs_clear(&stream_args);
    return FALSE;
  }
#endif
  SoapySDRKwargs_clear(&stream_args);

  const int mtu=(int)SoapySDRDevice_getStreamMTU(soapy_device,rx_stream[channel]);
  int read_block=mtu;
  // SoapyHackRF delivers one 131072-sample transfer at a time.  Its CF32
  // readStream path does not preserve that transfer completely when the caller
  // repeatedly asks for a smaller, non-dividing block: measured at 2 MS/s,
  // the old 10240-sample clamp delivered 98.9% of the stream even in the bare
  // soapy_bench, while reading the advertised MTU delivered 100.0%.  Those
  // missing tails are splices in I/Q, heard as the periodic chopped signal.
  //
  // Keep the historical DSP-sized reads for other drivers: a network driver
  // may advertise a very large MTU whose latency/working-set is undesirable,
  // and none has shown this HackRF transfer-tail behaviour.
  // MACHPSDR_SOAPY_READ_MTU=0/1 forces the choice either way, so "did this
  // change cause what I am looking at?" is one restart and one variable rather
  // than a rebuild off an older commit.
  gboolean needs_whole_mtu=(strcmp(radio->discovered->name,"hackrf")==0);
  {
    const char *e=g_getenv("MACHPSDR_SOAPY_READ_MTU");
    if(e!=NULL) {
      needs_whole_mtu=(atoi(e)!=0);
      log_info("%s: MACHPSDR_SOAPY_READ_MTU=%s: %s\n",__FUNCTION__,e,
               needs_whole_mtu?"reading whole transfers":"reading DSP-sized blocks");
    }
  }
  if(!needs_whole_mtu && read_block>(2*rx->fft_size)) read_block=2*rx->fft_size;
  int dsp_block=mtu;
  if(dsp_block>(2*rx->fft_size)) dsp_block=2*rx->fft_size;
  rx_read_block[channel]=read_block;
  rx_block[channel]=dsp_block;
  log_info("%s: stream MTU=%d, read block=%d%s, DSP block=%d\n",__FUNCTION__,
           mtu,read_block,needs_whole_mtu?" (whole HackRF transfer)":"",dsp_block);
  // The FIFO has to swallow a whole delivery burst several times over, and the
  // burst is the device's REAL MTU -- 131072 samples on a HackRF, i.e. 65 ms at
  // 2 MS/s -- not the read size above, which is clamped to what one DSP block
  // wants.  Four of them, bounded so a driver claiming an enormous MTU cannot
  // ask for hundreds of megabytes.
  {
    long long cap=4LL*(long long)((mtu>read_block)?mtu:read_block);
    if(cap<4LL*read_block) cap=4LL*read_block;
    if(cap>(1LL<<20)) cap=(1LL<<20);
    fifo_alloc(channel,(int)cap);
  }
  rx->buffer=g_new(double,dsp_block*2);
  // The freeing above cleared it; the builder allocates it to the size the real
  // stream rate calls for.
  rx->resampled_buffer_size=0;
  soapy_build_resampler(rx,dsp_block);
  // Everything riding on this stream was built for the one that just went away
  // (this function is re-entered by the reconnect path, and the MTU, the block
  // and the rate are all free to come back different).
  for(int i=1;i<MAX_ADC_RECEIVERS;i++) {
    if(adc_slot[channel][i].rx!=NULL) soapy_build_resampler(adc_slot[channel][i].rx,dsp_block);
  }
  soapy_refresh_offsets(channel);
  return TRUE;
}

void soapy_protocol_create_receiver(RECEIVER *rx) {
  if(!soapy_create_receiver(rx)) _exit(-1);
}

static gboolean soapy_start_receiver(RECEIVER *rx) {
  int rc;

  if(!slot_is_owner(rx)) {
    // The stream and both threads belong to the receiver that owns this ADC and
    // are already running; this one is fed out of the same block.
    log_info("%s: receiver %d rides adc %d's stream\n",__FUNCTION__,rx->channel,rx->adc);
    return TRUE;
  }

log_info("%s: activate_stream\n",__FUNCTION__);

  // Re-assert the RX sample rate right before activating the stream.  HackRF is
  // half-duplex with a single shared hardware clock, so whichever of RX/TX ran
  // setSampleRate last wins.  On the cold-start path add_transmitter ->
  // create_transmitter sets the TX rate AFTER create_receiver set the RX rate,
  // leaving the hardware clock at the TX rate while the app's resampler/DSP still
  // expect the RX rate -> artefacts (a live reconnect happened to set RX last and
  // sounded clean, which is why toggling the device "fixed" it).  Setting it here
  // makes the RX rate authoritative at activation, independent of call order.
  //
  // Re-assert only if the rate really has MOVED, though.  setSampleRate is not
  // a no-op on every driver even when the number does not change:
  // SoapyPlutoSDR re-sizes the RX stream's buffer from the rate whenever the
  // stream exists, and this runs after setupStream -- so on a networked Pluto
  // the unconditional re-assert silently undid the long buffer
  // soapy_rx_stream_args() had just asked for (measured: "asking for a
  // 262144-sample stream buffer" followed by the driver's own "Auto setting
  // Buffer Size: 65536", and the loss unchanged).  Whichever rate the hardware
  // is really at is what the readback says, which is the whole reason the
  // HackRF case is detectable: with the clock left at the TX rate the readback
  // differs and the set below still happens.  100 ppm, because a rate a hair
  // off the request is the same rate with a rounding error -- a Pluto asked
  // for 2 304 000 reports 2 303 999.
  const int rate_before=soapy_rx_actual_rate;
  const double at=SoapySDRDevice_getSampleRate(soapy_device,SOAPY_SDR_RX,rx->adc);
  int rate;
  // HackRF has one clock shared by RX and TX, but SoapyHackRF caches the two
  // requested rates separately.  create_transmitter() has just programmed the
  // hardware to 1.92 MHz; getSampleRate(RX) still answers the cached 2 MHz, so
  // the equality shortcut used to leave reception physically running at 96%
  // of the rate the resampler expects.  Measured consequence: 46080 rather
  // than 48000 audio frames/s and exactly 40 ms of inserted silence per second.
  // Re-assert RX unconditionally on this half-duplex device.  The shortcut is
  // retained for Pluto because setting an unchanged rate there re-sizes its
  // already-created network stream buffer.
  const gboolean force_rx_rate=(strcmp(radio->discovered->name,"hackrf")==0);
  if(!force_rx_rate && at>0.0 &&
     fabs(at-(double)soapy_rx_sample_rate)<=(double)soapy_rx_sample_rate*1.0e-4) {
    rate=(int)(at+0.5);
    soapy_rx_actual_rate=rate;
    log_info("%s: rate=%d (already there, not re-set)\n",__FUNCTION__,rate);
  } else {
    rate=soapy_set_rx_rate(rx->adc,soapy_rx_sample_rate);
    log_info("%s: rate=%d%s\n",__FUNCTION__,rate,
             force_rx_rate?" (re-asserted after HackRF TX setup)":"");
  }

  size_t channel=rx->adc;
  if(channel>=MAX_CHANNELS) {
    log_error("%s: adc %ld has no stream slot (MAX_CHANNELS=%d)\n",__FUNCTION__,(long)channel,MAX_CHANNELS);
    return FALSE;
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
    return FALSE;
  }

log_info("%s: create dsp_thread + receive_thread\n",__FUNCTION__);
  // The DSP thread first: it must be waiting on the FIFO before the reader
  // starts filling it, or the first burst is dropped for want of a consumer.
  dsp_thread_running[channel]=TRUE;
  dsp_thread_id[channel]=g_thread_new("soapy_dsp",dsp_thread,GSIZE_TO_POINTER(channel));
  if(dsp_thread_id[channel]==NULL) {
    log_error("%s: g_thread_new failed for dsp_thread\n",__FUNCTION__);
    dsp_thread_running[channel]=FALSE;
    return FALSE;
  }
  rx_thread_running[channel]=TRUE;
  running=TRUE;
  receive_thread_id[channel] = g_thread_new( "rx_thread", receive_thread, rx);
  if( ! receive_thread_id[channel] )
  {
    log_info("%s: g_thread_new failed for receive_thread\n",__FUNCTION__);
    return FALSE;
  }
  log_info( "%s: receive_thread: id=%p\n",__FUNCTION__,receive_thread_id[channel]);
  return TRUE;
}

void soapy_protocol_start_receiver(RECEIVER *rx) {
  if(!soapy_start_receiver(rx)) _exit(-1);
}

/* The TX twin of soapy_rx_stream_args, and it exists for a measured reason.
 *
 * SoapyPlutoSDR's tx_streamer hardcodes a 4096-sample buffer and reads the
 * `bufflen` argument on the RECEIVE side only -- 1.78 ms of DAC data at
 * 2.304 MS/s, pushed one non-cyclic iio_buffer at a time.  Every push on the
 * network and USB backends is a request/response round trip, so the DAC runs
 * dry between them and what comes out is the waveform chopped at the push rate.
 * Measured on a networked Pluto: a carrier streamed from here carried a comb of
 * sidebands out to +/-92 kHz at -21 dBc, while the same hardware generating the
 * same tone with its own FPGA DDS -- no host samples at all -- was clean to
 * -54 dBc.  Nothing in this application could see it: writeStream accepts every
 * sample it is offered whatever the DAC is doing, and SoapyPlutoSDR's
 * readStreamStatus is SOAPY_SDR_NOT_SUPPORTED, so the transmission summary read
 * "0 refused" throughout.
 *
 * tools/build-soapy-plutosdr.sh patches the driver to read the argument; a
 * bundle built before that patch, or a system driver, simply ignores it.
 *
 * ~36 ms, the same figure the receive side settled on by sweep -- and for the
 * same reason: past that a longer buffer holds the socket longer, so a stall
 * inside one costs more than the extra depth buys.
 */
static void soapy_tx_stream_args(SoapySDRKwargs *args, int rate) {
  long bufflen=0;
  const char *env=getenv("MACHPSDR_SOAPY_TX_BUFFLEN");

  if(env!=NULL) {
    bufflen=atol(env);
    if(bufflen<0) bufflen=0;
    if(bufflen>(1L<<22)) bufflen=(1L<<22);
  } else if(rate>0) {
    /* Unlike the receive side this is NOT restricted to a network device: the
       USB backend is request/response too, and the DAC has no second chance at
       a sample that arrived late.  A device whose driver ignores the argument
       is unaffected either way. */
    const long target=(long)(((long long)rate*36)/1000);
    bufflen=((target+8191)/8192)*8192;
    if(bufflen<8192) bufflen=8192;
    if(bufflen>(1L<<20)) bufflen=(1L<<20);
  }
  if(bufflen>0) {
    char temp[32];
    snprintf(temp,sizeof(temp),"%ld",bufflen);
    SoapySDRKwargs_set(args,"bufflen",temp);
    log_info("%s: asking for a %ld-sample TX buffer (%.1f ms at %d)\n",
             __FUNCTION__,bufflen,1000.0*(double)bufflen/(double)(rate>0?rate:1),rate);
  }
}

static gboolean soapy_create_transmitter(TRANSMITTER *tx,gboolean open_mic) {
  int rc;

log_info("soapy_protocol_create_transmitter: dac=%d\n",tx->dac);

  // Do this before bandwidth/rate/stream setup: some Pluto drivers enable the
  // TX LO as a side effect of setupStream(), so the hardware must already be
  // at maximum attenuation when that happens.
  soapy_tx_hold_safe();

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
  SoapySDRKwargs tx_args={0};
  soapy_tx_stream_args(&tx_args,soapy_tx_sample_rate);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&tx_stream,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,&tx_args);
  if(rc!=0) {
    log_info("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (TX) failed: %s\n",SoapySDR_errToStr(rc));
    SoapySDRKwargs_clear(&tx_args);
    return FALSE;
  }
#else
  tx_stream=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,&tx_args);
  if(tx_stream==NULL) {
    // setupStream answers with the pointer here, so there is no rc to report --
    // it used to print SoapySDR_errToStr(rc) on whatever the previous call left.
    log_info("soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (TX) failed: %s\n",SoapySDRDevice_lastError());
    SoapySDRKwargs_clear(&tx_args);
    return FALSE;
  }
#endif
  SoapySDRKwargs_clear(&tx_args);

  const int tx_mtu=(int)SoapySDRDevice_getStreamMTU(soapy_device,tx_stream);
  max_tx_samples=tx_mtu;
  if(max_tx_samples>(2*tx->fft_size)) {
    max_tx_samples=2*tx->fft_size;
  }
  // The MTU is the readback of the buffer the driver actually built, so it is
  // how "did bufflen take effect?" is answered -- a driver that ignores the
  // argument reports whatever it chose for itself, and there is no error.
log_info("soapy_protocol_create_transmitter: TX stream MTU=%d (%.1f ms at %d), write block=%d\n",
         tx_mtu,1000.0*(double)tx_mtu/(double)(soapy_tx_sample_rate>0?soapy_tx_sample_rate:1),
         soapy_tx_sample_rate,max_tx_samples);
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
log_info("soapy_protocol_create_transmitter: tx gain range=%f..%f\n",tx_gain_min,tx_gain_max);

  if(open_mic && radio->local_microphone) {
      if(audio_open_input(radio)!=0) {
        log_error("audio_open_input failed\n");
        radio->local_microphone=FALSE;
      }
    }

  return TRUE;
}

void soapy_protocol_create_transmitter(TRANSMITTER *tx) {
  if(!soapy_create_transmitter(tx,TRUE)) _exit(-1);
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
  log_info("soapy_protocol_init: device make returned\n");
  if(soapy_device==NULL) {
    log_info("%s: SoapySDRDevice_make failed: %s\n",__FUNCTION__,SoapySDRDevice_lastError());
    _exit(-1);
  }
  // Earliest point at which the application can touch the hardware.  Keep TX
  // attenuated throughout receiver/transmitter construction.
  soapy_tx_hold_safe();
  SoapySDRKwargs_clear(&args);
  log_info("soapy_protocol_init: safe TX setup complete\n");
}


/* Everything the receive thread used to do after the read.  One block at a time,
   in order, on its own thread: the reader is never blocked by it.
   It belongs to an ADC rather than to a receiver, because more than one receiver
   can listen to one ADC (see the slot table above) -- so the receivers are read
   out of that table on every block, inside delete_rx_mutex, which is also the
   only lock under which the table is changed.  Everything each slot touches
   (its decimator, rx->buffer, add_iq_samples) is freed by delete_receiver, and
   is therefore inside the same lock. */
static gpointer dsp_thread(gpointer data) {
  size_t channel=GPOINTER_TO_SIZE(data);
  const int block=rx_block[channel]>0?rx_block[channel]:2048;
  float *buffer=g_new(float,block*2);
  gint64 dropped_reported=g_get_monotonic_time();
  gint64 next_block_us=0;
  /* On this thread's stack, so nothing else can write it: see
     dc_block_reset_req above. */
  DCBLOCK dcb;
  dc_block_init(&dcb,soapy_rx_actual_rate);
  gboolean dc_on=FALSE;
  int dc_logged_rate=-1;
  gint64 dc_reported=g_get_monotonic_time();
log_info("%s: running (adc %ld, %d-sample blocks)\n",__FUNCTION__,(long)channel,block);
  while(dsp_thread_running[channel]) {
    int elements=fifo_pop(channel,buffer,block);
    if(elements<=0) continue;
    /* The zero-IF DC spike sits at the DEVICE's LO, so it is removed HERE:
       once, on the raw block, before the per-receiver NCO brings each centre
       to DC and the decimators throw the rest of the span away.  Anywhere
       further down is the wrong frequency -- after the mixer the spike is at
       minus the receiver's offset rather than at DC, and it would have to be
       found again in every receiver instead of being removed from the one
       stream they all share. */
    {
      const int rate=soapy_rx_actual_rate;
      if(rate!=dcb.rate) dc_block_init(&dcb,rate);
      if(g_atomic_int_get(&dc_block_reset_req[channel])) {
        g_atomic_int_set(&dc_block_reset_req[channel],0);
        dc_block_reset(&dcb);
      }
      /* One atomic load per block, not per sample -- the same rule the iqswap
         snapshot below is written to.  Switched off, the estimate is dropped
         rather than left to go stale, so switching it back on converges from
         zero instead of from an offset measured at some other gain. */
      const gboolean on=radio_dc_block_get(radio);
      if(on) dc_block_run(&dcb,buffer,elements);
      else   dc_block_reset(&dcb);
      /* "Is it even running?" has to be answerable from the log, or the next
         report is a screenshot and an argument.  The state is named when it
         changes, and the ESTIMATE is named every 5 s: a removal that is on and
         measuring nothing (an offset three orders below full scale) says the
         line in the middle of the picture was never a DC offset, which is a
         different fault and a different cure. */
      if(on!=dc_on || rate!=dc_logged_rate) {
        log_info("dc_block: adc %ld: DC spike removal %s (corner %.0f Hz at %d Hz)\n",
                 (long)channel,on?"ON":"OFF",DC_BLOCK_CORNER_HZ,rate);
        dc_on=on; dc_logged_rate=rate; dc_reported=g_get_monotonic_time();
      }
      if(on) {
        const gint64 now=g_get_monotonic_time();
        if(now-dc_reported>=5000000) {
          log_debug_area(LOG_RX, "dc_block: adc %ld: offset now I %+.6f Q %+.6f of full scale\n",
                    (long)channel,dcb.i,dcb.q);
          dc_reported=now;
        }
      }
    }
    // A whole HackRF transfer reaches the FIFO every ~65 ms, but DSP and the
    // analyzer need the smaller pieces at their sample-clock cadence.  Without
    // this, all pieces are processed back-to-back and the waterfall advances
    // only once per USB transfer even though no audio samples are missing.
    // Never sleep to catch up after a genuine stall/reconfigure: reset the
    // clock when it is more than two pieces behind.
    if(rx_read_block[channel]>block && soapy_rx_actual_rate>0) {
      const gint64 now=g_get_monotonic_time();
      const gint64 span_us=((gint64)elements*1000000LL)/soapy_rx_actual_rate;
      if(next_block_us==0 || now>next_block_us+2*span_us) next_block_us=now;
      if(now<next_block_us) g_usleep((gulong)(next_block_us-now));
      next_block_us+=span_us;
    }
    {
      const gint64 now=g_get_monotonic_time();
      if(now-dropped_reported>=5000000) {
        long long d;
        g_mutex_lock(&fifo_mutex[channel]);
        d=fifo_dropped[channel]; fifo_dropped[channel]=0;
        g_mutex_unlock(&fifo_mutex[channel]);
        if(d>0) {
          log_error("%s: the DSP fell behind the stream by %lld samples in the last 5 s on adc %ld\n",
                    __FUNCTION__,d,(long)channel);
        }
        dropped_reported=now;
      }
    }
    g_mutex_lock(&radio->delete_rx_mutex);
    // The GTK control can change this while this worker is running.  Take one
    // atomic snapshot per block, both to make a live toggle reliable and to
    // keep the sample loops free of millions of atomic loads per second.
    const gboolean iqswap=radio_iqswap_get(radio);
    // Every receiver listening to this ADC gets the same raw block; each brings
    // its own centre to DC and decimates to its own span (slot_feed).  The slot
    // table is only ever changed under this lock, which is why it can be walked
    // here without a second one.
    for(int i=0;i<MAX_ADC_RECEIVERS;i++) {
      RECEIVER *rx=adc_slot[channel][i].rx;
      if(rx==NULL) continue;
      // A receiver whose mixer and decimator are not built yet (it is being
      // added) or have just been freed (its span is being changed) must not be
      // fed: with neither, the block would go into a channel opened for a
      // different rate, at a centre that is not its own.  The window is small
      // and real -- the slot is claimed by the first frequency push, which
      // happens inside create_receiver, before the chain exists.
      if(!adc_slot[channel][i].ready) continue;
      if(!receiver_is_live(rx)) continue;
      slot_feed(&adc_slot[channel][i],buffer,elements,iqswap);
    }
    g_mutex_unlock(&radio->delete_rx_mutex);
  }
log_info("%s: exit (adc %ld)\n",__FUNCTION__,(long)channel);
  g_free(buffer);
  return NULL;
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
  // Capture this receiver's hardware read size once; it need not be the smaller
  // block the DSP thread drains from the FIFO.
  const int block=rx_read_block[rx->adc]>0?rx_read_block[rx->adc]:2048;
  float *buffer=g_new(float,block*2);
  void *buffs[]={buffer};
  int overruns=0;
  gint64 overrun_reported=g_get_monotonic_time();
  /* Stream-health accounting.  The overrun check below only catches a stream
     arriving FASTER than it is consumed; a stream arriving SHORT is invisible,
     because a driver owes no error for samples its device dropped while the
     host was not asking.  On the libiio network backend nothing is even
     capable of reporting it -- SoapyPlutoSDR's readStreamStatus() is
     SOAPY_SDR_NOT_SUPPORTED -- so a networked Pluto over a jittery link
     delivered 94.6 % of a 768 kHz stream with 0 overruns, 0 timeouts and 0
     errors (tools/soapy_bench.c).  What the operator gets is a stuttering
     waterfall and chopped audio, and the application had nothing to say about
     it.  So: count what arrives against what the device says it is running at,
     and name the shortfall.  Missing samples are not a silence the DSP can see
     -- the samples either side are spliced. */
  long long got=0;
  double worst_gap=0.0;
  gint64 health_t0=g_get_monotonic_time();
  gint64 last_read=health_t0;

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
      /* Not a shortfall: the stream is deactivated for the transmission.
         Start the window again when it comes back, or the pause is reported
         as loss. */
      got=0; worst_gap=0.0;
      health_t0=g_get_monotonic_time();
      last_read=health_t0;
      continue;
    }
    g_atomic_int_set(&rx_parked[channel],0);
    // flags is an OUTPUT of readStream and drivers OR into it, so it has to be
    // cleared before every call.  It was initialised once outside this loop, so
    // the first SOAPY_SDR_END_ABRUPT stuck for the life of the thread and every
    // later read counted as an overrun: the "N overrun(s) in 5 s" line then
    // reported the read RATE rather than the loss, for ever, over a stream that
    // may have recovered seconds ago.
    flags=0;
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
    {
      const gint64 now=g_get_monotonic_time();
      const double gap=(double)(now-last_read)/1000.0;    /* ms */
      last_read=now;
      if(gap>worst_gap) worst_gap=gap;
      if(elements>0) got+=elements;
      if(now-health_t0>=5000000) {
        const double secs=(double)(now-health_t0)/1.0e6;
        const double expect=(double)soapy_rx_actual_rate*secs;
        /* 1 % covers the block quantisation at the window edges; a link that is
           merely a little late does not trip this, and a link that is dropping
           does so by tens of milliseconds per second. */
        if(expect>0.0 && (double)got<0.99*expect) {
          log_error("%s: adc %ld received %.1f%% of the stream in the last %.0f s "
                    "(%.0f ms of signal lost per second, longest gap between reads %.0f ms) -- "
                    "this is loss, not latency: the samples either side of it are spliced\n",
                    __FUNCTION__,(long)channel,100.0*(double)got/expect,secs,
                    1000.0*(expect-(double)got)/(double)soapy_rx_actual_rate/secs,worst_gap);
        }
        got=0; worst_gap=0.0; health_t0=now;
      }
    }
    if(elements<0) continue;
    if(elements>0) reconnect_note_data();   // fed the disconnect watchdog
    // Read and hand over, nothing else: everything that costs time now happens
    // on dsp_thread, so a slow DSP frame cannot turn into a driver overrun.
    fifo_push(channel,buffer,elements);
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

  // Out of the block distribution first, under the lock the DSP thread reads the
  // table inside -- after this returns, nothing is feeding this receiver.
  g_mutex_lock(&radio->delete_rx_mutex);
  const gboolean was_owner=(adc_slot[channel][0].rx==rx);
  const int left=slot_remove(rx);
  g_mutex_unlock(&radio->delete_rx_mutex);
  if(left>0) {
    // Somebody is still listening to this ADC: the stream, the FIFO and both
    // threads stay up.  If what just went was the receiver the device was tuned
    // to, the promoted slot takes the LO over -- it is already listening at its
    // own frequency through its NCO, so this only moves the LO onto it and
    // zeroes that offset.  The hardware RATE is deliberately left where it is:
    // it is a rate the surviving receiver's decimator was built for.
    if(was_owner) {
      log_info("%s: receiver %d owned adc %ld; receiver %d takes it over\n",
               __FUNCTION__,rx->channel,(long)channel,adc_slot[channel][0].rx->channel);
      soapy_protocol_set_rx_frequency(adc_slot[channel][0].rx);
    }
    return;
  }

  if(receive_thread_id[channel]==NULL) return;

log_info("%s: stopping receive thread for channel %ld\n",__FUNCTION__,(long)channel);
  rx_thread_running[channel]=FALSE;
  g_thread_join(receive_thread_id[channel]);
  receive_thread_id[channel]=NULL;

  // The reader is gone, so nothing is pushing any more: stop the DSP thread
  // BEFORE the stream is closed and before the caller frees the receiver -- it
  // touches rx->buffer, the resampler and the WDSP channel on every block.
  stop_dsp_thread(channel);
  fifo_free(channel);

  if(soapy_device!=NULL && rx_stream[channel]!=NULL) {
    SoapySDRDevice_deactivateStream(soapy_device,rx_stream[channel],0,0LL);
    SoapySDRDevice_closeStream(soapy_device,rx_stream[channel]);
    rx_stream[channel]=NULL;
  }
  adc_lo[channel]=0.0;

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
    // Peak of what WDSP handed us, before the device sees it.  Keep this ahead
    // of the clamp so the transmission summary still exposes an overshoot.
    if(isfinite(isample) && fabsf(isample)>tx_peak) tx_peak=fabsf(isample);
    if(isfinite(qsample) && fabsf(qsample)>tx_peak) tx_peak=fabsf(qsample);
    // Backoff first, clamp second: the clamp is the driver's missing saturation
    // and has to see the value that will actually be converted.  tx_peak above
    // is deliberately the level WDSP produced, so the summary still reports the
    // modulator's own headroom rather than the backed-off copy of it.
    if(tx_backoff_scale!=1.0f) { isample*=tx_backoff_scale; qsample*=tx_backoff_scale; }
    // The driver's conversion has no saturation.  Both full-scale endpoints
    // are safe, but an overshoot is not.  A non-finite sample is never
    // meaningful RF and must not be handed to a float-to-integer cast either.
    const float hi=1.0f;
    if(!isfinite(isample)) { isample=0.0f; tx_clipped++; }
    else if(isample>hi)    { isample=hi;   tx_clipped++; }
    else if(isample<-1.0f) { isample=-1.0f; tx_clipped++; }
    if(!isfinite(qsample)) { qsample=0.0f; tx_clipped++; }
    else if(qsample>hi)    { qsample=hi;   tx_clipped++; }
    else if(qsample<-1.0f) { qsample=-1.0f; tx_clipped++; }
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
          tx_written+=elements;
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
  // Which of the two branches in rxtx() ran is not otherwise visible in a log,
  // and it decides whether the link/CPU is shared with RX for the whole over.
  log_debug_area(LOG_RX, "%s: RX stream(s) down for the transmission\n",__FUNCTION__);
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
  // The queue belongs to the stream that was just closed: what is in it is from
  // before the transmission, and playing it now would put a stale half-second of
  // band on the air-side of the DSP.
  fifo_clear(channel);
}

void soapy_protocol_rx_resume(void) {
  if(soapy_device==NULL) return;
  if(rx_stream_active) return;
  log_debug_area(LOG_RX, "%s: rebuilding the RX stream(s)\n",__FUNCTION__);
  for(size_t ch=0;ch<MAX_CHANNELS;ch++) {
    if(rx_stream[ch]!=NULL) rx_resume_channel(ch);
  }
  rx_stream_active=TRUE;   // last, so no thread reads a stream mid-rebuild
}

// ---- TX pump + stream activation ------------------------------------------

static gpointer tx_thread(gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  gboolean status_supported=TRUE;
  const gboolean status_enabled=(g_getenv("MACHPSDR_SOAPY_TX_STATUS")!=NULL);
  int block_tick=0;
  long long last_dropped=0,last_underflows=0;
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

    // Everything below runs ONCE PER OUTPUT BLOCK, not once per sample.  The
    // first version of this asked the driver for stream status 190 times a
    // second and read the clock 48000 times a second, both from inside the loop
    // that has to deliver the TX waveform in real time -- on a NETWORK-attached
    // Pluto a status call is a round trip, and the pump owning the GTK thread's
    // join on unkey means a pump that falls behind presents as "PTT stopped
    // working and the audio breaks up".  Reported as a regression by the
    // operator; the diagnostic is not worth a millisecond of the TX path.
    if(++block_tick < 1024) continue;
    block_tick=0;

    // The status poll stays OFF unless asked for: its cost on any given driver
    // is unknown, and this loop is the wrong place to find that out by trying.
    // MACHPSDR_SOAPY_TX_STATUS=1 turns it on when a starved transmitter is
    // actually being chased.  The dropped-sample count below is free -- it is
    // incremented only where a write has already failed.
    if(status_enabled && status_supported) {
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
      // Deltas, not a drain: the counters belong to the whole transmission and
      // are read again by deactivate_tx's summary.  Zeroing them here meant a
      // long over reported its holes and then forgot them.
      const long long dropped=tx_dropped-last_dropped;
      const long long under=tx_underflows-last_underflows;
      if(dropped>0 || under>0) {
        log_error("%s: TX starved in the last %.0f s -- %lld sample(s) the device would not take, "
                  "%lld underflow(s).  The transmitted waveform has holes in it.\n",
                  __FUNCTION__,(double)(now-reported)/1.0e6,dropped,under);
      }
      last_dropped=tx_dropped;
      last_underflows=tx_underflows;
      reported=now;
    }
  }
log_info("soapy tx_thread: exit\n");
  return NULL;
}

void soapy_protocol_activate_tx(TRANSMITTER *tx) {
  if(soapy_device==NULL || tx_stream==NULL || changing_device_rate) return;
  output_buffer_index=0;
  if(!tx_stream_active) {
    // Program the requested attenuation BEFORE activateStream turns on Pluto's
    // TX LO.  The old order briefly keyed it at the driver's/default gain and
    // only then moved it to Drive.
    double drive=tx->drive;
    if(drive<0.0) drive=0.0;
    if(drive>100.0) drive=100.0;
    double gain=tx_gain_min+((tx_gain_max-tx_gain_min)*(drive/100.0));
    radio->dac[0].gain=gain;
    int grc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,radio->dac[0].id,gain);
    if(grc!=0) {
      log_error("%s: refusing to key because TX gain %.1f dB could not be set: %s\n",
                __FUNCTION__,gain,SoapySDR_errToStr(grc));
      soapy_tx_hold_safe();
      return;
    }
    int rc=SoapySDRDevice_activateStream(soapy_device,tx_stream,0,0LL,0);
    if(rc!=0) {
      log_info("soapy_protocol_activate_tx: activateStream failed: %s\n",SoapySDR_errToStr(rc));
      soapy_tx_hold_safe();
      return;
    }
    tx_stream_active=TRUE;
  }
  // ...and the digital level matches the backoff setting, which the operator
  // may have changed with the transmitter down.
  soapy_protocol_set_tx_backoff(tx->dac_backoff_db);
  tx_written=0;
  tx_peak=0.0f;
  tx_dropped=0;
  tx_underflows=0;
  tx_clipped=0;
  tx_key_us=g_get_monotonic_time();
  // What the DAC is actually clocked at, asked for at key-up rather than only at
  // create_transmitter.  The two are not the same question: several drivers tie
  // the RX and TX sample rates to one clock chain (the AD9361 in a Pluto does),
  // so whichever direction was set LAST owns the hardware -- and this file
  // re-asserts the RX rate at stream activation, which happens AFTER the TX rate
  // was set and read back.  A disagreement here means WDSP is producing samples
  // at one rate and the DAC is consuming them at another: the transmitted tone
  // lands at the wrong offset by exactly that ratio and the waveform has holes
  // in it for the difference.  One round trip per transmission, before any
  // sample is due.
  {
    double got=SoapySDRDevice_getSampleRate(soapy_device,SOAPY_SDR_TX,tx->dac);
    double gain=SoapySDRDevice_getGain(soapy_device,SOAPY_SDR_TX,radio->dac[0].id);
    log_info("%s: DAC rate %.0f Hz (WDSP is producing %d), TX gain %.1f dB of %.1f..%.1f, drive %.0f%%, "
             "DAC backoff %.1f dB\n",
             __FUNCTION__,got,soapy_tx_sample_rate,gain,tx_gain_min,tx_gain_max,tx->drive,tx_backoff_db);
    long long diff=(long long)fabs(got-(double)soapy_tx_sample_rate);
    if(got>0.0 && diff*10000LL>=(long long)soapy_tx_sample_rate) {
      log_error("%s: the DAC is clocked at %.0f Hz while WDSP produces %d -- the transmitted "
                "signal is off by x%.4f and starved for the difference\n",
                __FUNCTION__,got,soapy_tx_sample_rate,got/(double)soapy_tx_sample_rate);
    }
  }
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
    // Attenuate first, then turn the LO off.  Even a driver whose deactivate is
    // only a buffer flush is left in the safe state.
    soapy_tx_hold_safe();
    SoapySDRDevice_deactivateStream(soapy_device,tx_stream,0,0LL);
    tx_stream_active=FALSE;
  }
  output_buffer_index=0;

  // One line per transmission, after the pump is joined and the stream is down,
  // so the counters can no longer move.  Read it as: samples/s should be the
  // DAC rate logged at key-up (lower = starved, and the difference is where the
  // holes in the waveform are); peak is what WDSP produced, so 0.000 means
  // nothing was modulated at all and the radio is not the place to look; the
  // gain is the one at key-up.  Costs nothing -- it runs once, on unkey.
  {
    const gint64 us=g_get_monotonic_time()-tx_key_us;
    const double secs=(double)us/1.0e6;
    log_info("%s: transmitted %.2f s, %lld sample(s) to the DAC (%.0f/s), %lld refused, "
             "%lld clipped, peak %.3f (%.3f at the DAC, backoff %.1f dB)\n",
             __FUNCTION__,secs,tx_written,secs>0.0?(double)tx_written/secs:0.0,
             tx_dropped,tx_clipped,(double)tx_peak,(double)tx_peak*tx_backoff_scale,tx_backoff_db);
    if(tx_written==0) {
      log_error("%s: nothing was written to the DAC during that transmission -- the TX exchange "
                "was never clocked (no microphone and no pump, or WDSP produced no output)\n",__FUNCTION__);
    } else if(tx_peak<=0.0f) {
      log_error("%s: the DAC was fed %lld sample(s) of pure silence -- the modulator produced "
                "nothing (check mic gain/source, or drive if this was Tune)\n",__FUNCTION__,tx_written);
    }
  }
}

// Digital backoff: how far below what WDSP produced the samples reach the DAC.
// This is NOT the drive slider, which moves the device's analogue gain and
// leaves the samples alone -- both exist because they fail differently.  Too
// much analogue gain is a nonlinear PA; too little digital backoff is a DAC and
// the filters behind it clipping, which no amount of turning the drive down can
// undo because the damage is already in the samples.
void soapy_protocol_set_tx_backoff(double db) {
  if(!(db<=0.0)) db=0.0;                                  /* NaN-safe */
  if(db<TX_DAC_BACKOFF_MIN_DB) db=TX_DAC_BACKOFF_MIN_DB;
  tx_backoff_db=db;
  tx_backoff_scale=(float)pow(10.0,db/20.0);
}

// Map the 0..100 drive slider onto the hardware TX gain range.
void soapy_protocol_set_tx_drive(double drive) {
  if(soapy_device==NULL) return;
  if(drive<0.0) drive=0.0;
  if(drive>100.0) drive=100.0;
  double g=tx_gain_min+((tx_gain_max-tx_gain_min)*(drive/100.0));
  radio->dac[0].gain=g;
  // Moving Drive while receiving only changes the cached key-up value.  Keep
  // the physical device at maximum attenuation until activate_tx.
  double applied=tx_stream_active?g:tx_gain_min;
  int rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,radio->dac[0].id,applied);
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

// Join producers before closing streams or changing any receiver buffers. Keep
// the ADC slot table intact: an owner may not be receiver[0], and promoting a
// follower here would move the shared LO for no reason.
static void soapy_rate_stop_streams(void) {
  for(int ch=0;ch<MAX_CHANNELS;ch++) rx_thread_running[ch]=FALSE;
  for(int ch=0;ch<MAX_CHANNELS;ch++) {
    if(receive_thread_id[ch]!=NULL) {
      g_thread_join(receive_thread_id[ch]);
      receive_thread_id[ch]=NULL;
    }
    stop_dsp_thread(ch);
    fifo_free(ch);
    if(rx_stream[ch]!=NULL) {
      SoapySDRDevice_deactivateStream(soapy_device,rx_stream[ch],0,0LL);
      SoapySDRDevice_closeStream(soapy_device,rx_stream[ch]);
      rx_stream[ch]=NULL;
    }
  }
  rx_stream_active=FALSE;
  running=FALSE;
  if(tx_stream!=NULL) {
    SoapySDRDevice_deactivateStream(soapy_device,tx_stream,0,0LL);
    SoapySDRDevice_closeStream(soapy_device,tx_stream);
    tx_stream=NULL;
  }
  tx_stream_active=FALSE;
  output_buffer_index=0;
}

static gboolean soapy_rate_matches(int direction,int channel,int requested) {
  const double actual=SoapySDRDevice_getSampleRate(soapy_device,direction,channel);
  return isfinite(actual) && actual>0.0 &&
         fabs(actual-requested)<=(double)requested*1.0e-4;
}

static gboolean soapy_rate_start_streams(void) {
  TRANSMITTER *tx=radio->transmitter;
  // Both directions share Pluto's clock. Configure TX first, as at startup,
  // then RX, and verify BOTH readbacks after the last rate write.
  if(radio->can_transmit && tx!=NULL) {
    transmitter_change_soapy_rate(tx);
    if(!soapy_create_transmitter(tx,FALSE)) return FALSE;
    soapy_protocol_set_tx_antenna(tx,radio->dac[tx->dac].antenna);
    soapy_protocol_set_tx_frequency(tx);
  }
  for(int ch=0;ch<MAX_CHANNELS;ch++) {
    RECEIVER *owner=adc_slot[ch][0].rx;
    if(owner!=NULL && !soapy_create_receiver(owner)) return FALSE;
  }
  for(int ch=0;ch<MAX_CHANNELS;ch++) {
    RECEIVER *owner=adc_slot[ch][0].rx;
    if(owner==NULL) continue;
    if(!soapy_rate_matches(SOAPY_SDR_RX,ch,radio->sample_rate)) return FALSE;
    soapy_protocol_set_rx_antenna(owner,radio->adc[ch].antenna);
    soapy_protocol_set_automatic_gain(owner,radio->adc[ch].agc);
    soapy_protocol_set_gain(&radio->adc[ch]);
    soapy_protocol_set_rx_frequency(owner);
    soapy_refresh_offsets(ch);
    // Partial blocks contain samples from the old stream. Even an unchanged
    // span must start with a new block after its input clock changes.
    for(int i=0;i<MAX_ADC_RECEIVERS;i++) {
      RECEIVER *rx=adc_slot[ch][i].rx;
      if(rx!=NULL) rx->samples=0;
    }
  }
  if(radio->can_transmit && tx!=NULL &&
     !soapy_rate_matches(SOAPY_SDR_TX,tx->dac,tx->iq_output_rate)) return FALSE;
  for(int ch=0;ch<MAX_CHANNELS;ch++) {
    RECEIVER *owner=adc_slot[ch][0].rx;
    if(owner!=NULL && !soapy_start_receiver(owner)) return FALSE;
  }
  return TRUE;
}

gboolean soapy_protocol_set_device_rate(int choice,GError **error) {
  const GQuark domain=g_quark_from_static_string("soapy-device-rate");
  if(radio==NULL || radio->discovered==NULL || soapy_device==NULL ||
     radio->discovered->device!=DEVICE_SOAPYSDR || soapy_span_driven() ||
     (choice!=0 && !soapy_adc_rate_valid(radio->discovered,choice))) {
    g_set_error_literal(error,domain,1,"This device rate is not available.");
    return FALSE;
  }
  const int rate=choice>0?choice:radio->soapy_adc_rate_default;
  if(rate==radio->sample_rate) {
    radio->soapy_adc_rate=choice;
    return TRUE;
  }
  if(changing_device_rate || isTransmitting(radio) || tx_stream_active || tx_thread_id!=NULL) {
    g_set_error_literal(error,domain,2,"Stop transmitting before changing the device rate.");
    return FALSE;
  }
  const int max_span=soapy_rx_span_max(rate);
  if(max_span==0) {
    g_set_error_literal(error,domain,1,"The device rate is below the supported receiver spans.");
    return FALSE;
  }
  const int old_rate=radio->sample_rate;
  struct {
    int span,band;
    long long frequency,ctun_frequency,ctun_min,ctun_max;
  } saved[MAX_RECEIVERS]={0};
  for(int i=0;i<MAX_RECEIVERS;i++) {
    RECEIVER *rx=radio->receiver[i];
    if(rx==NULL) continue;
    saved[i].span=rx->sample_rate;
    saved[i].band=rx->band_a;
    saved[i].frequency=rx->frequency_a;
    saved[i].ctun_frequency=rx->ctun_frequency;
    saved[i].ctun_min=rx->ctun_min;
    saved[i].ctun_max=rx->ctun_max;
  }
  changing_device_rate=TRUE;
  const gboolean microphone=radio->local_microphone;
  if(microphone) audio_close_input(radio);
  soapy_tx_hold_safe();
  soapy_rate_stop_streams();
  radio->sample_rate=rate;
  for(int i=0;i<MAX_RECEIVERS;i++) {
    RECEIVER *rx=radio->receiver[i];
    if(rx!=NULL && rx->sample_rate>max_span) receiver_change_sample_rate(rx,max_span);
  }
  gboolean ok=soapy_rate_start_streams();
  if(ok) {
    radio->soapy_adc_rate=choice;
    // Reuse normal tuning's shared-window clamp for followers which no longer
    // fit after a rate decrease, including their CTUN cursor and VFO display.
    for(int ch=0;ch<MAX_CHANNELS;ch++) {
      RECEIVER *owner=adc_slot[ch][0].rx;
      if(owner!=NULL) frequency_changed(owner);
    }
    log_info("%s: device rate %d -> %d applied without restarting the application\n",
             __FUNCTION__,old_rate,rate);
  } else {
    log_error("%s: could not apply %d; restoring %d\n",__FUNCTION__,rate,old_rate);
    soapy_rate_stop_streams();
    radio->sample_rate=old_rate;
    for(int i=0;i<MAX_RECEIVERS;i++) {
      RECEIVER *rx=radio->receiver[i];
      if(rx==NULL) continue;
      if(rx->sample_rate!=saved[i].span) receiver_change_sample_rate(rx,saved[i].span);
      rx->band_a=saved[i].band;
      rx->frequency_a=saved[i].frequency;
      rx->ctun_frequency=saved[i].ctun_frequency;
      rx->ctun_min=saved[i].ctun_min;
      rx->ctun_max=saved[i].ctun_max;
    }
    const gboolean restored=soapy_rate_start_streams();
    if(!restored) soapy_rate_stop_streams();
    else {
      for(int i=0;i<MAX_RECEIVERS;i++) {
        if(radio->receiver[i]!=NULL) frequency_changed(radio->receiver[i]);
      }
    }
    g_set_error_literal(error,domain,3,restored?
        "The device could not apply that rate. The previous rate has been restored.":
        "The device could not apply that rate or restore reception. Reconnect the device.");
  }
  changing_device_rate=FALSE;
  if(microphone && audio_open_input(radio)!=0) radio->local_microphone=FALSE;
  return ok;
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
  // ...and its DSP thread with it, or the reconnect below starts a second one
  // over a receiver the first is still processing into.
  if(channel<MAX_CHANNELS) {
    stop_dsp_thread(channel);
    fifo_free(channel);
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
  soapy_tx_hold_safe();

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

  if(rx==NULL) return;
  size_t ch=(size_t)rx->adc;
  if(ch>=MAX_CHANNELS) return;
  // The slot is claimed even when there is no device yet (the reconnect path
  // re-makes one under a live receiver): whoever gets slot 0 owns the LO, and
  // that decision must not depend on whether the hardware happens to be there.
  RXSLOT *sl=slot_add(rx);
  if(sl==NULL) return;

  const double f=soapy_rx_target_freq(rx);

  if(sl!=&adc_slot[ch][0]) {
    // A follower never moves the LO -- that would retune the receiver that owns
    // this ADC out from under its operator.  It moves its own NCO instead, and
    // the caller has already made sure the frequency fits in the window
    // (soapy_protocol_rx_window_error).
    slot_set_offset(sl,(long long)(f-adc_lo[ch]));
    return;
  }

  sl->offset=0;                 // the device is tuned to this receiver
  adc_lo[ch]=f;                 // ...or will be, the moment there is one
  if(soapy_device==NULL) return;

  //g_print("%s: %f\n",__FUNCTION__,f);
  rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_RX,rx->adc,f,NULL);
  if(rc!=0) {
    log_info("%s: SoapySDRDevice_setFrequency(RX) failed: %s\n",__FUNCTION__,SoapySDR_errToStr(rc));
  }
  if(adc_slot[ch][1].rx!=NULL) {
    // Every follower is measured from where the device REALLY is, so take the
    // readback rather than the request -- a driver is free to land on its own
    // tuning step.  Only paid for when there is a follower to pay it for: this
    // runs once per tuning step, and on a networked device it is a round trip.
    const double at=SoapySDRDevice_getFrequency(soapy_device,SOAPY_SDR_RX,rx->adc);
    if(at>0.0) adc_lo[ch]=at;
    soapy_refresh_offsets(ch);
  }
}

void soapy_protocol_set_tx_frequency(TRANSMITTER *tx) {
  int rc;
  double f;

  if(soapy_device!=NULL) {
    RECEIVER* rx=tx->rx;
    if(rx!=NULL) {
      // Split/ctun/freetune + converter LO + error + ppm + XIT: one sum, one
      // implementation (transmitter_get_frequency).  The copy that used to live
      // here asked rx->ctun and never rx->freetune, so with freetune on, the
      // transmitter stayed on the span centre wherever the operator tuned.
      f=(double)transmitter_get_frequency(tx);
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
  // Configuration is restored while the radio is still connecting.  Preserve
  // the requested value in dac->gain, but do not apply it to unkeyed hardware.
  double applied=tx_stream_active?dac->gain:tx_gain_min;
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,dac->id,applied);
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
