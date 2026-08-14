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


//#define ECHO_MIC

#include "net_compat.h"   // must precede gtk.h: winsock2 before windows.h
#include <gtk/gtk.h>
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <semaphore.h>
#include <math.h>

#include <wdsp.h>

#include "alex.h"
#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "wideband.h"
#include "receiver.h"
#include "transmitter.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "signal.h"
#include "vfo.h"
#include "audio.h"
#ifdef LOCALCW
#include "iambic.h"
#endif
//#include "vox.h"
#include "ext.h"
#include "main.h"
#include "reconnect.h"
#include "protocol2.h"

#define min(x,y) (x<y?x:y)

#define PI 3.1415926535897932F

int data_socket=-1;

static gboolean running;

static struct sockaddr_in base_addr;
static int base_addr_length;

static struct sockaddr_in receiver_addr;
static int receiver_addr_length;

static struct sockaddr_in transmitter_addr;
static int transmitter_addr_length;

static struct sockaddr_in high_priority_addr;
static int high_priority_addr_length;

static struct sockaddr_in audio_addr;
static int audio_addr_length;

static struct sockaddr_in iq_addr;
static int iq_addr_length;

static struct sockaddr_in data_addr[MAX_RECEIVERS];
static int data_addr_length[MAX_RECEIVERS];

static struct sockaddr_in wide_addr;
static int wide_addr_length;

static GThread *protocol2_thread_id;
static GThread *protocol2_timer_thread_id;

static long high_priority_sequence = 0;
static long general_sequence = 0;
static long rx_specific_sequence = 0;
static long tx_specific_sequence = 0;

static int micoutputsamples;  // 48000 in, 192000 out

//static double micinputbuffer[MAX_BUFFER_SIZE*2]; // 48000 ---- UNUSED
//static double iqoutputbuffer[MAX_BUFFER_SIZE*4*2]; //192000 --- UNUSED

static long tx_iq_sequence;
static unsigned char iqbuffer[1444];
static int iqindex;
static int micsamples;

//static float phase = 0.0F; //UNUSED

static long response_sequence;
static int response;

static int samples[MAX_RECEIVERS];
#ifdef INCLUDED
static int outputsamples;
#endif

static long audiosequence;
static unsigned char audiobuffer[260]; // was 1444
static int audioindex;

#define NET_BUFFER_SIZE 2048
// Network buffers
static struct sockaddr_in addr;
static socklen_t length;

static unsigned char general_buffer[60];
static unsigned char high_priority_buffer_to_radio[1444];
static unsigned char transmit_specific_buffer[60];
static unsigned char receive_specific_buffer[1444];

// Layout of the receive-specific ("DDC specific") register block, port 1025.
// There are eight DDC I/Q ports (RX_IQ_TO_HOST_PORT_0..7), so eight DDCs, and
// the per-DDC configuration blocks start at byte 17 with a stride of 6.
#define MAX_DDC_CHANNELS 8
// DDC synchronisation map: one byte per DDC starting here, bit k meaning "this
// DDC is synchronised to DDC k", i.e. started on the same sample.  Written for
// the PureSignal feedback pair and for the diversity pair; the offset itself is
// UNVERIFIED against real gateware (no Protocol-2 board here) and is the first
// thing to suspect if either pair arrives out of lock-step.
#define DDC_SYNC_BASE    1363

static gpointer protocol2_thread(gpointer data);
static gpointer protocol2_timer_thread(gpointer data);
static void  process_iq_data(RECEIVER *rx,int bytes,unsigned char *buffer);
static void  process_div_iq_data(DIVMIXER *dmix,int bytes,unsigned char *buffer);
static void  process_wideband_data(WIDEBAND *w,int bytes,unsigned char *buffer);
#ifdef PURESIGNAL_P2
static void  process_ps_iq_data(RECEIVER *fbk,int bytes,unsigned char *buffer);
#endif
static void  process_command_response(unsigned char *buffer);
static void  process_high_priority(unsigned char *buffer);
static void  process_mic_data(int bytes,unsigned char *buffer);

#ifdef INCLUDED
static void protocol2_calc_buffers(void) {
  switch(sample_rate) {
    case 48000:
      outputsamples=r->buffer_size;
      break;
    case 96000:
      outputsamples=r->buffer_size/2;
      break;
    case 192000:
      outputsamples=r->buffer_size/4;
      break;
    case 384000:
      outputsamples=r->buffer_size/8;
      break;
    case 768000:
      outputsamples=r->buffer_size/16;
      break;
    case 1536000:
      outputsamples=r->buffer_size/32;
      break;
  }
}
#endif

void filter_board_changed(void) {
    protocol2_general();
}

/*
void pa_changed() {
    protocol2_general();
}

void tuner_changed() {
    protocol2_general();
}
*/

void cw_changed(void) {
#ifdef LOCALCW
    // update the iambic keyer params
    if (radio->cw_keyer_internal == 0)
        keyer_update();
#endif
}

void protocol2_start_receiver(RECEIVER *r) {
  log_info("iq_thread: channel=%d\n", r->channel);
  protocol2_general();
  protocol2_high_priority();
  protocol2_receive_specific();
}

void protocol2_stop_receiver(RECEIVER *r) {
  protocol2_general();
  protocol2_high_priority();
}

void protocol2_start_wideband(WIDEBAND *w) {
log_info("protocol2_start_wideband\n");
  protocol2_general();
}

void protocol2_stop_wideband(void) {
  protocol2_general();
}

void protocol2_init(RADIO *r) {
    int i;
    // int rc; // UNUSED

    log_info("protocol2_init: MIC_SAMPLES=%d\n",MIC_SAMPLES);

#ifdef INCLUDED
    outputsamples=r->buffer_size;
#endif
    micoutputsamples=r->buffer_size*4;

    if(r->local_microphone) {
      if(audio_open_input(r)!=0) {
        log_error("audio_open_input failed\n");
        r->local_microphone=FALSE;
      }
    }

#ifdef INCLUDED
    protocol2_calc_buffers();
#endif

    for(i=0;i<r->discovered->supported_receivers;i++) {
      if(r->receiver[i]!=NULL) {
        protocol2_start_receiver(r->receiver[i]);
      }
    }

    data_socket=socket(PF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(data_socket<0) {
        log_error("protocol2_init: create socket failed for data_socket\n");
        exit(-1);
    }

    int optval = 1;
    setsockopt(data_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#ifdef SO_REUSEPORT   // no Winsock equivalent; SO_REUSEADDR above covers Windows
    setsockopt(data_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#endif

    // bind to the interface
    if(bind(data_socket,(struct sockaddr*)&r->discovered->info.network.interface_address,r->discovered->info.network.interface_length)<0) {
        log_error("protocol2_init: bind socket failed for data_socket\n");
        exit(-1);
    }

log_info("protocol2_init: date_socket %d bound to interface\n",data_socket);

    memcpy(&base_addr,&r->discovered->info.network.address,r->discovered->info.network.address_length);
    base_addr_length=r->discovered->info.network.address_length;
    base_addr.sin_port=htons(GENERAL_REGISTERS_FROM_HOST_PORT);

    memcpy(&receiver_addr,&r->discovered->info.network.address,r->discovered->info.network.address_length);
    receiver_addr_length=r->discovered->info.network.address_length;
    receiver_addr.sin_port=htons(RECEIVER_SPECIFIC_REGISTERS_FROM_HOST_PORT);

    memcpy(&transmitter_addr,&r->discovered->info.network.address,r->discovered->info.network.address_length);
    transmitter_addr_length=r->discovered->info.network.address_length;
    transmitter_addr.sin_port=htons(TRANSMITTER_SPECIFIC_REGISTERS_FROM_HOST_PORT);

    memcpy(&high_priority_addr,&r->discovered->info.network.address,r->discovered->info.network.address_length);
    high_priority_addr_length=r->discovered->info.network.address_length;
    high_priority_addr.sin_port=htons(HIGH_PRIORITY_FROM_HOST_PORT);

log_info("protocol2_init: high_priority_addr setup for port %d\n",HIGH_PRIORITY_FROM_HOST_PORT);

    memcpy(&audio_addr,&r->discovered->info.network.address,r->discovered->info.network.address_length);
    audio_addr_length=r->discovered->info.network.address_length;
    audio_addr.sin_port=htons(AUDIO_FROM_HOST_PORT);

    memcpy(&iq_addr,&r->discovered->info.network.address,r->discovered->info.network.address_length);
    iq_addr_length=r->discovered->info.network.address_length;
    iq_addr.sin_port=htons(TX_IQ_FROM_HOST_PORT);


    for(i=0;i<r->discovered->supported_receivers;i++) {
        memcpy(&data_addr[i],&r->discovered->info.network.address,r->discovered->info.network.address_length);
        data_addr_length[i]=r->discovered->info.network.address_length;
        data_addr[i].sin_port=htons(RX_IQ_TO_HOST_PORT_0+i);
        samples[i]=0;
    }

    memcpy(&wide_addr,&r->discovered->info.network.address,r->discovered->info.network.address_length);
    wide_addr_length=r->discovered->info.network.address_length;
    wide_addr.sin_port=htons(WIDE_BAND_TO_HOST_PORT);

    protocol2_thread_id = g_thread_new( "protocol2", protocol2_thread, NULL);
    if( ! protocol2_thread_id )
    {
        log_error("g_thread_new failed on protocol2_thread\n");
        exit( -1 );
    }
    log_info("protocol2_thread: id=%p\n",protocol2_thread_id);

}

void protocol2_general(void) {
    BAND *band;

    band=NULL;
    if(radio!=NULL) {
      if(radio->transmitter!=NULL) {
        if(radio->transmitter->rx!=NULL) {
#ifdef USE_VFO_B_MODE_AND_FILTER
          if(radio->transmitter->rx->split) {
              band=band_get_band(radio->transmitter->rx->band_b);
          } else {
#endif
              band=band_get_band(radio->transmitter->rx->band_a);
#ifdef USE_VFO_B_MODE_AND_FILTER
          }
#endif
        }
      }
    }
    memset(general_buffer, 0, sizeof(general_buffer));

    general_buffer[0]=general_sequence>>24;
    general_buffer[1]=general_sequence>>16;
    general_buffer[2]=general_sequence>>8;
    general_buffer[3]=general_sequence;

    if(radio!=NULL) {
      if(radio->wideband!=NULL) {
        WIDEBAND *w=radio->wideband;
        // Byte 23 is the wideband capture enable, one bit per ADC.  Bytes 24..28
        // are its PARAMETERS, and leaving them zero -- as this did for its whole
        // life -- is not "use the defaults": a board loads its wideband IP with
        // exactly these numbers, so nought samples in nought packets every
        // nought ms is a request for no data at all.  Checked against the one
        // board-side implementation that is open (laurencebarker/Saturn,
        // sw_projects/P2_app: generalpacket.c reads the enable at 23, a
        // big-endian sample count at 24, the sample size in bits at 26, the
        // update period in ms at 27 and packets-per-frame at 28;
        // Outwideband.c then sends samples*2+4 bytes per datagram and loops
        // packets-per-frame times, i.e. zero times for our old register block).
        //
        // Samples per packet is the number process_wideband_data() reads back,
        // and packets per frame is chosen so ONE frame fills the analyzer buffer
        // exactly: each FFT then covers a single contiguous ADC capture instead
        // of being spliced across two of them.  Both fields are one byte
        // (24..25 is the 16-bit one), hence the clamps.
        int per_frame=w->buffer_size/WIDEBAND_SAMPLES_PER_PACKET;
        int rate_ms=w->fps>0?1000/w->fps:100;
        if(per_frame<1)   per_frame=1;
        if(per_frame>255) per_frame=255;
        if(rate_ms<1)     rate_ms=1;
        if(rate_ms>255)   rate_ms=255;
        general_buffer[23]=1<<(w->adc&0x01);
        general_buffer[24]=(WIDEBAND_SAMPLES_PER_PACKET>>8)&0xFF;
        general_buffer[25]=WIDEBAND_SAMPLES_PER_PACKET&0xFF;
        general_buffer[26]=WIDEBAND_SAMPLE_BITS;
        general_buffer[27]=rate_ms;
        general_buffer[28]=per_frame;
      }
    }

    // use defaults apart from
    general_buffer[37]=0x08;  //  phase word (not frequency)
    general_buffer[38]=0x01;  //  enable hardware timer

    if(band==NULL || band->disablePA) {
      general_buffer[58]=0x00;
    } else {
      general_buffer[58]=0x01;  // enable PA
    }

    if(radio!=NULL) {
      if(radio->filter_board==APOLLO) {
        general_buffer[58]|=0x02; // enable APOLLO tuner
      }

      if(radio->filter_board==ALEX) {
        if(radio->discovered->device==NEW_DEVICE_ORION2) {
          general_buffer[59]=0x03;  // enable Alex 0 and 1
        } else {
          general_buffer[59]=0x01;  // enable Alex 0
        }
      }
    }

    if(sendto(data_socket,general_buffer,sizeof(general_buffer),0,(struct sockaddr*)&base_addr,base_addr_length)<0) {
      log_error("sendto socket failed for general: seq=%ld\n",general_sequence);
    }
    general_sequence++;
}

void protocol2_high_priority(void) {
    int r;
    BAND *band;
    int xvtr = 0; // IS THIS UNUSED?
    long long rxFrequency;
    long long txFrequency;
    long phase;
    int mode;

    if(data_socket==-1) {
      return;
    }

    if(radio==NULL) {
      return;
    }

    memset(high_priority_buffer_to_radio, 0, sizeof(high_priority_buffer_to_radio));

    high_priority_buffer_to_radio[0]=high_priority_sequence>>24;
    high_priority_buffer_to_radio[1]=high_priority_sequence>>16;
    high_priority_buffer_to_radio[2]=high_priority_sequence>>8;
    high_priority_buffer_to_radio[3]=high_priority_sequence;

    mode=USB;
      if(radio->transmitter->rx!=NULL) {
#ifdef USE_VFO_B_MODE_AND_FILTER
        if(radio->transmitter->rx->split) {
#endif
          mode=radio->transmitter->rx->mode_a;
#ifdef USE_VFO_B_MODE_AND_FILTER
        } else {
          mode=radio->transmitter->rx->mode_b;
        }
#endif
      }
      high_priority_buffer_to_radio[4]=running;
      if(mode==CWU || mode==CWL) {
        if(radio->tune) {
          high_priority_buffer_to_radio[4]|=0x02;
        }
#ifdef LOCALCW
      if (radio->cw_keyer_internal == 0) {
        // set the ptt if we're not in breakin mode and mox is on
        if(radio->cw_breakin == 0 && getMox()) high_priority_buffer_to_radio[4]|=0x02;
        high_priority_buffer_to_radio[5]|=(keyer_out) ? 0x01 : 0;
        //high_priority_buffer_to_radio[5]|=(*kdot) ? 0x02 : 0;
        //high_priority_buffer_to_radio[5]|=(*kdash) ? 0x04 : 0;
        high_priority_buffer_to_radio[5]|=(key_state==SENDDOT) ? 0x02 : 0;
        high_priority_buffer_to_radio[5]|=(key_state==SENDDASH) ? 0x04 : 0;
      }
#endif
      } else {
        if(isTransmitting(radio)) {
          high_priority_buffer_to_radio[4]|=0x02;
        }
      }

// rx

      for(r=0;r<radio->discovered->supported_receivers;r++) {
        if(radio->receiver[r]!=NULL) {
          //int v=radio->receiver[r]->channel; // UNUSED
          rxFrequency=radio->receiver[r]->frequency_a-radio->receiver[r]->lo_a+radio->receiver[r]->error_a;
          rxFrequency+=radio_ppm_correction(radio->receiver[r]->frequency_a-radio->receiver[r]->lo_a);
          if(radio->receiver[r]->rit_enabled) {
            rxFrequency+=radio->receiver[r]->rit;
          }
  
/*
        switch(radio->receiver[r]->mode_a) {
          case CWU:
            rxFrequency-=radio->cw_keyer_sidetone_frequency;
            break;
          case CWL:
            rxFrequency+=radio->cw_keyer_sidetone_frequency;
            break;
          default:
            break;
        }
*/
          phase=(long)((4294967296.0*(double)rxFrequency)/122880000.0);
          high_priority_buffer_to_radio[9+(radio->receiver[r]->channel*4)]=phase>>24;
          high_priority_buffer_to_radio[10+(radio->receiver[r]->channel*4)]=phase>>16;
          high_priority_buffer_to_radio[11+(radio->receiver[r]->channel*4)]=phase>>8;
          high_priority_buffer_to_radio[12+(radio->receiver[r]->channel*4)]=phase;
        }
      }

      // tx
      if(radio->transmitter->rx!=NULL) {
        RECEIVER *rx=radio->transmitter->rx;
        {   // rx is radio->transmitter->rx, already non-NULL above; bare block
            // keeps txFrequency unconditionally assigned (was a redundant
            // if(rx!=NULL) that left txFrequency uninitialised on the dead else)
          if(rx->split) {
            txFrequency=rx->frequency_b-rx->lo_b+rx->error_b;
            txFrequency+=radio_ppm_correction(rx->frequency_b-rx->lo_b);
          } else {
            if(rx->ctun) {
              txFrequency=rx->ctun_frequency-rx->lo_a+rx->error_a;
              txFrequency+=radio_ppm_correction(rx->ctun_frequency-rx->lo_a);
            } else {
              txFrequency=rx->frequency_a-rx->lo_a+rx->error_a;
              txFrequency+=radio_ppm_correction(rx->frequency_a-rx->lo_a);
            }
          }

          if(radio->transmitter->xit_enabled) {
            txFrequency+=radio->transmitter->xit;
          }
        }
        switch(radio->transmitter->rx->mode_a) {
          case CWU:
            txFrequency+=radio->cw_keyer_sidetone_frequency;
            break;
          case CWL:
            txFrequency-=radio->cw_keyer_sidetone_frequency;
            break;
          default:
            break;
        }
  
        phase=(long)((4294967296.0*(double)txFrequency)/122880000.0);
        }

#ifdef PURESIGNAL_P2
      if(isTransmitting(radio) && (radio->transmitter->puresignal != NULL)) {
        // set puresignal rx to transmit frequency
        high_priority_buffer_to_radio[9]=phase>>24;
        high_priority_buffer_to_radio[10]=phase>>16;
        high_priority_buffer_to_radio[11]=phase>>8;
        high_priority_buffer_to_radio[12]=phase;

        high_priority_buffer_to_radio[13]=phase>>24;
        high_priority_buffer_to_radio[14]=phase>>16;
        high_priority_buffer_to_radio[15]=phase>>8;
        high_priority_buffer_to_radio[16]=phase;
      }
#endif

    high_priority_buffer_to_radio[329]=phase>>24;
    high_priority_buffer_to_radio[330]=phase>>16;
    high_priority_buffer_to_radio[331]=phase>>8;
    high_priority_buffer_to_radio[332]=phase;

    int level=0;
    if(isTransmitting(radio)) {
      BAND *band=NULL;
      if(radio->transmitter!=NULL) {
        if(radio->transmitter->rx!=NULL) {
#ifdef USE_VFO_B_MODE_AND_FILTER
          if(radio->transmitter->rx->split) {
            band=band_get_band(radio->transmitter->rx->band_b);
          } else {
#endif
            band=band_get_band(radio->transmitter->rx->band_a);
#ifdef USE_VFO_B_MODE_AND_FILTER
          }
#endif
        }
      }

      int power=0;
      if(isTransmitting(radio)) {
        if(radio->tune && !radio->transmitter->tune_use_drive) {
          power=(int)(radio->transmitter->drive/100.0*radio->transmitter->tune_percent);
        } else {
          power=(int)radio->transmitter->drive;
        }
      }

      double target_dbm = 10.0 * log10(power * 1000.0);
      double gbb=(band!=NULL)?band->pa_calibration:0.0;
      target_dbm-=gbb;
      double target_volts = sqrt(pow(10, target_dbm * 0.1) * 0.05);
      double volts=min((target_volts / 0.8), 1.0);
      double actual_volts=volts*(1.0/0.98);

      if(actual_volts<0.0) {
        actual_volts=0.0;
      } else if(actual_volts>1.0) {
        actual_volts=1.0;
      }

      level=(int)(actual_volts*255.0);
log_info("protocol2_high_priority: band=%d %s level=%d\n",radio->transmitter->rx->band_a, band->title, level);
    }

    high_priority_buffer_to_radio[345]=level&0xFF;

    if(radio->transmitter->rx!=NULL) {
      if(isTransmitting(radio)) {
        //high_priority_buffer_to_radio[1401]=band->OCtx<<1;
        if(radio->tune) {
/*
          if(OCmemory_tune_time!=0) {
            struct timeval te;
            gettimeofday(&te,NULL);
            long long now=te.tv_sec*1000LL+te.tv_usec/1000;
            if(tune_timeout>now) {
              high_priority_buffer_to_radio[1401]|=OCtune<<1;
            }
          } else {
            high_priority_buffer_to_radio[1401]|=OCtune<<1;
          }
*/
        }
      } else {
        band=band_get_band(radio->transmitter->rx->band_a);
        high_priority_buffer_to_radio[1401]=band->OCrx<<1;
      }
/* 
      if(radio->discovered->device==NEW_DEVICE_ATLAS) {
        for(r=0;r<radio->discovered->supported_receivers;r++) {
          high_priority_buffer_to_radio[1403]|=radio->receiver[i]->preamp;
        }
      }
*/
    }


    long filters=0x00000000;

    if(isTransmitting(radio)) {
      filters=0x08000000;
#ifdef PURESIGNAL_P2
      if(radio->transmitter->puresignal != NULL) {
        filters|=0x00040000;
      }
#endif
    }

    switch(radio->adc[0].filters) {
      case AUTOMATIC:
        break;
      case MANUAL:
        break;
    }

    if(radio->transmitter->rx!=NULL) {
      rxFrequency=radio->transmitter->rx->frequency_a-radio->transmitter->rx->lo_a;
      switch(radio->adc[0].filters) {
        case AUTOMATIC:
          switch(radio->discovered->device) {
            case NEW_DEVICE_ORION2:
              if(rxFrequency<1500000L) {
                filters|=0x1000;
              } else if(rxFrequency<2100000L) {
                filters|=0x40;
              } else if(rxFrequency<5500000L) {
                filters|=0x20;
              } else if(rxFrequency<11000000L) {
                filters|=0x10;
              } else if(rxFrequency<22000000L) {
                filters|=0x02;
              } else if(rxFrequency<35000000L) {
                filters|=0x04;
              } else {
                filters|=0x08;
              }
              break;
            default:
              if(rxFrequency<1500000L) {
                filters|=0x1000;
              } else if(rxFrequency<6500000L) {
                filters|=0x40;
              } else if(rxFrequency<9500000L) {
                filters|=0x20;
              } else if(rxFrequency<13000000L) {
                filters|=0x10;
              } else if(rxFrequency<20000000L) {
                filters|=0x02;
              } else if(rxFrequency<50000000L) {
                filters|=0x04;
              } else {
                filters|=0x80;;
              }
              break;
          }
          break;
        case MANUAL:
          switch(radio->adc[0].hpf) {
            case BYPASS:
              filters|=0x1000;
              break;
            case HPF_1_5:
              filters|=0x40;
              break;
            case HPF_6_5:
              filters|=0x20;
              break;
            case HPF_9_5:
              filters|=0x10;
              break;
            case HPF_13:
              filters|=0x02;
              break;
            case HPF_20:
              filters|=0x04;
              break;
          }
          break;
      }

      switch(radio->adc[0].filters) {
        case AUTOMATIC:
          switch(radio->discovered->device) {
            case NEW_DEVICE_ORION2:
              if(txFrequency>32000000) {
                filters|=0x20000000;
              } else if(txFrequency>22000000) {
                filters|=0x40000000;
              } else if(txFrequency>11000000) {
                filters|=0x20000000;
              } else if(txFrequency>5500000) {
                filters|=0x100000;
              } else if(txFrequency>2100000) {
                filters|=0x200000;
              } else if(txFrequency>1500000) {
                filters|=0x400000;
              } else {
                filters|=0x800000;
              }
              break;
            default:
              if(txFrequency>35600000) {
                filters|=0x08;
              } else if(txFrequency>24000000) {
                filters|=0x04;
              } else if(txFrequency>16500000) {
                filters|=0x02;
              } else if(txFrequency>8000000) {
                filters|=0x10;
              } else if(txFrequency>5000000) {
                filters|=0x20;
              } else if(txFrequency>2500000) {
                filters|=0x40;
              } else {
                filters|=0x40;
              }
              break;
          }
       case MANUAL:
          switch(radio->adc[0].lpf) {
            case LPF_160:
              filters|=0x800000;
              break;
            case LPF_80:
              filters|=0x400000;
              break;
            case LPF_60_40:
              filters|=0x200000;
              break;
            case LPF_30_20:
              filters|=0x100000;
              break;
            case LPF_17_15:
              filters|=0x80000000;
              break;
            case LPF_12_10:
              filters|=0x40000000;
              break;
            case LPF_6:
              filters|=0x20000000;
              break;
          }
          break;
      }

      switch(radio->adc[0].antenna) {
          case 0:  // ANT 1
            break;
          case 1:  // ANT 2
            break;
          case 2:  // ANT 3
            break;
          case 3:  // EXT 1
            filters|=ALEX_RX_ANTENNA_EXT2;
            break;
          case 4:  // EXT 2
            filters|=ALEX_RX_ANTENNA_EXT1;
            break;
          case 5:  // XVTR
            if(!xvtr) {
              filters|=ALEX_RX_ANTENNA_XVTR;
            }
            break;
          default:
            // invalid value - set to 0
            band->alexRxAntenna=0;
            break;
      }

      if(isTransmitting(radio)) {
        if(!xvtr) {
          switch(radio->alex_tx_antenna) {
            case 0:  // ANT 1
              filters|=ALEX_TX_ANTENNA_1;
              break;
            case 1:  // ANT 2
              filters|=ALEX_TX_ANTENNA_2;
              break;
            case 2:  // ANT 3
              filters|=ALEX_TX_ANTENNA_3;
              break;
            default:
              // invalid value - set to 0
              filters|=ALEX_TX_ANTENNA_1;
              band->alexRxAntenna=0;
              break;
          }
        }
      } else {
        switch(radio->adc[0].antenna) {
          case 0:  // ANT 1
            filters|=ALEX_TX_ANTENNA_1;
            break;
          case 1:  // ANT 2
            filters|=ALEX_TX_ANTENNA_2;
            break;
          case 2:  // ANT 3
            filters|=ALEX_TX_ANTENNA_3;
            break;
          case 3:  // EXT 1
          case 4:  // EXT 2
          case 5:  // XVTR
            if(!xvtr) {
              switch(radio->alex_tx_antenna) {
                case 0:  // ANT 1
                  filters|=ALEX_TX_ANTENNA_1;
                  break;
                case 1:  // ANT 2
                  filters|=ALEX_TX_ANTENNA_2;
                  break;
                case 2:  // ANT 3
                  filters|=ALEX_TX_ANTENNA_3;
                  break;
              }
            }
            break;
        }
      }

      high_priority_buffer_to_radio[1432]=(filters>>24)&0xFF;
      high_priority_buffer_to_radio[1433]=(filters>>16)&0xFF;
      high_priority_buffer_to_radio[1434]=(filters>>8)&0xFF;
      high_priority_buffer_to_radio[1435]=filters&0xFF;

//fprintf(stderr,"filters: txrx0: %02X %02X %02X %02X for rx=%lld tx=%lld\n",high_priority_buffer_to_radio[1432],high_priority_buffer_to_radio[1433],high_priority_buffer_to_radio[1434],high_priority_buffer_to_radio[1435],rxFrequency,txFrequency);

      filters=0x00000000;
      rxFrequency=radio->receiver[0]->frequency_a-radio->receiver[0]->lo_a;

      switch(radio->discovered->device) {
        case NEW_DEVICE_ORION2:
          if(rxFrequency<1500000L) {
            filters|=ALEX_BYPASS_HPF;
          } else if(rxFrequency<2100000L) {
            filters|=ALEX_1_5MHZ_HPF;
          } else if(rxFrequency<5500000L) {
            filters|=ALEX_6_5MHZ_HPF;
          } else if(rxFrequency<11000000L) {
            filters|=ALEX_9_5MHZ_HPF;
          } else if(rxFrequency<22000000L) {
              filters|=ALEX_13MHZ_HPF;
          } else if(rxFrequency<35000000L) {
            filters|=ALEX_20MHZ_HPF;
          } else {
            filters|=ALEX_6M_PREAMP;
          }
          break;
        default:
          if(rxFrequency<1800000L) {
            filters|=ALEX_BYPASS_HPF;
          } else if(rxFrequency<6500000L) {
            filters|=ALEX_1_5MHZ_HPF;
          } else if(rxFrequency<9500000L) {
            filters|=ALEX_6_5MHZ_HPF;
          } else if(rxFrequency<13000000L) {
            filters|=ALEX_9_5MHZ_HPF;
          } else if(rxFrequency<20000000L) {
            filters|=ALEX_13MHZ_HPF;
          } else if(rxFrequency<50000000L) {
            filters|=ALEX_20MHZ_HPF;
          } else {
            filters|=ALEX_6M_PREAMP;
          }
          break;
      }

      //high_priority_buffer_to_radio[1428]=(filters>>24)&0xFF;
      //high_priority_buffer_to_radio[1429]=(filters>>16)&0xFF;
      high_priority_buffer_to_radio[1430]=(filters>>8)&0xFF;
      high_priority_buffer_to_radio[1431]=filters&0xFF;

//fprintf(stderr,"filters: rx1: %02X %02X for rx=%lld\n",high_priority_buffer_to_radio[1430],high_priority_buffer_to_radio[1431],rxFrequency);

//fprintf(stderr,"protocol2_high_priority: OC=%02X filters=%04X for frequency=%lld\n", high_priority_buffer_to_radio[1401], filters, rxFrequency);

  
      if(isTransmitting(radio)) {
        high_priority_buffer_to_radio[1443]=radio->transmitter->attenuation;
      } else {
        high_priority_buffer_to_radio[1443]=radio->adc[0].attenuation;
        high_priority_buffer_to_radio[1442]=radio->adc[1].attenuation;
      }
    }

    int rc;
    if((rc=sendto(data_socket,high_priority_buffer_to_radio,sizeof(high_priority_buffer_to_radio),0,(struct sockaddr*)&high_priority_addr,high_priority_addr_length))<0) {
        log_error("sendto socket failed for high priority: rc=%d errno=%d\n",rc,errno);
        //abort();
    }

    high_priority_sequence++;
}

static void protocol2_transmit_specific(void) {
    int mode;

    memset(transmit_specific_buffer, 0, sizeof(transmit_specific_buffer));

    transmit_specific_buffer[0]=tx_specific_sequence>>24;
    transmit_specific_buffer[1]=tx_specific_sequence>>16;
    transmit_specific_buffer[2]=tx_specific_sequence>>8;
    transmit_specific_buffer[3]=tx_specific_sequence;

    mode=USB;
    if(radio!=NULL) {
      if(radio->transmitter!=NULL) {
        if(radio->transmitter->rx!=NULL) {
#ifdef USE_VFO_B_MODE_AND_FILTER
            if(radio->transmitter->rx->split) {
#endif
              mode=radio->transmitter->rx->mode_a;
#ifdef USE_VFO_B_MODE_AND_FILTER
            } else {
              mode=radio->transmitter->rx->mode_b;
            }
#endif
        }
      }
    }

    transmit_specific_buffer[4]=1; // 1 DAC
    transmit_specific_buffer[5]=0; //  default no CW
    // may be using local pihpsdr OR hpsdr CW
    if(radio!=NULL) {
      if(mode==CWU || mode==CWL) {
        transmit_specific_buffer[5]|=0x02;
      }
      if(radio->cw_keys_reversed) {
        transmit_specific_buffer[5]|=0x04;
      }
      if(radio->cw_keyer_mode==KEYER_MODE_A) {
        transmit_specific_buffer[5]|=0x08;
      }
      if(radio->cw_keyer_mode==KEYER_MODE_B) {
        transmit_specific_buffer[5]|=0x28;
      }
      if(radio->cw_keyer_sidetone_volume!=0) {
        transmit_specific_buffer[5]|=0x10;
      }
      if(radio->cw_keyer_spacing) {
        transmit_specific_buffer[5]|=0x40;
      }
      if(radio->cw_breakin) {
        transmit_specific_buffer[5]|=0x80;
      }

      transmit_specific_buffer[6]=radio->cw_keyer_sidetone_volume; // sidetone off
      transmit_specific_buffer[7]=radio->cw_keyer_sidetone_frequency>>8;
      transmit_specific_buffer[8]=radio->cw_keyer_sidetone_frequency; // sidetone frequency
      transmit_specific_buffer[9]=radio->cw_keyer_speed; // cw keyer speed
      transmit_specific_buffer[10]=radio->cw_keyer_weight; // cw weight
      transmit_specific_buffer[11]=radio->cw_keyer_hang_time>>8;
      transmit_specific_buffer[12]=radio->cw_keyer_hang_time; // cw hang delay
      transmit_specific_buffer[13]=0; // rf delay
      transmit_specific_buffer[50]=0;
      if(radio->mic_linein) {
        transmit_specific_buffer[50]|=0x01;
      }
      if(radio->mic_boost) {
        transmit_specific_buffer[50]|=0x02;
      }
      if(radio->mic_ptt_enabled==0) {  // set if disabled
        transmit_specific_buffer[50]|=0x04;
      }
      if(radio->mic_ptt_tip_bias_ring) {
        transmit_specific_buffer[50]|=0x08;
      }
      if(radio->mic_bias_enabled) {
        transmit_specific_buffer[50]|=0x10;
      }
    }

    if(radio!=NULL) {
      transmit_specific_buffer[51]=radio->linein_gain;
    }     

    if(sendto(data_socket,transmit_specific_buffer,sizeof(transmit_specific_buffer),0,(struct sockaddr*)&transmitter_addr,transmitter_addr_length)<0) {
        log_error("sendto socket failed for tx specific: sequence=%ld\n",tx_specific_sequence);
    }

    tx_specific_sequence++;

}

void protocol2_receive_specific(void) {
  int i;

  // receive_specific_buffer is ONE static block that is memset, filled and then
  // sent -- and this function is called from the 100 ms timer thread, from the
  // GTK thread (protocol2_start_receiver, add_diversity_mixer) and from
  // protocol2_start.  Without this lock two threads interleave a memset with
  // another's fill and a HALF-BUILT register block goes on the wire.  Observed,
  // not theorised: tools/p2_emu.c logs the DDC enable/sync map on change, and
  // during start-up it saw the diversity sync map appear, vanish and reappear
  // several times -- i.e. the radio was alternately told "DDC1 follows DDC0"
  // and "DDC1 is a free-running receiver".  Static so it needs no init.
  static GMutex rxspec_mutex;
  g_mutex_lock(&rxspec_mutex);

  memset(receive_specific_buffer, 0, sizeof(receive_specific_buffer));

  receive_specific_buffer[0]=rx_specific_sequence>>24;
  receive_specific_buffer[1]=rx_specific_sequence>>16;
  receive_specific_buffer[2]=rx_specific_sequence>>8;
  receive_specific_buffer[3]=rx_specific_sequence;

  receive_specific_buffer[4]=2; // 2 ADCs

  if(radio!=NULL) {
    for(i=0;i<2;i++) {
      receive_specific_buffer[5]|=radio->adc[i].dither<<i; // dither enable
      receive_specific_buffer[6]|=radio->adc[i].random<<i; // random enable
    }
    for(i=0;i<radio->discovered->supported_receivers;i++) {
      if(radio->receiver[i]!=NULL) {
        receive_specific_buffer[7]|=(1<<radio->receiver[i]->channel); // DDC enable
        receive_specific_buffer[17+(radio->receiver[i]->channel*6)]=radio->receiver[i]->adc;
        receive_specific_buffer[18+(radio->receiver[i]->channel*6)]=((radio->receiver[i]->sample_rate/1000)>>8)&0xFF;
        receive_specific_buffer[19+(radio->receiver[i]->channel*6)]=(radio->receiver[i]->sample_rate/1000)&0xFF;
        receive_specific_buffer[22+(radio->receiver[i]->channel*6)]=24;
      }
    }

    // --- Diversity: sync DDC1 to DDC0 ------------------------------------
    // Diversity combines two COHERENT streams, which Protocol 1 gets for free
    // (its receivers' samples arrive interleaved in one frame off one clock).
    // Protocol 2 normally gives each DDC its own UDP stream on its own port,
    // which is NOT coherent -- so a synchronised pair is asked for explicitly,
    // and the radio then answers in a different shape entirely:
    //
    //   openHPSDR Ethernet Protocol v3.5, bytes 1363..1442: "Sets the DDC that
    //   DDC (n) is synchronised or multiplexed with... All DDC's frequencies
    //   will be set to the frequency of the base DDC.  NOTE: For the time
    //   being, due to FPGA size limitations and timing closure issues only DDC0
    //   and DDC1 may be synchronised, WITH SYNCHRONISED DATA PRESENTED FROM
    //   DDC0's OUTPUT."
    //
    // Three consequences, all of them load-bearing:
    //
    //  * The pair is DDC0 + DDC1 and nothing else.  Hence the guard in
    //    diversity_dialog.c that refuses to enable diversity unless the visual
    //    receiver is channel 0 and the hidden one channel 1.
    //  * DDC1's ENABLE BIT IS LEFT CLEAR.  It is configured (ADC, rate, bits)
    //    but not enabled: it does not stream on its own port, it rides DDC0's.
    //    Setting the bit would ask for a stream nothing reads.
    //  * DDC0's packets then carry BOTH streams, sample-interleaved, at twice
    //    the sample count -- which is what process_div_iq_data() below unpacks.
    //
    // The sync byte is indexed by the BASE DDC and its bits name the FOLLOWERS
    // ("if bit set then DDC (n) is synched to DDC 0"), so DDC1-follows-DDC0 is
    // buffer[1363] |= 0x02, not buffer[1364] = 0x01.  pihpsdr and linhpsdr both
    // write the literal `receive_specific_buffer[1363] = 0x02`.
    //
    // ADC assignment: visual DDC on ADC 0, hidden on ADC 1 -- two ADCs, i.e.
    // two antennas, is the entire point.  create_receiver() already gives any
    // channel!=0 an ADC of 1 on a two-ADC board, so this mostly restates it;
    // stating it from the mixer means it cannot come apart.
    //
    // UNVERIFIED, and this is the half no emulator can settle: that real
    // gateware honours this register as the document describes, and that the
    // two ADCs of a given board are in fact phase-locked.  tools/p2_emu.c does
    // read the sync map and does interleave accordingly, so the app's half is
    // exercised end to end -- but the emulator was written from the same
    // document, so it can only catch this code drifting from that reading, not
    // a shared misreading of it.
    for(i=0;i<MAX_DIVERSITY_MIXERS;i++) {
      DIVMIXER *dmix=radio->divmixer[i];
      if(dmix==NULL) continue;
      RECEIVER *rxv=dmix->rx_visual;
      RECEIVER *rxh=dmix->rx_hidden;
      if(rxv==NULL || rxh==NULL) continue;
      int cv=rxv->channel;
      int ch=rxh->channel;
      // Only the DDC0/DDC1 pair may be synchronised (see above).  Anything
      // else is refused here rather than half-configured: the alternative is a
      // radio streaming two unsynchronised DDCs into a mixer that assumes they
      // are coherent, which looks like working diversity and is not.
      if(cv!=0 || ch!=1) continue;
      receive_specific_buffer[17+(cv*6)]=0;               // DDC0 -> ADC 0
      receive_specific_buffer[17+(ch*6)]=1;               // DDC1 -> ADC 1
      receive_specific_buffer[18+(ch*6)]=receive_specific_buffer[18+(cv*6)];
      receive_specific_buffer[19+(ch*6)]=receive_specific_buffer[19+(cv*6)];
      receive_specific_buffer[22+(ch*6)]=24;
      receive_specific_buffer[DDC_SYNC_BASE+cv]|=(1<<ch); // DDC1 follows DDC0
      receive_specific_buffer[7]&=~(1<<ch);               // ... and does not stream alone
    }
  }

#ifdef PURESIGNAL_P2
    // --- PureSignal feedback DDCs (EXPERIMENTAL / UNVERIFIED, see below) ------
    // During a PureSignal TX two extra DDCs carry the feedback the correction
    // loop needs: the DUC/pre-PA reference (what we asked the radio to send) and
    // the post-PA/ADC feedback (what the amplifier actually produced).  Reuse
    // the two RECEIVERs the transmitter already points at (rx_puresignal_txfbk /
    // rx_puresignal_rxfbk, set in transmitter_set_ps) rather than inventing new
    // dedicated feedback receiver structs (which this fork, unlike pihpsdr, does
    // not have).  Both DDCs run at the feedback receiver's sample rate and the
    // post-PA DDC is sync-slaved to the DUC DDC so the two streams stay
    // sample-aligned when process_ps_iq_data() pairs them into pscc().
    //
    // UNVERIFIED: the exact P2 sync-register layout, the feedback DDC indices
    // and the feedback rate are hardware-specific and have never been checked
    // against a real Orion2/ANAN P2 radio.  If the loop misbehaves, this block
    // and the discovery-time ps_tx_fdbk_chan assignment are the first suspects.
    if((radio->transmitter!=NULL) && (radio->transmitter->puresignal != NULL)
        && isTransmitting(radio)) {
      RECEIVER *txfbk=radio->transmitter->rx_puresignal_txfbk; // DUC / pre-PA reference
      RECEIVER *rxfbk=radio->transmitter->rx_puresignal_rxfbk; // post-PA / ADC feedback
      if(txfbk!=NULL && rxfbk!=NULL) {
        int ps_rate=txfbk->sample_rate;   // feedback rate (e.g. 192000)
        int ct=txfbk->channel;            // DUC feedback DDC index
        int cr=rxfbk->channel;            // post-PA feedback DDC index

        // post-PA / ADC feedback DDC
        receive_specific_buffer[5]|=radio->adc[rxfbk->adc].dither<<cr;
        receive_specific_buffer[6]|=radio->adc[rxfbk->adc].random<<cr;
        receive_specific_buffer[17+(cr*6)]=rxfbk->adc;
        receive_specific_buffer[18+(cr*6)]=((ps_rate/1000)>>8)&0xFF;
        receive_specific_buffer[19+(cr*6)]=(ps_rate/1000)&0xFF;
        receive_specific_buffer[22+(cr*6)]=24;

        // DUC / pre-PA reference DDC
        receive_specific_buffer[5]|=radio->adc[txfbk->adc].dither<<ct;
        receive_specific_buffer[6]|=radio->adc[txfbk->adc].random<<ct;
        receive_specific_buffer[17+(ct*6)]=txfbk->adc;
        receive_specific_buffer[18+(ct*6)]=((ps_rate/1000)>>8)&0xFF;
        receive_specific_buffer[19+(ct*6)]=(ps_rate/1000)&0xFF;
        receive_specific_buffer[22+(ct*6)]=24;

        // Sync the post-PA DDC to the DUC DDC.  The sync byte is indexed by the
        // BASE DDC and its bits name the FOLLOWERS, so this is [base] |= 1<<
        // follower; it was written the other way round ([cr] = 1<<ct), i.e. the
        // wrong byte AND the wrong bit, which for a DDC0/DDC1 pair says
        // "DDC0 follows DDC1" instead of the reverse.  See DDC_SYNC_BASE.
        //
        // TWO THINGS HERE ARE STILL WRONG and are NOT fixed by this change,
        // because PureSignal-over-P2 needs TX hardware to test and there is
        // none (see CLAUDE.md, Verification status).  Recorded so the next
        // person does not have to rediscover them:
        //   1. The spec allows ONLY DDC0/DDC1 to be synchronised, but
        //      ps_tx_fdbk_chan is 4 on Orion2 (protocol2_discovery.c), so this
        //      asks to sync DDC3 to DDC4.  pihpsdr pins the feedback pair to
        //      DDC0/DDC1 for exactly this reason.
        //   2. A synchronised pair is presented INTERLEAVED ON THE BASE DDC's
        //      port, so process_ps_iq_data()'s two-separate-streams model is
        //      the wrong shape -- the diversity path below now does it the
        //      other way, and PureSignal should follow it.
        receive_specific_buffer[DDC_SYNC_BASE+ct]|=(1<<cr);

        receive_specific_buffer[7]|=(1<<ct)|(1<<cr); // enable both feedback DDCs
      }
    }
#endif

//fprintf(stderr,"protocol2_receive_specific: enable=%02X\n",receive_specific_buffer[7]);
    if(sendto(data_socket,receive_specific_buffer,sizeof(receive_specific_buffer),0,(struct sockaddr*)&receiver_addr,receiver_addr_length)<0) {
      log_error("sendto socket failed for receive_specific: sequence=%ld\n",rx_specific_sequence);
    }
    rx_specific_sequence++;
    g_mutex_unlock(&rxspec_mutex);
}

static void protocol2_start(void) {
    protocol2_transmit_specific();
    protocol2_receive_specific();
    protocol2_timer_thread_id = g_thread_new( "protocol2 timer", protocol2_timer_thread, NULL);
    if( ! protocol2_timer_thread_id )
    {
        log_error("g_thread_new failed on protocol2_timer_thread\n");
        exit( -1 );
    }
    log_info("protocol2_timer_thread: id=%p\n",protocol2_timer_thread_id);

}

void protocol2_stop(void) {
    running=0;
    protocol2_high_priority();
    usleep(100000); // 100 ms
    //_exit(0);
}

void protocol2_run(void) {
    protocol2_high_priority();
}

// In-place restart after a disconnect: stop the current data thread and relaunch
// protocol2_thread, which re-creates the socket and re-issues the general/start/
// high-priority sequence (and its timer thread).  Runs on the GTK main thread.
void protocol2_reconnect(void) {
    log_info("protocol2_reconnect\n");
    running=0;
    if(protocol2_thread_id!=NULL) {
        g_thread_join(protocol2_thread_id);  // returns within one SO_RCVTIMEO period
        protocol2_thread_id=NULL;
    }
    // The register timer thread is started by protocol2_start(), which runs
    // INSIDE protocol2_thread -- so every reconnect starts another one.  It must
    // be joined here, not merely left to notice running==0: it only re-checks
    // between 100 ms sleeps, and the restarted data thread sets running back to
    // TRUE, so an old timer that had not woken yet survives for the life of the
    // process.  Each survivor sends its own transmit_specific/receive_specific
    // block every 100 ms over the same socket, i.e. one extra register writer
    // per reconnect racing the others on receive_specific_buffer.
    if(protocol2_timer_thread_id!=NULL) {
        g_thread_join(protocol2_timer_thread_id);   // exits within one 100 ms tick
        protocol2_timer_thread_id=NULL;
    }
    if(data_socket>=0) {
        closesocket(data_socket);
        data_socket=-1;
    }
    protocol2_thread_id = g_thread_new( "protocol2", protocol2_thread, NULL);
    if( ! protocol2_thread_id ) {
        log_error("g_thread_new failed on protocol2_thread (reconnect)\n");
    }
}

double calibrate(int v) {
    // Angelia
    double v1;
    v1=(double)v/4095.0*3.3;

    return (v1*v1)/0.095;
}

static gpointer protocol2_thread(gpointer data) {

    int i;
    int ddc;
    //short sourceport;
    static unsigned char *buffer;
    int bytesread;

log_info("protocol2_thread\n");

    micsamples=0;
    iqindex=4;

    data_socket=socket(PF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(data_socket<0) {
        log_error("protocol2_thread: create socket failed for data_socket\n");
        exit(-1);
    }

    int optval = 1;
    setsockopt(data_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#ifdef SO_REUSEPORT   // no Winsock equivalent; SO_REUSEADDR above covers Windows
    setsockopt(data_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#endif

    // Receive timeout so recvfrom() returns periodically: lets the thread notice
    // running==FALSE (clean stop/reconnect) and lets the disconnect watchdog see
    // the data gap instead of the thread blocking forever on a dead radio.
    net_set_rcvtimeo(data_socket, 1000);

    // bind to the interface
    if(bind(data_socket,(struct sockaddr*)&radio->discovered->info.network.interface_address,radio->discovered->info.network.interface_length)<0) {
        log_error("protocol2_thread: bind socket failed for data_socket\n");
        exit(-1);
    }

log_info("protocol2_thread: data_socket bound to interface\n");

    memcpy(&base_addr,&radio->discovered->info.network.address,radio->discovered->info.network.address_length);
    base_addr_length=radio->discovered->info.network.address_length;
    base_addr.sin_port=htons(GENERAL_REGISTERS_FROM_HOST_PORT);

    memcpy(&receiver_addr,&radio->discovered->info.network.address,radio->discovered->info.network.address_length);
    receiver_addr_length=radio->discovered->info.network.address_length;
    receiver_addr.sin_port=htons(RECEIVER_SPECIFIC_REGISTERS_FROM_HOST_PORT);

    memcpy(&transmitter_addr,&radio->discovered->info.network.address,radio->discovered->info.network.address_length);
    transmitter_addr_length=radio->discovered->info.network.address_length;
    transmitter_addr.sin_port=htons(TRANSMITTER_SPECIFIC_REGISTERS_FROM_HOST_PORT);

    memcpy(&high_priority_addr,&radio->discovered->info.network.address,radio->discovered->info.network.address_length);
    high_priority_addr_length=radio->discovered->info.network.address_length;
    high_priority_addr.sin_port=htons(HIGH_PRIORITY_FROM_HOST_PORT);

log_info("protocol2_thread: high_priority_addr setup for port %d\n",HIGH_PRIORITY_FROM_HOST_PORT);

    memcpy(&audio_addr,&radio->discovered->info.network.address,radio->discovered->info.network.address_length);
    audio_addr_length=radio->discovered->info.network.address_length;
    audio_addr.sin_port=htons(AUDIO_FROM_HOST_PORT);

    memcpy(&iq_addr,&radio->discovered->info.network.address,radio->discovered->info.network.address_length);
    iq_addr_length=radio->discovered->info.network.address_length;
    iq_addr.sin_port=htons(TX_IQ_FROM_HOST_PORT);

   
    for(i=0;i<radio->discovered->supported_receivers;i++) {
        memcpy(&data_addr[i],&radio->discovered->info.network.address,radio->discovered->info.network.address_length);
        data_addr_length[i]=radio->discovered->info.network.address_length;
        data_addr[i].sin_port=htons(RX_IQ_TO_HOST_PORT_0+i);
        samples[i]=0;
    }

    audioindex=4; // leave space for sequence
    audiosequence=0L;

    running=TRUE;
    protocol2_general();
    protocol2_start();
    protocol2_high_priority();

    while(running) {

        buffer=malloc(NET_BUFFER_SIZE);
        length=sizeof(struct sockaddr_in);
        bytesread=recvfrom(data_socket,buffer,NET_BUFFER_SIZE,0,(struct sockaddr*)&addr,&length);
        if(bytesread<0) {
            // EAGAIN/EWOULDBLOCK is the normal SO_RCVTIMEO expiry - the radio
            // just went quiet.  Loop back (re-checking running) and let the
            // disconnect watchdog decide, rather than killing the whole app.
            free(buffer);
            if(errno!=EAGAIN && errno!=EWOULDBLOCK) {
                log_error("recvfrom socket failed for protocol2_thread: %s\n",strerror(errno));
            }
            continue;
        }
        reconnect_note_data();   // a packet arrived: the radio is alive

        int sourceport=ntohs(addr.sin_port);

        switch(sourceport) {
            case RX_IQ_TO_HOST_PORT_0:
            case RX_IQ_TO_HOST_PORT_1:
            case RX_IQ_TO_HOST_PORT_2:
            case RX_IQ_TO_HOST_PORT_3:
            case RX_IQ_TO_HOST_PORT_4:
            case RX_IQ_TO_HOST_PORT_5:
            case RX_IQ_TO_HOST_PORT_6:
            case RX_IQ_TO_HOST_PORT_7:
              ddc=sourceport-RX_IQ_TO_HOST_PORT_0;
#ifdef PURESIGNAL_P2
              // While a PureSignal TX is running the two feedback DDCs arrive on
              // their own IQ ports; steer them into the feedback pairing path
              // instead of the normal per-receiver demod. (UNVERIFIED — the
              // feedback DDC indices are hardware-specific.)
              if(isTransmitting(radio) && radio->transmitter!=NULL
                  && radio->transmitter->puresignal!=NULL) {
                RECEIVER *txfbk=radio->transmitter->rx_puresignal_txfbk;
                RECEIVER *rxfbk=radio->transmitter->rx_puresignal_rxfbk;
                if(txfbk!=NULL && ddc==txfbk->channel) {
                  process_ps_iq_data(txfbk,bytesread,buffer);
                  free(buffer);
                  break;
                }
                if(rxfbk!=NULL && ddc==rxfbk->channel) {
                  process_ps_iq_data(rxfbk,bytesread,buffer);
                  free(buffer);
                  break;
                }
              }
#endif
              if(ddc>=radio->discovered->supported_receivers)  {
                log_info("unexpected iq data from ddc %d\n",ddc);
              } else {
                // delete_receiver frees the receiver, so the slot is read and
                // used under delete_rx_mutex -- the lock protocol1 has always
                // taken around its own add_iq_samples() and this path never
                // did.  process_iq_data() ends in add_iq_samples(), i.e. it
                // walks the receiver's buffers and its WDSP channel.
                g_mutex_lock(&radio->delete_rx_mutex);
                RECEIVER *rx=radio->receiver[ddc];
                if(rx!=NULL) {
                  // A synchronised DDC pair is presented on the BASE DDC's
                  // port with both streams interleaved (see the diversity
                  // block in protocol2_receive_specific), so this receiver's
                  // packets carry its hidden partner's samples too.
                  // rx->dmix_id's "none" value is a permanently-NULL sentinel
                  // slot, so this needs no bounds test.
                  DIVMIXER *dmix=radio->divmixer[rx->dmix_id];
                  if(dmix!=NULL && dmix->rx_visual==rx && dmix->rx_hidden!=NULL) {
                    process_div_iq_data(dmix,bytesread,buffer);
                  } else {
                    process_iq_data(rx,bytesread,buffer);
                  }
                }
                g_mutex_unlock(&radio->delete_rx_mutex);
              }
              free(buffer);
              break;
            case WIDE_BAND_TO_HOST_PORT:
              // Freed either way: a board keeps streaming until the next general
              // packet clears the enable, and delete_wideband() NULLs the pointer
              // as soon as the window closes -- so the packets that arrive in
              // that gap used to leak one NET_BUFFER_SIZE block each.
              if(radio->wideband!=NULL) {
                process_wideband_data(radio->wideband,bytesread,buffer);
              }
              free(buffer);
              break;
            case COMMAND_RESPONCE_TO_HOST_PORT:
              process_command_response(buffer);
              free(buffer);
              break;
            case HIGH_PRIORITY_TO_HOST_PORT:
              process_high_priority(buffer);
              free(buffer);
              break;
            case MIC_LINE_TO_HOST_PORT:
              process_mic_data(bytesread,buffer);
              free(buffer);
              break;
            default:
log_info("protocol2_thread: Unknown port %d free %p\n",sourceport,buffer);
              free(buffer);
              break;
        }
    }

    closesocket(data_socket);
    return NULL;
}

// One 24-bit two's-complement sample, MSB first, normalised to +-1.
// *65536 rather than <<16: a left shift of a NEGATIVE value is undefined
// behaviour and half of every I/Q sample is negative (see process_iq_data).
static inline double p2_sample24(const unsigned char *p) {
  int s  = (int)(signed char)p[0]*65536;
  s     |= (int)(((unsigned int)p[1]<<8)&0xFF00);
  s     |= (int)((unsigned int)p[2]&0xFF);
  return (double)s/8388607.0;   // 2^23-1, see the note in process_iq_data
}

// A SYNCHRONISED DDC PAIR ARRIVES AS ONE STREAM.
//
// Protocol 2 normally gives every DDC its own UDP port, and that is what
// process_iq_data() above handles.  A pair synchronised through the sync map
// is different in shape, not just in timing: the spec says the data is
// "presented from DDC0's output", i.e. one packet on the base DDC's port
// carrying both streams SAMPLE-INTERLEAVED -- base, follower, base, follower --
// with the frame's sample count covering both, so the loop steps by two.
// pihpsdr's process_div_iq_data() is the same function and was the reference.
//
// This is the whole reason the diversity register block enables only DDC0: a
// second port would deliver nothing, and reading the pair off two ports would
// be reading two free-running DDCs and calling them coherent.
//
// The visual receiver is fed FIRST and the hidden one second, and that order
// matters: add_iq_samples() runs the mixer when the higher-numbered channel's
// buffer completes, so the visual buffer must already be copied into the
// mixer's stream 0 (diversity_add_buffer) when the hidden one triggers
// diversity_mix_full_buffers().  Feeding them the other way round would mix
// this block of hidden against the PREVIOUS block of visual -- one buffer of
// skew, which is not a crash and not visible on a panadapter, it just quietly
// destroys the coherence the whole feature rests on.
//
// Called with radio->delete_rx_mutex held (the caller in protocol2_thread
// takes it), so both receivers are alive for the duration.
/* The wire's sample count, clamped to what actually ARRIVED.  Bytes 14..15 are
   a 16-bit field, so a radio that miscounts -- or anything else on the LAN,
   since the dispatch is on source port alone -- can ask for 65535 samples of 6
   bytes out of a 2048-byte block: a ~384 kB heap over-read whose values go
   straight into add_iq_samples.  A short datagram is the other half of the
   same question: the count itself would then be read out of uninitialised
   malloc memory, so anything below one sample is dropped. */
static int p2_iq_samples(int bytes,unsigned char *buffer) {
  int n=((buffer[14]&0xFF)<<8)+(buffer[15]&0xFF);
  int fits=(bytes-16)/6;
  if(bytes<16+6) return 0;
  if(n>fits) n=fits;
  if(n<0) n=0;
  return n;
}

static void process_div_iq_data(DIVMIXER *dmix,int bytes,unsigned char *buffer) {
  if(buffer==NULL) return;
  RECEIVER *rxv=dmix->rx_visual;
  RECEIVER *rxh=dmix->rx_hidden;

  long sequence=((buffer[0]&0xFF)<<24)+((buffer[1]&0xFF)<<16)+((buffer[2]&0xFF)<<8)+(buffer[3]&0xFF);
  if(rxv->iq_sequence!=sequence) {
    rxv->iq_sequence=sequence;
  }
  rxv->iq_sequence++;

  // 6 bytes an I/Q sample after the 16-byte header; never walk past the packet
  // even if a radio (or an emulator) lies about the count.
  int samplesperframe=p2_iq_samples(bytes,buffer);

  int b=16;
  for(int i=0;i+1<samplesperframe;i+=2) {
    double lv=p2_sample24(&buffer[b]); b+=3;
    double rv=p2_sample24(&buffer[b]); b+=3;
    double lh=p2_sample24(&buffer[b]); b+=3;
    double rh=p2_sample24(&buffer[b]); b+=3;
    add_iq_samples(rxv,lv,rv);
    add_iq_samples(rxh,lh,rh);
  }
}

static void process_iq_data(RECEIVER *rx,int bytes,unsigned char *buffer) {
  long sequence;
  /*
  long long timestamp;
  int bitspersample;
  */
  
  int samplesperframe;
  int b;
  int leftsample;
  int rightsample;
  double leftsampledouble;
  double rightsampledouble;
  //unsigned char *buffer;

  //buffer=iq_buffer[rx->channel];

  if(buffer!=NULL) {
  sequence=((buffer[0]&0xFF)<<24)+((buffer[1]&0xFF)<<16)+((buffer[2]&0xFF)<<8)+(buffer[3]&0xFF);
  

  if(rx->iq_sequence!=sequence) {
    //fprintf(stderr,"rx %d sequence error: expected %ld got %ld\n",rx->channel,rx->iq_sequence,sequence);
    rx->iq_sequence=sequence;
  }
  rx->iq_sequence++;

  /* UNUSED
  timestamp=((long long)(buffer[4]&0xFF)<<56)+((long long)(buffer[5]&0xFF)<<48)+((long long)(buffer[6]&0xFF)<<40)+((long long)(buffer[7]&0xFF)<<32)+
  ((long long)(buffer[8]&0xFF)<<24)+((long long)(buffer[9]&0xFF)<<16)+((long long)(buffer[10]&0xFF)<<8)+(long long)(buffer[11]&0xFF);

  bitspersample=((buffer[12]&0xFF)<<8)+(buffer[13]&0xFF);
  */
  samplesperframe=p2_iq_samples(bytes,buffer);

//fprintf(stderr,"process_iq_data: rx=%d seq=%ld bitspersample=%d samplesperframe=%d\n",rx->id, sequence,bitspersample,samplesperframe);
  b=16;
  int i;
  for(i=0;i<samplesperframe;i++) {
    // *65536 rather than <<16 throughout this file: a left shift of a NEGATIVE
    // value is undefined behaviour and half of every I/Q sample is negative.
    // Identical arithmetic, defined. The protocol-1 twin of this was caught by
    // UBSan against tools/metis_emu.c; these are the same expression and are
    // corrected with it, unverified only for want of a Protocol-2 board.
    leftsample   = (int)(signed char)buffer[b++]*65536;
    leftsample  |= (int)((((unsigned char)buffer[b++])<<8)&0xFF00);
    leftsample  |= (int)((unsigned char)buffer[b++]&0xFF);
    rightsample  = (int)(signed char)buffer[b++]*65536;
    rightsample |= (int)((((unsigned char)buffer[b++])<<8)&0xFF00);
    rightsample |= (int)((unsigned char)buffer[b++]&0xFF);

    // Full scale for a 24-bit two's-complement sample is 2^23-1, NOT 2^24-1.
    // This divided by 16777215.0 (with the correct line sitting commented out
    // right above it, which is how it arrived from LinHPSDR and how it still
    // reads upstream today), so every Protocol-2 DDC sample entered the DSP at
    // HALF amplitude -- 6 dB below Protocol 1 for the identical wire code, and
    // 6 dB below this same file's own PureSignal feedback unpacking a hundred
    // lines down, which always used 8388607.0.
    //
    // Measured against tools/p2_emu.c: a tone sent at 0.4 of 24-bit full scale
    // came back as |z| = 0.199999.  Cross-checked against every independent
    // implementation reachable -- dl1ycf/pihpsdr uses 1/2^23 in BOTH protocols
    // (spelled 1.1920928955078125E-7, with a comment saying so), g0orx/pihpsdr
    // divides by 8388608.0, and this tree's own protocol1.c uses 8388607.0.
    // Nothing uses 2^24, and the openHPSDR Ethernet Protocol v3.5 document
    // mandates no scale at all: it defines only "signed 2's complement, 24
    // bits per sample" and leaves normalisation to the client, so there is no
    // spec-level headroom convention this could have been implementing.
    //
    // FOR THE OPERATOR: this moves Protocol-2 S-meter and panadapter readings
    // up by 6 dB.  They were the ones that were wrong -- P2 and P1 now agree,
    // as do the RX and PureSignal paths within this file -- but a P2 operator
    // with a remembered noise floor or a calibration offset will need to
    // re-set it once.  Still UNVERIFIED against real P2 hardware (there is
    // none here); the evidence is the emulator, the arithmetic and four
    // independent implementations.
    leftsampledouble=(double)leftsample/8388607.0; // for 24 bits, 2^23-1
    rightsampledouble=(double)rightsample/8388607.0; // for 24 bits, 2^23-1

    add_iq_samples(rx, leftsampledouble,rightsampledouble);
  }
  }
}

#ifdef PURESIGNAL_P2
// Decode one feedback-DDC packet (single 24-bit I/Q stream) into its feedback
// receiver's iq_input_buffer, then — once the DUC/reference buffer is full —
// hand the paired reference + post-PA blocks to WDSP's pscc() correction pass.
//
// This is the P2 analogue of add_ps_iq_samples() (transmitter.c), but the two
// feedback streams arrive as two SEPARATE synced DDC packet streams rather than
// interleaved in one Protocol-1 frame, so each is accumulated independently and
// the DUC stream drives the pscc() trigger.  The post-PA (rxfbk) stream is
// assumed to advance in lock-step because its DDC is sync-slaved to the DUC DDC
// in protocol2_receive_specific().
//
// UNVERIFIED: never run against a real P2 feedback ADC.  If the correction loop
// diverges, suspect (a) the two streams drifting out of lock-step here, (b) the
// I/Q sign/scaling convention, or (c) the sync-register setup upstream.
static void process_ps_iq_data(RECEIVER *fbk,int bytes,unsigned char *buffer) {
  TRANSMITTER *tx=radio->transmitter;
  if(tx==NULL) return;

  int samplesperframe=p2_iq_samples(bytes,buffer);
  int b=16;

  for(int i=0;i<samplesperframe;i++) {
    int isample  = (int)(signed char)buffer[b++]*65536;
    isample     |= (int)((((unsigned char)buffer[b++])<<8)&0xFF00);
    isample     |= (int)((unsigned char)buffer[b++]&0xFF);
    int qsample  = (int)(signed char)buffer[b++]*65536;
    qsample     |= (int)((((unsigned char)buffer[b++])<<8)&0xFF00);
    qsample     |= (int)((unsigned char)buffer[b++]&0xFF);

    double id=(double)isample/8388607.0; // 24-bit
    double qd=(double)qsample/8388607.0;

    if(fbk->samples < fbk->buffer_size) {
      fbk->iq_input_buffer[fbk->samples*2]   = id;
      fbk->iq_input_buffer[fbk->samples*2+1] = qd;
      fbk->samples++;
    }

    // The DUC/reference stream drives the correction pass: when its block is
    // complete, run pscc() with both blocks and reset the pair.
    if(fbk==tx->rx_puresignal_txfbk && fbk->samples>=fbk->buffer_size) {
      RECEIVER *rxfbk=tx->rx_puresignal_rxfbk;
      if(rxfbk!=NULL && rxfbk->samples>=fbk->buffer_size && isTransmitting(radio)) {
        pscc(tx->channel, fbk->buffer_size,
             tx->rx_puresignal_txfbk->iq_input_buffer,
             rxfbk->iq_input_buffer);
      }
      fbk->samples=0;
      if(rxfbk!=NULL) rxfbk->samples=0;
    }
  }
}
#endif

static void process_wideband_data(WIDEBAND *w,int bytes,unsigned char *buffer) {
  //long sequence; // UNUSED
  int b;
  int sample;
  double sampledouble;

  //sequence=((buffer[0]&0xFF)<<24)+((buffer[1]&0xFF)<<16)+((buffer[2]&0xFF)<<8)+(buffer[3]&0xFF); // UNUSED

  // Driven by the datagram LENGTH, not by the fixed 1028 this used to walk.  The
  // packet carries the samples-per-packet the general registers asked for
  // (WIDEBAND_SAMPLES_PER_PACKET, byte 24..25), and a board sends that many
  // 16-bit samples after the 4-byte sequence -- so a constant reads 1024 bytes
  // out of a shorter datagram and silently drops the tail of a longer one.
  b=4;
  while(b+1<bytes) {
    sample   = (int)(signed char)buffer[b++]*256;
    sample  |= (int)((unsigned char)buffer[b++]&0xFF);
    sampledouble=(double)sample/32767.0; // for 16 bits
    add_wideband_sample(w, sampledouble);
  }
}

static void process_command_response(unsigned char *buffer) {
    response_sequence=((buffer[0]&0xFF)<<24)+((buffer[1]&0xFF)<<16)+((buffer[2]&0xFF)<<8)+(buffer[3]&0xFF);
    response=buffer[4]&0xFF;
    log_info("response_sequence=%ld response=%d\n",response_sequence,response);
}

static void process_high_priority(unsigned char *buffer) {


    int previous_ptt;
    /* UNUSED
    long sequence; 
    gint mode;
    int previous_dot;
    int previous_dash;
    */ 



    previous_ptt=radio->ptt;
    /* UNUSED
    int channel=radio->transmitter->rx->channel; 
    sequence=((buffer[0]&0xFF)<<24)+((buffer[1]&0xFF)<<16)+((buffer[2]&0xFF)<<8)+(buffer[3]&0xFF);
    previous_dot=radio->dot;
    previous_dash=radio->dash;
    */

    radio->ptt=buffer[4]&0x01;
    radio->dot=(buffer[4]>>1)&0x01;
    radio->dash=(buffer[4]>>2)&0x01;

    radio->pll_locked=(buffer[4]>>3)&0x01;


    radio->adc_overload=buffer[5]&0x01;
    radio->transmitter->exciter_power=((buffer[6]&0xFF)<<8)|(buffer[7]&0xFF);
    radio->transmitter->alex_forward_power=((buffer[14]&0xFF)<<8)|(buffer[15]&0xFF);
    radio->transmitter->alex_reverse_power=((buffer[22]&0xFF)<<8)|(buffer[23]&0xFF);
    radio->supply_volts=((buffer[49]&0xFF)<<8)|(buffer[50]&0xFF);

    /* UNUSED
    if(radio->transmitter->rx->split) {
      mode=radio->transmitter->rx->mode_a;
    } else {
      mode=radio->transmitter->rx->mode_b;
    }
    */
    gint tx_mode=USB;
    RECEIVER *tx_receiver=radio->transmitter->rx;
    if(tx_receiver!=NULL) {
#ifdef USE_VFO_B_MODE_AND_FILTER
      if(radio->transmitter->rx->split) {
        tx_mode=tx_receiver->mode_b;
      } else {
#endif
        tx_mode=tx_receiver->mode_a;
#ifdef USE_VFO_B_MODE_AND_FILTER
      }
#endif
    }

    radio->local_ptt=radio->ptt;
    if(tx_mode==CWL || tx_mode==CWU) {
      radio->local_ptt=radio->ptt|radio->dot|radio->dash;
    }
    if(previous_ptt!=radio->local_ptt) {
      g_idle_add(ext_ptt_changed,(gpointer)radio);
    }

}

static void process_mic_data(int bytes,unsigned char *buffer) {
  // long sequence; //UNUSED
  int b;
  int i;
  short sample;

  //sequence=((buffer[0]&0xFF)<<24)+((buffer[1]&0xFF)<<16)+((buffer[2]&0xFF)<<8)+(buffer[3]&0xFF); // UNUSED
  b=4;
  for(i=0;i<MIC_SAMPLES;i++) {
    sample=(short)(buffer[b++]<<8);
    sample|=(buffer[b++]&0xFF);
    add_mic_sample(radio->transmitter,(float)sample/32768.0);
  }
}

void protocol2_process_local_mic(RADIO *r) {
  int i;
  short sample;

  for(i=0;i<r->local_microphone_buffer_size;i++) {
    add_mic_sample(r->transmitter,r->local_microphone_buffer[i]);
  }
}


void protocol2_audio_samples(RECEIVER *rx,short left_audio_sample,short right_audio_sample) {
  int rc;

  // insert the samples
  audiobuffer[audioindex++]=left_audio_sample>>8;
  audiobuffer[audioindex++]=left_audio_sample;
  audiobuffer[audioindex++]=right_audio_sample>>8;
  audiobuffer[audioindex++]=right_audio_sample;
            
  if(audioindex>=sizeof(audiobuffer)) {

    // insert the sequence
    audiobuffer[0]=audiosequence>>24;
    audiobuffer[1]=audiosequence>>16;
    audiobuffer[2]=audiosequence>>8;
    audiobuffer[3]=audiosequence;

    // send the buffer

    rc=sendto(data_socket,audiobuffer,sizeof(audiobuffer),0,(struct sockaddr*)&audio_addr,audio_addr_length);
    if(rc!=sizeof(audiobuffer)) {
      log_error("sendto socket failed for %ld bytes of audio: %d\n",(long)sizeof(audiobuffer),rc);
    }
    audiosequence++;
    audioindex=4;
  }
}

void protocol2_iq_samples(int isample,int qsample) {
  iqbuffer[iqindex++]=isample>>16;
  iqbuffer[iqindex++]=isample>>8;
  iqbuffer[iqindex++]=isample;
  iqbuffer[iqindex++]=qsample>>16;
  iqbuffer[iqindex++]=qsample>>8;
  iqbuffer[iqindex++]=qsample;

  if(iqindex==sizeof(iqbuffer)) {
    iqbuffer[0]=tx_iq_sequence>>24;
    iqbuffer[1]=tx_iq_sequence>>16;
    iqbuffer[2]=tx_iq_sequence>>8;
    iqbuffer[3]=tx_iq_sequence;

    // send the buffer
    if(sendto(data_socket,iqbuffer,sizeof(iqbuffer),0,(struct sockaddr*)&iq_addr,iq_addr_length)<0) {
      log_error("sendto socket failed for iq: sequence=%ld\n",tx_iq_sequence);
    }
    tx_iq_sequence++;
    iqindex=4;
  }
}

void* protocol2_timer_thread(void* arg) {
  // int specific=0; // UNUSED
log_info("protocol2_timer_thread\n");
  while(running) {
    usleep(100000); // 100ms
    // Both walk radio->receiver[], radio->divmixer[] and transmitter->rx from
    // THIS thread, ten times a second, while the GTK thread can be inside
    // delete_receiver() freeing exactly those -- the "sampled the pointer
    // outside the lock" shape protocol1 was fixed for.
    //
    // The lock goes HERE and not inside the two functions: delete_receiver_locked
    // reaches protocol2_receive_specific() (via the PureSignal reopen,
    // add_receiver -> protocol2_start_receiver) while already holding it, and a
    // non-recursive GMutex re-locked on the same thread is the self-deadlock
    // protocol 1's output path taught. Every other caller is the GTK thread,
    // which is the only thread that deletes, so it needs nothing.
    if(radio!=NULL) {
      g_mutex_lock(&radio->delete_rx_mutex);
      protocol2_transmit_specific();
      protocol2_receive_specific();
      g_mutex_unlock(&radio->delete_rx_mutex);
    }
  }
  return NULL;
}

gboolean protocol2_is_running(void) {
  return running;
}
