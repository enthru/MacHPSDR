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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>

#include <soundio/soundio.h>
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
// macOS 12 renamed kAudioObjectPropertyElementMaster -> …ElementMain (same
// value); fall back on older SDKs that only have the deprecated spelling.
#ifndef kAudioObjectPropertyElementMain
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif
#endif
#ifndef __APPLE__
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <pulse/simple.h>
#include <alsa/asoundlib.h>
#endif

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "adc.h"
#include "dac.h"
#include "discovered.h"
#include "wideband.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "audio.h"

int n_input_devices;
AUDIO_DEVICE input_devices[MAX_AUDIO_DEVICES];
int n_output_devices;
AUDIO_DEVICE output_devices[MAX_AUDIO_DEVICES];



static int running=FALSE;

#ifndef __APPLE__
static snd_pcm_format_t record_audio_format;

#define FORMATS 3
static snd_pcm_format_t formats[3]={
  SND_PCM_FORMAT_FLOAT_LE,
  SND_PCM_FORMAT_S32_LE,
  SND_PCM_FORMAT_S16_LE};
#endif

static GThread *mic_read_thread_id;
static void *mic_read_thread(void *arg);


struct SoundIo *soundio;

#ifndef __APPLE__
//static pa_buffer_attr bufattr;
static pa_glib_mainloop *main_loop;
static pa_mainloop_api *main_loop_api;
static pa_operation *op;
static pa_context *pa_ctx;
#endif

static int ready=0;
static int sample_rate=48000;

// Sentinel device name for the synthetic "System Default" output entry.  When a
// receiver's audio_name is this string, the output stream is (re)opened on
// whatever device macOS currently considers the default output, resolved fresh
// every time — so the user can pick it once and never have to change devices in
// the app when they connect e.g. Bluetooth headphones.
#define AUDIO_SYSTEM_DEFAULT_NAME "__system_default__"


static int underflow_count=0;

static void underflow_callback(struct SoundIoOutStream *outstream) {
  underflow_count++;
  //g_print("audio_write: underflow %d\n", underflow_count);
}

// Output device runs at a rate other than the 48 kHz DSP rate (e.g. a Bluetooth
// headset locked to 44.1 kHz): linear-resample the 48 kHz interleaved stereo
// audio held in the ring buffer to the device rate on the fly.  The ring buffer
// still stores 48 kHz frames; only whole input frames actually consumed are
// released, and the fractional read position is carried across callbacks in
// rx->audio_resample_phase so pitch stays exact and continuous.
static void write_callback_resample(struct SoundIoOutStream *outstream,
                                    int frame_count_min, int frame_count_max) {
  RECEIVER *rx=(RECEIVER *)outstream->userdata;
  struct SoundIoChannelArea *areas;
  int err;
  const int chans=outstream->layout.channel_count;
  const double ratio=(double)sample_rate/(double)outstream->sample_rate; // input frames per output frame

  char *read_ptr=soundio_ring_buffer_read_ptr(rx->ring_buffer);
  int in_avail=soundio_ring_buffer_fill_count(rx->ring_buffer)/(int)(sizeof(float)*2);
  const float (*in)[2]=(const float (*)[2])read_ptr;

  double phase=rx->audio_resample_phase;

  // How many output frames can we produce?  Output frame j reads input frames
  // floor(phase+j*ratio) and +1, so we need phase+j*ratio <= in_avail-1.
  int producible=0;
  if(in_avail>=2 && phase<(double)(in_avail-1))
    producible=(int)(((double)(in_avail-1)-phase)/ratio)+1;

  // Not enough data to satisfy the callback minimum: emit silence and wait
  // (the producer keeps filling the ring); do not consume any input.
  if(producible<frame_count_min) {
    int frames_left=frame_count_min;
    while(frames_left>0) {
      int fc=frames_left;
      if((err=soundio_outstream_begin_write(outstream,&areas,&fc)) || fc<=0) return;
      for(int f=0;f<fc;f++)
        for(int ch=0;ch<chans;ch++) {
          memset(areas[ch].ptr,0,outstream->bytes_per_sample);
          areas[ch].ptr+=areas[ch].step;
        }
      if((err=soundio_outstream_end_write(outstream))) return;
      frames_left-=fc;
    }
    return;
  }

  int want=producible;
  if(want>frame_count_max) want=frame_count_max;

  int produced=0;
  double pos=phase;
  while(produced<want) {
    int fc=want-produced;
    if((err=soundio_outstream_begin_write(outstream,&areas,&fc))) {
      log_info("write_callback_resample: begin write error: %s\n",soundio_strerror(err));
      return;
    }
    if(fc<=0) break;
    for(int f=0;f<fc;f++) {
      int i=(int)pos;
      int i1=(i+1<in_avail)?i+1:i;
      double frac=pos-(double)i;
      for(int ch=0;ch<chans;ch++) {
        float s0=in[i][ch&1];
        float s1=in[i1][ch&1];
        float v=(float)(s0+(s1-s0)*frac);
        memcpy(areas[ch].ptr,&v,sizeof(float));
        areas[ch].ptr+=areas[ch].step;
      }
      pos+=ratio;
    }
    if((err=soundio_outstream_end_write(outstream))) return;
    produced+=fc;
  }

  // Release the whole input frames consumed; keep the fraction for next time.
  int consumed=(int)pos;
  if(consumed>in_avail) consumed=in_avail;
  soundio_ring_buffer_advance_read_ptr(rx->ring_buffer, consumed*(int)(sizeof(float)*2));
  rx->audio_resample_phase=pos-(double)consumed;
}

static void write_callback(struct SoundIoOutStream *outstream, int frame_count_min, int frame_count_max) {
  RECEIVER *rx=(RECEIVER *)outstream->userdata;
  struct SoundIoChannelArea *areas;
  int frames_left;
  int frame_count;
  int err;

  // Device not running at the 48 kHz DSP rate: hand off to the resampling path.
  if(outstream->sample_rate!=sample_rate) {
    write_callback_resample(outstream,frame_count_min,frame_count_max);
    return;
  }

  char *read_ptr = soundio_ring_buffer_read_ptr(rx->ring_buffer);
  int fill_bytes = soundio_ring_buffer_fill_count(rx->ring_buffer);
  int fill_count = fill_bytes / outstream->bytes_per_frame;

  if (frame_count_min > fill_count) {
    //g_print("write_callback: not enough data: frame_count_min=%d fill_count=%d bytes_per_frame=%d\n",frame_count_min,fill_count,outstream->bytes_per_frame);
    // Ring buffer does not have enough data, fill with zeroes.
    frames_left = frame_count_min;
    for (;;) {
      frame_count = frames_left;
      if (frame_count <= 0)
        return;
      if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
        //g_print("write_callback: begin write error: %s\n", soundio_strerror(err));
        return;
      }
      if (frame_count <= 0)
        return;
      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
          memset(areas[ch].ptr, 0, outstream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
        }
      }
      if ((err = soundio_outstream_end_write(outstream)))
        //g_print("write_callback: end write error: %s\n", soundio_strerror(err));
        frames_left -= frame_count;
      }
    }

    int read_count;
    if(frame_count_max<fill_count) read_count=frame_count_max; else read_count=fill_count;
    frames_left = read_count;

    while (frames_left > 0) {
      int frame_count = frames_left;

      if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
        log_info("begin write error: %s", soundio_strerror(err));
        return;
      }

      if (frame_count <= 0)
        break;

      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
          memcpy(areas[ch].ptr, read_ptr, outstream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
          read_ptr += outstream->bytes_per_sample;
        }
      }

      if ((err = soundio_outstream_end_write(outstream))) {
        //g_print("end write error: %s\n", soundio_strerror(err));
        return;
      }

      frames_left -= frame_count;
  }

  soundio_ring_buffer_advance_read_ptr(rx->ring_buffer, read_count * outstream->bytes_per_frame);
}

static void read_callback(struct SoundIoInStream *instream, int frame_count_min, int frame_count_max) {
    RADIO *r=(RADIO *)instream->userdata;
    struct SoundIoChannelArea *areas;
    int err;

    //if(frame_count_min!=frame_count_max) {
    if(frame_count_min<=0) {
      //g_print("read_callback: frame_counts differ min=%d max=%d\n",frame_count_min,frame_count_max);
      return;
    }

    if(r->local_microphone_buffer!=NULL) {
      if(r->local_microphone_buffer_size!=frame_count_min) {
        free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
      }
    }

    if(r->local_microphone_buffer==NULL) {
      r->local_microphone_buffer_size=frame_count_min;
      r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
log_info("read_callback: create microphone buffer: %p length=%d (%d bytes)\n",r->local_microphone_buffer,r->local_microphone_buffer_size,instream->bytes_per_sample*r->local_microphone_buffer_size);
    }

    int frame_count=frame_count_min;

    if((err = soundio_instream_begin_read(instream, &areas, &frame_count))) {
      log_info("read_callback: begin read error: %s\n", soundio_strerror(err));
      return;
    }

    g_mutex_lock(&r->ring_buffer_mutex);

    if(r->mic_resample_rate!=0) {
      // Capture device is not at 48 kHz (e.g. a 16 kHz Bluetooth headset mic):
      // linear-resample the captured mono audio up to 48 kHz before it enters
      // the ring buffer, so the rest of the mic path sees a plain 48 kHz stream.
      // ratio = input frames consumed per output frame (<1 when upsampling).
      double ratio=(double)r->mic_resample_rate/(double)sample_rate;
      float *out=(float *)soundio_ring_buffer_write_ptr(r->ring_buffer);
      int free_count=soundio_ring_buffer_free_count(r->ring_buffer)/(int)sizeof(float);
      char *base=(areas!=NULL)?areas[0].ptr:NULL;
      int step=(areas!=NULL)?areas[0].step:0;
      double pos=r->mic_resample_phase;
      float prev=r->mic_resample_prev;
      int produced=0;

      // Virtual input sequence c[0]=prev, c[k]=s[k-1] for k>=1 (s = this
      // callback's frames). Output sample at pos interpolates c[floor(pos)] and
      // c[floor(pos)+1]; valid while pos < frame_count.
      while(pos<(double)frame_count && produced<free_count) {
        int i=(int)pos;
        double frac=pos-(double)i;
        float a=(i==0)?prev:(base?*(float *)(base+(i-1)*step):0.0f);
        float b=base?*(float *)(base+i*step):0.0f;   // c[i+1] == s[i]
        out[produced++]=(float)(a+(b-a)*frac);
        pos+=ratio;
      }

      if(frame_count>0 && base)
        r->mic_resample_prev=*(float *)(base+(frame_count-1)*step);
      r->mic_resample_phase=pos-(double)frame_count;
      if(r->mic_resample_phase<0.0) r->mic_resample_phase=0.0;

      if((err = soundio_instream_end_read(instream)))
        log_info("read_callback: end read error: %s", soundio_strerror(err));

      if(produced>0) {
        soundio_ring_buffer_advance_write_ptr(r->ring_buffer, produced*(int)sizeof(float));
        g_cond_signal (&r->ring_buffer_cond);
      }
      g_mutex_unlock (&r->ring_buffer_mutex);
      return;
    }

    char *write_ptr = soundio_ring_buffer_write_ptr(r->ring_buffer);
    int free_bytes = soundio_ring_buffer_free_count(r->ring_buffer);
    int free_count = free_bytes / instream->bytes_per_frame;
    if(frame_count!=0 && free_count>=frame_count) {
      if(areas==NULL) {
        log_info("read_callback: areas is NULL\n");
        memset(write_ptr,0,frame_count*instream->bytes_per_sample);
      } else {
        memcpy(write_ptr,areas[0].ptr,frame_count*instream->bytes_per_sample);
      }

      if((err = soundio_instream_end_read(instream))) {
        log_info("read_callback: end read error: %s", soundio_strerror(err));
      }

      soundio_ring_buffer_advance_write_ptr(r->ring_buffer, frame_count*instream->bytes_per_frame);
      g_cond_signal (&r->ring_buffer_cond);
    } else {
      log_info("read_callback: frame_count is 0 or free_count too small\n");
    }
    g_mutex_unlock (&r->ring_buffer_mutex);

}

int audio_open_output(RECEIVER *rx) {
  int result=0;
  int err;
#ifndef __APPLE__
  pa_sample_spec sample_spec;
#endif
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
log_info("audio_open_output: SOUNDIO: %s\n",rx->audio_name);
      if(soundio==NULL) {
        log_info("audio_open_output: no soundio backend connected\n");
        return -1;
      }
      // Idempotent: drop any stream already open for this receiver so callers
      // (normal startup and the system-default monitor) can never race into
      // leaking a stream/device.
      if(rx->output_stream!=NULL) audio_close_output(rx);
      g_mutex_lock(&rx->local_audio_mutex);

      // find the device
      rx->output_index=-1;
      if(rx->audio_name!=NULL) {
        for(int i=0;i<n_output_devices;i++) {
          if(strcmp(rx->audio_name,output_devices[i].name)==0) {
            rx->output_index=output_devices[i].index;
            break;
          }
        }
      }

      // output_index==-1 here means either the user picked the synthetic
      // "System Default" entry (index -1 by design), or the configured device
      // was not found (stale "Dummy Output Device" name, or a device that has
      // since been unplugged).  In both cases follow the current system default
      // output.  Re-flush events first so a device connected after launch (e.g.
      // Bluetooth headphones macOS just made the default) is picked up.
      if(rx->output_index==-1) {
        soundio_flush_events(soundio);
        int def=soundio_default_output_device_index(soundio);
        if(def>=0) {
          log_info("audio_open_output: '%s' -> using current default output device\n",
                  rx->audio_name?rx->audio_name:"(none)");
          rx->output_index=def;
        }
      }

      if(rx->output_index==-1) {
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }
  
      rx->output_device = soundio_get_output_device(soundio, rx->output_index);
      if(!rx->output_device) {
        log_info("audio_open_output: could not get output device: out of memory");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      if(!soundio_device_supports_format(rx->output_device, SoundIoFormatFloat32NE)) {
        log_info("audio_open_output: device does not support SoundIoFormatFloat32NE");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      // Pick the stream rate: prefer the 48 kHz DSP rate, but if the device does
      // not accept it (e.g. a Bluetooth headset locked to 44.1 kHz) fall back to
      // the nearest rate the device supports.  When they differ, write_callback
      // resamples the 48 kHz audio to the device rate on the fly.
      int device_rate=sample_rate;
      if(!soundio_device_supports_sample_rate(rx->output_device, sample_rate)) {
        device_rate=soundio_device_nearest_sample_rate(rx->output_device, sample_rate);
        if(device_rate<=0) {
          log_info("audio_open_output: device has no usable sample rate\n");
          g_mutex_unlock(&rx->local_audio_mutex);
          return -1;
        }
        log_info("audio_open_output: device does not support %d Hz; opening at %d Hz with resampling\n",
                sample_rate, device_rate);
      }
      rx->audio_resample_phase=0.0;

      // Size the ring buffer for a fixed ~200 ms of output audio so there is
      // room for a start-up cushion at any DSP rate.  At high DSP rates
      // output_samples is small (e.g. 128 at 384 kHz), so the old "8 buffers"
      // guess collapsed to the 32 kB floor with almost no slack.
      int size=(int)(0.200 * (double)sample_rate) * (int)sizeof(float) * 2;
      if(size<32768) size=32768;
      rx->ring_buffer = soundio_ring_buffer_create(soundio, size);
      if(!rx->ring_buffer) {
        log_info("audio_open_output: soundio_ring_buffer_create failed");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      rx->output_stream = soundio_outstream_create(rx->output_device);
      if(!rx->output_stream) {
        log_info("audio_open_output: could not open output device: out of memory");
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }
      rx->output_stream->format = SoundIoFormatFloat32NE;
      rx->output_stream->sample_rate = device_rate;
      rx->output_stream->write_callback = write_callback;
      rx->output_stream->underflow_callback = underflow_callback;
      rx->output_stream->software_latency = 0.01;
      rx->output_stream->userdata=(void *)rx;

      if((err = soundio_outstream_open(rx->output_stream))) {
        log_info("audio_open_output: unable to open output stream: %s", soundio_strerror(err));
        g_mutex_unlock(&rx->local_audio_mutex);
        return -1;
      }

      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
log_info("audio_open_output: PULSEAUDIO: %s\n",rx->audio_name);

      if(rx->audio_name==NULL) {
        result=-1;
      } else {
        g_mutex_lock(&rx->local_audio_mutex);
        sample_spec.rate=48000;
        sample_spec.channels=2;
        sample_spec.format=PA_SAMPLE_FLOAT32NE;

        char stream_id[16];
        sprintf(stream_id,"RX-%d",rx->channel);
    
        rx->playstream=pa_simple_new(NULL,               // Use the default server.
                        "MacHPSDR",           // Our application's name.
                        PA_STREAM_PLAYBACK,
                        rx->audio_name,
                        stream_id,            // Description of our stream.
                        &sample_spec,                // Our sample format.
                        NULL,               // Use default channel map
                        NULL,               // Use default buffering attributes.
                        &err               // error code if returns NULL
                        );
    
        if(rx->playstream!=NULL) {
          rx->local_audio_buffer_offset=0;
          rx->local_audio_buffer=g_new0(float,2*rx->local_audio_buffer_size);
          log_info("audio_open_output: allocated local_audio_buffer %p size %ld bytes\n",rx->local_audio_buffer,2*rx->local_audio_buffer_size*sizeof(float));
        } else {
          result=-1;
          log_error("pa-simple_new failed: err=%d\n",err);
        }
        g_mutex_unlock(&rx->local_audio_mutex);
      }
      break;
    }
    case USE_ALSA: {
log_info("audio_open_output: ALSA: %s\n",rx->audio_name);
      int err;
      unsigned int rate = 48000;
      unsigned int channels=2;
      int soft_resample=1;
      unsigned int latency=125000;      
      
      if(rx->audio_name==NULL) {
        rx->local_audio=0;
        return -1;
      }

      int i;
      char hw[128];

      i=0;
      while(i<127 && rx->audio_name[i]!=' ') {
        hw[i]=rx->audio_name[i];
        i++;
      }
      hw[i]='\0';
      
    log_info("audio_open_output: hw=%s\n",hw);

      for(i=0;i<FORMATS;i++) {
        g_mutex_lock(&rx->local_audio_mutex);
        if ((err = snd_pcm_open (&rx->playback_handle, hw, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
          log_info("audio_open_output: cannot open audio device %s (%s)\n", 
                  hw,
                  snd_strerror (err));
          g_mutex_unlock(&rx->local_audio_mutex);
          return err;
        }
    log_info("audio_open_output: handle=%p\n",rx->playback_handle);

    log_info("audio_open_output: trying format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
        if ((err = snd_pcm_set_params (rx->playback_handle,formats[i],SND_PCM_ACCESS_RW_INTERLEAVED,channels,rate,soft_resample,latency)) < 0) {
          log_info("audio_open_output: snd_pcm_set_params failed: %s\n",snd_strerror(err));
          g_mutex_unlock(&rx->local_audio_mutex);
          audio_close_output(rx);
          continue;
        } else {
    log_info("audio_open_output: using format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
          rx->local_audio_format=formats[i];
          break;
        }
      }

      if(i>=FORMATS) {
        log_info("audio_open_output: cannot find usable format\n");
        return err;
      }

      rx->local_audio_buffer_offset=0;
      switch(rx->local_audio_format) {
        case SND_PCM_FORMAT_S16_LE:
    log_info("audio_open_output: local_audio_buffer: size=%d sample=%ld\n",rx->local_audio_buffer_size,sizeof(gint16));
          rx->local_audio_buffer=g_new(gint16,2*rx->local_audio_buffer_size);
            break;
        case SND_PCM_FORMAT_S32_LE:
    log_info("audio_open_output: local_audio_buffer: size=%d sample=%ld\n",rx->local_audio_buffer_size,sizeof(gint32));
          rx->local_audio_buffer=g_new(gint32,2*rx->local_audio_buffer_size);
          break;
        case SND_PCM_FORMAT_FLOAT_LE:
    log_info("audio_open_output: local_audio_buffer: size=%d sample=%ld\n",rx->local_audio_buffer_size,sizeof(gfloat));
          rx->local_audio_buffer=g_new(gfloat,2*rx->local_audio_buffer_size);
          break;

        default: return -1;
      }
      
      log_info("audio_open_output: rx=%d handle=%p buffer=%p size=%d\n",rx->channel,rx->playback_handle,rx->local_audio_buffer,rx->local_audio_buffer_size);      

      g_mutex_unlock(&rx->local_audio_mutex);          
      break;
    }
#endif
  }
  return result;
}
	
int audio_open_input(RADIO *r) {
  int result=0;
  int err;
#ifndef __APPLE__
  pa_sample_spec sample_spec;
#endif

  log_info("%s\n",__FUNCTION__);
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      if(soundio==NULL) {
        log_info("audio_open_input: no soundio backend connected\n");
        return -1;
      }
      if(r->microphone_name==NULL) {
        log_info("audio_open_input: microphone name is NULL\n");
        return -1;
      }
      // Idempotent: drop any mic stream already open (normal open path and the
      // system-default input monitor must not race into two read threads).
      if(r->input_stream!=NULL) audio_close_input(r);

      log_info("audio_open_input: %s\n",r->microphone_name);
      // find the device
      int input_index=-1;
      for(int i=0;i<n_input_devices;i++) {
        if(strcmp(r->microphone_name,input_devices[i].name)==0) {
          input_index=input_devices[i].index;
          break;
        }
      }

      // input_index==-1 means the user picked the synthetic "System Default"
      // entry (index -1 by design), or the configured mic was not found.  In
      // both cases follow the current system default input; re-flush events so
      // a device connected after launch is picked up.
      if(input_index==-1) {
        soundio_flush_events(soundio);
        int def=soundio_default_input_device_index(soundio);
        if(def>=0) {
          log_info("audio_open_input: '%s' -> using current default input device\n",r->microphone_name);
          input_index=def;
        }
      }

      if(input_index==-1) {
        log_info("audio_open_input: did not find %s\n",r->microphone_name);
        return -1;
      }


      r->input_device = soundio_get_input_device(soundio, input_index);
      if(!r->input_device) {
        log_info("audio_open_input: could not get input device: out of memory");
        return -1;
      }

      if(!soundio_device_supports_format(r->input_device, SoundIoFormatFloat32NE)) {
        log_info("audio_open_input: device does not support SoundIoFormatFloat32NE");
        return -1;
      }

      // As for output: if the capture device does not run at 48 kHz (e.g. a
      // Bluetooth headset mic at 16 kHz) open it at its nearest rate and let
      // read_callback resample the captured audio up to 48 kHz.
      int in_device_rate=sample_rate;
      if(!soundio_device_supports_sample_rate(r->input_device, sample_rate)) {
        in_device_rate=soundio_device_nearest_sample_rate(r->input_device, sample_rate);
        if(in_device_rate<=0) {
          log_info("audio_open_input: device has no usable sample rate\n");
          return -1;
        }
        log_info("audio_open_input: device does not support %d Hz; opening at %d Hz with resampling\n",
                sample_rate, in_device_rate);
      }
      r->mic_resample_rate=(in_device_rate==sample_rate)?0:in_device_rate;
      r->mic_resample_phase=0.0;
      r->mic_resample_prev=0.0f;

      r->input_stream = soundio_instream_create(r->input_device);
      if(!r->input_stream) {
        log_info("audio_open_input: could not open input device: out of memory");
        return -1;
      }
      r->input_stream->format = SoundIoFormatFloat32NE;
      r->input_stream->sample_rate = in_device_rate;
      r->input_stream->read_callback = read_callback;
      r->input_stream->userdata=(void *)r;

      if((err = soundio_instream_open(r->input_stream))) {
        log_info("audio_open_input: unable to open input stream: %s", soundio_strerror(err));
        return -1;
      }

      // guess that 8 input buffers should be enough
      int size=8*512*sizeof(float);
      r->ring_buffer = soundio_ring_buffer_create(soundio, size);
      if(!r->ring_buffer) {
        log_info("audio_open_input: soundio_ring_buffer_create failed");
        return -1;
      }

      if((err = soundio_instream_start(r->input_stream))) {
        log_info("unable to start input device: %s", soundio_strerror(err));
        return -1;
      }
      r->input_started=TRUE;
      running=TRUE;
      log_info("%s: SOUNDIO mic_read_thread\n",__FUNCTION__);
      mic_read_thread_id = g_thread_new( "mic_thread", mic_read_thread, r);
      if(!mic_read_thread_id ) {
        log_error("g_thread_new failed on mic_read_thread\n");
        soundio_instream_destroy(r->input_stream);
        soundio_device_unref(r->input_device);
        soundio_ring_buffer_destroy(r->ring_buffer);
        if(r->local_microphone_buffer!=NULL) {
          g_free(r->local_microphone_buffer);
          r->local_microphone_buffer=NULL;
        }
        running=FALSE;
        result=-1;
      }
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      if(r->microphone_name==NULL) {
        return -1;
      }

      g_mutex_lock(&r->local_microphone_mutex);
      
      
      pa_buffer_attr attr;
      attr.maxlength = (uint32_t) -1;
      attr.tlength = (uint32_t) -1;
      attr.prebuf = (uint32_t) -1;
      attr.minreq = (uint32_t) -1;
      attr.fragsize = 512;    
      
      
      sample_spec.rate=48000;
      sample_spec.channels=1;
      sample_spec.format=PA_SAMPLE_FLOAT32NE;

      r->microphone_stream=pa_simple_new(NULL,               // Use the default server.
                      "MacHPSDR",           // Our application's name.
                      PA_STREAM_RECORD,
                      r->microphone_name,
                      "TX",            // Description of our stream.
                      &sample_spec,                // Our sample format.
                      NULL,               // Use default channel map
                      //NULL,
                      &attr,               // Use default buffering attributes.
                      NULL               // Ignore error code.
                      );

      if(r->microphone_stream!=NULL) {
        r->local_microphone_buffer_offset=0;
        r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
        running=TRUE;
        log_info("%s: PULSEAUDIO mic_read_thread\n",__FUNCTION__);
        mic_read_thread_id = g_thread_new( "mic_thread", mic_read_thread, r);
        if(!mic_read_thread_id ) {
          log_error("g_thread_new failed on mic_read_thread\n");
          g_free(r->local_microphone_buffer);
          r->local_microphone_buffer=NULL;
          running=FALSE;
          result=-1;
        }
      } else {
        result=-1;
      }
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
    case USE_ALSA: {
      int err;
      unsigned int rate=48000;
      unsigned int channels=1;
      int soft_resample=1;
      unsigned int latency=125000;
      
      char hw[64];
      int i = 0;
      
      if(r->microphone_name==NULL) {
        r->local_microphone=0;
        return -1;
      }
        
      while(r->microphone_name[i]!=' ') {
        hw[i]=r->microphone_name[i];
        i++;
      }
      hw[i]='\0';      
      
      log_info("audio_open_input: hw=%s\n",hw);

      for(i=0;i<FORMATS;i++) {
        if ((err = snd_pcm_open (&r->record_handle, hw, SND_PCM_STREAM_CAPTURE, SND_PCM_ASYNC)) < 0) {
          log_info("audio_open_input: cannot open audio device %s (%s)\n",
                  hw,
                  snd_strerror (err));
          return err;
        }
log_info("audio_open_input: handle=%p\n",r->record_handle);

log_info("audio_open_input: trying format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
        if ((err = snd_pcm_set_params (r->record_handle,formats[i],SND_PCM_ACCESS_RW_INTERLEAVED,channels,rate,soft_resample,latency)) < 0) {
          log_info("audio_open_input: snd_pcm_set_params failed: %s\n",snd_strerror(err));
          audio_close_input(r);
          continue;
        } else {
log_info("audio_open_input: using format %s (%s)\n",snd_pcm_format_name(formats[i]),snd_pcm_format_description(formats[i]));
          record_audio_format=formats[i];
          break;
        }
      }

      if(i>=FORMATS) {
log_info("audio_open_input: cannot find usable format\n");
        return err;
      }

log_info("audio_open_input: format=%d\n",record_audio_format);

      switch(record_audio_format) {
        case SND_PCM_FORMAT_S16_LE:
log_info("audio_open_input: mic_buffer: size=%d channels=%d sample=%ld bytes\n",r->local_microphone_buffer_size,channels,sizeof(gint16));
          r->local_microphone_buffer = g_new(float, r->local_microphone_buffer_size);
          break;
        case SND_PCM_FORMAT_S32_LE:
log_info("audio_open_input: mic_buffer: size=%d channels=%d sample=%ld bytes\n",r->local_microphone_buffer_size,channels,sizeof(gint32));
          r->local_microphone_buffer = g_new(float, r->local_microphone_buffer_size);
          break;
        case SND_PCM_FORMAT_FLOAT_LE:
log_info("audio_open_input: mic_buffer: size=%d channels=%d sample=%ld bytes\n",r->local_microphone_buffer_size,channels,sizeof(gfloat));
          r->local_microphone_buffer=g_new(gfloat, r->local_microphone_buffer_size);
          break;
          
        default: return -1;          
      }

      //r->local_microphone_buffer_offset=0;
      //r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
      running=TRUE;

      log_info("%s: ALSA mic_read_thread\n",__FUNCTION__);
      GError *error;
      mic_read_thread_id = g_thread_try_new("microphone",mic_read_thread,r,&error);
      if(!mic_read_thread_id ) {
        log_info("g_thread_new failed on mic_read_thread: %s\n",error->message);
        g_free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
        running=FALSE;
        result=-1;        
      }
      break;
  }
#endif  
  }
  return result;
}

void audio_close_output(RECEIVER *rx) {
 log_info("audio_close_output\n");
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->output_stream!=NULL) {
        soundio_outstream_destroy(rx->output_stream);
        rx->output_stream=NULL;
      }
      if(rx->output_device!=NULL) {
        soundio_device_unref(rx->output_device);
        rx->output_device=NULL;
      } 
      if(rx->ring_buffer!=NULL) {
        soundio_ring_buffer_destroy(rx->ring_buffer);
        rx->ring_buffer=NULL;
      }
      rx->output_started=FALSE;
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->playstream!=NULL) {
        pa_simple_free(rx->playstream);
        rx->playstream=NULL;
      }
      if(rx->local_audio_buffer!=NULL) {
        g_free(rx->local_audio_buffer);
        rx->local_audio_buffer=NULL;
      }
      rx->output_started=FALSE;
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
    case USE_ALSA: {    
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->playback_handle!=NULL) {
        snd_pcm_close (rx->playback_handle);
        rx->playback_handle=NULL;
      }
      if(rx->local_audio_buffer!=NULL) {
        g_free(rx->local_audio_buffer);
        rx->local_audio_buffer=NULL;
      }
      rx->output_started=FALSE;
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#endif
  }
}

void audio_close_input(RADIO *r) {
  log_info("Close audio input\n");
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      // Stop the mic read thread first so it can't touch the ring buffer/stream
      // we are about to destroy.  It may be blocked waiting for data, so clear
      // `running` and wake it, then join.
      running=FALSE;
      g_mutex_lock(&r->ring_buffer_mutex);
      g_cond_signal(&r->ring_buffer_cond);
      g_mutex_unlock(&r->ring_buffer_mutex);
      if(mic_read_thread_id!=NULL) {
        g_thread_join(mic_read_thread_id);
        mic_read_thread_id=NULL;
      }
      g_mutex_lock(&r->local_microphone_mutex);
      // Destroying the stream stops its read_callback before we free the ring
      // buffer it writes into.
      if(r->input_stream!=NULL) { soundio_instream_destroy(r->input_stream); r->input_stream=NULL; }
      if(r->input_device!=NULL) { soundio_device_unref(r->input_device); r->input_device=NULL; }
      if(r->ring_buffer!=NULL) { soundio_ring_buffer_destroy(r->ring_buffer); r->ring_buffer=NULL; }
      r->input_started=FALSE;
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      g_mutex_lock(&r->local_microphone_mutex);
      if(r->microphone_stream!=NULL) {
        pa_simple_free(r->microphone_stream);
        r->microphone_stream=NULL;
        g_free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
      }
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
    case USE_ALSA: {
      g_mutex_lock(&r->local_microphone_mutex);
      running=FALSE;
      if(mic_read_thread_id!=NULL) {
log_info("audio_close_input: wait for thread to complete\n");
        g_thread_join(mic_read_thread_id);
        mic_read_thread_id=NULL;
      }      
      if(r->record_handle!=NULL) {
log_info("audio_close_input: snd_pcm_close\n");        
        snd_pcm_close (r->record_handle);
        r->record_handle=NULL;
      }
      if(r->local_microphone_buffer!=NULL) {
log_info("audio_close_input: free mic buffer\n");
        g_free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
      }      
      
      g_free(r->local_microphone_buffer);
      r->local_microphone_buffer=NULL;
      g_mutex_unlock(&r->local_microphone_mutex);
      break;
    }
#endif
  }
}

void audio_start_output(RECEIVER *rx) {
  int err;
  switch(radio->which_audio) {
    case USE_SOUNDIO:
      if(rx->output_stream!=NULL) {
        if(!rx->output_started) {
          // Don't start playback until the ring buffer holds a cushion (~60 ms):
          // starting on a near-empty buffer makes the consumer callback drain to
          // empty and underrun on the slightest producer jitter, heard as
          // periodic "buffer gaps" (worst at high DSP rates where each feed is
          // only a few ms of audio).  audio_write keeps filling the ring while
          // we wait; this is retried every buffer until the cushion is reached.
          int frames = soundio_ring_buffer_fill_count(rx->ring_buffer) / (int)(sizeof(float)*2);
          if(frames < rx->output_stream->sample_rate / 16) break;
          underflow_count=0;
          if((err = soundio_outstream_start(rx->output_stream))) {
              log_info("audio_start_output: unable to start output device: %s", soundio_strerror(err));
          } else {
            rx->output_started=TRUE;
          }
        }
      }
      break;
#ifndef __APPLE__
    case USE_PULSEAUDIO:
      rx->output_started=TRUE;
      break;
    case USE_ALSA:
      rx->output_started=TRUE;
      break;
#endif
  }
}

int audio_write(RECEIVER *rx,float left_sample,float right_sample) {
  int result=0;
  int rc;
  int err;
  float *float_buffer;  
  
  switch(radio->which_audio) {
    case USE_SOUNDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      float samples[2];
      samples[0]=left_sample;
      samples[1]=right_sample;
      if(rx->ring_buffer!=NULL) {
        char *buf = soundio_ring_buffer_write_ptr(rx->ring_buffer);
        int fill_count = sizeof(float)*2;
        int free=soundio_ring_buffer_free_count(rx->ring_buffer);
        if(free<fill_count) {
          //g_print("audio_write: ring buffer full: need %d free %d\n",fill_count,free);
        } else {
          memcpy(buf, &samples[0], fill_count);
          soundio_ring_buffer_advance_write_ptr(rx->ring_buffer, fill_count);
        }
      }
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
#ifndef __APPLE__
    case USE_PULSEAUDIO: {
      g_mutex_lock(&rx->local_audio_mutex);
      if(rx->local_audio_buffer==NULL) {
        rx->local_audio_buffer_offset=0;
        rx->local_audio_buffer=g_new0(float,2*rx->local_audio_buffer_size);
      }
      
      float_buffer=(float *)rx->local_audio_buffer;
      float_buffer[rx->local_audio_buffer_offset*2]=left_sample;
      float_buffer[(rx->local_audio_buffer_offset*2)+1]=right_sample;
      //rx->local_audio_buffer[rx->local_audio_buffer_offset*2]=left_sample;
      //rx->local_audio_buffer[(rx->local_audio_buffer_offset*2)+1]=right_sample;
      
      rx->local_audio_buffer_offset++;
      if(rx->local_audio_buffer_offset>=rx->local_audio_buffer_size) {
        rc=pa_simple_write(rx->playstream,
                           rx->local_audio_buffer,
                           rx->local_audio_buffer_size*sizeof(float)*2,
                           &err); 
        if(rc!=0) {
          log_error("audio_write failed err=%d\n",err);
        }
        rx->local_audio_buffer_offset=0;
      }
      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }
    case USE_ALSA: {
      snd_pcm_sframes_t delay;
      long trim;
      

      gint32 *long_buffer;
      gint16 *short_buffer;      

      g_mutex_lock(&rx->local_audio_mutex);

      if(rx->playback_handle!=NULL && rx->local_audio_buffer!=NULL) {
        switch(rx->local_audio_format) {
          case SND_PCM_FORMAT_S16_LE:
            short_buffer=(gint16 *)rx->local_audio_buffer;
            short_buffer[rx->local_audio_buffer_offset*2]=(gint16)(left_sample*32767.0F);
            short_buffer[(rx->local_audio_buffer_offset*2)+1]=(gint16)(right_sample*32767.0F);
            break;
          case SND_PCM_FORMAT_S32_LE:
            long_buffer=(gint32 *)rx->local_audio_buffer;
            long_buffer[rx->local_audio_buffer_offset*2]=(gint32)(left_sample*4294967295.0F);
            long_buffer[(rx->local_audio_buffer_offset*2)+1]=(gint32)(right_sample*4294967295.0F);
            break;
          case SND_PCM_FORMAT_FLOAT_LE:
            float_buffer=(float *)rx->local_audio_buffer;
            float_buffer[rx->local_audio_buffer_offset*2]=left_sample;
            float_buffer[(rx->local_audio_buffer_offset*2)+1]=right_sample;
            break;
            
          default: return -1;            
        }
        rx->local_audio_buffer_offset++;

        if(rx->local_audio_buffer_offset>=rx->local_audio_buffer_size) {

          trim=0;

/*
          int max_delay=rx->local_audio_buffer_size*6;
          if(snd_pcm_delay(rx->playback_handle,&delay)==0) {
            if(delay>max_delay) {
              trim=delay-max_delay;
log_info("audio delay=%ld trim=%ld audio_buffer_size=%d\n",delay,trim,rx->local_audio_buffer_size);
            }
          }
*/
          if(trim<rx->local_audio_buffer_size) {
            if ((rc = snd_pcm_writei (rx->playback_handle, rx->local_audio_buffer, rx->local_audio_buffer_size-trim)) != rx->local_audio_buffer_size-trim) {
              if(rc<0) {
                if(rc==-EPIPE) {
                  if ((rc = snd_pcm_prepare (rx->playback_handle)) < 0) {
                    log_info("audio_write: cannot prepare audio interface for use %d (%s)\n", rc, snd_strerror (rc));
                    rx->local_audio_buffer_offset=0;
                    g_mutex_unlock(&rx->local_audio_mutex);
                    return rc;
                  }
                } else {
                  // ignore short write
                }
              }
            }
          }
          rx->local_audio_buffer_offset=0;
        }
      }

      g_mutex_unlock(&rx->local_audio_mutex);
      break;
    }      
#endif
  }
  return result;
}

static void *mic_read_thread(gpointer arg) {
  RADIO *r=(RADIO *)arg;
  int rc;
  int err;
  log_info("mic_read_thread: ENTRY\n");
  switch(radio->which_audio) {
    case USE_SOUNDIO:
      while(running) {
        g_mutex_lock (&r->ring_buffer_mutex);
        // Also watch `running` so audio_close_input can wake and stop us even
        // when no more mic data is arriving (needed to join cleanly on a live
        // input-device switch or shutdown).
        while(running && soundio_ring_buffer_fill_count(r->ring_buffer)==0)
          g_cond_wait (&r->ring_buffer_cond, &r->ring_buffer_mutex);
        if(!running) { g_mutex_unlock (&r->ring_buffer_mutex); break; }
        char *read_ptr = soundio_ring_buffer_read_ptr(r->ring_buffer);
        int fill_bytes = soundio_ring_buffer_fill_count(r->ring_buffer);
        if(fill_bytes>(r->local_microphone_buffer_size*sizeof(float))) {
          fill_bytes=r->local_microphone_buffer_size*sizeof(float);
        }
        memcpy(r->local_microphone_buffer,read_ptr,fill_bytes);
        soundio_ring_buffer_advance_read_ptr(r->ring_buffer, fill_bytes);
        g_mutex_unlock (&r->ring_buffer_mutex);
        switch(r->discovered->protocol) {
          case PROTOCOL_1:
            protocol1_process_local_mic(r);
            break;
          case PROTOCOL_2:
            protocol2_process_local_mic(r);
            break;
#ifdef SOAPYSDR
          case PROTOCOL_SOAPYSDR:
            soapy_protocol_process_local_mic(r);
            break;
#endif
        }
      }
      break;
#ifndef __APPLE__
    case USE_PULSEAUDIO:
      while(running) {
        g_mutex_lock(&r->local_microphone_mutex);
        if(r->local_microphone_buffer==NULL) {
          running=0;
        } else {
          rc=pa_simple_read(r->microphone_stream,
      		r->local_microphone_buffer,
      		r->local_microphone_buffer_size*sizeof(float),
      		&err); 
          if(rc<0) {
            running=0;
            log_info("mic_read_thread: returned %d error=%d (%s)\n",rc,err,pa_strerror(err));
          } else {
            switch(r->discovered->protocol) {
              case PROTOCOL_1:
                protocol1_process_local_mic(r);
                break;
              case PROTOCOL_2:
                protocol2_process_local_mic(r);
                break;
#ifdef SOAPYSDR
              case PROTOCOL_SOAPYSDR:
                soapy_protocol_process_local_mic(r);
                break;
#endif
            }
          }
        }
        g_mutex_unlock(&r->local_microphone_mutex);
      }
      break;

    case USE_ALSA:
      {
      int rc;
      if ((rc = snd_pcm_start (r->record_handle)) < 0) {
    log_info("mic_read_thread: ALSA: cannot start audio interface for use (%s)\n",
            snd_strerror (rc));

        return NULL;
      }
log_info("mic_read_thread: ALSA: mic_buffer_size=%d\n",radio->local_microphone_buffer_size);
      running=TRUE;
      while(running) {
        if ((rc = snd_pcm_readi (r->record_handle, r->local_microphone_buffer, r->local_microphone_buffer_size)) != r->local_microphone_buffer_size) {
          if(running) {
            if(rc<0) {
              if(rc==-EPIPE) {
                //g_print("mic_read_thread: -EPIPE: snd_pcm_prepare\n");
                if ((rc = snd_pcm_prepare (r->record_handle)) < 0) {
                    log_info("mic_read_thread: ALSA: cannot prepare audio interface for use %d (%s)\n", rc, snd_strerror (rc));
                    //return rc;
                }
              } else {
                log_error("mic_read_thread: ALSA: read from audio interface failed (%s)\n",
                      snd_strerror (rc));
                running=FALSE;
              }
            } else {
              log_info("mic_read_thread: ALSA: read %d\n",rc);
            }
          }
        } else {
          // process the mic input
          switch(r->discovered->protocol) {
            case PROTOCOL_1:        
              protocol1_process_local_mic(r);
              break;
            case PROTOCOL_2:
              protocol2_process_local_mic(r);
              break;
#ifdef SOAPYSDR
            case PROTOCOL_SOAPYSDR:
              soapy_protocol_process_local_mic(r);
              break;
#endif
          }
        }
      }

      }
      break;
#endif
  }
log_info("mic_read_thread: EXIT\n");
  return NULL;
}

void audio_get_cards() {
}

#ifndef __APPLE__
static void source_list_cb(pa_context *context,const pa_source_info *s,int eol,void *data) {
  int i;
  if(eol>0) {
    for(i=0;i<n_input_devices;i++) {
      log_info("Input: %d: %s (%s)\n",input_devices[i].index,input_devices[i].name,input_devices[i].description);
    }
  } else if(n_input_devices<MAX_AUDIO_DEVICES) {
    input_devices[n_input_devices].name=g_new0(char,strlen(s->name)+1);
    memcpy(input_devices[n_input_devices].name, s->name, strlen(s->name));
    input_devices[n_input_devices].description=g_new0(char,strlen(s->description)+1);
    memcpy(input_devices[n_input_devices].description,s->description, strlen(s->description));
    input_devices[n_input_devices].index=s->index;
    
    n_input_devices++;
  }
}

static void sink_list_cb(pa_context *context,const pa_sink_info *s,int eol,void *data) {
  int i;
  if(eol>0) {
    for(i=0;i<n_output_devices;i++) {
      log_info("Output: %d: %s (%s)\n",output_devices[i].index,output_devices[i].name,output_devices[i].description);
    }
    op=pa_context_get_source_info_list(pa_ctx,source_list_cb,NULL);
  } else if(n_output_devices<MAX_AUDIO_DEVICES) {
    output_devices[n_output_devices].name=g_new0(char,strlen(s->name)+1);
    memcpy(output_devices[n_output_devices].name,s->name,strlen(s->name));
    output_devices[n_output_devices].description=g_new0(char,strlen(s->description)+1);
    memcpy(output_devices[n_output_devices].description,s->description,strlen(s->description));
    output_devices[n_output_devices].index=s->index;
    n_output_devices++;
  }
}

static void state_cb(pa_context *c, void *userdata) {
        pa_context_state_t state;
        int *ready = userdata;

        state = pa_context_get_state(c);

log_info("%s: %d\n",__FUNCTION__,state);
        switch  (state) {
                // There are just here for reference
                case PA_CONTEXT_UNCONNECTED:
log_info("audio: state_cb: PA_CONTEXT_UNCONNECTED\n");
                        break;
                case PA_CONTEXT_CONNECTING:
log_info("audio: state_cb: PA_CONTEXT_CONNECTING\n");
                        break;
                case PA_CONTEXT_AUTHORIZING:
log_info("audio: state_cb: PA_CONTEXT_AUTHORIZING\n");
                        break;
                case PA_CONTEXT_SETTING_NAME:
log_info("audio: state_cb: PA_CONTEXT_SETTING_NAME\n");
                        break;
                case PA_CONTEXT_FAILED:
log_info("audio: state_cb: PA_CONTEXT_FAILED\n");
                        *ready = 2;
                        break;
                case PA_CONTEXT_TERMINATED:
log_info("audio: state_cb: PA_CONTEXT_TERMINATED\n");
                        *ready = 2;
                        break;
                case PA_CONTEXT_READY:
log_info("audio: state_cb: PA_CONTEXT_READY\n");
                        *ready = 1;
// get a list of the output devices
                        n_input_devices=0;
                        n_output_devices=0;
                        op = pa_context_get_sink_info_list(pa_ctx,sink_list_cb,NULL);
                        break;
                default:
                        log_info("audio: state_cb: unknown state %d\n",state);
                        break;
        }
}
#endif

// (Re)build the SoundIo output/input device lists from the current CoreAudio
// state.  Called at startup and again every time the receiver audio page is
// opened, so devices that appear after launch (Bluetooth headphones the user
// just connected) show up without restarting the app.  A synthetic "System
// Default" entry is always placed first (see AUDIO_SYSTEM_DEFAULT_NAME).
static void soundio_build_device_lists(void) {
  // free strings from a previous scan
  for(int i=0;i<n_output_devices;i++) {
    if(output_devices[i].name) { g_free(output_devices[i].name); output_devices[i].name=NULL; }
    if(output_devices[i].description) { g_free(output_devices[i].description); output_devices[i].description=NULL; }
  }
  for(int i=0;i<n_input_devices;i++) {
    if(input_devices[i].name) { g_free(input_devices[i].name); input_devices[i].name=NULL; }
    if(input_devices[i].description) { g_free(input_devices[i].description); input_devices[i].description=NULL; }
  }
  n_output_devices=0;
  n_input_devices=0;

  // refresh soundio's cached device list from the backend
  soundio_flush_events(soundio);

  // synthetic "System Default" output at index 0; index -1 is resolved to the
  // current system default output device each time the stream is (re)opened
  output_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
  output_devices[0].description=g_strdup("System Default");
  output_devices[0].index=-1;
  n_output_devices=1;

  int output_count=soundio_output_device_count(soundio);
  for(int i=0;i<output_count;i++) {
    if(n_output_devices>=MAX_AUDIO_DEVICES) break;
    struct SoundIoDevice *device=soundio_get_output_device(soundio,i);
    if(!device) continue;

    int ok_fmt=soundio_device_supports_format(device, SoundIoFormatFloat32NE);
    int nearest=soundio_device_nearest_sample_rate(device, sample_rate);

    // Only Float32 output is required.  Devices that don't accept the 48 kHz
    // DSP rate (e.g. Bluetooth headsets locked to 44.1 kHz) are still listed
    // and usable: audio_open_output opens them at their nearest rate and the
    // write callback resamples on the fly.  A device with no usable rate at all
    // (nearest<=0) is skipped.
    if(!ok_fmt || nearest<=0) {
      soundio_device_unref(device);
      continue;
    }

    output_devices[n_output_devices].name=g_strdup(device->name);
    output_devices[n_output_devices].description=g_strdup(device->name);
    output_devices[n_output_devices].index=i;
    soundio_device_unref(device);
    n_output_devices++;
  }

  // synthetic "System Default" input at index 0 (mirrors the output list);
  // index -1 is resolved to the current system default input each time the
  // mic stream is opened
  input_devices[0].name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
  input_devices[0].description=g_strdup("System Default");
  input_devices[0].index=-1;
  n_input_devices=1;

  int input_count=soundio_input_device_count(soundio);
  for(int i=0;i<input_count;i++) {
    if(n_input_devices>=MAX_AUDIO_DEVICES) break;
    struct SoundIoDevice *device=soundio_get_input_device(soundio,i);
    if(!device) continue;
    input_devices[n_input_devices].name=g_strdup(device->name);
    input_devices[n_input_devices].description=g_strdup(device->name);
    input_devices[n_input_devices].index=i;
    soundio_device_unref(device);
    n_input_devices++;
  }
}

// Public: re-scan audio devices (macOS/SoundIo only).  Safe to call while audio
// is streaming — it only rebuilds the UI-facing device metadata, not the open
// stream, which holds its own device reference.
void audio_refresh_devices(void) {
  if(radio!=NULL && radio->which_audio==USE_SOUNDIO && soundio!=NULL) {
    soundio_build_device_lists();
  }
}

// A receiver "follows the system default" output when the user picked the
// synthetic "System Default" entry, or left the output unconfigured.  A receiver
// pinned to a specific named device is intentionally left where the user put it.
static gboolean rx_follows_system_default(RECEIVER *rx) {
  return rx->audio_name==NULL ||
         strcmp(rx->audio_name,AUDIO_SYSTEM_DEFAULT_NAME)==0;
}

// libsoundio binds an output stream to a *concrete* device resolved when the
// stream is opened; it does not track later changes to the macOS default output.
// So when a "System Default" receiver is playing and the user switches the
// system output device (System Settings > Sound, or (un)plugging headphones),
// audio keeps going to the now-stale device.  This timer polls the current
// default output and, when it differs from the device a System Default receiver
// is bound to, re-opens that receiver's stream onto the new device.  Playback
// restarts on its own once the receiver feed refills the ring buffer
// (see the audio_start_output call at the end of the receiver audio loop).
static guint default_output_monitor_id=0;

// Core of the follow-the-default logic: resolve the current system default
// output and (re)open every System Default receiver that isn't already on it.
// Runs on the GTK main thread (from the CoreAudio-listener idle handler and
// from the fallback timer).  Returns TRUE if at least one receiver still wants
// to follow the default (used to decide whether the fallback timer keeps
// ticking).
static gboolean default_output_apply(void) {
  if(radio==NULL || radio->which_audio!=USE_SOUNDIO || soundio==NULL)
    return FALSE;

  // Anything that should be following the system default?  Note we do NOT
  // require an open stream here: a previous transition may have left a receiver
  // with output_stream==NULL, and we want to recover it, not abandon it (that
  // was the "switched once, then never again" bug — the receiver got dropped
  // from the poll forever).
  int any=0;
  for(int i=0;i<radio->receivers;i++) {
    RECEIVER *rx=radio->receiver[i];
    if(rx!=NULL && rx->local_audio && rx_follows_system_default(rx)) { any=1; break; }
  }
  if(!any) return FALSE;

  // Apply whatever device change CoreAudio has already signalled.
  soundio_flush_events(soundio);
  int def=soundio_default_output_device_index(soundio);
  if(def<0) return TRUE;
  struct SoundIoDevice *ddev=soundio_get_output_device(soundio,def);
  if(ddev==NULL) return TRUE;

  // Log default transitions so a terminal run shows exactly what happened.
  static char last_default[256]="";
  const char *did = ddev->id ? ddev->id : "";
  if(strncmp(last_default, did, sizeof(last_default)-1)!=0) {
    log_info("audio: system default output = '%s' (%s)\n", ddev->name, did);
    snprintf(last_default,sizeof(last_default),"%s",did);
  }

  for(int i=0;i<radio->receivers;i++) {
    RECEIVER *rx=radio->receiver[i];
    if(rx==NULL || !rx->local_audio || !rx_follows_system_default(rx)) continue;

    // Already streaming to the current default device *at the right rate*?
    // NB: a Bluetooth headset keeps the same device id but silently changes its
    // sample rate when it flips A2DP<->HFP (its mic starting/stopping being
    // used forces the "hands-free" 16 kHz profile).  Our open stream does not
    // follow that and it plays as garbage, so we must compare the rate too, not
    // just the device identity.
    if(rx->output_stream!=NULL && rx->output_device!=NULL &&
       rx->output_device->id!=NULL &&
       strcmp(rx->output_device->id, ddev->id)==0 &&
       rx->output_device->is_raw==ddev->is_raw) {
      int want = soundio_device_supports_sample_rate(ddev, sample_rate)
                   ? sample_rate
                   : soundio_device_nearest_sample_rate(ddev, sample_rate);
      if(want>0 && rx->output_stream->sample_rate==want)
        continue;   // same device, same rate — nothing to do
      log_info("audio: RX%d output '%s' changed rate %d -> %d; re-opening\n",
              rx->channel, ddev->name, rx->output_stream->sample_rate, want);
    }

    // Either the default moved, its rate changed, or a previous (re)open left
    // this receiver with no stream.  (Re)open onto the current default.
    //
    // Debounce: re-opening a soundio output is slow (tens of ms of
    // soundio_outstream_destroy/open, worse on Bluetooth) and it happens under
    // rx->local_audio_mutex, which the RX receive thread also takes in
    // audio_write.  A Bluetooth A2DP<->HFP transition makes the device's
    // reported rate flip-flop, so without a guard we would re-open again and
    // again, stalling the receive thread each time until the WDSP RX ring
    // starves (endless "fexchange0: error=-2").  Re-open at most once every few
    // seconds so the transition can settle and the RX feed is not starved.
    static gint64 last_reopen=0;
    gint64 now=g_get_monotonic_time();
    if(now-last_reopen < 3000000) {
      log_info("audio: RX%d output change to '%s' debounced (recent re-open)\n",
              rx->channel, ddev->name);
      continue;
    }
    last_reopen=now;
    log_info("audio: (re)opening RX%d output on system default '%s'\n",
            rx->channel, ddev->name);
    audio_close_output(rx);
    if(audio_open_output(rx)<0)
      log_info("audio: RX%d re-open on '%s' failed; will retry\n",
              rx->channel, ddev->name);
  }

  soundio_device_unref(ddev);
  return TRUE;
}

// The microphone "follows the system default" input under the same rule as the
// output: the user picked the synthetic "System Default" entry (or left it
// unconfigured).  A mic pinned to a specific device is left alone.
static gboolean mic_follows_system_default(RADIO *r) {
  return r->microphone_name==NULL ||
         strcmp(r->microphone_name,AUDIO_SYSTEM_DEFAULT_NAME)==0;
}

// Input counterpart of default_output_apply().  Unlike the output, the mic is
// only open while the protocol/TX path wants it, so we act ONLY when a stream
// is already open (input_stream!=NULL) and re-open it onto the new default.  A
// failed re-open just leaves the mic closed until the next TX re-opens it — no
// "permanently muted" hazard, so no recovery loop is needed here.
static gboolean default_input_apply(void) {
  if(radio==NULL || radio->which_audio!=USE_SOUNDIO || soundio==NULL)
    return FALSE;
  RADIO *r=radio;
  if(r->input_stream==NULL || !mic_follows_system_default(r)) return FALSE;

  soundio_flush_events(soundio);
  int def=soundio_default_input_device_index(soundio);
  if(def<0) return TRUE;
  struct SoundIoDevice *ddev=soundio_get_input_device(soundio,def);
  if(ddev==NULL) return TRUE;

  static char last_input[256]="";
  const char *did = ddev->id ? ddev->id : "";
  if(strncmp(last_input, did, sizeof(last_input)-1)!=0) {
    log_info("audio: system default input = '%s' (%s)\n", ddev->name, did);
    snprintf(last_input,sizeof(last_input),"%s",did);
  }

  // Already capturing from the current default device?  Nothing to do.
  if(!(r->input_device!=NULL && r->input_device->id!=NULL &&
       strcmp(r->input_device->id, ddev->id)==0 &&
       r->input_device->is_raw==ddev->is_raw)) {
    // Debounce: opening a mic on a Bluetooth headset forces it into the HFP
    // "hands-free" profile, which itself fires a burst of CoreAudio device
    // events (and can briefly flip the system default between the headset and
    // the built-in mic).  Without a guard each event would re-open the mic,
    // re-trigger the profile negotiation, and spiral until it wedges.  Re-open
    // at most once every few seconds so the negotiation can settle.
    static gint64 last_reopen=0;
    gint64 now=g_get_monotonic_time();
    if(now-last_reopen < 3000000) {
      log_info("audio: mic default changed to '%s' but debouncing (recent re-open)\n", ddev->name);
    } else {
      last_reopen=now;
      log_info("audio: re-opening microphone on system default '%s'\n", ddev->name);
      audio_close_input(r);
      if(audio_open_input(r)<0)
        log_info("audio: microphone re-open on '%s' failed\n", ddev->name);
    }
  }

  soundio_device_unref(ddev);
  return TRUE;
}

// Fallback poll: a slow safety net that also handles retrying a failed re-open
// (no CoreAudio event fires for that).  The instant path is the CoreAudio
// listener below; on macOS this timer rarely does more than a cheap
// flush_events + early-out, so it costs next to nothing.
static gboolean default_output_monitor_cb(gpointer data) {
  default_output_apply();
  default_input_apply();
  return TRUE;
}

#ifdef __APPLE__
// Event-driven fast path.  CoreAudio calls this (on one of its own threads) the
// instant the user changes the system default output device — no polling, zero
// cost until it actually fires.  We must not touch soundio/GTK from this thread,
// so we force a device rescan and bounce the real work onto the GTK main thread.
// A short delay lets soundio's async rescan land before we read the new default.
static gboolean default_output_apply_idle(gpointer data) {
  default_output_apply();
  return FALSE;   // one-shot
}
static gboolean default_output_changed_idle(gpointer data) {
  if(soundio!=NULL) soundio_force_device_scan(soundio);
  g_timeout_add(120, default_output_apply_idle, NULL);
  return FALSE;   // one-shot
}
static OSStatus default_output_listener(AudioObjectID objectID, UInt32 n,
                                        const AudioObjectPropertyAddress *addr,
                                        void *client) {
  g_idle_add(default_output_changed_idle, NULL);
  return noErr;
}
static void install_default_output_listener(void) {
  static gboolean installed=FALSE;
  if(installed) return;
  AudioObjectPropertyAddress addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };
  OSStatus st=AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                             default_output_listener, NULL);
  if(st==noErr) {
    installed=TRUE;
    log_info("audio: installed CoreAudio default-output listener (instant switching)\n");
  } else {
    log_info("audio: AudioObjectAddPropertyListener failed (%d); using timer fallback only\n",(int)st);
  }
}

// Same event-driven fast path for the microphone / default *input* device.
static gboolean default_input_apply_idle(gpointer data) {
  default_input_apply();
  return FALSE;   // one-shot
}
static gboolean default_input_changed_idle(gpointer data) {
  if(soundio!=NULL) soundio_force_device_scan(soundio);
  g_timeout_add(120, default_input_apply_idle, NULL);
  return FALSE;   // one-shot
}
static OSStatus default_input_listener(AudioObjectID objectID, UInt32 n,
                                       const AudioObjectPropertyAddress *addr,
                                       void *client) {
  g_idle_add(default_input_changed_idle, NULL);
  return noErr;
}
static void install_default_input_listener(void) {
  static gboolean installed=FALSE;
  if(installed) return;
  AudioObjectPropertyAddress addr = {
    kAudioHardwarePropertyDefaultInputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };
  OSStatus st=AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                             default_input_listener, NULL);
  if(st==noErr) {
    installed=TRUE;
    log_info("audio: installed CoreAudio default-input listener (instant switching)\n");
  } else {
    log_info("audio: AudioObjectAddPropertyListener (input) failed (%d); using timer fallback only\n",(int)st);
  }
}
#endif

void create_audio(int backend_index,const char *backend) {
  int rc;

  n_output_devices=0;
  n_input_devices=0;

  switch(radio->which_audio) {
    case USE_SOUNDIO:
      soundio=soundio_create();
      if(!soundio) {
        log_info("create_audio: soundio_create failed\n");
        return;
      }
      // Resolve the requested backend, but NEVER use the Dummy backend: it
      // enumerates a fake "Dummy Output Device" and silently discards all audio
      // (this is what left a Fake-SDR config with no real output devices). If the
      // saved index points at Dummy/None, fall back to the first real backend and
      // heal the persisted selection so it doesn't recur.
      {
        enum SoundIoBackend want=soundio_get_backend(soundio,backend_index);
        if(want==SoundIoBackendDummy || want==SoundIoBackendNone) {
          int nb=soundio_backend_count(soundio);
          want=SoundIoBackendNone;
          for(int b=0;b<nb;b++) {
            enum SoundIoBackend cand=soundio_get_backend(soundio,b);
            if(cand!=SoundIoBackendDummy && cand!=SoundIoBackendNone) {
              want=cand;
              radio->which_audio_backend=b;
              break;
            }
          }
        }
        log_info("audio: create_audio: USE_SOUNDIO backend=%s\n",soundio_backend_name(want));
        if(want==SoundIoBackendNone)
          rc=soundio_connect(soundio);            // let soundio auto-pick a real backend
        else {
          rc=soundio_connect_backend(soundio,want);
          // The saved backend (index 0 is often JACK on Linux) may be compiled
          // into libsoundio but not actually usable (no server running), so
          // connect_backend fails. Rather than bail out — which used to leave
          // the global `soundio` created but unconnected, so the very first
          // audio_open_output crashed on soundio_flush_events'
          // `current_backend != SoundIoBackendNone` assertion — fall back to
          // letting libsoundio auto-pick a working backend (PulseAudio/ALSA/…).
          if(rc) {
            log_info("create_audio: backend %s failed (%s); auto-selecting\n",
                    soundio_backend_name(want),soundio_strerror(rc));
            rc=soundio_connect(soundio);
          }
        }
      }
      if(rc) {
        // No backend could be connected at all: destroy and NULL the object so
        // the audio_open_*/monitor paths skip it instead of dereferencing an
        // unconnected soundio (which asserts in libsoundio).
        log_info("create_audio: soundio_connect: %s\n",soundio_strerror(rc));
        soundio_destroy(soundio);
        soundio=NULL;
        return;
      }

      soundio_build_device_lists();
      // Follow the system default output live when the user switches the macOS
      // output device.  On macOS this is event-driven (instant, no polling) via
      // a CoreAudio property listener; the timer below is just a slow safety net
      // that also retries a failed re-open.
#ifdef __APPLE__
      install_default_output_listener();
      install_default_input_listener();
#endif
      if(default_output_monitor_id==0)
        default_output_monitor_id=g_timeout_add(2000, default_output_monitor_cb, NULL);
      break;

#ifndef __APPLE__
    case USE_PULSEAUDIO:
log_info("audio: create_audio: USE_PULSEAUDIO\n");
      main_loop=pa_glib_mainloop_new(NULL);
      main_loop_api=pa_glib_mainloop_get_api(main_loop);
      pa_ctx=pa_context_new(main_loop_api,"machpsdr");
      pa_context_connect(pa_ctx,NULL,0,NULL);
      pa_context_set_state_callback(pa_ctx, state_cb, &ready);
      break;

    case USE_ALSA:
      {

        snd_ctl_card_info_t *info;
        snd_pcm_info_t *pcminfo;
        snd_ctl_card_info_alloca(&info);
        snd_pcm_info_alloca(&pcminfo);
        int i;
        char *device_id;
        int card = -1;

        n_input_devices=0;
        n_output_devices=0;

        snd_ctl_card_info_alloca(&info);
        snd_pcm_info_alloca(&pcminfo);
        while (snd_card_next(&card) >= 0 && card >= 0) {
          int err = 0;
          snd_ctl_t *handle;
          char name[20];
          snprintf(name, sizeof(name), "hw:%d", card);
          if ((err = snd_ctl_open(&handle, name, 0)) < 0) {
            continue;
          }

          if ((err = snd_ctl_card_info(handle, info)) < 0) {
            snd_ctl_close(handle);
            continue;
          }

          int dev = -1;

          while (snd_ctl_pcm_next_device(handle, &dev) >= 0 && dev >= 0) {
            snd_pcm_info_set_device(pcminfo, dev);
            snd_pcm_info_set_subdevice(pcminfo, 0);

            // input devices
            snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);
            if ((err = snd_ctl_pcm_info(handle, pcminfo)) == 0) {
              device_id=g_new(char,128);
              snprintf(device_id, 128, "plughw:%d,%d %s", card, dev, snd_ctl_card_info_get_name(info));
              if(n_input_devices<MAX_AUDIO_DEVICES) {
                input_devices[n_input_devices].name=g_new0(char,strlen(device_id)+1);
                strcpy(input_devices[n_input_devices].name,device_id);
                input_devices[n_input_devices].description=g_new0(char,strlen(device_id)+1);
                strcpy(input_devices[n_input_devices].description,device_id);
                input_devices[n_input_devices].index=0; // not used
                n_input_devices++;
log_info("input_device: %s\n",device_id);
              }
	      g_free(device_id);
            }

            // ouput devices
            snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
            if ((err = snd_ctl_pcm_info(handle, pcminfo)) == 0) {
              device_id=g_new(char,128);
              snprintf(device_id, 128, "plughw:%d,%d %s", card, dev, snd_ctl_card_info_get_name(info));
              if(n_output_devices<MAX_AUDIO_DEVICES) {
                output_devices[n_output_devices].name=g_new0(char,strlen(device_id)+1);
                strcpy(output_devices[n_output_devices].name,device_id);
                output_devices[n_output_devices].description=g_new0(char,strlen(device_id)+1);
                strcpy(output_devices[n_output_devices].description,device_id);
                input_devices[n_output_devices].index=0; // not used
                n_output_devices++;
log_info("output_device: %s\n",device_id);
              }
	    g_free(device_id);
            }
          }
          snd_ctl_close(handle);
        }
	
        // look for dmix
        void **hints, **n;
        char *name, *descr, *io;

        if (snd_device_name_hint(-1, "pcm", &hints) < 0)
          return;
        n = hints;
        while (*n != NULL) {
          name = snd_device_name_get_hint(*n, "NAME");
          descr = snd_device_name_get_hint(*n, "DESC");
          io = snd_device_name_get_hint(*n, "IOID");

          if(strncmp("dmix:", name, 5)==0) {
            if(n_output_devices<MAX_AUDIO_DEVICES) {
              output_devices[n_output_devices].name=g_new0(char,strlen(name)+1);
              strcpy(output_devices[n_output_devices].name,name);
              output_devices[n_output_devices].description=g_new0(char,strlen(descr)+1);
	      i=0;
              while(i<strlen(descr) && descr[i]!='\n') {
		output_devices[n_output_devices].description[i]=descr[i];
	        i++;
	      }
          output_devices[n_output_devices].description[i]='\0';
              input_devices[n_output_devices].index=0;  // not used
              n_output_devices++;
log_info("output_device: name=%s descr=%s\n",name,descr);
            }
log_info("output_device: %s\n",device_id);
          }

          if (name != NULL)
            free(name);
          if (descr != NULL)
            free(descr);
          if (io != NULL)
            free(io);
          n++;
        }
        snd_device_name_free_hint(hints);
      }
      break;
#endif
  }
  log_info("n_input_devices=%d\n", n_input_devices);
}

int audio_get_backends(RADIO *r) {
  int count=0;
  switch(r->which_audio) {
    case USE_SOUNDIO:
      count=soundio_backend_count(soundio);
      break;
#ifndef __APPLE__
    case USE_PULSEAUDIO:
      count=0;
      break;
#endif
  }
log_info("audio_get_backends: %d\n",count);
  return count;
}

const char *audio_get_backend_name(int backend_index) {
  const char *name=soundio_backend_name(soundio_get_backend(soundio,backend_index));
log_info("audio_get_backend_name: %s\n",name);
  return name;
}
