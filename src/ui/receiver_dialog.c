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

#include "waterfall.h"
#include "log.h"
#include "waterfall_theme.h"
#include <gtk/gtk.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <wdsp.h>

#include "button_text.h"
#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "receiver_dialog.h"
#include "vfo.h"
#include "audio.h"
#include "main.h"
#include "rigctl.h"

#define BAND_COLUMNS 5
#define MODE_COLUMNS 4
#define FILTER_COLUMNS 5

typedef struct _SELECT {
  RECEIVER *rx;
  gint choice;
} SELECT;

static void update_filters(RECEIVER *rx);

/* TO REMOVE
static gboolean close_cb (GtkWidget *widget, GdkEventButton *event, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->dialog=NULL;
  rx->band_grid=NULL;
  rx->mode_grid=NULL;
  rx->filter_frame=NULL;
  rx->filter_grid=NULL;
  return TRUE;
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->dialog=NULL;
  rx->band_grid=NULL;
  rx->mode_grid=NULL;
  rx->filter_frame=NULL;
  rx->filter_grid=NULL;
  return FALSE;
}
*/

static void sample_rate_cb(GtkWidget *widget,gpointer data) {
  if(!gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) return;
  SELECT *select=(SELECT *)data;
  RECEIVER *rx=select->rx;
  int sample_rate=select->choice;
  receiver_change_sample_rate(rx,sample_rate);
}

static void adc_cb(GtkWidget *widget,gpointer data) {
  if(!gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) return;
  SELECT *select=(SELECT *)data;
  RECEIVER *rx=select->rx;
  rx->adc=select->choice;
  receiver_update_title(rx);
}

static gboolean filter_select_cb(GtkWidget *widget,gpointer data) {
  if(!gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) return TRUE;
  SELECT *select=(SELECT *)data;
  RECEIVER *rx=select->rx;
  gint f=select->choice;
  GtkWidget *grid=gtk_widget_get_ancestor(widget, GTK_TYPE_GRID);
  int x=rx->filter_a%FILTER_COLUMNS;
  int y=rx->filter_a/FILTER_COLUMNS;
  if(rx->filter_a>=FVar1) {
    y=1+((rx->filter_a+4)/5);
    x=0;
  }
  GtkWidget *b=gtk_grid_get_child_at(GTK_GRID(grid),x,y);
  set_button_text_color(b,"black");
  set_button_text_color(widget,"orange");
  receiver_filter_changed(rx,f);
  return TRUE;
}

static gboolean deviation_select_cb(GtkWidget *widget,gpointer data) {
  if(!gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) return TRUE;
  SELECT *select=(SELECT *)data;
  RECEIVER *rx=select->rx;
  rx->deviation=select->choice;
  //transmitter->deviation=select->choice;
  if(rx->deviation==2500) {
    set_filter(rx,-4000,4000);
    if(radio->transmitter) transmitter_set_filter(radio->transmitter,-4000,4000);
  } else {
    set_filter(rx,-8000,8000);
    if(radio->transmitter) transmitter_set_filter(radio->transmitter,-8000,8000);
  }
  set_deviation(rx);
  if(radio->transmitter) transmitter_set_deviation(radio->transmitter);
  update_vfo(rx);
  return TRUE;
}

static void var_spin_low_cb (GtkWidget *widget, gpointer data) {
  SELECT *select=(SELECT *)data;
  RECEIVER *rx=select->rx;
  gint f=select->choice;

  FILTER *mode_filters=filters[rx->mode_a];
  FILTER *filter=&mode_filters[f];

  filter->low=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
  if(rx->mode_a==CWL || rx->mode_a==CWU) {
    filter->high=filter->low;
  }
  if(f==rx->filter_a) {
    receiver_filter_changed(rx,f);
  }
}

static void var_spin_high_cb (GtkWidget *widget, gpointer data) {
  SELECT *select=(SELECT *)data;
  RECEIVER *rx=select->rx;
  gint f=select->choice;

  FILTER *mode_filters=filters[rx->mode_a];
  FILTER *filter=&mode_filters[f];

  filter->high=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
  if(f==rx->filter_a) {
    receiver_filter_changed(rx,f);
  }
}


static void update_filters(RECEIVER *rx) {
  int i;
  int row;
  SELECT *select;

  FILTER* band_filters=filters[rx->mode_a];

  if(rx->filter_frame!=NULL && rx->filter_grid!=NULL) {
    // GTK4: detach the old child (a frame holds a single child).
    gtk_frame_set_child(GTK_FRAME(rx->filter_frame),NULL);
  }

  rx->filter_grid=gtk_grid_new();
log_info("update_filters: new filter grid %p\n",rx->filter_grid);
  gtk_grid_set_row_homogeneous(GTK_GRID(rx->filter_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(rx->filter_grid),FALSE);
  gtk_frame_set_child(GTK_FRAME(rx->filter_frame),rx->filter_grid);
  switch(rx->mode_a) {
    case FMN:
      {
      GtkWidget *l=gtk_label_new("Deviation:");
      gtk_grid_attach(GTK_GRID(rx->filter_grid),l,0,1,1,1);

      GtkWidget *b=gtk_button_new_with_label("2.5K");
      if(rx->deviation==2500) {
        set_button_text_color(b,"orange");
        //last_filter=b;
      } else {
        set_button_text_color(b,"black");
      }
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=2500;
      g_signal_connect(b,"toggled",G_CALLBACK(deviation_select_cb),(gpointer)select);
      gtk_grid_attach(GTK_GRID(rx->filter_grid),b,1,1,1,1);

      b=gtk_button_new_with_label("5.0K");
      if(rx->deviation==5000) {
        set_button_text_color(b,"orange");
        //last_filter=b;
      } else {
        set_button_text_color(b,"black");
      }
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=5000;
      g_signal_connect(b,"toggled",G_CALLBACK(deviation_select_cb),(gpointer)select);
      gtk_grid_attach(GTK_GRID(rx->filter_grid),b,2,1,1,1);
      }
      break;

    default:
      /*
      for(i=0;i<FILTERS-2;i++) {
        FILTER* band_filter=&band_filters[i];
        GtkWidget *b=gtk_button_new_with_label(band_filters[i].title);
        if(i==rx->filter_a) {
          set_button_text_color(b,"orange");
          //last_filter=b;
        } else {
          set_button_text_color(b,"black");
        }
        select=g_new0(SELECT,1);
        select->rx=rx;
        select->choice=i;
        g_signal_connect(b,"toggled",G_CALLBACK(filter_select_cb),(gpointer)select);
        gtk_grid_attach(GTK_GRID(rx->filter_grid),b,i%FILTER_COLUMNS,i/FILTER_COLUMNS,1,1);
      }
      */

  // last 2 are var1 and var2
      i=FILTERS-2;
      row=1+((i+4)/5);
      FILTER* band_filter=&band_filters[i];
      GtkWidget *b=gtk_button_new_with_label(band_filters[i].title);
      if(i==rx->filter_a) {
        set_button_text_color(b,"orange");
        //last_filter=b;
      } else {
        set_button_text_color(b,"black");
      }
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=i;
      g_signal_connect(b,"toggled",G_CALLBACK(filter_select_cb),(gpointer)select);
      gtk_grid_attach(GTK_GRID(rx->filter_grid),b,0,row,1,1);

      GtkWidget *var1_spin_low=gtk_spin_button_new_with_range(-8000.0,+8000.0,1.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(var1_spin_low),(double)band_filter->low);
      gtk_grid_attach(GTK_GRID(rx->filter_grid),var1_spin_low,1,row,2,1);
      g_signal_connect(var1_spin_low,"value-changed",G_CALLBACK(var_spin_low_cb),(gpointer)select);

      if(rx->mode_a!=CWL && rx->mode_a!=CWU) {
        GtkWidget *var1_spin_high=gtk_spin_button_new_with_range(-8000.0,+8000.0,1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(var1_spin_high),(double)band_filter->high);
        gtk_grid_attach(GTK_GRID(rx->filter_grid),var1_spin_high,3,row,2,1);
        g_signal_connect(var1_spin_high,"value-changed",G_CALLBACK(var_spin_high_cb),(gpointer)select);
      }

      row++;

      i++;
      band_filter=&band_filters[i];
      b=gtk_button_new_with_label(band_filters[i].title);
      if(i==rx->filter_a) {
        set_button_text_color(b,"orange");
        //last_filter=b;
      } else {
        set_button_text_color(b,"black");
      }
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=i;
      gtk_grid_attach(GTK_GRID(rx->filter_grid),b,0,row,1,1);
      g_signal_connect(b,"toggled",G_CALLBACK(filter_select_cb),(gpointer)select);

     GtkWidget *var2_spin_low=gtk_spin_button_new_with_range(-8000.0,+8000.0,1.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(var2_spin_low),(double)band_filter->low);
      gtk_grid_attach(GTK_GRID(rx->filter_grid),var2_spin_low,1,row,2,1);
      g_signal_connect(var2_spin_low,"value-changed",G_CALLBACK(var_spin_low_cb),(gpointer)select);

     if(rx->mode_a!=CWL && rx->mode_a!=CWU) {
        GtkWidget *var2_spin_high=gtk_spin_button_new_with_range(-8000.0,+8000.0,1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(var2_spin_high),(double)band_filter->high);
        gtk_grid_attach(GTK_GRID(rx->filter_grid),var2_spin_high,3,row,2,1);
        g_signal_connect(var2_spin_high,"value-changed",G_CALLBACK(var_spin_high_cb),(gpointer)select);
      }
    gtk_widget_set_visible(rx->filter_frame, TRUE);
  }
}

static void tx_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  RECEIVER *temp=radio->transmitter->rx;
  if(radio->transmitter->rx==rx) {
    gtk_check_button_set_active (GTK_CHECK_BUTTON (widget), radio->transmitter->rx==rx);
  } else {
    radio->transmitter->rx=rx;
  }
  update_vfo(temp);
  update_vfo(rx);
  transmitter_set_mode(radio->transmitter,rx->mode_a);
}

static void meter_smoothing_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->meter_smoothing=(int)gtk_range_get_value(GTK_RANGE(widget));
  rx->meter_needle_init=0;   // reseed so the needle snaps to the live reading
}

static void fps_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->fps=gtk_range_get_value(GTK_RANGE(widget));
  receiver_fps_changed(rx);
}


static void panadapter_average_time_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->display_average_time=gtk_range_get_value(GTK_RANGE(widget));
  calculate_display_average(rx);
}

static void panadapter_high_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_high=gtk_range_get_value(GTK_RANGE(widget));
}

static void panadapter_low_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_low=gtk_range_get_value(GTK_RANGE(widget));
}

static void panadapter_step_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_step=gtk_range_get_value(GTK_RANGE(widget));
}

static void panadapter_filled_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_filled=rx->panadapter_filled==TRUE?FALSE:TRUE;
}

static void panadapter_gradient_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_gradient=rx->panadapter_gradient==TRUE?FALSE:TRUE;
}

static void panadapter_agc_line_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_agc_line=rx->panadapter_agc_line==TRUE?FALSE:TRUE;
}

static void show_panadapter_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->show_panadapter=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  receiver_apply_panadapter_visibility(rx);
}


// Panadapter trace-colour choices, in combo-box order. The index stored in
// rx->panadapter_single_color matches the switch() in rx_panadapter.c:
//   0=gradient, 1=skin accent, 2..9=fixed colours.
static const char *panadapter_color_names[] = {
  "Gradient (S-meter)",
  "Skin Accent",
  "Red",
  "Orange",
  "Yellow",
  "Green",
  "Blue",
  "Violet",
  "Magenta",
  "Cyan",
};
#define PANADAPTER_COLOR_COUNT (sizeof(panadapter_color_names)/sizeof(panadapter_color_names[0]))

static void panadapter_single_color_changed_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_single_color=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
}

// WDSP display detector/averaging mode choices, in combo-box order == the
// WDSP DETECTOR_MODE_*/AVERAGE_MODE_* constants (wdsp.h).
static const char *display_detector_mode_names[] = {
  "Peak",
  "Rosenfell",
  "Average",
  "Sample",
};
static const char *display_average_mode_names[] = {
  "None",
  "Recursive",
  "Time Window",
  "Log Recursive",
};

static void display_detector_mode_changed_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->display_detector_mode=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  SetDisplayDetectorMode(rx->channel, 0, rx->display_detector_mode);
}

static void display_average_mode_changed_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->display_average_mode=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  SetDisplayAverageMode(rx->channel, 0, rx->display_average_mode);
  calculate_display_average(rx);
}

static void panadapter_peak_hold_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_peak_hold=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  if(rx->panadapter_peak_hold && rx->panadapter_peaks!=NULL) {
    for(int i=0;i<rx->pixels;i++) rx->panadapter_peaks[i]=-220.0f;
  }
}

static void panadapter_peak_decay_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_peak_decay=(int)gtk_range_get_value(GTK_RANGE(widget));
}

static void waterfall_high_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->waterfall_high=gtk_range_get_value(GTK_RANGE(widget));
}

static void waterfall_low_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->waterfall_low=gtk_range_get_value(GTK_RANGE(widget));
}

static void waterfall_automatic_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->waterfall_automatic=rx->waterfall_automatic==1?0:1;
}

static void waterfall_ft8_marker_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->waterfall_ft8_marker=rx->waterfall_ft8_marker==TRUE?FALSE:TRUE;
}

static void waterfall_theme_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  int theme = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  rx->waterfall_color_theme = theme;
  waterfall_set_theme(rx, theme);
  // Persist immediately so the theme survives even if the app is quit without
  // a clean window-close (e.g. Cmd-Q), which would skip the exit-time save.
  radio_save_state(radio);
}

static void remote_audio_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->remote_audio=rx->remote_audio==TRUE?FALSE:TRUE;
}

static void mute_while_tx_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->mute_while_transmitting = rx->mute_while_transmitting == TRUE?FALSE:TRUE;
}

static void local_audio_cb(GtkWidget *widget,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->local_audio=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  if(rx->local_audio) {
    if(audio_open_output(rx)<0) {
      rx->local_audio=FALSE;
      gtk_check_button_set_active(GTK_CHECK_BUTTON (widget),FALSE);
    }
  } else {
    audio_close_output(rx);
  }
}

static void audio_channels_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->audio_channels = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
}

// Sub-RX crossfeed: 0 = hard split (main left / sub right), 100 = mono blend in
// both ears. Read live by process_rx_buffer(), so no extra WDSP call is needed.
static void subrx_mix_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->subrx_mix=(int)gtk_range_get_value(GTK_RANGE(widget));
}

static void audio_choice_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  int i;
  if(rx->local_audio) {
    audio_close_output(rx);
    i=(int)gtk_drop_down_get_selected(widget);
    if(rx->audio_name!=NULL) {
      g_free(rx->audio_name);
      //rx->audio_name=NULL;
    }
    if(i>=0) {
      rx->audio_name=g_new0(gchar,strlen(output_devices[i].name)+1);
      rx->output_index=output_devices[i].index;
      strcpy(rx->audio_name,output_devices[i].name);
      if(audio_open_output(rx)<0) {
        rx->local_audio=FALSE;
        gtk_check_button_set_active(GTK_CHECK_BUTTON (rx->local_audio_b),FALSE);
      }
    }
  } else {
    i=(int)gtk_drop_down_get_selected(widget);
    if(rx->audio_name!=NULL) {
      g_free(rx->audio_name);
      rx->audio_name=NULL;
    }
    if(i>=0) {
      rx->audio_name=g_new0(gchar,strlen(output_devices[i].name)+1);
      strcpy(rx->audio_name,output_devices[i].name);
    }
  }
  if((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(rx->audio_choice_b))==-1) {
    gtk_widget_set_sensitive(rx->local_audio_b, FALSE);
  } else {
    gtk_widget_set_sensitive(rx->local_audio_b, TRUE);
  }
  if(i>=0) {
    log_info("Output device changed: %d: %s (%s)\n",i,output_devices[i].name,output_devices[i].description);
  } else {
    log_info("Output device changed: %d\n",i);
  }
}

/* TO REMOVE
static void buffer_size_spin_cb(GtkWidget *widget,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->local_audio_buffer_size=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
}

static void latency_spin_cb(GtkWidget *widget,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->local_audio_latency=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
}
*/

static void enable_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->enable_equalizer=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  SetRXAEQRun(rx->channel, rx->enable_equalizer);
}

static void rx_eq_value_changed_cb (GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"eq_band"));
  rx->equalizer[idx]=(int)gtk_range_get_value(GTK_RANGE(widget));
  SetRXAGrphEQ10(rx->channel, rx->equalizer);
}


static void cat_debug_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->rigctl_debug=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  if(rx->rigctl!=NULL) {
    rigctl_set_debug(rx);
  }
}

static void cat_enable_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->rigctl_enable=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  if(rx->rigctl_enable) {
    launch_rigctl(rx);
  } else {
    disable_rigctl(rx);
  }
}

static void rigctl_value_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->rigctl_port = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
}

static void cat_serial_enable_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  strcpy(rx->rigctl_serial_port,gtk_editable_get_text(GTK_EDITABLE(rx->serial_port_entry)));
  rx->rigctl_serial_enable=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  if(rx->rigctl_serial_enable) {
    launch_serial(rx);
  } else {
    disable_serial(rx);
  }
}

static void cat_serial_port_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  strcpy(rx->rigctl_serial_port,gtk_editable_get_text(GTK_EDITABLE(widget)));
}

static void cat_baudrate_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  int selected=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  switch(selected) {
    case 0:
      rx->rigctl_serial_baudrate=B4800;
      break;
    case 1:
      rx->rigctl_serial_baudrate=B9600;
      break;
    case 2:
      rx->rigctl_serial_baudrate=B19200;
      break;
    case 3:
      rx->rigctl_serial_baudrate=B38400;
      break;
  }
}

void update_receiver_dialog(RECEIVER *rx) {
  int i;

  // re-scan audio devices so a device connected after launch (e.g. Bluetooth
  // headphones) shows up in the list without restarting the app
  audio_refresh_devices();

  // update audio
  g_signal_handler_block(G_OBJECT(rx->audio_choice_b),rx->audio_choice_signal_id);
  g_signal_handler_block(G_OBJECT(rx->local_audio_b),rx->local_audio_signal_id);
  GtkStringList *audio_sl=GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(rx->audio_choice_b)));
  gtk_string_list_splice(audio_sl,0,g_list_model_get_n_items(G_LIST_MODEL(audio_sl)),NULL);
  for(i=0;i<n_output_devices;i++) {
    gtk_string_list_append(audio_sl,output_devices[i].description);
    if(rx->audio_name!=NULL) {
      if(strcmp(output_devices[i].name,rx->audio_name)==0) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(rx->audio_choice_b),i);
      }
    }
  }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (rx->local_audio_b), rx->local_audio);

  if((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(rx->audio_choice_b))==-1) {
    gtk_widget_set_sensitive(rx->local_audio_b, FALSE);
  } else {
    gtk_widget_set_sensitive(rx->local_audio_b, TRUE);
  }

  g_signal_handler_unblock(G_OBJECT(rx->local_audio_b),rx->local_audio_signal_id);
  g_signal_handler_unblock(G_OBJECT(rx->audio_choice_b),rx->audio_choice_signal_id);

  if(radio->transmitter) {
    // update TX Frequency
    g_signal_handler_block(G_OBJECT(rx->tx_control_b),rx->tx_control_signal_id);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (rx->tx_control_b), radio->transmitter->rx==rx);
    g_signal_handler_unblock(G_OBJECT(rx->tx_control_b),rx->tx_control_signal_id);
  }

}

GtkWidget *create_receiver_dialog(RECEIVER *rx) {
  int i;
  int col=0;
  int row=0;
  SELECT *select;

  GtkWidget *grid=gtk_grid_new();
  sui_style_page(grid);
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);

  row=0;
  col=0;

  if(radio->discovered->adcs>1) {
    GtkWidget *adc_frame=gtk_frame_new("ADC");
    GtkWidget *adc_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(adc_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(adc_grid),FALSE);
    sui_style_group(adc_grid);
    gtk_frame_set_child(GTK_FRAME(adc_frame),adc_grid);
    gtk_grid_attach(GTK_GRID(grid),adc_frame,col,row++,1,1);

    GtkWidget *adc0_b=gtk_check_button_new_with_label("ADC-0");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (adc0_b), rx->adc==0);
    gtk_grid_attach(GTK_GRID(adc_grid),adc0_b,0,0,1,1);
    select=g_new0(SELECT,1);
    select->rx=rx;
    select->choice=0;
    g_signal_connect(adc0_b,"toggled",G_CALLBACK(adc_cb),(gpointer)select);

    GtkWidget *adc1_b=gtk_check_button_new_with_label("ADC-1"); gtk_check_button_set_group(GTK_CHECK_BUTTON(adc1_b),GTK_CHECK_BUTTON(adc0_b));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (adc1_b), rx->adc==1);
    select=g_new0(SELECT,1);
    select->rx=rx;
    select->choice=1;
    g_signal_connect(adc1_b,"toggled",G_CALLBACK(adc_cb),(gpointer)select);
    gtk_grid_attach(GTK_GRID(adc_grid),adc1_b,1,0,1,1);
  }

  switch(radio->discovered->protocol) {
    case PROTOCOL_2:
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      if(strcmp(radio->discovered->name,"sdrplay")!=0)
#endif
      {
      int x=0;
      int y=0;

      GtkWidget *sample_rate_frame=gtk_frame_new("Sample Rate");
      GtkWidget *sample_rate_grid=gtk_grid_new();
      gtk_grid_set_row_homogeneous(GTK_GRID(sample_rate_grid),FALSE);
      gtk_grid_set_column_homogeneous(GTK_GRID(sample_rate_grid),FALSE);
      sui_style_group(sample_rate_grid);
      gtk_frame_set_child(GTK_FRAME(sample_rate_frame),sample_rate_grid);
      gtk_grid_attach(GTK_GRID(grid),sample_rate_frame,col,row,1,2);
      row+=2;

      GtkWidget *sample_rate_48=gtk_check_button_new_with_label("48000");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_48), rx->sample_rate==48000);
      gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_48,x,y++,1,1);
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=48000;
      g_signal_connect(sample_rate_48,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);

      GtkWidget *sample_rate_96=gtk_check_button_new_with_label("96000"); gtk_check_button_set_group(GTK_CHECK_BUTTON(sample_rate_96),GTK_CHECK_BUTTON(sample_rate_48));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_96), rx->sample_rate==96000);
      gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_96,x,y++,1,1);
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=96000;
      g_signal_connect(sample_rate_96,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);

      GtkWidget *sample_rate_192=gtk_check_button_new_with_label("192000"); gtk_check_button_set_group(GTK_CHECK_BUTTON(sample_rate_192),GTK_CHECK_BUTTON(sample_rate_96));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_192), rx->sample_rate==192000);
      gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_192,x,y++,1,1);
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=192000;
      g_signal_connect(sample_rate_192,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);

      x++;
      y=0;
      GtkWidget *sample_rate_384=gtk_check_button_new_with_label("384000"); gtk_check_button_set_group(GTK_CHECK_BUTTON(sample_rate_384),GTK_CHECK_BUTTON(sample_rate_192));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_384), rx->sample_rate==384000);
      gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_384,x,y++,1,1);
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=384000;
      g_signal_connect(sample_rate_384,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);

      if((radio->discovered->protocol==PROTOCOL_2)
#ifdef SOAPYSDR
          || (radio->discovered->protocol==PROTOCOL_SOAPYSDR)
#endif
      ) {
        GtkWidget *sample_rate_768=gtk_check_button_new_with_label("768000"); gtk_check_button_set_group(GTK_CHECK_BUTTON(sample_rate_768),GTK_CHECK_BUTTON(sample_rate_384));
        gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_768), rx->sample_rate==768000);
        gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_768,x,y++,1,1);
        select=g_new0(SELECT,1);
        select->rx=rx;
        select->choice=768000;
        g_signal_connect(sample_rate_768,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);

        if(radio->discovered->protocol==PROTOCOL_2) {
          GtkWidget *sample_rate_1536=gtk_check_button_new_with_label("1536000"); gtk_check_button_set_group(GTK_CHECK_BUTTON(sample_rate_1536),GTK_CHECK_BUTTON(sample_rate_768));
          gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_1536), rx->sample_rate==1536000);
          gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_1536,x,y++,1,1);
          select=g_new0(SELECT,1);
          select->rx=rx;
          select->choice=1536000;
          g_signal_connect(sample_rate_1536,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);
        }
      }
    }
    break;
  }

  rx->filter_frame=gtk_frame_new("Filter");
  gtk_grid_attach(GTK_GRID(grid),rx->filter_frame,col,row,1,1);
  row++;

  update_filters(rx);

  col=0;

  if(n_output_devices>=0) {
    GtkWidget *audio_frame=gtk_frame_new("Audio");
    GtkWidget *audio_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(audio_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(audio_grid),FALSE);
    sui_style_group(audio_grid);
    gtk_frame_set_child(GTK_FRAME(audio_frame),audio_grid);
    gtk_grid_attach(GTK_GRID(grid),audio_frame,col,row,1,1);
    row++;

    rx->local_audio_b=gtk_check_button_new_with_label("Local Audio");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (rx->local_audio_b), rx->local_audio);
    gtk_grid_attach(GTK_GRID(audio_grid),rx->local_audio_b,0,0,1,1);
    rx->local_audio_signal_id=g_signal_connect(rx->local_audio_b,"toggled",G_CALLBACK(local_audio_cb),rx);


    if(radio->discovered->device!=DEVICE_HERMES_LITE2
#ifdef SOAPYSDR
       && radio->discovered->device!=DEVICE_SOAPYSDR
#endif
      ) {

      GtkWidget *remote_audio=gtk_check_button_new_with_label("Remote Audio");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (remote_audio), rx->remote_audio);
      gtk_grid_attach(GTK_GRID(audio_grid),remote_audio,1,0,1,1);
      g_signal_connect(remote_audio,"toggled",G_CALLBACK(remote_audio_cb),rx);
    }

    rx->audio_choice_b=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    gtk_grid_attach(GTK_GRID(audio_grid),rx->audio_choice_b,0,2,2,1);
    rx->audio_choice_signal_id=g_signal_connect(rx->audio_choice_b,"notify::selected",G_CALLBACK(audio_choice_cb),rx);

    // Stereo, left, right audio
    const char *audio_ch_opts[]={"Stereo","Left","Right",NULL};
    GtkWidget *audio_channels_combo=gtk_drop_down_new_from_strings(audio_ch_opts);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(audio_channels_combo),rx->audio_channels);
    gtk_grid_attach(GTK_GRID(audio_grid),audio_channels_combo,0,1,2,1);
    g_signal_connect(audio_channels_combo,"notify::selected",G_CALLBACK(audio_channels_cb),rx);

    GtkWidget *tx_mute_b = gtk_check_button_new_with_label("Mute while TX");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(tx_mute_b), rx->mute_while_transmitting);
    gtk_grid_attach(GTK_GRID(audio_grid),tx_mute_b,0,3,1,1);
    g_signal_connect(tx_mute_b,"toggled",G_CALLBACK(mute_while_tx_cb),rx);

    // Sub-RX audio balance: split (main L / sub R) .. mono (both in both ears).
    GtkWidget *subrx_mix_label=gtk_label_new("Sub-RX mix (split↔mono):");
    gtk_label_set_xalign(GTK_LABEL(subrx_mix_label),0.0);
    gtk_grid_attach(GTK_GRID(audio_grid),subrx_mix_label,0,4,1,1);
    GtkWidget *subrx_mix_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,
        gtk_adjustment_new(rx->subrx_mix,0.0,100.0,1.0,10.0,0.0));
    gtk_widget_set_size_request(subrx_mix_scale,160,25);
    sui_scale_show_value(subrx_mix_scale,0);
    gtk_grid_attach(GTK_GRID(audio_grid),subrx_mix_scale,1,4,1,1);
    g_signal_connect(G_OBJECT(subrx_mix_scale),"value_changed",G_CALLBACK(subrx_mix_changed_cb),rx);
  }

  GtkWidget *equalizer_frame=gtk_frame_new("Equalizer");
  GtkWidget *equalizer_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(equalizer_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(equalizer_grid),FALSE);
  sui_style_group(equalizer_grid);
  // Keep the 11 bands tight (see the matching TX-EQ comment).
  gtk_grid_set_column_spacing(GTK_GRID(equalizer_grid),2);
  gtk_widget_set_halign(equalizer_grid,GTK_ALIGN_START);
  gtk_frame_set_child(GTK_FRAME(equalizer_frame),equalizer_grid);
  gtk_grid_attach(GTK_GRID(grid),equalizer_frame,col,row,1,4);
  row+=4;

  GtkWidget *enable_b=gtk_check_button_new_with_label("Enable Equalizer");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (enable_b), rx->enable_equalizer);
  g_signal_connect(enable_b,"toggled",G_CALLBACK(enable_cb),rx);
  gtk_grid_attach(GTK_GRID(equalizer_grid),enable_b,0,0,11,1);

  const char *eq_band_labels[11]={"Pre","32","63","125","250","500","1k","2k","4k","8k","16k"};
  for(int i=0;i<11;i++) {
    GtkWidget *label=gtk_label_new(eq_band_labels[i]);
    gtk_widget_set_halign(label,GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(equalizer_grid),label,i,1,1,1);

    GtkWidget *scale=gtk_scale_new(GTK_ORIENTATION_VERTICAL,gtk_adjustment_new(rx->equalizer[i],-12.0,15.0,1.0,1.0,1.0));
    gtk_range_set_inverted(GTK_RANGE(scale),TRUE);
    gtk_widget_set_hexpand(scale,FALSE);
    gtk_widget_set_halign(scale,GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(scale,"eq-scale");
    g_object_set_data(G_OBJECT(scale),"eq_band",GINT_TO_POINTER(i));
    g_signal_connect(scale,"value-changed",G_CALLBACK(rx_eq_value_changed_cb),rx);
    gtk_grid_attach(GTK_GRID(equalizer_grid),scale,i,2,1,10);
    gtk_widget_set_size_request(scale,16,220);
    // Only the leftmost band carries the dB scale; the rest get no marks so the
    // 11 bands pack tight (see the matching TX-EQ comment).
    if(i==0) {
      for(int m=-12;m<=15;m+=3) {
        gtk_scale_add_mark(GTK_SCALE(scale),(double)m,GTK_POS_LEFT,
                           (m==-12)?"-12":(m==0)?"0":(m==15)?"+15":NULL);
      }
    }
  }

  col++;
  row=0;

  if(strcmp(radio->discovered->name,"rtlsdr")!=0) {
    GtkWidget *tx_frame=gtk_frame_new("TX Frequency");
    GtkWidget *tx_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(tx_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(tx_grid),FALSE);
    sui_style_group(tx_grid);
    gtk_frame_set_child(GTK_FRAME(tx_frame),tx_grid);
    gtk_grid_attach(GTK_GRID(grid),tx_frame,col,row,1,1);
    row++;

    rx->tx_control_b=gtk_check_button_new_with_label("Use This Receivers Frequency");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (rx->tx_control_b), radio->transmitter!=NULL && radio->transmitter->rx==rx);
    gtk_grid_attach(GTK_GRID(tx_grid),rx->tx_control_b,0,0,1,1);
    rx->tx_control_signal_id=g_signal_connect(rx->tx_control_b,"toggled",G_CALLBACK(tx_cb),rx);
  }

  GtkWidget *panadapter_frame=gtk_frame_new("Panadapter");
  GtkWidget *panadapter_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(panadapter_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(panadapter_grid),FALSE);
  sui_style_group(panadapter_grid);
  gtk_frame_set_child(GTK_FRAME(panadapter_frame),panadapter_grid);
  gtk_grid_attach(GTK_GRID(grid),panadapter_frame,col,row,1,3);
  row+=3;


  GtkWidget *fps_label=gtk_label_new("FPS:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),fps_label,0,0,1,1);

  GtkWidget *fps_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->fps, 1.0, 50.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(fps_scale,200,30);
  sui_scale_show_value(fps_scale,0);
  gtk_widget_set_visible(fps_scale, TRUE);
  g_signal_connect(G_OBJECT(fps_scale),"value_changed",G_CALLBACK(fps_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),fps_scale,1,0,1,1);

  GtkWidget *average_label=gtk_label_new("Average:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),average_label,0,1,1,1);

  GtkWidget *average_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->display_average_time,1.0, 500.0, 1.00, 1.0, 1.0));
  gtk_widget_set_size_request(average_scale,200,30);
  sui_scale_show_value(average_scale,0);
  gtk_widget_set_visible(average_scale, TRUE);
  g_signal_connect(G_OBJECT(average_scale),"value_changed",G_CALLBACK(panadapter_average_time_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),average_scale,1,1,1,1);

  GtkWidget *high_label=gtk_label_new("High:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),high_label,0,2,1,1);

  GtkWidget *panadapter_high_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_high,-200.0, 20.0, 1.00, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_high_scale,200,30);
  sui_scale_show_value(panadapter_high_scale,0);
  gtk_widget_set_visible(panadapter_high_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_high_scale),"value_changed",G_CALLBACK(panadapter_high_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_high_scale,1,2,1,1);

  GtkWidget *low_label=gtk_label_new("Low:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),low_label,0,3,1,1);

  GtkWidget *panadapter_low_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_low,-200.0, 20.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_low_scale,200,30);
  sui_scale_show_value(panadapter_low_scale,0);
  gtk_widget_set_visible(panadapter_low_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_low_scale),"value_changed",G_CALLBACK(panadapter_low_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_low_scale,1,3,1,1);

  GtkWidget *step_label=gtk_label_new("Step:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),step_label,0,4,1,1);

  GtkWidget *panadapter_step_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_step,1.0, 40.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_step_scale,200,30);
  sui_scale_show_value(panadapter_step_scale,0);
  gtk_widget_set_visible(panadapter_step_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_step_scale),"value_changed",G_CALLBACK(panadapter_step_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_step_scale,1,4,1,1);

  GtkWidget *panadapter_filled=gtk_check_button_new_with_label("Panadapter Filled");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_filled), rx->panadapter_filled);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_filled,0,5,2,1);
  g_signal_connect(panadapter_filled,"toggled",G_CALLBACK(panadapter_filled_changed_cb),rx);

  GtkWidget *panadapter_gradient=gtk_check_button_new_with_label("Panadapter Gradient");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_gradient), rx->panadapter_gradient);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_gradient,0,6,2,1);
  g_signal_connect(panadapter_gradient,"toggled",G_CALLBACK(panadapter_gradient_changed_cb),rx);

  GtkWidget *panadapter_agc_line=gtk_check_button_new_with_label("AGC Lines");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_agc_line), rx->panadapter_agc_line);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_agc_line,0,7,2,1);
  g_signal_connect(panadapter_agc_line,"toggled",G_CALLBACK(panadapter_agc_line_changed_cb),rx);

  GtkWidget *panadapter_single_color_label=gtk_label_new("Panadapter Color:");
  gtk_widget_set_visible(panadapter_single_color_label, TRUE);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_single_color_label,0,8,1,1);

  GtkStringList *psc_sl=gtk_string_list_new(NULL);
  for(i=0; i<(int)PANADAPTER_COLOR_COUNT; i++) {
    gtk_string_list_append(psc_sl,panadapter_color_names[i]);
  }
  GtkWidget *panadapter_single_color_b=gtk_drop_down_new(G_LIST_MODEL(psc_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(panadapter_single_color_b),rx->panadapter_single_color);
  gtk_widget_set_visible(panadapter_single_color_b, TRUE);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_single_color_b,1,8,1,1);
  g_signal_connect(panadapter_single_color_b,"notify::selected",G_CALLBACK(panadapter_single_color_changed_cb),rx);

  // Turn the spectroscope off entirely (waterfall then fills the whole area).
  GtkWidget *show_panadapter=gtk_check_button_new_with_label("Show Panadapter");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (show_panadapter), rx->show_panadapter);
  gtk_grid_attach(GTK_GRID(panadapter_grid),show_panadapter,0,9,2,1);
  g_signal_connect(show_panadapter,"toggled",G_CALLBACK(show_panadapter_cb),rx);

  // S-meter needle ballistics (0 = instant, 100 = heaviest damping).
  GtkWidget *meter_smoothing_label=gtk_label_new("Meter smoothing:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),meter_smoothing_label,0,10,1,1);

  GtkWidget *meter_smoothing_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->meter_smoothing, 0.0, 100.0, 1.0, 10.0, 1.0));
  gtk_widget_set_size_request(meter_smoothing_scale,200,30);
  sui_scale_show_value(meter_smoothing_scale,0);
  gtk_widget_set_visible(meter_smoothing_scale, TRUE);
  g_signal_connect(G_OBJECT(meter_smoothing_scale),"value_changed",G_CALLBACK(meter_smoothing_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),meter_smoothing_scale,1,10,1,1);

  GtkWidget *display_detector_mode_label=gtk_label_new("Detector:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_detector_mode_label,0,11,1,1);

  GtkStringList *ddm_sl=gtk_string_list_new(NULL);
  for(i=0; i<(int)(sizeof(display_detector_mode_names)/sizeof(display_detector_mode_names[0])); i++) {
    gtk_string_list_append(ddm_sl,display_detector_mode_names[i]);
  }
  GtkWidget *display_detector_mode_b=gtk_drop_down_new(G_LIST_MODEL(ddm_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(display_detector_mode_b),rx->display_detector_mode);
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_detector_mode_b,1,11,1,1);
  g_signal_connect(display_detector_mode_b,"notify::selected",G_CALLBACK(display_detector_mode_changed_cb),rx);

  GtkWidget *display_average_mode_label=gtk_label_new("Averaging:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_average_mode_label,0,12,1,1);

  GtkStringList *dam_sl=gtk_string_list_new(NULL);
  for(i=0; i<(int)(sizeof(display_average_mode_names)/sizeof(display_average_mode_names[0])); i++) {
    gtk_string_list_append(dam_sl,display_average_mode_names[i]);
  }
  GtkWidget *display_average_mode_b=gtk_drop_down_new(G_LIST_MODEL(dam_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(display_average_mode_b),rx->display_average_mode);
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_average_mode_b,1,12,1,1);
  g_signal_connect(display_average_mode_b,"notify::selected",G_CALLBACK(display_average_mode_changed_cb),rx);

  GtkWidget *panadapter_peak_hold=gtk_check_button_new_with_label("Peak Hold");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_peak_hold), rx->panadapter_peak_hold);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_peak_hold,0,13,2,1);
  g_signal_connect(panadapter_peak_hold,"toggled",G_CALLBACK(panadapter_peak_hold_changed_cb),rx);

  GtkWidget *panadapter_peak_decay_label=gtk_label_new("Peak Decay (dB/s):");
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_peak_decay_label,0,14,1,1);

  GtkWidget *panadapter_peak_decay_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_peak_decay,0.0, 50.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_peak_decay_scale,200,30);
  sui_scale_show_value(panadapter_peak_decay_scale,0);
  gtk_widget_set_visible(panadapter_peak_decay_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_peak_decay_scale),"value_changed",G_CALLBACK(panadapter_peak_decay_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_peak_decay_scale,1,14,1,1);

  GtkWidget *waterfall_frame=gtk_frame_new("Waterfall");
    GtkWidget *waterfall_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(waterfall_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(waterfall_grid),FALSE);
    sui_style_group(waterfall_grid);
    gtk_frame_set_child(GTK_FRAME(waterfall_frame),waterfall_grid);
    gtk_grid_attach(GTK_GRID(grid),waterfall_frame,col,row,1,2);
    row+=2;

    GtkWidget *waterfall_high_label=gtk_label_new("High:");
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_high_label,0,0,1,1);

    GtkWidget *waterfall_high_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->waterfall_high,-200.0, 20.0, 1.0, 1.0, 1.0));
    gtk_widget_set_size_request(waterfall_high_scale,200,30);
    sui_scale_show_value(waterfall_high_scale,0);
    gtk_widget_set_visible(waterfall_high_scale, TRUE);
    g_signal_connect(G_OBJECT(waterfall_high_scale),"value_changed",G_CALLBACK(waterfall_high_value_changed_cb),rx);
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_high_scale,1,0,1,1);

    GtkWidget *waterfall_low_label=gtk_label_new("Low:");
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_low_label,0,1,1,1);

    GtkWidget *waterfall_low_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->waterfall_low,-200.0, 20.0, 1.0, 1.0, 1.0));
    gtk_widget_set_size_request(waterfall_low_scale,200,30);
    sui_scale_show_value(waterfall_low_scale,0);
    gtk_widget_set_visible(waterfall_low_scale, TRUE);
    g_signal_connect(G_OBJECT(waterfall_low_scale),"value_changed",G_CALLBACK(waterfall_low_value_changed_cb),rx);
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_low_scale,1,1,1,1);

    GtkWidget *waterfall_automatic=gtk_check_button_new_with_label("Waterfall Automatic");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (waterfall_automatic), rx->waterfall_automatic);
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_automatic,0,2,2,1);
    g_signal_connect(waterfall_automatic,"toggled",G_CALLBACK(waterfall_automatic_cb),rx);

    GtkWidget *waterfall_ft8_marker=gtk_check_button_new_with_label("Waterfall FT8 Marker");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (waterfall_ft8_marker), rx->waterfall_ft8_marker);
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_ft8_marker,0,3,2,1);
    g_signal_connect(waterfall_ft8_marker,"toggled",G_CALLBACK(waterfall_ft8_marker_cb),rx);

    GtkWidget *waterfall_theme_label=gtk_label_new("Color Theme:");
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_theme_label,0,4,1,1);

    GtkStringList *wt_sl=gtk_string_list_new(NULL);
    for(i=0; i<get_theme_count(); i++) {
      gtk_string_list_append(wt_sl, get_theme_name(i));
    }
    GtkWidget *waterfall_theme_combo=gtk_drop_down_new(G_LIST_MODEL(wt_sl),NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(waterfall_theme_combo),rx->waterfall_color_theme);
    gtk_grid_attach(GTK_GRID(waterfall_grid),waterfall_theme_combo,1,4,1,1);
    g_signal_connect(waterfall_theme_combo,"notify::selected",G_CALLBACK(waterfall_theme_cb),rx);

  col++;
  row=0;

  GtkWidget *cat_frame=gtk_frame_new("CAT");
  GtkWidget *cat_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(cat_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(cat_grid),FALSE);
  sui_style_group(cat_grid);
  gtk_frame_set_child(GTK_FRAME(cat_frame),cat_grid);
  gtk_grid_attach(GTK_GRID(grid),cat_frame,col,row,1,3);
  row++;

  GtkWidget *cat_debug_b=gtk_check_button_new_with_label("CAT Debug");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (cat_debug_b), rx->rigctl_debug);
  gtk_grid_attach(GTK_GRID(cat_grid),cat_debug_b,0,0,1,1);
  g_signal_connect(cat_debug_b,"toggled",G_CALLBACK(cat_debug_cb),rx);


  GtkWidget *cat_enable_b=gtk_check_button_new_with_label("TCP/IP enable");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (cat_enable_b), rx->rigctl_enable);
  gtk_grid_attach(GTK_GRID(cat_grid),cat_enable_b,0,1,1,1);
  g_signal_connect(cat_enable_b,"toggled",G_CALLBACK(cat_enable_cb),rx);

  GtkWidget *rigctl_port_spinner =gtk_spin_button_new_with_range(18000,21000,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(rigctl_port_spinner),(double)rx->rigctl_port);
  gtk_widget_set_visible(rigctl_port_spinner, TRUE);
  gtk_grid_attach(GTK_GRID(cat_grid),rigctl_port_spinner,0,2,2,1);
  g_signal_connect(rigctl_port_spinner,"value_changed",G_CALLBACK(rigctl_value_changed_cb),rx);



  GtkWidget *cat_serial_enable_b=gtk_check_button_new_with_label("Serial Port Enable");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (cat_serial_enable_b), rx->rigctl_serial_enable);
  gtk_grid_attach(GTK_GRID(cat_grid),cat_serial_enable_b,0,4,1,1);
  g_signal_connect(cat_serial_enable_b,"toggled",G_CALLBACK(cat_serial_enable_cb),rx);

  GtkWidget *serial_text_label=gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(serial_text_label), "<b>Serial Port: </b>");
  gtk_grid_attach(GTK_GRID(cat_grid),serial_text_label,0,5,1,1);

  rx->serial_port_entry=gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(rx->serial_port_entry),rx->rigctl_serial_port);
  gtk_widget_set_visible(rx->serial_port_entry, TRUE);
  gtk_grid_attach(GTK_GRID(cat_grid),rx->serial_port_entry,1,5,2,1);
  g_signal_connect(rx->serial_port_entry,"activate",G_CALLBACK(cat_serial_port_cb),rx);

  GtkWidget *serial_baudrate_label=gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(serial_baudrate_label), "<b>Baudrate: </b>");
  gtk_grid_attach(GTK_GRID(cat_grid),serial_baudrate_label,0,6,1,1);

  const char *baud_opts[]={"4800","9600","19200","38400",NULL};
  GtkWidget *cat_serial_port_baudrate=gtk_drop_down_new_from_strings(baud_opts);
  if(rx->rigctl_serial_baudrate==B4800) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(cat_serial_port_baudrate),0);
  } else if(rx->rigctl_serial_baudrate==B9600) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(cat_serial_port_baudrate),1);
  } else if(rx->rigctl_serial_baudrate==B19200) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(cat_serial_port_baudrate),2);
  } else if(rx->rigctl_serial_baudrate==B38400) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(cat_serial_port_baudrate),3);
  }
  gtk_grid_attach(GTK_GRID(cat_grid),cat_serial_port_baudrate,1,6,1,1);
  g_signal_connect(cat_serial_port_baudrate,"notify::selected",G_CALLBACK(cat_baudrate_cb),rx);
  return grid;
}
