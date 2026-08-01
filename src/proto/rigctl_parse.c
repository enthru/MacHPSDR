/* CAT-command parser for TS-2000 emulation (rigctl)
 *
 * Split out of rigctl.c: this file holds the two big command parsers
 * (parse_cmd / parse_extended_cmd) and the small helpers used only by
 * them (step_size, cat_band, s_meter_level, ts2000_mode, send_resp).
 * See rigctl.c for the server/client plumbing, globals, and the
 * original license header.
 */
#include <gtk/gtk.h>
#include "log.h"
#include <gdk/gdk.h>
#include <errno.h>
#include <fcntl.h> 
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <math.h>

#include <wdsp.h>

#include "receiver.h"
//#include "band_menu.h"
#include "rigctl.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "transmitter.h"
#include "receiver.h"
#include "wideband.h"
#include "radio.h"
#include "channel.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "bandstack.h"
//#include "filter_menu.h"
#include "vfo.h"
//#include "sliders.h"
#include "transmitter.h"
#include "agc.h"
//#include "store.h"
#include "ext.h"
//#include "rigctl_menu.h"
//#include "noise_menu.h"
#include "protocol1.h"
#ifdef LOCALCW
#include "iambic.h"              // declare keyer_update()
#endif
#include "main.h"
#include "subrx.h"

#include "rigctl_internal.h"

#define NEW_PARSER

static int step_size(RECEIVER *rx) {
  int i=0;
  for(i=0;i<=14;i++) {
    if(steps[i]==rx->step) break;
  }
  if(i>14) i=0;
  return i+1;
}

static int cat_band(RECEIVER *rx) {
  int b;
  switch(rx->band_a) {
    case band2200:
      b=888; //2200;
      break;
    case band630:
      b=888; //630;
      break;
    case band160:
      b=160;
      break;
    case band80:
      b=80;
      break;
    case band60:
      b=60;
      break;
    case band40:
      b=40;
      break;
    case band30:
      b=30;
      break;
    case band20:
      b=20;
      break;
    case band17:
      b=17;
      break;
    case band15:
      b=15;
      break;
    case band12:
      b=12;
      break;
    case band10:
      b=10;
      break;
    case band6:
      b=6;
      break;
    case bandGen:
      b=888;
      break;
    case bandWWV:
      b=999;
      break;
    default:
      b=20;
      break;
  }
  return b;
}

static double s_meter_level(RECEIVER *rx) {
  double attenuation = radio->adc[rx->adc].attenuation;
  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
    attenuation = attenuation * -1;
  }
  double level=rx->meter_db+attenuation;
  return level;
}

static int ts2000_mode(int m) {
  int mode=1;
  switch(m) {
    case LSB:
      mode=1;
      break;
    case USB:
      mode=2;
      break;
    case CWL:
      mode=7;
      break;
    case CWU:
      mode=3;
      break;
    case FMN:
      mode=4;
      break;
    case AM:
    case SAM:
      mode=5;
      break;
    case DIGL:
      mode=6;
      break;
    case DIGU:
      mode=9;
      break;
    default:
      break;
  }
  return mode;
}

// Looks up entry INDEX_NUM in the command structure and
// returns the command string
//
static void send_resp(COMMAND *cmd,char * msg) {
  RECEIVER *rx=cmd->rx;
  RIGCTL *rigctl=rx->rigctl;

  if(rigctl->debug) log_info("%s: fd=%d RESP=%s\n",__FUNCTION__,cmd->fd,msg);
  int length=strlen(msg);
  int written=0;
  while(written<length) {
    ssize_t n=write(cmd->fd,&msg[written],length-written);
    if(n<=0) {
      // A failed write() returns -1; the old `written+=n` then spun forever on a
      // dead/errored fd — and this runs on the GTK main thread, so it froze the
      // whole UI. Retry on EINTR, otherwise give up on this response.
      if(n<0 && errno==EINTR) continue;
      break;
    }
    written+=(int)n;
  }
}

gboolean parse_extended_cmd(COMMAND *cmd) {
  RECEIVER *rx=cmd->rx;
  RIGCTL *rigctl=rx->rigctl;
  char *command=cmd->command;
  gboolean implemented=TRUE;
  char reply[256];
  reply[0]='\0';
  switch(command[2]) {
    case 'A': //ZZAx
      switch(command[3]) {
        case 'A': //ZZAA
          implemented=FALSE;
          break;
        case 'B': //ZZAB
          implemented=FALSE;
          break;
        case 'C': //ZZAC
          // sets or reads the Step Size
          if(command[4]==';') {
            sprintf(reply,"ZZAC%02d;",step_size(rx));
            send_resp(cmd,reply) ;
          } else if(command[6]==';') {
            // set the step size
            int i=atoi(&command[4]) ;
            if(i>0 && i<=14) {
              rx->step=steps[i+1];
              update_vfo(rx);
            }
          } else {
          }
          break;
        case 'D': //ZZAD
          // move VFO A down by selected step
          if(command[6]==';') {
            int step_index=atoi(&command[4]);
            long long hz=0;
            if(step_index>0 && step_index<=14) {
              hz=(long long)steps[step_index-1];
            }
            if(hz!=0LL) {
              receiver_move(rx,-hz,FALSE);
            }
          } else {
          }
          break;
        case 'E': //ZZAE
          // move VFO A down nn tune steps
          if(command[6]==';') {
            int steps=-atoi(&command[4]);
            receiver_move(rx,rx->step*steps,TRUE);
          }
          break;
        case 'F': //ZZAF
          // move VFO A up nn tune steps
          if(command[6]==';') {
            int steps=atoi(&command[4]);
            receiver_move(rx,rx->step*steps,TRUE);
          }
          break;
        case 'G': //ZZAG
          // read/set audio gain
          if(command[4]==';') {
            // send reply back
            sprintf(reply,"ZZAG%03d;",(int)(rx->volume*100.0));
            send_resp(cmd,reply) ;
          } else {
            int gain=atoi(&command[4]);
            rx->volume=(double)gain/100.0;
            receiver_set_volume(rx);
	    update_vfo(rx);
          }
          break;
        case 'I': //ZZAI
          implemented=FALSE;
          break;
        case 'P': //ZZAP
          implemented=FALSE;
          break;
        case 'R': //ZZAR
          // read/set RX0 AGC Threshold
          if(command[4]==';') {
            // send reply back
            sprintf(reply,"ZZAR%+04d;",(int)(rx->agc_gain));
            send_resp(cmd,reply) ;
          } else {
            rx->agc_gain=(double)atoi(&command[4]);
            receiver_set_agc_gain(rx);
          }
          break;
        case 'S': //ZZAS
          // read/set RX1 AGC Threshold
          implemented=FALSE;
          break;
        case 'T': //ZZAT
          implemented=FALSE;
          break;
        case 'U': //ZZAU
          // move VFO A up by selected step
          if(command[6]==';') {
            int step_index=atoi(&command[4]);
            long long hz=0;
            if(step_index>0 && step_index<=14) {
              hz=(long long)steps[step_index-1];
            }
            if(hz!=0LL) {
              receiver_move(rx,hz,TRUE);
            }
          } else {
          }
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'B': //ZZBx
      switch(command[3]) {
        case 'A': //ZZBA
          // move RX2 up down one band
          implemented=FALSE;
          break;
        case 'B': //ZZBB
          // move RX2 up one band
          implemented=FALSE;
          break;
        case 'D': //ZZBD
          // move RX1 down one band
          if(command[4]==';') {
            int b=previous_band(rx->band_a);
            set_band(rx,b,-1);
          }
          break;
        case 'E': //ZZBE
          // move VFO B down nn tune steps
          if(command[6]==';') {
            int steps=-atoi(&command[4]);
            receiver_move(rx,rx->step*steps,TRUE);
          }

          break;
        case 'F': //ZZBF
          // move VFO B up nn tune steps
          if(command[6]==';') {
            int steps=atoi(&command[4]);
            receiver_move(rx,rx->step*steps,TRUE);
          }
          break;
        case 'G': //ZZBG
	  if(command[4]==';') {
            sprintf(reply,"ZZBG%d;",0);
            send_resp(cmd,reply) ;
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'I': //ZZBI
	  if(command[4]==';') {
            sprintf(reply,"ZZBI%d;",0);
            send_resp(cmd,reply) ;
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'M': //ZZBM
          // move VFO B down by selected step
          if(command[6]==';') {
            int step_index=atoi(&command[4]);
            long long hz=0;
            if(step_index>0 && step_index<=14) {
              hz=(long long)steps[step_index-1];
            }
            if(hz!=0LL) {
              receiver_move_b(rx,-hz,FALSE,FALSE);
            }
          } else {
          }

          break;
        case 'P': //ZZBP
          // move VFO B up by selected step
          if(command[6]==';') {
            int step_index=atoi(&command[4]);
            long long hz=0;
            if(step_index>0 && step_index<=14) {
              hz=(long long)steps[step_index-1];
            }
            if(hz!=0LL) {
              receiver_move_b(rx,hz,FALSE,FALSE);
            }
          } else {
          }
          break;
        case 'R': //ZZBR
	  if(command[4]==';') {
            sprintf(reply,"ZZBR%d;",0);
            send_resp(cmd,reply) ;
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'S': //ZZBS
          // set/read RX1 band switch
          if(command[4]==';') {
            sprintf(reply,"ZZBS%03d;",cat_band(rx));
            send_resp(cmd,reply) ;
          } else {
            implemented=FALSE;
	  }
          break;
        case 'T': //ZZBT
	   // set or reads RX2 Band Switch
           implemented=FALSE;
           break;
        case 'U': //ZZBU
	   // moves RX1 band swithc up one band
           implemented=FALSE;
           break;
        case 'Y': //ZZBY
	   // closes console
           implemented=FALSE;
           break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'C': //ZZCx
      switch(command[3]) {
	case 'B': //ZZCB
	  // sets/reads break in enable
          implemented=FALSE;
	  break;
	case 'D': //ZZCD
	  // sets/reads break delay
          implemented=FALSE;
	  break;
	case 'F': //ZZCF
	  // sets/reads show TX CW frequency
          implemented=FALSE;
	  break;
	case 'I': //ZZCI
	  // sets/reads CW iambic
          implemented=FALSE;
	  break;
	case 'L': //ZZCL
	  // sets/reads CW pitch
          implemented=FALSE;
	  break;
	case 'M': //ZZCM
	  // sets/reads CW monitor
          implemented=FALSE;
	  break;
	case 'N': //ZZCN
	  // sets/reads VFO A CTUN
	  if(command[4]==';') {
            sprintf(reply,"ZZCN%d;",rx->ctun);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            rx->ctun=atoi(&command[4]);
	    receiver_set_ctun(rx);
          } else {
            implemented=FALSE;
	  }
	  break;
	case 'O': //ZZCO
	  // sets/reads VFO B CTUN
          implemented=FALSE;
	  break;
	case 'P': //ZZCP
	  // sets/reads Compander
	  if(command[4]==';') {
            sprintf(reply,"ZZCP%d;",0);
            send_resp(cmd,reply) ;
	  } else {
            implemented=FALSE;
	  }
	  break;
	case 'S': //ZZCS
	  // sets/reads CW Speed
          implemented=FALSE;
	  break;
	case 'T': //ZZCT
	  // sets/reads Campander threshold
          implemented=FALSE;
	  break;
	case 'U': //ZZCU
	  // sets/reads CPU usage
          implemented=FALSE;
	  break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'D': //ZZDx
      switch(command[3]) {
        case 'A': //ZZDA
	  // set/reads display average status
	  if(command[4]==';') {
            sprintf(reply,"ZZDA%d;",rx->display_average_time>1.0);
            send_resp(cmd,reply) ;
          } else {
            implemented=FALSE;
          }
          break;
        case 'E': //ZZDE
	  // set/reads enhanced signal clarity form enable
          implemented=FALSE;
          break;
        case 'F': //ZZDF
	  // opens/closes enhanced signal clarity form
          implemented=FALSE;
          break;
        case 'M': //ZZDM
	  // sets/reads display mode
	  if(command[4]==';') {
            sprintf(reply,"ZZDM%d;",8); // Panafall
            send_resp(cmd,reply) ;
          } else {
            implemented=FALSE;
	  }
          break;
        case 'N': //ZZDN
	  // sets/reads waterfall low
	  if(command[4]==';') {
            sprintf(reply,"ZZDN%+4d;",rx->waterfall_low);
            send_resp(cmd,reply) ;
	  } else if(command[8]==';') {
	    rx->waterfall_low=atoi(&command[4]);
          } else {
            implemented=FALSE;
	  }
          break;
        case 'O': //ZZDO
	  // sets/reads waterfall high
	  if(command[4]==';') {
            sprintf(reply,"ZZDO%+4d;",rx->waterfall_high);
            send_resp(cmd,reply) ;
	  } else if(command[8]==';') {
	    rx->waterfall_high=atoi(&command[4]);
          } else {
            implemented=FALSE;
	  }
          break;
        case 'P': //ZZDP
	  // sets/reads spectrum high
	  if(command[4]==';') {
            sprintf(reply,"ZZDP%+4d;",rx->panadapter_high);
            send_resp(cmd,reply) ;
	  } else if(command[8]==';') {
	    rx->panadapter_high=atoi(&command[4]);
          } else {
            implemented=FALSE;
	  }
          break;
        case 'Q': //ZZDQ
	  // sets/reads spectrum low
	  if(command[4]==';') {
            sprintf(reply,"ZZDQ%+4d;",rx->panadapter_low);
            send_resp(cmd,reply) ;
	  } else if(command[8]==';') {
	    rx->panadapter_low=atoi(&command[4]);
          } else {
            implemented=FALSE;
	  }
          implemented=FALSE;
          break;
        case 'R': //ZZDR
	  // sets/reads spectrum step size
	  if(command[4]==';') {
            sprintf(reply,"ZZDR%02d;",rx->panadapter_step);
            send_resp(cmd,reply) ;
	  } else if(command[6]==';') {
	    rx->panadapter_step=atoi(&command[4]);
          } else {
            implemented=FALSE;
	  }
          break;
        case 'U': //ZZDU
	  sprintf(reply,
	      "%1d" //P1
	      "%1d" //P2
	      "%1d" //P3
	      "%1d" //P4
	      "%1d" //P5
	      "%1d" //P6
	      "%1d" //P7
	      "%1d" //P8
	      "%1d" //P9
	      "%1d" //P10
	      "%1d" //P11
	      "%1d" //P12
	      "%1d" //P13
	      "%02d" //P14
	      "%02d" //P15
	      "%02d" //P16
	      "%02d" //P17
	      "%02d" //P18
	      "%03d" //P19
	      "%03d" //P20
	      "%03d" //P21
	      "%03d" //P22
	      "%03d" //P23
	      "%02d" //P24
	      "%03d" //P25
	      "%04d" //P26
	      "%04d" //P27
	      "%05lld" //P28
	      "%05d" //P29
	      "%05lld" //P30
	      "%03.2f" //P31
	      "%011lld" //P32
	      "%011lld" //P33
	      ";",
              rx->split!=SPLIT_OFF, //P1
              rx->split!=SPLIT_OFF, // P2
              radio->tune, // P3
              radio->mox, // P4
              radio->adc[rx->adc].antenna, // P5
              radio->adc[rx->adc].antenna, // P6
              radio->transmitter?radio->dac[radio->transmitter->dac].antenna:0, // P7
              0, // P8
              rx->rit_enabled, // P9
              0, // P10
              rx->agc, // P11
              0, // P12
              radio->transmitter?(int)radio->transmitter->xit_enabled:0, // P13
	      step_size(rx), // P14
	      rx->mode_a, // P15
	      0, // P16
	      0, // P17
	      rx->filter_a, // P18
	      0, // P19
	      0, // P20
	      radio->transmitter?(int)radio->transmitter->drive:0, //P21
	      cat_band(rx), //P22
              (int)(rx->volume*100.0), //P23
              0, //P24,
              radio->transmitter?(int)radio->transmitter->tune_percent:0, //P25
              0, // P26
              (int)s_meter_level(rx), //P27
              rx->rit, //P28
              0, //P29
              radio->transmitter?radio->transmitter->xit:0, //P30
              0.0, //P31
              rx->frequency_a, //P32
              rx->frequency_b // P33
            );
            send_resp(cmd,reply) ;
          break;
        case 'X': //ZZDX
	  // sets/reads phone dx button
          implemented=FALSE;
          break;
        case 'Y': //ZZDY
	  // sets/reads phone dx
          implemented=FALSE;
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'E': //ZZEx
      switch(command[3]) {
        case 'A': //ZZEA
          // set/read rx equalizer values
          if(command[4]==';') {
            sprintf(reply,"ZZEA%03d%03d%03d%03d%03d00000000000000000000;",3,rx->equalizer[0],rx->equalizer[1],rx->equalizer[2],rx->equalizer[3]);
            send_resp(cmd,reply) ;
          } else if(command[37]==';') {
            char temp[4];
            temp[3]='\0';
            strncpy(temp,&command[4],3);
            int bands=atoi(temp);
            if(bands==3) {
              strncpy(temp,&command[7],3);
              rx->equalizer[0]=atoi(temp);
              strncpy(temp,&command[10],3);
              rx->equalizer[1]=atoi(temp);
              strncpy(temp,&command[13],3);
              rx->equalizer[2]=atoi(temp);
            } else {
            }
          } else {
          }
          break;
        case 'B': //ZZEB
          // set/read tx equalizer values
          if(radio->transmitter && radio->transmitter->rx==rx) {
            TRANSMITTER *tx=radio->transmitter;
            if(command[4]==';') {
              sprintf(reply,"ZZEB%03d%03d%03d%03d%03d00000000000000000000;",3,tx->equalizer[0],tx->equalizer[1],tx->equalizer[2],tx->equalizer[3]);
              send_resp(cmd,reply) ;
            } else if(command[37]==';') {
              char temp[4];
              temp[3]='\0';
              strncpy(temp,&command[4],3);
              int bands=atoi(temp);
              if(bands==3) {
                strncpy(temp,&command[7],3);
                tx->equalizer[0]=atoi(temp);
                strncpy(temp,&command[10],3);
                tx->equalizer[1]=atoi(temp);
                strncpy(temp,&command[13],3);
                tx->equalizer[2]=atoi(temp);
              } else {
              }
            } else {
            }
          } else {
            implemented=FALSE;
          }
          break;
        case 'M': //ZZEM
          implemented=FALSE;
          break;
        case 'R': //ZZER
          // set/read rx equalizer
	  if(radio->transmitter) {
            if(command[4]==';') {
              sprintf(reply,"ZZER%d;",rx->enable_equalizer);
              send_resp(cmd,reply) ;
            } else if(command[5]==';') {
              rx->enable_equalizer=atoi(&command[4]);
    	    }
          } else {
            implemented=FALSE;
          }
          break;
        case 'T': //ZZET
          // set/read tx equalizer
	  if(radio->transmitter) {
            if(command[4]==';') {
              sprintf(reply,"ZZET%d;",radio->transmitter->enable_equalizer);
              send_resp(cmd,reply) ;
            } else if(command[5]==';') {
              radio->transmitter->enable_equalizer=atoi(&command[4]);
	    }
          } else {
            implemented=FALSE;
          }
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'F': //ZZFx
      switch(command[3]) {
        case 'A': //ZZFA
          // set/read VFO-A frequency
          if(command[4]==';') {
            if(rx->ctun) {
              sprintf(reply,"ZZFA%011lld;",rx->ctun_frequency);
            } else {
              sprintf(reply,"ZZFA%011lld;",rx->frequency_a);
            }
            send_resp(cmd,reply) ;
          } else if(command[15]==';') {
            long long f=atoll(&command[4]);
            rx->frequency_a=f;
            frequency_changed(rx);
            update_frequency(rx);
          }
          break;
        case 'B': //ZZFB
          // set/read VFO-B frequency
          if(command[4]==';') {
            sprintf(reply,"ZZFB%011lld;",rx->frequency_b);
            send_resp(cmd,reply) ;
          } else if(command[15]==';') {
            long long f=atoll(&command[4]);
            rx->frequency_b=f;
            frequency_changed(rx);
            update_frequency(rx);
          }
          break;
        case 'D': //ZZFD
          // set/read deviation
          if(command[4]==';') {
            sprintf(reply,"ZZFD%d;",rx->deviation==2500?0:1);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            int d=atoi(&command[4]);
            if(d==0) {
              rx->deviation=2500;
            } else if(d==1) {
              rx->deviation=5000;
            } else {
            }
            update_vfo(rx);
          }
          break;
        case 'H': //ZZFH
          // set/read RX1 filter high
          if(command[4]==';') {
            sprintf(reply,"ZZFH%05d;",rx->filter_high_a);
            send_resp(cmd,reply) ;
          } else if(command[9]==';') {
            int fh=atoi(&command[4]);
            fh=fmin(9999,fh);
            fh=fmax(-9999,fh);
            // make sure filter is Var1
            if(rx->filter_a!=FVar1) {
              receiver_filter_changed(rx,FVar1);
            }
            FILTER *mode_filters=filters[rx->mode_a];
            FILTER *filter=&mode_filters[FVar1];
            filter->high=fh;
            receiver_filter_changed(rx,FVar1);
          }
          break;
        case 'I': //ZZFI
          // set/read RX1 DSP receive filter
          if(command[4]==';') {
            sprintf(reply,"ZZFI%02d;",rx->filter_a);
            send_resp(cmd,reply) ;
          } else if(command[6]==';') {
            int filter=atoi(&command[4]);
            // update RX1 filter
          }
          break;
        case 'J': //ZZFJ
          // set/read RX2 DSP receive filter
          if(command[4]==';') {
            sprintf(reply,"ZZFJ%02d;",rx->filter_b);
            send_resp(cmd,reply) ;
          } else if(command[6]==';') {
            int filter=atoi(&command[4]);
            // update RX2 filter
          }
          break;
        case 'L': //ZZFL
          // set/read RX1 filter low
          if(command[4]==';') {
            sprintf(reply,"ZZFL%05d;",rx->filter_low_a);
            send_resp(cmd,reply) ;
          } else if(command[9]==';') {
            int fl=atoi(&command[4]);
            fl=fmin(9999,fl);
            fl=fmax(-9999,fl);
            // make sure filter is filterVar1
            if(rx->filter_a!=FVar1) {
              receiver_filter_changed(rx,FVar1);
            }
            FILTER *mode_filters=filters[rx->mode_a];
            FILTER *filter=&mode_filters[FVar1];
            filter->low=fl;
            receiver_filter_changed(rx,FVar1);
          }
          break;
        case 'M': //ZZFM
          implemented=FALSE;
          break;
        case 'R': //ZZFR
          implemented=FALSE;
          break;
        case 'S': //ZZFS
          implemented=FALSE;
          break;
        case 'V': //ZZFV
          implemented=FALSE;
          break;
        case 'W': //ZZFW
          implemented=FALSE;
          break;
        case 'X': //ZZFX
          implemented=FALSE;
          break;
        case 'Y': //ZZFY
          implemented=FALSE;
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'G': //ZZGx
      switch(command[3]) {
        case 'E': //ZZGE
          implemented=FALSE;
          break;
        case 'L': //ZZGL
          implemented=FALSE;
          break;
        case 'T': //ZZGT
          // set/read RX1 AGC
          if(command[4]==';') {
            sprintf(reply,"ZZGT%d;",rx->agc);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            int agc=atoi(&command[4]);
            // update RX1 AGC
            rx->agc=agc;
            update_vfo(rx);
          }
          break;
        case 'U': //ZZGU
          // set/read RX2 AGC
          implemented=FALSE;
/*
          if(command[4]==';') {
            sprintf(reply,"ZZGU%d;",receiver[1]->agc);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            int agc=atoi(&command[4]);
            // update RX2 AGC
            receiver[1]->agc=agc;
            update_vfo(rx);
          }
*/
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'H': //ZZHx
      switch(command[3]) {
        case 'A': //ZZHA
          implemented=FALSE;
          break;
        case 'R': //ZZHR
          implemented=FALSE;
          break;
        case 'T': //ZZHT
          implemented=FALSE;
          break;
        case 'U': //ZZHU
          implemented=FALSE;
          break;
        case 'V': //ZZHV
          implemented=FALSE;
          break;
        case 'W': //ZZHW
          implemented=FALSE;
          break;
        case 'X': //ZZHX
          implemented=FALSE;
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'I': //ZZIx
      switch(command[3]) {
        case 'D': //ZZID
          strcpy(reply,"ZZID240;");
          send_resp(cmd,reply) ;
          break;
        case 'F': //ZZIF
          implemented=FALSE;
          break;
        case 'O': //ZZIO
          implemented=FALSE;
          break;
        case 'S': //ZZIS
          implemented=FALSE;
          break;
        case 'T': //ZZIT
          implemented=FALSE;
          break;
        case 'U': //ZZIU
          implemented=FALSE;
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'K': //ZZKx
      switch(command[3]) {
        case 'M': //ZZIM
          implemented=FALSE;
          break;
        case 'O': //ZZIO
          implemented=FALSE;
          break;
        case 'S': //ZZIS
          implemented=FALSE;
          break;
        case 'Y': //ZZIY
          implemented=FALSE;
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'L': //ZZLx
      switch(command[3]) {
        case 'A': //ZZLA
          // read/set RX0 gain
          if(command[4]==';') {
            // send reply back
            sprintf(reply,"ZZLA%03d;",(int)(rx->volume*100.0));
            send_resp(cmd,reply) ;
          } else {
            int gain=atoi(&command[4]);
            rx->volume=(double)gain/100.0;
            receiver_set_volume(rx);
	    update_vfo(rx);
          }
          break;
        case 'B': //ZZLB
          implemented=FALSE;
          break;
        case 'C': //ZZLC
          // read/set RX1 gain
          implemented=FALSE;
/*
          if(receivers==2) {
            if(command[4]==';') {
              // send reply back
              sprintf(reply,"ZZLC%03d;",(int)(receiver[1]->volume*100.0));
              send_resp(cmd,reply) ;
            } else {
              int gain=atoi(&command[4]);
              receiver[1]->volume=(double)gain/100.0;
              update_af_gain();
            }
          }
*/
          break;
        case 'D': //ZZLD
          implemented=FALSE;
          break;
        case 'E': //ZZLE
          implemented=FALSE;
          break;
        case 'F': //ZZLF
          implemented=FALSE;
          break;
        case 'G': //ZZLG
          implemented=FALSE;
          break;
        case 'H': //ZZLH
          implemented=FALSE;
          break;
        case 'I': //ZZLI
          if(radio->transmitter) {
            if(command[4]==';') {
              // send reply back
              sprintf(reply,"ZZLI%d;",radio->transmitter->puresignal_enabled);
              send_resp(cmd,reply) ;
            } else {
              int ps=atoi(&command[4]);
              // TODO enable PS via CAT
              //radio->transmitter->puresignal_enabled=ps;
            }
            update_vfo(rx);
          } else {
            implemented=FALSE;
	  }
          break;
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'M': //ZZMx
      switch(command[3]) {
        case 'A': //ZZMA
          implemented=FALSE;
          break;
        case 'B': //ZZMB
          implemented=FALSE;
          break;
        case 'D': //ZZMD
          // set/read RX1 operating mode
          if(command[4]==';') {
            sprintf(reply,"ZZMD%02d;",rx->mode_a);
            send_resp(cmd,reply);
          } else if(command[6]==';') {
            receiver_mode_changed(rx,atoi(&command[4]));
          }
          break;
        case 'E': //ZZME
          // set/read RX2 operating mode
          if(command[4]==';') {
            sprintf(reply,"ZZMD%02d;",rx->mode_b);
            send_resp(cmd,reply);
          } else if(command[6]==';') {
            rx->mode_b=atoi(&command[4]);
            if(rx->subrx_enable) {
              subrx_mode_changed(rx);
            }
          }
          break;
        case 'G': //ZZMG
          // set/read mic gain
	  if(radio->transmitter) {
            if(command[4]==';') {
              sprintf(reply,"ZZMG%03d;",(int)radio->transmitter->mic_gain);
              send_resp(cmd,reply);
            } else if(command[7]==';') {
              radio->transmitter->mic_gain=(double)atoi(&command[4]);
            }
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'L': //ZZML
          // read DSP modes and indexes
          if(command[4]==';') {
            sprintf(reply,"ZZML LSB00: USB01: DSB02: CWL03: CWU04: FMN05:  AM06:DIGU07:SPEC08:DIGL09: SAM10: DRM11;");
            send_resp(cmd,reply);
          }
          break;
        case 'N': //ZZMN
          // read Filter Names and indexes
          if(command[6]==';') {
            int mode=atoi(&command[4])-1;
            FILTER *f=filters[mode];
            sprintf(reply,"ZZMN");
            char temp[32];
            for(int i=0;i<FILTERS;i++) {
              sprintf(temp,"%5s%5d%5d",f[i].title,f[i].high,f[i].low);
              strcat(reply,temp);
            }
            strcat(reply,";");
            send_resp(cmd,reply);
          }
          break;
        case 'O': //ZZMO
          // set/read MON status
          if(command[4]==';') {
            sprintf(reply,"ZZMO%d;",0);
            send_resp(cmd,reply);
          }
          break;
        case 'R': //ZZMR
          // set/read RX Meter mode
          if(command[4]==';') {
            sprintf(reply,"ZZMR%d;",rx->smeter+1);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->smeter=atoi(&command[4])-1;
          }
          break;
        case 'S': //ZZMS
          implemented=FALSE;
          break;
        case 'T': //ZZMT
          if(command[4]==';') {
            sprintf(reply,"ZZMT%02d;",1); // forward power
            send_resp(cmd,reply);
          } else {
          }
          break;
        case 'U': //ZZMU
          implemented=FALSE;
          break;
        case 'V': //ZZMV
          implemented=FALSE;
          break;
        case 'W': //ZZMW
          implemented=FALSE;
          break;
        case 'X': //ZZMX
          implemented=FALSE;
          break;
        case 'Y': //ZZMY
          implemented=FALSE;
          break;
        case 'Z': //ZZMZ
          implemented=FALSE;
          break;
        default:
           implemented=FALSE;
           break;

      }
      break;
    case 'N': //ZZNx
      switch(command[3]) {
        case 'A': //ZZNA
          // set/read RX1 NB1
          if(command[4]==';') {
            sprintf(reply,"ZZNA%d;",rx->nb);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nb=atoi(&command[4]);
            if(rx->nb) {
              rx->nb2=0;
            }
            update_noise(rx);
          }
          break;
        case 'B': //ZZNB
          // set/read RX1 NB2
          if(command[4]==';') {
            sprintf(reply,"ZZNB%d;",rx->nb2);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nb2=atoi(&command[4]);
            if(rx->nb2) {
              rx->nb=0;
            }
            update_noise(rx);
          }
          break;
        case 'C': //ZZNC
          // set/read RX2 NB1
          if(command[4]==';') {
            sprintf(reply,"ZZNC%d;",rx->nb);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nb=atoi(&command[4]);
            if(rx->nb) {
              rx->nb2=0;
            }
            update_noise(rx);
          }
          break;
        case 'D': //ZZND
          // set/read RX2 NB2
          if(command[4]==';') {
            sprintf(reply,"ZZND%d;",rx->nb2);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nb2=atoi(&command[4]);
            if(rx->nb2) {
              rx->nb=0;
            }
            update_noise(rx);
          }
          break;
        case 'L': //ZZNL
          // set/read NB1 threshold
          implemented=FALSE;
          break;
        case 'M': //ZZNM
          // set/read NB2 threshold
          implemented=FALSE;
          break;
        case 'N': //ZZNN
          // set/read RX1 SNB status
          if(command[4]==';') {
            sprintf(reply,"ZZNN%d;",rx->snb);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->snb=atoi(&command[4]);
            update_noise(rx);
          }
          break;
        case 'O': //ZZNO
          // set/read RX2 SNB status
          if(command[4]==';') {
            sprintf(reply,"ZZNO%d;",rx->snb);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->snb=atoi(&command[4]);
            update_noise(rx);
          }
          break;
        case 'R': //ZZNR
          // set/read RX1 NR
          if(command[4]==';') {
            sprintf(reply,"ZZNR%d;",rx->nr);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nr=atoi(&command[4]);
            if(rx->nr) {
              rx->nr2=0;
              rx->nr3=0;
              rx->nr4=0;
            }
            update_noise(rx);
          }
          break;
        case 'S': //ZZNS
          // set/read RX1 NR2
          if(command[4]==';') {
            sprintf(reply,"ZZNS%d;",rx->nr2);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nr2=atoi(&command[4]);
            if(rx->nr2) {
              rx->nr=0;
              rx->nr3=0;
              rx->nr4=0;
            }
            update_noise(rx);
          }
          break;
        case 'T': //ZZNT
          // set/read RX1 ANF
          if(command[4]==';') {
            sprintf(reply,"ZZNT%d;",rx->anf);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->anf=atoi(&command[4]);
            update_noise(rx);
          }
          break;
        case 'U': //ZZNU
          // set/read RX2 ANF
          if(command[4]==';') {
            sprintf(reply,"ZZNU%d;",rx->anf);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->anf=atoi(&command[4]);
            update_noise(rx);
          }
          break;
        case 'V': //ZZNV
          // set/read RX2 NR
          if(command[4]==';') {
            sprintf(reply,"ZZNV%d;",rx->nr);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nr=atoi(&command[4]);
            if(rx->nr) {
              rx->nr2=0;
              rx->nr3=0;
              rx->nr4=0;
            }
            update_noise(rx);
          }
          break;
        case 'W': //ZZNW
          // set/read RX2 NR2
          if(command[4]==';') {
            sprintf(reply,"ZZNW%d;",rx->nr2);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->nr2=atoi(&command[4]);
            if(rx->nr2) {
              rx->nr=0;
              rx->nr3=0;
              rx->nr4=0;
            }
            update_noise(rx);
          }
          break;
        default:
           implemented=FALSE;
           break;
      }
    case 'O': //ZZOx
      switch(command[3]) {
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'P': //ZZPx
      switch(command[3]) {
        case 'A': //ZZPA
          // set/read preamp setting
          if(command[4]==';') {
            int a=radio->adc[rx->adc].attenuation;
            if(a==0) {
              a=1;
            } else if(a<=-30) {
              a=4;
            } else if(a<=-20) {
              a=0;
            } else if(a<=-10) {
              a=2;
            } else {
              a=3;
            }
            sprintf(reply,"ZZPA%d;",a);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            int a=atoi(&command[4]);
            switch(a) {
              case 0:
                radio->adc[rx->adc].attenuation=-20;
                break;
              case 1:
                radio->adc[rx->adc].attenuation=0;
                break;
              case 2:
                radio->adc[rx->adc].attenuation=-10;
                break;
              case 3:
                radio->adc[rx->adc].attenuation=-20;
                break;
              case 4:
                radio->adc[rx->adc].attenuation=-30;
                break;
              default:
                radio->adc[rx->adc].attenuation=0;
                break;
            }
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'Q': //ZZQx
      switch(command[3]) {
        default:
           implemented=FALSE;
           break;
      }
      break;
    case 'R': //ZZRx
      switch(command[3]) {
        case 'C': //ZZRC
          // clear RIT frequency
          if(command[4]==';') {
            rx->rit=0;
            update_vfo(rx);
          }
          break;
        case 'D': //ZZRD
          // decrement RIT frequency
          if(command[4]==';') {
            if(rx->mode_a==CWL || rx->mode_a==CWU) {
              rx->rit-=10;
            } else {
              rx->rit-=50;
            }
            update_vfo(rx);
          } else if(command[9]==';') {
            rx->rit=atoi(&command[4]);
            update_vfo(rx);
          }
          break;
        case 'F': //ZZRF
          // set/read RIT frequency
          if(command[4]==';') {
            sprintf(reply,"ZZRF%+5lld;",rx->rit);
            send_resp(cmd,reply);
          } else if(command[9]==';') {
            rx->rit=atoi(&command[4]);
            update_vfo(rx);
          }
          break;
        case 'M': //ZZRM
          // read meter value
          if(command[5]==';') {
            int m=atoi(&command[4]);
            sprintf(reply,"ZZRM%d%20d;",rx->smeter,(int)s_meter_level(rx));
            send_resp(cmd,reply);
          }
          break;
        case 'S': //ZZRS
          // set/read RX2 enable
          implemented=FALSE;
/*
          if(command[4]==';') {
            sprintf(reply,"ZZRS%d;",receivers==2);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            int state=atoi(&command[4]);
            if(state) {
              radio_change_receivers(2);
            } else {
              radio_change_receivers(1);
            }
          }
*/
          break;
        case 'T': //ZZRT
          // set/read RIT enable
          if(command[4]==';') {
            sprintf(reply,"ZZRT%d;",rx->rit_enabled);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->rit_enabled=atoi(&command[4]);
            update_vfo(rx);
          }
          break;
        case 'U': //ZZRU
          // increments RIT Frequency
          if(command[4]==';') {
            if(rx->mode_a==CWL || rx->mode_a==CWU) {
              rx->rit+=10;
            } else {
              rx->rit+=50;
            }
            update_vfo(rx);
          } else if(command[9]==';') {
            rx->rit=atoi(&command[4]);
            update_vfo(rx);
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
    case 'S': //ZZSx
      switch(command[3]) {
        case 'A': //ZZSA
          // move VFO A down one step 
          if(command[4]==';') {
            receiver_move(rx,-rx->step,TRUE);
          }
          break;
        case 'B': //ZZSB
          // move VFO A up one step 
          if(command[4]==';') {
            receiver_move(rx,rx->step,TRUE);
          }
          break;
        case 'D': //ZZSD
          implemented=FALSE;
          break;
        case 'F': //ZZSF
          implemented=FALSE;
          break;
        case 'G': //ZZSG
          // move VFO B down 1 step
          if(command[4]==';') {
            receiver_move_b(rx,-rx->step,FALSE,TRUE);
          }
          break;
        case 'H': //ZZSH
          // move VFO B up 1 step
          if(command[4]==';') {
            receiver_move_b(rx,rx->step,FALSE,TRUE);
          }
          break;
        case 'M': //ZZSM
          // reads the S Meter (in dB)
          if(command[5]==';') {
            int v=atoi(&command[4]);
            if(v==0 || v==1) {
              double level=s_meter_level(rx);
              level=fmax(-140.0,level);
              level=fmin(-10.0,level);
              sprintf(reply,"ZZSM%d%03d;",v,(int)((level+140.0)*2));
              send_resp(cmd,reply);
            }
          }
          break;
        case 'N': //ZZSN
          implemented=FALSE;
          break;
        case 'P': //ZZSP
          // set/read split
          if(command[4]==';') {
            sprintf(reply,"ZZSP%d;",rx->split);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            rx->split=atoi(&command[4]);
            if(radio->transmitter) transmitter_set_mode(radio->transmitter,rx->mode_b);
            update_vfo(rx);
          }
          break;
        case 'R': //ZZSR
          implemented=FALSE;
          break;
        case 'S': //ZZSS
          implemented=FALSE;
          break;
        case 'T': //ZZST
          implemented=FALSE;
          break;
        case 'U': //ZZSU
          implemented=FALSE;
          break;
        case 'V': //ZZSV
          implemented=FALSE;
          break;
        case 'W': //ZZSW
          // set/read split
          if(command[4]==';') {
            sprintf(reply,"ZZSW%d;",rx->split);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            rx->split=atoi(&command[4]);
            if(radio->transmitter) transmitter_set_mode(radio->transmitter,rx->mode_b);
            update_vfo(rx);
          }
          break;
        case 'Y': //ZZSY
          implemented=FALSE;
          break;
        case 'Z': //ZZSZ
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'T': //ZZTx
      switch(command[3]) {
        case 'A': //ZZTA
          implemented=FALSE;
          break;
        case 'B': //ZZTB
          implemented=FALSE;
          break;
        case 'F': //ZZTF
          implemented=FALSE;
          break;
        case 'H': //ZZTH
          implemented=FALSE;
          break;
        case 'I': //ZZTI
          implemented=FALSE;
          break;
        case 'L': //ZZTL
          implemented=FALSE;
          break;
        case 'M': //ZZTM
          implemented=FALSE;
          break;
        case 'O': //ZZTO
          implemented=FALSE;
          break;
        case 'P': //ZZTP
          implemented=FALSE;
          break;
        case 'S': //ZZTS
          implemented=FALSE;
          break;
        case 'U': //ZZTU
          // sets or reads TUN status
          if(command[4]==';') {
            sprintf(reply,"ZZTU%d;",radio->tune);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            set_tune(radio,atoi(&command[4]));
          }
          break;
        case 'V': //ZZTV
          implemented=FALSE;
          break;
        case 'X': //ZZTX
          // sets or reads MOX status
          if(command[4]==';') {
            sprintf(reply,"ZZTX%d;",radio->mox);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            set_mox(radio,atoi(&command[4]));
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'U': //ZZUx
      switch(command[3]) {
        case 'A': //ZZUA
          implemented=FALSE;
          break;
        case 'S': //ZZUS
          implemented=FALSE;
          break;
        case 'T': //ZZUT
          implemented=FALSE;
          break;
        case 'X': //ZZUX
	  // sets or reads the VFO A Lock
	  if(command[4]==';') {
            sprintf(reply,"ZZUX%d;",rx->locked);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            rx->locked=command[4]=='1';
            update_vfo(rx);
          }
          break;
        case 'Y': //ZZUY
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'V': //ZZVx
      switch(command[3]) {
        case 'A': //ZZVA
          implemented=FALSE;
          break;
        case 'B': //ZZVB
          implemented=FALSE;
          break;
        case 'C': //ZZVC
          implemented=FALSE;
          break;
        case 'D': //ZZVD
          implemented=FALSE;
          break;
        case 'E': //ZZVE
          implemented=FALSE;
          break;
        case 'F': //ZZVF
          implemented=FALSE;
          break;
        case 'H': //ZZVH
          implemented=FALSE;
          break;
        case 'I': //ZZVI
          implemented=FALSE;
          break;
        case 'J': //ZZVJ
          implemented=FALSE;
          break;
        case 'K': //ZZVK
          implemented=FALSE;
          break;
        case 'L': //ZZVL
          // set/get VFO Lock
          implemented=FALSE;
          break;
        case 'M': //ZZVM
          implemented=FALSE;
          break;
        case 'N': //ZZVN
          implemented=FALSE;
          break;
        case 'O': //ZZVO
          implemented=FALSE;
          break;
        case 'P': //ZZVP
          implemented=FALSE;
          break;
        case 'Q': //ZZVQ
          implemented=FALSE;
          break;
        case 'R': //ZZVR
          implemented=FALSE;
          break;
        case 'S': //ZZVS
	  if(command[5]==';') {
            switch(atoi(&command[4])) {
	      case 1: // A to B
		break;
	      case 2: // B to A
		break;
              case 3: // A swap B
		break;
	    }
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'T': //ZZVT
          implemented=FALSE;
          break;
        case 'U': //ZZVU
          implemented=FALSE;
          break;
        case 'V': //ZZVV
          implemented=FALSE;
          break;
        case 'W': //ZZVW
          implemented=FALSE;
          break;
        case 'X': //ZZVX
          implemented=FALSE;
          break;
        case 'Y': //ZZVY
          implemented=FALSE;
          break;
        case 'Z': //ZZVZ
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'W': //ZZWx
      switch(command[3]) {
        case 'A': //ZZWA
          implemented=FALSE;
          break;
        case 'B': //ZZWB
          implemented=FALSE;
          break;
        case 'C': //ZZWC
          implemented=FALSE;
          break;
        case 'D': //ZZWD
          implemented=FALSE;
          break;
        case 'E': //ZZWE
          implemented=FALSE;
          break;
        case 'F': //ZZWF
          implemented=FALSE;
          break;
        case 'G': //ZZWG
          implemented=FALSE;
          break;
        case 'H': //ZZWH
          implemented=FALSE;
          break;
        case 'J': //ZZWJ
          implemented=FALSE;
          break;
        case 'K': //ZZWK
          implemented=FALSE;
          break;
        case 'L': //ZZWL
          implemented=FALSE;
          break;
        case 'M': //ZZWM
          implemented=FALSE;
          break;
        case 'N': //ZZWN
          implemented=FALSE;
          break;
        case 'O': //ZZWO
          implemented=FALSE;
          break;
        case 'P': //ZZWP
          implemented=FALSE;
          break;
        case 'Q': //ZZWQ
          implemented=FALSE;
          break;
        case 'R': //ZZWR
          implemented=FALSE;
          break;
        case 'S': //ZZWS
          implemented=FALSE;
          break;
        case 'T': //ZZWT
          implemented=FALSE;
          break;
        case 'U': //ZZWU
          implemented=FALSE;
          break;
        case 'V': //ZZWV
          implemented=FALSE;
          break;
        case 'W': //ZZWW
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'X': //ZZXx
      switch(command[3]) {
        case 'C': //ZZXC
          // clear transmitter XIT
	  if(radio->transmitter) {
            if(command[4]==';') {
              radio->transmitter->xit=0;
              update_vfo(rx);
            }
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'F': //ZZXF
          // set/read XIT
	  if(radio->transmitter) {
            if(command[4]==';') {
              sprintf(reply,"ZZXT%+05lld;",radio->transmitter->xit);
              send_resp(cmd,reply) ;
            } else if(command[9]==';') {
              radio->transmitter->xit=(long long)atoi(&command[4]);
              update_vfo(rx);
            }
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'H': //ZZXH
          implemented=FALSE;
          break;
        case 'N': //ZZXN
          // read combined RX1 status
          if(command[4]==';') {
            int status=0;
            status=status|((rx->agc)&0x03);
            int a=radio->adc[rx->adc].attenuation;
            if(a==0) {
              a=1;
            } else if(a<=-30) {
              a=4;
            } else if(a<=-20) {
              a=0;
            } else if(a<=-10) {
              a=2;
            } else {
              a=3;
            }
            status=status|((a&0x03)<<3);
            //status=status|((rx->squelch_enable&0x01)<<6);
            status=status|((rx->nb&0x01)<<7);
            status=status|((rx->nb2&0x01)<<8);
            status=status|((rx->nr&0x01)<<9);
            status=status|((rx->nr2&0x01)<<10);
            status=status|((rx->snb&0x01)<<11);
            status=status|((rx->anf&0x01)<<12);
            sprintf(reply,"ZZXN%04d;",status);
            send_resp(cmd,reply);
          }
          break;
        case 'O': //ZZXO
          // read combined RX2 status
          implemented=FALSE;
/*
          if(receivers==2) {
            if(command[4]==';') {
              int status=0;
              status=status|((receiver[1]->agc)&0x03);
              int a=adc[receiver[1]->adc].attenuation;
              if(a==0) {
                a=1;
              } else if(a<=-30) {
                a=4;
              } else if(a<=-20) {
                a=0;
              } else if(a<=-10) {
                a=2;
              } else {
                a=3;
              }
              status=status|((a&0x03)<<3);
              status=status|((receiver[1]->squelch_enable&0x01)<<6);
              status=status|((receiver[1]->nb&0x01)<<7);
              status=status|((receiver[1]->nb2&0x01)<<8);
              status=status|((receiver[1]->nr&0x01)<<9);
              status=status|((receiver[1]->nr2&0x01)<<10);
              status=status|((receiver[1]->snb&0x01)<<11);
              status=status|((receiver[1]->anf&0x01)<<12);
              sprintf(reply,"ZZXO%04d;",status);
              send_resp(cmd,reply);
            }
          }
*/
          break;
        case 'S': //ZZXS
          /// set/read XIT enable
	  if(radio->transmitter) {
            if(command[4]==';') {
              sprintf(reply,"ZZXS%d;",radio->transmitter->xit_enabled);
              send_resp(cmd,reply);
            } else if(command[5]==';') {
              radio->transmitter->xit_enabled=atoi(&command[4]);
              update_vfo(rx);
            }
  	  } else {
            implemented=FALSE;
	  }
          break;
        case 'T': //ZZXT
          implemented=FALSE;
          break;
        case 'V': //ZZXV
          // read combined VFO status
          if(command[4]==';') {
            int status=0;
            if(rx->rit_enabled) {
              status=status|0x01;
            }
            if(rx->locked) {
              status=status|0x02;
              status=status|0x04;
            }
            if(rx->split) {
              status=status|0x08;
            }
            if(rx->ctun) {
              status=status|0x10;
            }
            if(rx->ctun) {
              status=status|0x20;
            }
            if(radio->mox) {
              status=status|0x40;
            }
            if(radio->tune) {
              status=status|0x80;
            }
            sprintf(reply,"ZZXV%03d;",status);
            send_resp(cmd,reply);
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'Y': //ZZYx
      switch(command[3]) {
	case 'A': //ZZYA
          implemented=FALSE;
          break;
	case 'B': //ZZYB
          implemented=FALSE;
          break;
	case 'C': //ZZYC
          implemented=FALSE;
          break;
	case 'R': //ZZYR
          // switch receivers
          implemented=FALSE;
/*
          if(command[5]==';') {
            int v=atoi(&command[4]);
            if(v==0) {
              rx=receiver[0];
            } else if(v==1) {
              if(receivers==2) {
                rx=receiver[1];
              }
            }
            update_vfo(rx);
          }
*/
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'Z': //ZZZx
      switch(command[3]) {
        case 'A': //ZZZA
          implemented=FALSE;
          break;
	case 'B': //ZZZB
          implemented=FALSE;
          break;
	case 'Z': //ZZZZ
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    default:
       implemented=FALSE;
       break;
  }
  return implemented;
}

// called with g_idle_add so that the processing is running on the main thread
int parse_cmd(void *data) {
  COMMAND *cmd=(COMMAND *)data;
  RECEIVER *rx=cmd->rx;
  RIGCTL *rigctl=rx->rigctl;
  char *command=cmd->command;
  char reply[256];
  reply[0]='\0';
  gboolean implemented=TRUE;
  gboolean errord=FALSE;

  if(rigctl->debug) {
    log_info("%s: fd=%d %s\n",__FUNCTION__,cmd->fd,command);
  }
  switch(command[0]) {
    case 'A':
      switch(command[1]) {
        case 'C': //AC
          // set/read internal atu status
          implemented=FALSE;
          break;
        case 'G': //AG
          // set/read AF Gain
          if(command[2]==';') {
            // send reply back (covert from 0..1 to 0..255)
            sprintf(reply,"AG0%03d;",(int)(rx->volume*255.0));
            send_resp(cmd,reply) ;
          } else if(command[6]==';') {
            int gain=atoi(&command[3]);
            rx->volume=(double)gain/255.0;
            receiver_set_volume(rx);
          }
          break;
        case 'I': //AI
          // set/read Auto Information
          if(command[2]==';') {
            sprintf(reply,"AI0%d;",0);
            send_resp(cmd,reply) ;
          } else if(command[3]==';') {
          }
          break;
        case 'L': // AL
          // set/read Auto Notch level
          implemented=FALSE;
          break;
        case 'M': // AM
          // set/read Auto Mode
          implemented=FALSE;
          break;
        case 'N': // AN
          // set/read Antenna Connection
          implemented=FALSE;
          break;
        case 'S': // AS
          // set/read Auto Mode Function Parameters
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'B':
      switch(command[1]) {
        case 'C': //BC
          // set/read Beat Canceller
          implemented=FALSE;
          break;
        case 'D': //BD
          {
          //band down 1 band
          int b=previous_band(rx->band_a);
          set_band(rx,b,-1);
          }
          break;
        case 'P': //BP
          // set/read Manual Beat Canceller frequency
          implemented=FALSE;
          break;
        case 'U': //BU
          {
          //band up 1 band
          int b=next_band(rx->band_a);
          set_band(rx,b,-1);
          }
          break;
        case 'Y': //BY
          // read busy signal
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'C':
      switch(command[1]) {
        case 'A': //CA
          // set/read CW Auto Tune
          implemented=FALSE;
          break;
        case 'G': //CG
          // set/read Carrier Gain
          implemented=FALSE;
          break;
        case 'I': //CI
          // sets the current frequency to the CALL Channel
          implemented=FALSE;
          break;
        case 'M': //CM
          // sets/reads the Packet Cluster Tune function
          implemented=FALSE;
          break;
        case 'N': //CN
          // sets/reads CTCSS function
	  if(radio->transmitter) {
            if(command[3]==';') {
              sprintf(reply,"CN%02d;",radio->transmitter->ctcss+1);
              send_resp(cmd,reply) ;
            } else if(command[4]==';') {
              int i=atoi(&command[2])-1;
              transmitter_set_ctcss(radio->transmitter,radio->transmitter->ctcss_enabled,i);
            }
	  }
          break;
        case 'T': //CT
          // sets/reads CTCSS status
          if(radio->transmitter) {
            if(command[3]==';') {
              sprintf(reply,"CT%d;",radio->transmitter->ctcss_enabled);
              send_resp(cmd,reply) ;
            } else if(command[3]==';') {
              int state=atoi(&command[2]);
              transmitter_set_ctcss(radio->transmitter,state,radio->transmitter->ctcss);
            }
          } else {
            implemented=FALSE;
	  }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'D':
      switch(command[1]) {
        case 'C': //DC
          // set/read TX band status
          implemented=FALSE;
          break;
        case 'N': //DN
          // move VFO A down 1 step size
          receiver_move(rx,-rx->step,TRUE);
          break;
        case 'Q': //DQ
          // set/read DCS function status
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'E':
      switch(command[1]) {
        case 'X': //EX
          // set/read the extension menu
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'F':
      switch(command[1]) {
        case 'A': //FA
          // set/read VFO-A frequency
          if(command[2]==';') {
            if(rx->ctun) {
              sprintf(reply,"FA%011lld;",rx->ctun_frequency);
            } else {
              sprintf(reply,"FA%011lld;",rx->frequency_a);
            }
            send_resp(cmd,reply) ;
          } else if(command[13]==';') {
            long long f=atoll(&command[2]);
            rx->frequency_a=f;
            frequency_changed(rx);
            update_frequency(rx);
          }
          break;
        case 'B': //FB
          // set/read VFO-B frequency
          if(command[2]==';') {
            sprintf(reply,"FB%011lld;",rx->frequency_b);
            send_resp(cmd,reply) ;
          } else if(command[13]==';') {
            long long f=atoll(&command[2]);
            rx->frequency_b=f;
            frequency_changed(rx);
            update_frequency(rx);
          }
          break;
        case 'C': //FC
          // set/read the sub receiver VFO frequency menu
          implemented=FALSE;
          break;
        case 'D': //FD
          // set/read the filter display dot pattern
          implemented=FALSE;
          break;
        case 'R': //FR
          // set/read transceiver receive VFO
          if(command[2]==';') {
            sprintf(reply,"FR%d;",0);
            send_resp(cmd,reply) ;
          } else if(command[3]==';') {
            // ?
          }
          break;
        case 'S': //FS
          // set/read the fine tune function status
          implemented=FALSE;
          break;
        case 'T': //FT
          // set/read transceiver transmit VFO
          if(command[2]==';') {
            sprintf(reply,"FT%d;",rx->split!=SPLIT_OFF);
            send_resp(cmd,reply) ;
          } else if(command[3]==';') {
            if(atoi(&command[2])==0) {
              rx->split=SPLIT_OFF;
              if(radio->transmitter) transmitter_set_mode(radio->transmitter,rx->mode_a);
            } else {
              rx->split=SPLIT_ON;
              if(radio->transmitter) transmitter_set_mode(radio->transmitter,rx->mode_b);
            }
            update_vfo(rx);
          }
          break;
        case 'W': //FW
          // set/read filter width
          // make sure filter is filterVar1
          if(rx->filter_a!=FVar1) {
            receiver_filter_changed(rx,FVar1);
          }
          FILTER *mode_filters=filters[rx->mode_a];
          FILTER *filter=&mode_filters[FVar1];
          int val=0;
          if(command[2]==';') {
            switch(rx->mode_a) {
              case CWL:
              case CWU:
                val=filter->low*2;
                break;
              case AM:
              case SAM:
                val=filter->low>=-4000;
                break;
              case FMN:
                val=rx->deviation==5000;
                break;
              default:
                implemented=FALSE;
                break;
            }
            if(implemented) {
              sprintf(reply,"FW%04d;",val);
              send_resp(cmd,reply) ;
            }
          } else if(command[6]==';') {
            int fw=atoi(&command[2]);
            int val=
            filter->low=fw;
            switch(rx->mode_a) {
              case CWL:
              case CWU:
                filter->low=fw/2;
                filter->high=fw/2;
                break;
              case FMN:
                if(fw==0) {
                  filter->low=-4000;
                  filter->high=4000;
                  rx->deviation=2500;
                } else {
                  filter->low=-8000;
                  filter->high=8000;
                  rx->deviation=5000;
                }
                break;
              case AM:
              case SAM:
                if(fw==0) {
                  filter->low=-4000;
                  filter->high=4000;
                } else {
                  filter->low=-8000;
                  filter->high=8000;
                }
                break;
              default:
                implemented=FALSE;
                break;
            }
            if(implemented) {
              receiver_filter_changed(rx,FVar1);
            }
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'G':
      switch(command[1]) {
        case 'T': //GT
          // set/read RX1 AGC
          if(command[2]==';') {
            sprintf(reply,"GT%03d;",rx->agc*5);
            send_resp(cmd,reply) ;
          } else if(command[5]==';') {
            // update RX1 AGC
            rx->agc=atoi(&command[2])/5;
            update_vfo(rx);
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'H':
      switch(command[1]) {
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'I':
      switch(command[1]) {
        case 'D': //ID
          // get ID
          strcpy(reply,"ID019;"); // TS-2000
          send_resp(cmd,reply);
          break;
        case 'F': //IF
          {
          int mode=ts2000_mode(rx->mode_a);
          sprintf(reply,"IF%011lld%04lld%+06lld%d%d%d%02d%d%d%d%d%d%d%02d%d;",
                  rx->ctun?rx->ctun_frequency:rx->frequency_a,
                  rx->step,rx->rit,rx->rit_enabled,
                  radio->transmitter?radio->transmitter->xit_enabled:0,
                  0,0,isTransmitting(radio),mode,0,0,rx->split,
                  radio->transmitter&&radio->transmitter->ctcss_enabled?2:0,
                  radio->transmitter?radio->transmitter->ctcss:0,0);
          send_resp(cmd,reply);
          }
          break;
        case 'S': //IS
          // set/read IF shift
          if(command[2]==';') {
            strcpy(reply,"IS 0000;");
            send_resp(cmd,reply);
          } else {
            implemented=FALSE;
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'J':
      switch(command[1]) {
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'K':
      switch(command[1]) {
        case 'S': //KS
/*
          // set/read keying speed
          if(command[2]==';') {
            sprintf(reply,"KS%03d;",cw_keyer_speed);
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            int speed=atoi(&command[2]);
            if(speed>=1 && speed<=60) {
              cw_keyer_speed=speed;
#ifdef LOCALCW
              keyer_update();
#endif
              update_vfo(rx);
            }
          } else {
          }
*/
          break;
        case 'Y': //KY
/*
          // convert the chaaracters into Morse Code
          if(command[2]==';') {
            sprintf(reply,"KY%d;",cw_busy);
            send_resp(cmd,reply);
          } else if(command[27]==';') {
            if(cw_busy==0) {
              strncpy(cw_buf,&command[3],24);
              cw_busy=1;
            }
          } else {
          }
*/
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'L':
      switch(command[1]) {
        case 'K': //LK
          // set/read key lock
          if(command[2]==';') {
            sprintf(reply,"LK%d%d;",rx->locked,rx->locked);
            send_resp(cmd,reply);
          } else if(command[27]==';') {
            rx->locked=command[2]=='1';
            update_vfo(rx);
          }
          break;
        case 'M': //LM
          // set/read keyer recording status
          implemented=FALSE;
          break;
        case 'T': //LT
          // set/read ALT fucntion status
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'M':
      switch(command[1]) {
        case 'C': //MC
          // set/read Memory Channel
          implemented=FALSE;
          break;
        case 'D': //MD
          // set/read operating mode
          if(command[2]==';') {
            int mode=ts2000_mode(rx->mode_a);
            sprintf(reply,"MD%d;",mode);
            send_resp(cmd,reply);
          } else if(command[3]==';') {
            int mode=USB;
            switch(atoi(&command[2])) {
              case 1:
                mode=LSB;
                break;
              case 2:
                mode=USB;
                break;
              case 3:
                mode=CWU;
                break;
              case 4:
                mode=FMN;
                break;
              case 5:
                mode=AM;
                break;
              case 6:
                mode=DIGL;
                break;
              case 7:
                mode=CWL;
                break;
              case 9:
                mode=DIGU;
                break;
              default:
                break;
            }
            receiver_mode_changed(rx,mode);
          }
          break;
        case 'F': //MF
          // set/read Menu
          implemented=FALSE;
          break;
        case 'G': //MG
          // set/read Menu Gain (-12..60 converts to 0..100)
	  if(radio->transmitter) {
            if(command[2]==';') {
              sprintf(reply,"MG%03d;",(int)(((radio->transmitter->mic_gain+12.0)/72.0)*100.0));
              send_resp(cmd,reply);
            } else if(command[5]==';') {
              double gain=(double)atoi(&command[2]);
              gain=((gain/100.0)*72.0)-12.0;
              radio->transmitter->mic_gain=gain;
            }
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'L': //ML
          // set/read Monitor Function Level
          implemented=FALSE;
          break;
        case 'O': //MO
          // set/read Monitor Function On/Off
          implemented=FALSE;
          break;
        case 'R': //MR
          // read Memory Channel
          implemented=FALSE;
          break;
        case 'U': //MU
          // set/read Memory Group
          implemented=FALSE;
          break;
        case 'W': //MW
          // Write Memory Channel
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'N':
      switch(command[1]) {
        case 'B': //NB
          // set/read noise blanker
          if(command[2]==';') {
            sprintf(reply,"NB%d;",rx->nb);
            send_resp(cmd,reply);
          } else if(command[3]==';') {
            rx->nb=atoi(&command[2]);
            if(rx->nb) {
              rx->nb2=0;
            }
            update_noise(rx);
          }
          break;
        case 'L': //NL
          // set/read noise blanker level
          implemented=FALSE;
          break;
        case 'R': //NR
          // set/read noise reduction
          if(command[2]==';') {
            int n=0;
            if(rx->nr) {
              n=1;
            } else if(rx->nr2) {
              n=2;
            }
            sprintf(reply,"NR%d;",n);
            send_resp(cmd,reply);
          } else if(command[3]==';') {
            int n=atoi(&command[2]);
            switch(n) {
              case 0: // NR OFF
                rx->nr=0;
                rx->nr2=0;
                rx->nr3=0;
                rx->nr4=0;
                break;
              case 1: // NR ON
                rx->nr=1;
                rx->nr2=0;
                rx->nr3=0;
                rx->nr4=0;
                break;
              case 2: // NR2 ON
                rx->nr=0;
                rx->nr2=1;
                rx->nr3=0;
                rx->nr4=0;
                break;
            }
            update_noise(rx);
          }
          break;
        case 'T': //NT
          // set/read ANF
          if(command[2]==';') {
            sprintf(reply,"NT%d;",rx->anf);
            send_resp(cmd,reply);
          } else if(command[3]==';') {
            rx->anf=atoi(&command[2]);
            SetRXAANFRun(rx->channel, rx->anf);
            update_vfo(rx);
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'O':
      switch(command[1]) {
        case 'F': //OF
          // set/read offset frequency
          implemented=FALSE;
          break;
        case 'I': //OI
          // set/read offset frequency
          implemented=FALSE;
          break;
        case 'S': //OS
          // set/read offset function status
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'P':
      switch(command[1]) {
        case 'A': //PA
          // set/read preamp function status
          if(command[2]==';') {
            sprintf(reply,"PA%d0;",radio->adc[rx->adc].preamp);
            send_resp(cmd,reply);
          } else if(command[4]==';') {
            radio->adc[rx->adc].preamp=command[2]=='1';
          }
          break;
        case 'B': //PB
          // set/read FRU-3A playback status
          implemented=FALSE;
          break;
        case 'C': //PC
          // set/read PA Power
	  if(radio->transmitter) {
            if(command[2]==';') {
              sprintf(reply,"PC%03d;",(int)radio->transmitter->drive);
              send_resp(cmd,reply);
            } else if(command[5]==';') {
              radio->transmitter->drive=(double)atoi(&command[2]);
            }
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'I': //PI
          // store in program memory channel
          implemented=FALSE;
          break;
        case 'K': //PK
          // read packet cluster data
          implemented=FALSE;
          break;
        case 'L': //PL
          // set/read speach processor input/output level
	  if(radio->transmitter) {
            if(command[2]==';') {
              sprintf(reply,"PL%03d000;",(int)((radio->transmitter->compressor_level/20.0)*100.0));
              send_resp(cmd,reply);
            } else if(command[8]==';') {
              command[5]='\0';
              double level=(double)atoi(&command[2]);
              level=(level/100.0)*20.0;
              radio->transmitter->compressor_level=level;
              //transmitter_set_compressor_level(transmitter,level);
              update_vfo(rx);
            }
	  } else {
            implemented=FALSE;
	  }
          break;
        case 'M': //PM
          // recall program memory
          implemented=FALSE;
          break;
        case 'R': //PR
          // set/read speech processor function
          implemented=FALSE;
          break;
        case 'S': //PS
          // set/read Power (always ON)
          if(command[2]==';') {
            sprintf(reply,"PS1;");
            send_resp(cmd,reply);
          } else if(command[3]==';') {
            // ignore set
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'Q':
      switch(command[1]) {
        case 'C': //QC
          // set/read DCS code
          implemented=FALSE;
          break;
        case 'I': //QI
          // store in quick memory
          implemented=FALSE;
          break;
        case 'R': //QR
          // set/read quick memory channel data
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'R':
      switch(command[1]) {
        case 'A': //RA
          // set/read Attenuator function
          if(command[2]==';') {
            int att=0;
/*
            if(have_rx_gain) {
              att=(int)(adc_attenuation[rx->adc]+12);
              att=(int)(((double)att/60.0)*99.0);
            } else {
              att=(int)(adc_attenuation[rx->adc]);
              att=(int)(((double)att/31.0)*99.0);
            }
*/
            att=(int)(radio->adc[rx->adc].attenuation);
            att=(int)(((double)att/31.0)*99.0);
            sprintf(reply,"RA%02d00;",att);
            send_resp(cmd,reply);
          } else if(command[4]==';') {
            int att=atoi(&command[2]);
/*
            if(have_rx_gain) {
              att=(int)((((double)att/99.0)*60.0)-12.0);
            } else {
              att=(int)(((double)att/99.0)*31.0);
            }
*/
            att=(int)(((double)att/99.0)*31.0);
            radio->adc[rx->adc].attenuation=att;
            //set_attenuation_value((double)att);
          }
          break;
        case 'C': //RC
          // clears RIT
          if(command[2]==';') {
            rx->rit=0;
            update_vfo(rx);
          }
          break;
        case 'D': //RD
          // decrements RIT Frequency
          if(command[2]==';') {
            if(rx->mode_a==CWL || rx->mode_a==CWU) {
              rx->rit-=10;
            } else {
              rx->rit-=50;
            }
            update_vfo(rx);
          } else if(command[7]==';') {
            rx->rit=atoi(&command[2]);
            update_vfo(rx);
          }
          break;
        case 'G': //RG
          // set/read RF gain status
          implemented=FALSE;
          break;
        case 'L': //RL
          // set/read noise reduction level
          implemented=FALSE;
          break;
        case 'M': //RM
          // set/read meter function
          implemented=FALSE;
          break;
        case 'T': //RT
          // set/read RIT enable
          if(command[2]==';') {
            sprintf(reply,"RT%d;",rx->rit_enabled);
            send_resp(cmd,reply);
          } else if(command[3]==';') {
            rx->rit_enabled=atoi(&command[2]);
            update_vfo(rx);
          }
          break;
        case 'U': //RU
          // increments RIT Frequency
          if(command[2]==';') {
            if(rx->mode_a==CWL || rx->mode_a==CWU) {
              rx->rit+=10;
            } else {
              rx->rit+=50;
            }
            update_vfo(rx);
          } else if(command[7]==';') {
            rx->rit=atoi(&command[2]);
            update_vfo(rx);
          }
          break;
        case 'X': //RX
          // set transceiver to RX mode
          if(command[2]==';') {
            //set_mox(radio,FALSE);
            MOX_STATE *m=g_new0(MOX_STATE,1);
            m->radio=radio;
            m->state=0;
            g_idle_add(ext_set_mox,(gpointer)m);
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'S':
      switch(command[1]) {
        case 'A': //SA
          // set/read stallite mode status
          if(command[2]==';') {
            sprintf(reply,"SA%d%d%d%d%d%d%dSAT?    ;",rx->split==SPLIT_SAT|rx->split==SPLIT_RSAT,0,0,0,rx->split=SPLIT_SAT,rx->split=SPLIT_RSAT,0);
            send_resp(cmd,reply);
          } else if(command[9]==';') {
            if(command[2]=='0') {
              rx->split=SPLIT_OFF;
            } else if(command[2]=='1') {
              if(command[6]=='0' && command[7]=='0') {
                rx->split=SPLIT_OFF;
              } else if(command[6]=='1' && command[7]=='0') {
                rx->split=SPLIT_SAT;
              } else if(command[6]=='0' && command[7]=='1') {
                rx->split=SPLIT_RSAT;
              } else {
                implemented=FALSE;
              }
            }
          } else {
            implemented=FALSE;
          }
          break;
        case 'B': //SB
          // set/read SUB,TF-W status
          implemented=FALSE;
          break;
        case 'C': //SC
          // set/read SCAN function status
          implemented=FALSE;
          break;
        case 'D': //SD
          // set/read CW break-in time delay
/*
          if(command[2]==';') {
            sprintf(reply,"SD%04d;",(int)fmin(cw_keyer_hang_time,1000));
            send_resp(cmd,reply);
          } else if(command[6]==';') {
            int b=fmin(atoi(&command[2]),1000);
            cw_breakin=b==0;
            cw_keyer_hang_time=b;
          } else {
            implemented=FALSE;
          }
*/
          break;
        case 'H': //SH
          {
          // set/read filter high
          // make sure filter is filterVar1
          if(rx->filter_a!=FVar1) {
            receiver_filter_changed(rx,FVar1);
          }
          FILTER *mode_filters=filters[rx->mode_a];
          FILTER *filter=&mode_filters[FVar1];
	  if(command[2]==';') {
            int fh=5;
            int high=filter->high;
            if(rx->mode_a==LSB) {
              high=abs(filter->low);
            }
            if(high<=1400) {
              fh=0;
            } else if(high<=1600) {
              fh=1;
            } else if(high<=1800) {
              fh=2;
            } else if(high<=2000) {
              fh=3;
            } else if(high<=2200) {
              fh=4;
            } else if(high<=2400) {
              fh=5;
            } else if(high<=2600) {
              fh=6;
            } else if(high<=2800) {
              fh=7;
            } else if(high<=3000) {
              fh=8;
            } else if(high<=3400) {
              fh=9;
            } else if(high<=4000) {
              fh=10;
            } else {
              fh=11;
            }
            sprintf(reply,"SH%02d;",fh);
            send_resp(cmd,reply) ;
          } else if(command[4]==';') {
            int i=atoi(&command[2]);
            int fh=100;
            switch(rx->mode_a) {
              case LSB:
              case USB:
              case FMN:
                switch(i) {
                  case 0:
                    fh=1400;
                    break;
                  case 1:
                    fh=1600;
                    break;
                  case 2:
                    fh=1800;
                    break;
                  case 3:
                    fh=2000;
                    break;
                  case 4:
                    fh=2200;
                    break;
                  case 5:
                    fh=2400;
                    break;
                  case 6:
                    fh=2600;
                    break;
                  case 7:
                    fh=2800;
                    break;
                  case 8:
                    fh=3000;
                    break;
                  case 9:
                    fh=3400;
                    break;
                  case 10:
                    fh=4000;
                    break;
                  case 11:
                    fh=5000;
                    break;
                  default:
                    fh=100;
                    break;
                }
                break;
              case AM:
              case SAM:
                switch(i) {
                  case 0:
                    fh=10;
                    break;
                  case 1:
                    fh=100;
                    break;
                  case 2:
                    fh=200;
                    break;
                  case 3:
                    fh=500;
                    break;
                  default:
                    fh=100;
                    break;
                }
                break;
            }
            if(rx->mode_a==LSB) {
              filter->low=-fh;
            } else {
              filter->high=fh;
            }
            receiver_filter_changed(rx,FVar1);
          }
          }
          break;
        case 'I': //SI
          // enter satellite memory name
          implemented=FALSE;
          break;
        case 'L': //SL
          {
          // set/read filter low
          // make sure filter is filterVar1
          if(rx->filter_a!=FVar1) {
            receiver_filter_changed(rx,FVar1);
          }
          FILTER *mode_filters=filters[rx->mode_a];
          FILTER *filter=&mode_filters[FVar1];
	  if(command[2]==';') {
            int fl=2;
            int low=filter->low;
            if(rx->mode_a==LSB) {
              low=abs(filter->high);
            }
          
            if(low<=10) {
              fl=0;
            } else if(low<=50) {
              fl=1;
            } else if(low<=100) {
              fl=2;
            } else if(low<=200) {
              fl=3;
            } else if(low<=300) {
              fl=4;
            } else if(low<=400) {
              fl=5;
            } else if(low<=500) {
              fl=6;
            } else if(low<=600) {
              fl=7;
            } else if(low<=700) {
              fl=8;
            } else if(low<=800) {
              fl=9;
            } else if(low<=900) {
              fl=10;
            } else {
              fl=11;
            }
            sprintf(reply,"SL%02d;",fl);
            send_resp(cmd,reply) ;
          } else if(command[4]==';') {
            int i=atoi(&command[2]);
            int fl=100;
            switch(rx->mode_a) {
              case LSB:
              case USB:
              case FMN:
                switch(i) {
                  case 0:
                    fl=10;
                    break;
                  case 1:
                    fl=50;
                    break;
                  case 2:
                    fl=100;
                    break;
                  case 3:
                    fl=200;
                    break;
                  case 4:
                    fl=300;
                    break;
                  case 5:
                    fl=400;
                    break;
                  case 6:
                    fl=500;
                    break;
                  case 7:
                    fl=600;
                    break;
                  case 8:
                    fl=700;
                    break;
                  case 9:
                    fl=800;
                    break;
                  case 10:
                    fl=900;
                    break;
                  case 11:
                    fl=1000;
                    break;
                  default:
                    fl=100;
                    break;
                }
                break;
              case AM:
              case SAM:
                switch(i) {
                  case 0:
                    fl=10;
                    break;
                  case 1:
                    fl=100;
                    break;
                  case 2:
                    fl=200;
                    break;
                  case 3:
                    fl=500;
                    break;
                  default:
                    fl=100;
                    break;
                }
                break;
            }
            if(rx->mode_a==LSB) {
              filter->high=-fl;
            } else {
              filter->low=fl;
            }
            receiver_filter_changed(rx,FVar1);
          }
          }
          break;
        case 'M': //SM
          // read the S meter
          if(command[3]==';') {
            int id=atoi(&command[2]);
            if(id==0 || id==1) {
              sprintf(reply,"SM%04d;",(int)rx->meter_db);
              send_resp(cmd,reply);
            }
          }
          break;
        case 'Q': //SQ
          // set/read Squelch level
/*
          if(command[3]==';') {
            int p1=atoi(&command[2]);
            if(p1==0) { // Main receiver
              sprintf(reply,"SQ%d%03d;",p1,(int)((double)rx->squelch/100.0*255.0));
              send_resp(cmd,reply);
            }
          } else if(command[6]==';') {
            if(command[2]=='0') {
              int p2=atoi(&command[3]);
              rx->squelch=(int)((double)p2/255.0*100.0);
              set_squelch();
            }
          } else {
          }
*/
          break;
        case 'R': //SR
          // reset transceiver
          implemented=FALSE;
          break;
        case 'S': //SS
          // set/read program scan pause frequency
          implemented=FALSE;
          break;
        case 'T': //ST
          // set/read MULTI/CH channel frequency steps
          implemented=FALSE;
          break;
        case 'U': //SU
          // set/read program scan pause frequency
          implemented=FALSE;
          break;
        case 'V': //SV
          // execute memory transfer function
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'T':
      switch(command[1]) {
        case 'C': //TC
          // set/read internal TNC mode
          implemented=FALSE;
          break;
        case 'D': //TD
          // send DTMF memory channel data
          implemented=FALSE;
          break;
        case 'I': //TI
          // read TNC LED status
          implemented=FALSE;
          break;
        case 'N': //TN
          // set/read sub-tone frequency
          implemented=FALSE;
          break;
        case 'O': //TO
          // set/read TONE function
          implemented=FALSE;
          break;
        case 'S': //TS
          // set/read TF-SET function
          implemented=FALSE;
          break;
        case 'X': //TX
          // set transceiver to TX mode
          if(command[2]==';') {
            //set_mox(radio,TRUE);
            MOX_STATE *m=g_new0(MOX_STATE,1);
            m->radio=radio;
            m->state=1;
            g_idle_add(ext_set_mox,(gpointer)m);
          }
          break;
        case 'Y': //TY
          // set/read microprocessor firmware type
          implemented=FALSE;
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'U':
      switch(command[1]) {
        case 'L': //UL
          // detects the PLL unlock status
          implemented=FALSE;
          break;
        case 'P': //UP
          // move VFO A up by step
          if(command[2]==';') {
            receiver_move(rx,rx->step,TRUE);
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'V':
      switch(command[1]) {
        case 'D': //VD
          // set/read VOX delay time
          implemented=FALSE;
          break;
        case 'G': //VG
          // set/read VOX gain (0..9)
          if(command[2]==';') {
            // convert 0.0..1.0 to 0..9
            sprintf(reply,"VG%03d;",(int)((radio->vox_threshold*100.0)*0.9));
            send_resp(cmd,reply);
          } else if(command[5]==';') {
            // convert 0..9 to 0.0..1.0
            radio->vox_threshold=atof(&command[2])/9.0;
            update_vfo(rx);
          }
          break;
        case 'R': //VR
          // emulate VOICE1 or VOICE2 key
          implemented=FALSE;
          break;
        case 'X': //VX
          // set/read VOX status
          if(command[2]==';') {
            sprintf(reply,"VX%d;",radio->vox_enabled);
            send_resp(cmd,reply);
          } else if(command[3]==';') {
            radio->vox_enabled=atoi(&command[2]);
            update_vfo(rx);
          }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'W':
      switch(command[1]) {
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'X':
      switch(command[1]) {
        case 'T': //XT
          // set/read XIT enable
	  if(radio->transmitter) {
            if(command[2]==';') {
              sprintf(reply,"XT%d;",radio->transmitter->xit_enabled);
              send_resp(cmd,reply);
            } else if(command[3]==';') {
              radio->transmitter->xit_enabled=atoi(&command[2]);
              update_vfo(rx);
            }
          } else {
            implemented=FALSE;
	  }
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'Y':
      switch(command[1]) {
        default:
          implemented=FALSE;
          break;
      }
      break;
    case 'Z':
      switch(command[1]) {
        case 'Z':
          implemented=parse_extended_cmd(cmd);
          break;
        default:
          implemented=FALSE;
          break;
      }
      break;
    default:
      implemented=FALSE;
      break;
  }

  if(!implemented) {
    //if(rigctl->debug)
      log_info("%s: fd=%d UNIMPLEMENTED COMMAND: %s\n",__FUNCTION__,cmd->fd,cmd->command);
    send_resp(cmd,"?;");
  }

  g_free(cmd->command);
  g_free(cmd);
  return 0;
}
