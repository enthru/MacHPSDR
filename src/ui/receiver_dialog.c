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
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "serial_compat.h"
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
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "receiver_dialog.h"
#include "vfo.h"
#include "audio.h"
#include "main.h"
#include "rigctl.h"
#include "tci.h"
#include "subrx.h"

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

// These are plain GtkButtons (they only look like a radio group, via the orange
// text on the selected one), so the signal is "clicked" and there is no active
// state to interrogate.  Both of these used to be connected to "toggled" and to
// open with gtk_check_button_get_active() — GTK3 radio-button code left behind
// by the GTK4 port.  GtkButton has no "toggled" signal, so g_signal_connect
// logged a GLib-GObject-CRITICAL for each one and the handler was NEVER
// connected: the Var1/Var2 filter buttons and the FMN deviation buttons did
// nothing at all when clicked.
static void filter_select_cb(GtkWidget *widget,gpointer data) {
  SELECT *select=(SELECT *)data;
  RECEIVER *rx=select->rx;
  gint f=select->choice;
  // Un-highlight whatever was selected before.  Only the Var1/Var2 buttons are
  // actually attached to this grid (the loop that built one button per filter
  // is commented out above), so for any other filter_a there is simply no
  // widget at those coordinates — hence both NULL checks.
  GtkWidget *grid=gtk_widget_get_ancestor(widget, GTK_TYPE_GRID);
  if(grid!=NULL) {
    int x=rx->filter_a%FILTER_COLUMNS;
    int y=rx->filter_a/FILTER_COLUMNS;
    if(rx->filter_a>=FVar1) {
      y=1+((rx->filter_a+4)/5);
      x=0;
    }
    set_button_text_color(gtk_grid_get_child_at(GTK_GRID(grid),x,y),"black");
  }
  set_button_text_color(widget,"orange");
  receiver_filter_changed(rx,f);
}

static void deviation_select_cb(GtkWidget *widget,gpointer data) {
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
      g_signal_connect(b,"clicked",G_CALLBACK(deviation_select_cb),(gpointer)select);
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
      g_signal_connect(b,"clicked",G_CALLBACK(deviation_select_cb),(gpointer)select);
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
        g_signal_connect(b,"clicked",G_CALLBACK(filter_select_cb),(gpointer)select);
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
      g_signal_connect(b,"clicked",G_CALLBACK(filter_select_cb),(gpointer)select);
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
      g_signal_connect(b,"clicked",G_CALLBACK(filter_select_cb),(gpointer)select);

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
  // TCI addresses PTT per trx, and the trx that transmits is this receiver's
  // index - which just changed. Re-announce it so a connected client stops
  // attributing PTT to the receiver the transmitter used to be on. GTK thread;
  // the notify is a best-effort broadcast that early-returns when TCI is off.
  tci_notify_trx(radio->mox);
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

/* The two sliders the automatic scale takes over, so it can grey them out. */
typedef struct {
  RECEIVER *rx;
  GtkWidget *high;
  GtkWidget *low;
} PAN_AUTO_UI;

static void panadapter_automatic_cb(GtkWidget *widget, gpointer data) {
  PAN_AUTO_UI *ui=(PAN_AUTO_UI *)data;
  RECEIVER *rx=ui->rx;
  rx->panadapter_automatic=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  rx->pan_auto_seeded=FALSE;   // re-seed instead of drifting in from the old window
  gtk_widget_set_sensitive(ui->high,!rx->panadapter_automatic);
  gtk_widget_set_sensitive(ui->low,!rx->panadapter_automatic);
  if(!rx->panadapter_automatic) {
    // Hand the sliders back where the auto-fit left the display.
    gtk_range_set_value(GTK_RANGE(ui->high),(double)rx->panadapter_high);
    gtk_range_set_value(GTK_RANGE(ui->low),(double)rx->panadapter_low);
  }
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

static void panadapter_histogram_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_histogram=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  // zero the density buffer on enable so it starts clean
  if(rx->panadapter_histogram && rx->panadapter_histogram_bins!=NULL) {
    // The buffer is HALF resolution; _w/_h are the FULL widget dims (see
    // receiver_histogram_cells).  Multiplying those wrote four times the
    // allocation -- 1.8 MB past the end of a 600 kB block, on one click.
    memset(rx->panadapter_histogram_bins,0,
           sizeof(float)*receiver_histogram_cells(rx->panadapter_histogram_w,
                                                  rx->panadapter_histogram_h));
  }
}

static void panadapter_histogram_decay_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_histogram_decay=(int)gtk_range_get_value(GTK_RANGE(widget));
}

static void panadapter_phase_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_phase=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
}

static void panadapter_phase_mode_changed_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_phase_mode=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
}

static void panadapter_phase_gain_changed_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_phase_gain=(int)gtk_range_get_value(GTK_RANGE(widget));
}

// NR4 (libspecbleach) live parameter tuning. Each pushes straight to WDSP so the
// operator can hear the change while A/B-testing on real signals.
// --- Squelch (AMSQ calibration) -------------------------------------------
// The 0..1 SQL bar maps onto [amsq_min_db, amsq_max_db]; both ends plus the
// gate's tail time are settable because the useful range depends on the
// station's noise floor and can only be found against a live band.
static void amsq_min_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->amsq_min_db=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  set_squelch(rx);
}
static void amsq_max_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->amsq_max_db=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  set_squelch(rx);
}
static void amsq_tail_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->amsq_tail=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  set_squelch(rx);
}

// --- Manual notch (MNF) list editor ---------------------------------------
// One fixed row per possible notch, shown/hidden as the list grows and shrinks.
// Rows are never created or destroyed at runtime, so no callback can ever run
// against a row widget that is being torn down underneath it.
typedef struct _mnf_ui {
  RECEIVER *rx;
  GtkWidget *active[MAX_NOTCHES];
  GtkWidget *freq[MAX_NOTCHES];
  GtkWidget *width[MAX_NOTCHES];
  GtkWidget *af[MAX_NOTCHES];
  GtkWidget *del[MAX_NOTCHES];
  GtkWidget *empty;
} MNF_UI;

static void mnf_refresh(MNF_UI *ui) {
  RECEIVER *rx=ui->rx;
  for(int i=0;i<MAX_NOTCHES;i++) {
    gboolean used=(i<rx->notches);
    gtk_widget_set_visible(ui->active[i],used);
    gtk_widget_set_visible(ui->freq[i],used);
    gtk_widget_set_visible(ui->width[i],used);
    gtk_widget_set_visible(ui->af[i],used);
    gtk_widget_set_visible(ui->del[i],used);
    if(!used) continue;
    // Writing a value back through these setters re-fires the callbacks, but
    // each one then stores the value it just read — a no-op.
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ui->active[i]),rx->notch[i].active);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->freq[i]),rx->notch[i].fcenter/1000000.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->width[i]),rx->notch[i].fwidth);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ui->af[i]),rx->notch[i].af);
  }
  gtk_widget_set_visible(ui->empty,rx->notches==0);
}

static int mnf_index(GtkWidget *w) {
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w),"notch_idx"));
}

static void notch_active_cb(GtkCheckButton *widget, gpointer data) {
  MNF_UI *ui=(MNF_UI *)data;
  int i=mnf_index(GTK_WIDGET(widget));
  if(i>=ui->rx->notches) return;
  ui->rx->notch[i].active=gtk_check_button_get_active(widget);
  receiver_notch_sync(ui->rx);
  if(ui->rx->panadapter!=NULL) gtk_widget_queue_draw(ui->rx->panadapter);
}

static void notch_af_cb(GtkCheckButton *widget, gpointer data) {
  MNF_UI *ui=(MNF_UI *)data;
  int i=mnf_index(GTK_WIDGET(widget));
  if(i>=ui->rx->notches) return;
  receiver_set_notch_af(ui->rx,i,gtk_check_button_get_active(widget));
}

static void notch_freq_cb(GtkWidget *widget, gpointer data) {
  MNF_UI *ui=(MNF_UI *)data;
  int i=mnf_index(widget);
  if(i>=ui->rx->notches) return;
  gdouble hz=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget))*1000000.0;
  ui->rx->notch[i].fcenter=hz;
  // An AF notch is defined by its offset, so moving it by frequency has to
  // re-derive that offset or the next sync would snap it straight back.
  if(ui->rx->notch[i].af)
    ui->rx->notch[i].af_offset=hz-receiver_notch_anchor(ui->rx);
  receiver_notch_sync(ui->rx);
  if(ui->rx->panadapter!=NULL) gtk_widget_queue_draw(ui->rx->panadapter);
}

static void notch_width_cb(GtkWidget *widget, gpointer data) {
  MNF_UI *ui=(MNF_UI *)data;
  int i=mnf_index(widget);
  if(i>=ui->rx->notches) return;
  ui->rx->notch[i].fwidth=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  receiver_notch_sync(ui->rx);
  if(ui->rx->panadapter!=NULL) gtk_widget_queue_draw(ui->rx->panadapter);
}

static void notch_delete_cb(GtkWidget *widget, gpointer data) {
  MNF_UI *ui=(MNF_UI *)data;
  int i=mnf_index(widget);
  if(i>=ui->rx->notches) return;
  receiver_delete_notch(ui->rx,i);
  mnf_refresh(ui);
  if(ui->rx->panadapter!=NULL) gtk_widget_queue_draw(ui->rx->panadapter);
}

static void notch_add_cb(GtkWidget *widget, gpointer data) {
  MNF_UI *ui=(MNF_UI *)data;
  receiver_add_notch(ui->rx,receiver_notch_anchor(ui->rx),ui->rx->notch_default_width);
  mnf_refresh(ui);
  if(ui->rx->panadapter!=NULL) gtk_widget_queue_draw(ui->rx->panadapter);
}

static void notch_default_width_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->notch_default_width=gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
}

static void nr3_depth_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->nr3_depth=gtk_range_get_value(GTK_RANGE(widget));
  SetRXARNNRdepth(rx->channel,rx->nr3_depth/100.0);
  if(rx->subrx_enable && rx->subrx!=NULL) subrx_update_nr(rx);
}
static void nr4_reduction_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->nr4_reduction=gtk_range_get_value(GTK_RANGE(widget));
  SetRXASBNRreductionAmount(rx->channel,(float)rx->nr4_reduction);
  if(rx->subrx_enable && rx->subrx!=NULL) subrx_update_nr(rx);
}
static void nr4_smoothing_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->nr4_smoothing=gtk_range_get_value(GTK_RANGE(widget));
  SetRXASBNRsmoothingFactor(rx->channel,(float)rx->nr4_smoothing);
  if(rx->subrx_enable && rx->subrx!=NULL) subrx_update_nr(rx);
}
static void nr4_whitening_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->nr4_whitening=gtk_range_get_value(GTK_RANGE(widget));
  SetRXASBNRwhiteningFactor(rx->channel,(float)rx->nr4_whitening);
  if(rx->subrx_enable && rx->subrx!=NULL) subrx_update_nr(rx);
}
static void nr4_rescale_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->nr4_rescale=gtk_range_get_value(GTK_RANGE(widget));
  SetRXASBNRnoiseRescale(rx->channel,(float)rx->nr4_rescale);
  if(rx->subrx_enable && rx->subrx!=NULL) subrx_update_nr(rx);
}
static void nr4_postfilter_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->nr4_postfilter=gtk_range_get_value(GTK_RANGE(widget));
  SetRXASBNRpostFilterThreshold(rx->channel,(float)rx->nr4_postfilter);
  if(rx->subrx_enable && rx->subrx!=NULL) subrx_update_nr(rx);
}

static void panadapter_phase_source_changed_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->panadapter_phase_source=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget));
  // Switching source: reset the tuned-path DSP state so a stale downmixed/
  // filtered snapshot from before the switch doesn't linger on screen. A
  // torn frame here is harmless visually (main thread writes, audio thread
  // only reads these when source==1, which we've just changed).
  rx->scope_iq_n=0;
  rx->scope_fir_hist_n=0;
  rx->scope_nco_ph=0.0;
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

static void audio_status_set(RECEIVER *rx,const char *text) {
  if(rx->audio_choice_b==NULL) return;
  GtkWidget *l=g_object_get_data(G_OBJECT(rx->audio_choice_b),"audio_status");
  if(l==NULL) return;
  gtk_label_set_text(GTK_LABEL(l),text==NULL?"":text);
  gtk_widget_set_visible(l,text!=NULL && *text!='\0');
}

// Open rx->audio_name, and if that device will not open, fall back to "System
// Default" instead of giving up on local audio altogether.
//
// The device an operator picks can be legitimately unavailable — a raw
// plughw:/dmix: entry is refused with EBUSY the moment a sound server owns the
// card, which on a desktop is most of the time.  The old behaviour was to
// clear rx->local_audio, untick the box and LEAVE rx->audio_name pointing at
// the device that just failed, so ticking the box again retried the same busy
// device, failed again, and unticked itself: local audio could not be turned
// back on at all without restarting.  (Reported from a running session on
// Linux/ALSA.)  Falling back keeps the receiver audible and leaves the
// drop-down showing what is actually playing.
static gboolean audio_open_with_fallback(RECEIVER *rx) {
  int rc=audio_open_output(rx);
  if(rc>=0) { audio_status_set(rx,NULL); return TRUE; }
  if(rx->audio_name!=NULL && strcmp(rx->audio_name,AUDIO_SYSTEM_DEFAULT_NAME)==0) {
    audio_status_set(rx,"System Default would not open — no local audio.");
    return FALSE;                       // already the fallback — nothing left to try
  }

  char *asked=g_strdup(rx->audio_name==NULL?"(none)":rx->audio_name);
  log_info("audio: %s would not open (%s), falling back to System Default\n",
           asked, rc==-EBUSY?"busy":"error");
  if(rx->audio_name!=NULL) g_free(rx->audio_name);
  rx->audio_name=g_strdup(AUDIO_SYSTEM_DEFAULT_NAME);
  rx->output_index=-1;
  if(audio_open_output(rx)<0) {
    audio_status_set(rx,"No audio device would open.");
    g_free(asked);
    return FALSE;
  }

  // Say why the selection is not what was clicked.  EBUSY on a raw plughw:/
  // dmix: entry is the normal answer on any desktop running PulseAudio or
  // PipeWire — the sound server owns the card, and it keeps owning it for some
  // seconds after the last stream closes — so this is a fact of the machine
  // rather than a fault, and it needs saying rather than fixing.
  char *msg = rc==-EBUSY
    ? g_strdup_printf("%s is busy (owned by the sound server) — using System Default.",asked)
    : g_strdup_printf("%s would not open — using System Default.",asked);
  audio_status_set(rx,msg);
  g_free(msg);
  g_free(asked);

  // Point the drop-down at what is really playing.  Blocked via the stored
  // handler id, or setting it re-enters audio_choice_cb and closes the stream
  // just opened.
  if(rx->audio_choice_b!=NULL && rx->audio_choice_signal_id!=0) {
    for(int j=0;j<n_output_devices;j++) {
      if(output_devices[j].name!=NULL &&
         strcmp(output_devices[j].name,AUDIO_SYSTEM_DEFAULT_NAME)==0) {
        g_signal_handler_block(rx->audio_choice_b,rx->audio_choice_signal_id);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(rx->audio_choice_b),j);
        g_signal_handler_unblock(rx->audio_choice_b,rx->audio_choice_signal_id);
        break;
      }
    }
  }
  return TRUE;
}

static void local_audio_cb(GtkWidget *widget,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->local_audio=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  if(rx->local_audio) {
    if(!audio_open_with_fallback(rx)) {
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
      rx->audio_name=NULL;      // freed and not cleared left this dangling when i<0
    }
    if(i>=0) {
      rx->audio_name=g_strdup(output_devices[i].name);
      rx->output_index=output_devices[i].index;
      if(!audio_open_with_fallback(rx)) {
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

// Reset every band slider to 0 dB (flat). Each set fires value-changed, which
// updates rx->equalizer[] and re-pushes SetRXAGrphEQ10, so no extra WDSP call.
static void rx_eq_reset_cb (GtkWidget *widget, gpointer data) {
  GtkWidget **scales = g_object_get_data(G_OBJECT(widget),"eq_scales");
  if(!scales) return;
  for(int i=0;i<11;i++) gtk_range_set_value(GTK_RANGE(scales[i]),0.0);
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
  g_strlcpy(rx->rigctl_serial_port,gtk_editable_get_text(GTK_EDITABLE(rx->serial_port_entry)),sizeof(rx->rigctl_serial_port));
  rx->rigctl_serial_enable=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  if(rx->rigctl_serial_enable) {
    launch_serial(rx);
  } else {
    disable_serial(rx);
  }
}

static void cat_serial_port_cb(GtkWidget *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  g_strlcpy(rx->rigctl_serial_port,gtk_editable_get_text(GTK_EDITABLE(widget)),sizeof(rx->rigctl_serial_port));
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
  SELECT *select;

  // The page is a vbox: the two-column grid on top, then the full-width
  // Squelch/MNF row under it. Putting that row in the grid instead left a tall
  // gap — the left column's Audio/EQ/NR4 box spans 4 grid rows and hugs the
  // top, so the leftover height of its last row opened up above the new row.
  GtkWidget *page=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);  // = SUI_PAGE_ROW_SP
  GtkWidget *grid=gtk_grid_new();
  // A GtkGrid inherits vexpand from any expanding child, so inside the vbox it
  // would swallow the scroller's spare height and shove the Squelch/MNF row to
  // the bottom of the viewport. Pin it to its natural height.
  gtk_widget_set_vexpand(grid,FALSE);
  gtk_widget_set_valign(grid,GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(page),grid);
  sui_style_page(grid);
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);

  // Each column is its OWN vbox, not a run of grid rows: the two columns hold a
  // different number of frames, so as grid rows (with multi-row spans) the
  // taller column's row heights leaked ~150px of slack into the shorter one and
  // the grid ended up much taller than its content.
  GtkWidget *left_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
  gtk_widget_set_valign(left_box,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),left_box,0,0,1,1);
  GtkWidget *right_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
  // FILL, not START: the right column is the shorter of the two, and its last
  // frame (CAT) expands to take up the difference so the column bottoms line up
  // and no dead space is left under CAT.
  gtk_widget_set_valign(right_box,GTK_ALIGN_FILL);
  gtk_grid_attach(GTK_GRID(grid),right_box,1,0,1,1);

  if(radio->discovered->adcs>1) {
    GtkWidget *adc_frame=gtk_frame_new("ADC");
    GtkWidget *adc_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(adc_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(adc_grid),FALSE);
    sui_style_group(adc_grid);
    gtk_frame_set_child(GTK_FRAME(adc_frame),adc_grid);
    gtk_box_append(GTK_BOX(left_box),adc_frame);

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

  // Per-RX sample-rate picker. The fixed 48k..1536k set below is protocol 2's;
  // SoapySDR gets its own list, because the rates it can run are the device's
  // (see the Radio page's soapy_rx_rate_cb, which sets the same thing for every
  // receiver at once). It is per RECEIVER and not merely per radio because
  // receivers sharing one hardware RX channel each decimate the shared stream
  // by a ratio of their own: RX0 can hold the whole transponder in view while
  // RX1 sits in a 192 kHz window around one QSO.
  switch(radio->discovered->protocol) {
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      {
      GtkWidget *span_frame=gtk_frame_new("Span");
      GtkWidget *span_grid=gtk_grid_new();
      gtk_grid_set_row_homogeneous(GTK_GRID(span_grid),FALSE);
      gtk_grid_set_column_homogeneous(GTK_GRID(span_grid),FALSE);
      sui_style_group(span_grid);
      gtk_frame_set_child(GTK_FRAME(span_frame),span_grid);
      gtk_box_append(GTK_BOX(left_box),span_frame);

      // The same list, and for the same reasons, as the Radio page's: every one
      // an exact multiple of 48000 by a factor that divides the I/Q block.
      const int rates[]={192000,384000,768000,1536000,1920000,4800000,9600000};
      GtkWidget *first=NULL;
      int y=0;
      for(int r=0;r<(int)G_N_ELEMENTS(rates);r++) {
        if(rates[r]>radio->sample_rate) continue;
        char buf[16];
        snprintf(buf,sizeof(buf),"%d",rates[r]);
        GtkWidget *b=gtk_check_button_new_with_label(buf);
        if(first==NULL) first=b; else gtk_check_button_set_group(GTK_CHECK_BUTTON(b),GTK_CHECK_BUTTON(first));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(b),rx->sample_rate==rates[r]);
        gtk_grid_attach(GTK_GRID(span_grid),b,0,y++,1,1);
        SELECT *sel=g_new0(SELECT,1);
        sel->rx=rx;
        sel->choice=rates[r];
        g_signal_connect(b,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)sel);
      }
      gtk_widget_set_tooltip_text(span_frame,
          soapy_protocol_rx_owns_hardware(rx)?
            "Width of this receiver's view of the band. The hardware runs at the "
            "device's own rate and this receiver decimates it down to the span."
          : "Width of this receiver's view of the band. It shares the hardware "
            "channel with another receiver, so its centre has to stay inside the "
            "window that receiver's LO covers -- a narrower span leaves it more "
            "room to move.");
      }
      break;
#endif
    case PROTOCOL_2:
      {
      int x=0;
      int y=0;

      GtkWidget *sample_rate_frame=gtk_frame_new("Sample Rate");
      GtkWidget *sample_rate_grid=gtk_grid_new();
      gtk_grid_set_row_homogeneous(GTK_GRID(sample_rate_grid),FALSE);
      gtk_grid_set_column_homogeneous(GTK_GRID(sample_rate_grid),FALSE);
      sui_style_group(sample_rate_grid);
      gtk_frame_set_child(GTK_FRAME(sample_rate_frame),sample_rate_grid);
      gtk_box_append(GTK_BOX(left_box),sample_rate_frame);

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

      // Only Protocol 2 reaches this block now, so 768k/1536k are unconditional.
      GtkWidget *sample_rate_768=gtk_check_button_new_with_label("768000"); gtk_check_button_set_group(GTK_CHECK_BUTTON(sample_rate_768),GTK_CHECK_BUTTON(sample_rate_384));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_768), rx->sample_rate==768000);
      gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_768,x,y++,1,1);
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=768000;
      g_signal_connect(sample_rate_768,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);

      GtkWidget *sample_rate_1536=gtk_check_button_new_with_label("1536000"); gtk_check_button_set_group(GTK_CHECK_BUTTON(sample_rate_1536),GTK_CHECK_BUTTON(sample_rate_768));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (sample_rate_1536), rx->sample_rate==1536000);
      gtk_grid_attach(GTK_GRID(sample_rate_grid),sample_rate_1536,x,y++,1,1);
      select=g_new0(SELECT,1);
      select->rx=rx;
      select->choice=1536000;
      g_signal_connect(sample_rate_1536,"toggled",G_CALLBACK(sample_rate_cb),(gpointer)select);
    }
    break;
  }

  rx->filter_frame=gtk_frame_new("Filter");
  gtk_box_append(GTK_BOX(left_box),rx->filter_frame);

  update_filters(rx);

  // Audio + Equalizer share column 0 and are stacked in one top-aligned box so
  // they hug each other; without it the tall Panadapter in the next column
  // stretches Audio's grid row and opens a gap above the Equalizer.
  GtkWidget *audio_eq_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
  gtk_widget_set_valign(audio_eq_box,GTK_ALIGN_START);

  if(n_output_devices>=0) {
    GtkWidget *audio_frame=gtk_frame_new("Audio");
    GtkWidget *audio_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(audio_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(audio_grid),FALSE);
    sui_style_group(audio_grid);
    gtk_frame_set_child(GTK_FRAME(audio_frame),audio_grid);
    gtk_box_append(GTK_BOX(audio_eq_box),audio_frame);

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

    // Why the device above is not the one that was picked.  A refused device is
    // otherwise completely mute as an event: the selection snaps back and the
    // only explanation is a line on a terminal the operator is not reading.
    // Hung off the drop-down rather than added to RECEIVER, so that no shared
    // header changes size (a stale incremental build of that is its own bug).
    GtkWidget *audio_status=gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(audio_status),0.0);
    gtk_label_set_wrap(GTK_LABEL(audio_status),TRUE);
    gtk_widget_set_visible(audio_status,FALSE);
    gtk_grid_attach(GTK_GRID(audio_grid),audio_status,0,3,2,1);
    g_object_set_data(G_OBJECT(rx->audio_choice_b),"audio_status",audio_status);

    // Stereo, left, right audio
    const char *audio_ch_opts[]={"Stereo","Left","Right",NULL};
    GtkWidget *audio_channels_combo=gtk_drop_down_new_from_strings(audio_ch_opts);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(audio_channels_combo),rx->audio_channels);
    gtk_grid_attach(GTK_GRID(audio_grid),audio_channels_combo,0,1,2,1);
    g_signal_connect(audio_channels_combo,"notify::selected",G_CALLBACK(audio_channels_cb),rx);

    GtkWidget *tx_mute_b = gtk_check_button_new_with_label("Mute while TX");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(tx_mute_b), rx->mute_while_transmitting);
    gtk_grid_attach(GTK_GRID(audio_grid),tx_mute_b,0,4,1,1);
    g_signal_connect(tx_mute_b,"toggled",G_CALLBACK(mute_while_tx_cb),rx);

    // Sub-RX audio balance: split (main L / sub R) .. mono (both in both ears).
    GtkWidget *subrx_mix_label=gtk_label_new("Sub-RX mix (split↔mono):");
    gtk_label_set_xalign(GTK_LABEL(subrx_mix_label),0.0);
    gtk_grid_attach(GTK_GRID(audio_grid),subrx_mix_label,0,5,1,1);
    GtkWidget *subrx_mix_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,
        gtk_adjustment_new(rx->subrx_mix,0.0,100.0,1.0,10.0,0.0));
    gtk_widget_set_size_request(subrx_mix_scale,160,25);
    sui_scale_show_value(subrx_mix_scale,0);
    gtk_grid_attach(GTK_GRID(audio_grid),subrx_mix_scale,1,5,1,1);
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
  gtk_widget_set_halign(equalizer_frame,GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(audio_eq_box),equalizer_frame);

  // NR3 (RNNoise) has exactly one control and it is a wet/dry mix: the network
  // applies its whole mask or none, and on a signal it does not read as speech
  // -- weak SSB through a transponder is not what it was trained on -- that is
  // the voice going with the noise.  100 % is what it has always done.
  {
    GtkWidget *nr3_frame=gtk_frame_new("Noise Reduction (NR3)");
    GtkWidget *nr3_grid=gtk_grid_new();
    sui_style_group(nr3_grid);
    gtk_widget_set_halign(nr3_frame,GTK_ALIGN_FILL);
    GtkWidget *l=gtk_label_new("Depth (%):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(nr3_grid),l,0,0,1,1);
    GtkWidget *s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->nr3_depth,0.0,100.0,1.0,10.0,0.0));
    gtk_widget_set_size_request(s,160,25);
    gtk_widget_set_hexpand(s,TRUE);
    gtk_widget_set_tooltip_text(s,"How much of RNNoise's output is mixed in. "
      "100 % is all-RNNoise; lower it when it thins the voice out.");
    sui_scale_show_value(s,0);
    gtk_grid_attach(GTK_GRID(nr3_grid),s,1,0,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(nr3_depth_cb),rx);
    gtk_frame_set_child(GTK_FRAME(nr3_frame),nr3_grid);
    gtk_box_append(GTK_BOX(audio_eq_box),nr3_frame);
  }

  // NR4 (libspecbleach) tuning: live sliders so the operator can dial the
  // spectral denoiser in on real signals (defaults are conservative).
  {
    GtkWidget *nr4_frame=gtk_frame_new("Noise Reduction (NR4)");
    GtkWidget *nr4_grid=gtk_grid_new();
    sui_style_group(nr4_grid);
    // Fill the column width so the frame matches the Audio/Equalizer blocks
    // stacked above it (a shrink-to-content frame looked narrower than them).
    gtk_widget_set_halign(nr4_frame,GTK_ALIGN_FILL);

    GtkWidget *l;
    GtkWidget *s;

    l=gtk_label_new("Reduction (dB):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(nr4_grid),l,0,0,1,1);
    s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->nr4_reduction,0.0,20.0,0.5,5.0,0.0));
    gtk_widget_set_size_request(s,160,25);
    gtk_widget_set_hexpand(s,TRUE);
    sui_scale_show_value(s,1);
    gtk_grid_attach(GTK_GRID(nr4_grid),s,1,0,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(nr4_reduction_cb),rx);

    l=gtk_label_new("Smoothing (%):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(nr4_grid),l,0,1,1,1);
    s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->nr4_smoothing,0.0,100.0,1.0,10.0,0.0));
    gtk_widget_set_size_request(s,160,25);
    gtk_widget_set_hexpand(s,TRUE);
    sui_scale_show_value(s,0);
    gtk_grid_attach(GTK_GRID(nr4_grid),s,1,1,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(nr4_smoothing_cb),rx);

    l=gtk_label_new("Whitening (%):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(nr4_grid),l,0,2,1,1);
    s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->nr4_whitening,0.0,100.0,1.0,10.0,0.0));
    gtk_widget_set_size_request(s,160,25);
    gtk_widget_set_hexpand(s,TRUE);
    sui_scale_show_value(s,0);
    gtk_grid_attach(GTK_GRID(nr4_grid),s,1,2,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(nr4_whitening_cb),rx);

    l=gtk_label_new("Noise rescale (dB):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(nr4_grid),l,0,3,1,1);
    s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->nr4_rescale,0.0,12.0,0.5,2.0,0.0));
    gtk_widget_set_size_request(s,160,25);
    gtk_widget_set_hexpand(s,TRUE);
    sui_scale_show_value(s,1);
    gtk_grid_attach(GTK_GRID(nr4_grid),s,1,3,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(nr4_rescale_cb),rx);

    l=gtk_label_new("Post-filter (dB):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(nr4_grid),l,0,4,1,1);
    s=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->nr4_postfilter,-10.0,10.0,0.5,2.0,0.0));
    gtk_widget_set_size_request(s,160,25);
    gtk_widget_set_hexpand(s,TRUE);
    sui_scale_show_value(s,1);
    gtk_grid_attach(GTK_GRID(nr4_grid),s,1,4,1,1);
    g_signal_connect(G_OBJECT(s),"value_changed",G_CALLBACK(nr4_postfilter_cb),rx);

    gtk_frame_set_child(GTK_FRAME(nr4_frame),nr4_grid);
    gtk_box_append(GTK_BOX(audio_eq_box),nr4_frame);
  }

  // (The CW Audio Peak Filter lives on the CW configuration tab now — see
  // cw_dialog.c — since it only does anything in CWL/CWU.)

  gtk_box_append(GTK_BOX(left_box),audio_eq_box);

  GtkWidget *enable_b=gtk_check_button_new_with_label("Enable Equalizer");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (enable_b), rx->enable_equalizer);
  g_signal_connect(enable_b,"toggled",G_CALLBACK(enable_cb),rx);
  gtk_grid_attach(GTK_GRID(equalizer_grid),enable_b,0,0,10,1);

  GtkWidget *rx_eq_reset_b=gtk_button_new_with_label("Reset");
  gtk_widget_set_tooltip_text(rx_eq_reset_b,"Reset all bands to 0 dB (flat)");
  gtk_grid_attach(GTK_GRID(equalizer_grid),rx_eq_reset_b,10,0,1,1);
  GtkWidget **rx_eq_scales=g_new0(GtkWidget*,11);

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
    rx_eq_scales[i]=scale;
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
  g_object_set_data_full(G_OBJECT(rx_eq_reset_b),"eq_scales",rx_eq_scales,g_free);
  g_signal_connect(rx_eq_reset_b,"clicked",G_CALLBACK(rx_eq_reset_cb),rx);

  if(strcmp(radio->discovered->name,"rtlsdr")!=0) {
    GtkWidget *tx_frame=gtk_frame_new("TX Frequency");
    GtkWidget *tx_grid=gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(tx_grid),FALSE);
    gtk_grid_set_column_homogeneous(GTK_GRID(tx_grid),FALSE);
    sui_style_group(tx_grid);
    gtk_frame_set_child(GTK_FRAME(tx_frame),tx_grid);
    gtk_box_append(GTK_BOX(right_box),tx_frame);

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
  gtk_box_append(GTK_BOX(right_box),panadapter_frame);


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

  // Automatic dB scale. It owns High/Low while it is on, so the two sliders are
  // greyed out; switching it off hands them back with the values the auto-fit
  // ended on, so the manual scale picks up where the display already is.
  PAN_AUTO_UI *pau=g_new0(PAN_AUTO_UI,1);
  pau->rx=rx;
  pau->high=panadapter_high_scale;
  pau->low=panadapter_low_scale;
  GtkWidget *panadapter_automatic=gtk_check_button_new_with_label("Panadapter Automatic");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(panadapter_automatic),rx->panadapter_automatic);
  gtk_widget_set_sensitive(panadapter_high_scale,!rx->panadapter_automatic);
  gtk_widget_set_sensitive(panadapter_low_scale,!rx->panadapter_automatic);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_automatic,0,4,2,1);
  g_object_set_data_full(G_OBJECT(panadapter_automatic),"pan_auto_ui",pau,g_free);
  g_signal_connect(panadapter_automatic,"toggled",G_CALLBACK(panadapter_automatic_cb),pau);

  GtkWidget *step_label=gtk_label_new("Step:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),step_label,0,5,1,1);

  GtkWidget *panadapter_step_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_step,1.0, 40.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_step_scale,200,30);
  sui_scale_show_value(panadapter_step_scale,0);
  gtk_widget_set_visible(panadapter_step_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_step_scale),"value_changed",G_CALLBACK(panadapter_step_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_step_scale,1,5,1,1);

  // Second internal column (grid cols 3-4): the display/rendering toggles, so
  // the frame is ~9 rows tall instead of a 17-row single-column strip. A left
  // margin on the col-3 widgets separates the two groups (col 2 is the gap).
  GtkWidget *panadapter_filled=gtk_check_button_new_with_label("Panadapter Filled");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_filled), rx->panadapter_filled);
  gtk_widget_set_margin_start(panadapter_filled,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_filled,3,0,2,1);
  g_signal_connect(panadapter_filled,"toggled",G_CALLBACK(panadapter_filled_changed_cb),rx);

  GtkWidget *panadapter_gradient=gtk_check_button_new_with_label("Panadapter Gradient");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_gradient), rx->panadapter_gradient);
  gtk_widget_set_margin_start(panadapter_gradient,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_gradient,3,1,2,1);
  g_signal_connect(panadapter_gradient,"toggled",G_CALLBACK(panadapter_gradient_changed_cb),rx);

  GtkWidget *panadapter_agc_line=gtk_check_button_new_with_label("AGC Lines");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_agc_line), rx->panadapter_agc_line);
  gtk_widget_set_margin_start(panadapter_agc_line,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_agc_line,3,2,2,1);
  g_signal_connect(panadapter_agc_line,"toggled",G_CALLBACK(panadapter_agc_line_changed_cb),rx);

  GtkWidget *panadapter_single_color_label=gtk_label_new("Panadapter Color:");
  gtk_widget_set_visible(panadapter_single_color_label, TRUE);
  gtk_widget_set_margin_start(panadapter_single_color_label,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_single_color_label,3,3,1,1);

  GtkStringList *psc_sl=gtk_string_list_new(NULL);
  for(i=0; i<(int)PANADAPTER_COLOR_COUNT; i++) {
    gtk_string_list_append(psc_sl,panadapter_color_names[i]);
  }
  GtkWidget *panadapter_single_color_b=gtk_drop_down_new(G_LIST_MODEL(psc_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(panadapter_single_color_b),rx->panadapter_single_color);
  gtk_widget_set_visible(panadapter_single_color_b, TRUE);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_single_color_b,4,3,1,1);
  g_signal_connect(panadapter_single_color_b,"notify::selected",G_CALLBACK(panadapter_single_color_changed_cb),rx);

  // Turn the spectroscope off entirely (waterfall then fills the whole area).
  GtkWidget *show_panadapter=gtk_check_button_new_with_label("Show Panadapter");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (show_panadapter), rx->show_panadapter);
  gtk_widget_set_margin_start(show_panadapter,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),show_panadapter,3,4,2,1);
  g_signal_connect(show_panadapter,"toggled",G_CALLBACK(show_panadapter_cb),rx);

  // S-meter needle ballistics (0 = instant, 100 = heaviest damping).
  GtkWidget *meter_smoothing_label=gtk_label_new("Meter smoothing:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),meter_smoothing_label,0,6,1,1);

  GtkWidget *meter_smoothing_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->meter_smoothing, 0.0, 100.0, 1.0, 10.0, 1.0));
  gtk_widget_set_size_request(meter_smoothing_scale,200,30);
  sui_scale_show_value(meter_smoothing_scale,0);
  gtk_widget_set_visible(meter_smoothing_scale, TRUE);
  g_signal_connect(G_OBJECT(meter_smoothing_scale),"value_changed",G_CALLBACK(meter_smoothing_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),meter_smoothing_scale,1,6,1,1);

  GtkWidget *display_detector_mode_label=gtk_label_new("Detector:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_detector_mode_label,0,7,1,1);

  GtkStringList *ddm_sl=gtk_string_list_new(NULL);
  for(i=0; i<(int)(sizeof(display_detector_mode_names)/sizeof(display_detector_mode_names[0])); i++) {
    gtk_string_list_append(ddm_sl,display_detector_mode_names[i]);
  }
  GtkWidget *display_detector_mode_b=gtk_drop_down_new(G_LIST_MODEL(ddm_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(display_detector_mode_b),rx->display_detector_mode);
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_detector_mode_b,1,7,1,1);
  g_signal_connect(display_detector_mode_b,"notify::selected",G_CALLBACK(display_detector_mode_changed_cb),rx);

  GtkWidget *display_average_mode_label=gtk_label_new("Averaging:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_average_mode_label,0,8,1,1);

  GtkStringList *dam_sl=gtk_string_list_new(NULL);
  for(i=0; i<(int)(sizeof(display_average_mode_names)/sizeof(display_average_mode_names[0])); i++) {
    gtk_string_list_append(dam_sl,display_average_mode_names[i]);
  }
  GtkWidget *display_average_mode_b=gtk_drop_down_new(G_LIST_MODEL(dam_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(display_average_mode_b),rx->display_average_mode);
  gtk_grid_attach(GTK_GRID(panadapter_grid),display_average_mode_b,1,8,1,1);
  g_signal_connect(display_average_mode_b,"notify::selected",G_CALLBACK(display_average_mode_changed_cb),rx);

  GtkWidget *panadapter_peak_hold=gtk_check_button_new_with_label("Peak Hold");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_peak_hold), rx->panadapter_peak_hold);
  gtk_widget_set_margin_start(panadapter_peak_hold,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_peak_hold,3,5,2,1);
  g_signal_connect(panadapter_peak_hold,"toggled",G_CALLBACK(panadapter_peak_hold_changed_cb),rx);

  GtkWidget *panadapter_peak_decay_label=gtk_label_new("Peak Decay (dB/s):");
  gtk_widget_set_margin_start(panadapter_peak_decay_label,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_peak_decay_label,3,6,1,1);

  GtkWidget *panadapter_peak_decay_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_peak_decay,0.0, 50.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_peak_decay_scale,200,30);
  sui_scale_show_value(panadapter_peak_decay_scale,0);
  gtk_widget_set_visible(panadapter_peak_decay_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_peak_decay_scale),"value_changed",G_CALLBACK(panadapter_peak_decay_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_peak_decay_scale,4,6,1,1);

  GtkWidget *panadapter_histogram=gtk_check_button_new_with_label("Histogram");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_histogram), rx->panadapter_histogram);
  gtk_widget_set_margin_start(panadapter_histogram,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_histogram,3,7,2,1);
  g_signal_connect(panadapter_histogram,"toggled",G_CALLBACK(panadapter_histogram_changed_cb),rx);

  GtkWidget *panadapter_histogram_decay_label=gtk_label_new("Persistence Decay:");
  gtk_widget_set_margin_start(panadapter_histogram_decay_label,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_histogram_decay_label,3,8,1,1);

  GtkWidget *panadapter_histogram_decay_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_histogram_decay,1.0, 100.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_histogram_decay_scale,200,30);
  sui_scale_show_value(panadapter_histogram_decay_scale,0);
  gtk_widget_set_visible(panadapter_histogram_decay_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_histogram_decay_scale),"value_changed",G_CALLBACK(panadapter_histogram_decay_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_histogram_decay_scale,4,8,1,1);

  GtkWidget *panadapter_phase=gtk_check_button_new_with_label("Phase Scope");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (panadapter_phase), rx->panadapter_phase);
  gtk_widget_set_margin_start(panadapter_phase,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_phase,3,9,2,1);
  g_signal_connect(panadapter_phase,"toggled",G_CALLBACK(panadapter_phase_changed_cb),rx);

  GtkWidget *panadapter_phase_mode_label=gtk_label_new("Phase Style:");
  gtk_widget_set_margin_start(panadapter_phase_mode_label,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_phase_mode_label,3,10,1,1);

  GtkStringList *pm_sl=gtk_string_list_new(NULL);
  gtk_string_list_append(pm_sl,"Dots");
  gtk_string_list_append(pm_sl,"Lines");
  GtkWidget *panadapter_phase_mode_b=gtk_drop_down_new(G_LIST_MODEL(pm_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(panadapter_phase_mode_b),rx->panadapter_phase_mode);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_phase_mode_b,4,10,1,1);
  g_signal_connect(panadapter_phase_mode_b,"notify::selected",G_CALLBACK(panadapter_phase_mode_changed_cb),rx);

  GtkWidget *panadapter_phase_gain_label=gtk_label_new("Phase Gain:");
  gtk_widget_set_margin_start(panadapter_phase_gain_label,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_phase_gain_label,3,11,1,1);

  GtkWidget *panadapter_phase_gain_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->panadapter_phase_gain,10.0, 400.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(panadapter_phase_gain_scale,200,30);
  sui_scale_show_value(panadapter_phase_gain_scale,0);
  gtk_widget_set_visible(panadapter_phase_gain_scale, TRUE);
  g_signal_connect(G_OBJECT(panadapter_phase_gain_scale),"value_changed",G_CALLBACK(panadapter_phase_gain_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_phase_gain_scale,4,11,1,1);

  GtkWidget *panadapter_phase_source_label=gtk_label_new("Phase Source:");
  gtk_widget_set_margin_start(panadapter_phase_source_label,24);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_phase_source_label,3,12,1,1);

  GtkStringList *ps_sl=gtk_string_list_new(NULL);
  gtk_string_list_append(ps_sl,"Wideband");
  gtk_string_list_append(ps_sl,"Tuned");
  gtk_string_list_append(ps_sl,"Diversity");
  GtkWidget *panadapter_phase_source_b=gtk_drop_down_new(G_LIST_MODEL(ps_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(panadapter_phase_source_b),rx->panadapter_phase_source);
  gtk_grid_attach(GTK_GRID(panadapter_grid),panadapter_phase_source_b,4,12,1,1);
  g_signal_connect(panadapter_phase_source_b,"notify::selected",G_CALLBACK(panadapter_phase_source_changed_cb),rx);

  // Waterfall controls live INSIDE the Panadapter frame now, filling the empty
  // lower half of its left column (the left group ended at Averaging/row 7 while
  // the right group runs to row 12). No separate Waterfall frame — the operator
  // asked for these elements tucked into the panadapter block itself.
  GtkWidget *waterfall_hdr=gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(waterfall_hdr),"<b>Waterfall</b>");
  gtk_widget_set_halign(waterfall_hdr,GTK_ALIGN_START);
  gtk_widget_set_margin_top(waterfall_hdr,8);
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_hdr,0,9,2,1);

  GtkWidget *waterfall_high_label=gtk_label_new("High:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_high_label,0,10,1,1);

  GtkWidget *waterfall_high_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->waterfall_high,-200.0, 20.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(waterfall_high_scale,200,30);
  sui_scale_show_value(waterfall_high_scale,0);
  gtk_widget_set_visible(waterfall_high_scale, TRUE);
  g_signal_connect(G_OBJECT(waterfall_high_scale),"value_changed",G_CALLBACK(waterfall_high_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_high_scale,1,10,1,1);

  GtkWidget *waterfall_low_label=gtk_label_new("Low:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_low_label,0,11,1,1);

  GtkWidget *waterfall_low_scale=gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,gtk_adjustment_new(rx->waterfall_low,-200.0, 20.0, 1.0, 1.0, 1.0));
  gtk_widget_set_size_request(waterfall_low_scale,200,30);
  sui_scale_show_value(waterfall_low_scale,0);
  gtk_widget_set_visible(waterfall_low_scale, TRUE);
  g_signal_connect(G_OBJECT(waterfall_low_scale),"value_changed",G_CALLBACK(waterfall_low_value_changed_cb),rx);
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_low_scale,1,11,1,1);

  GtkWidget *waterfall_automatic=gtk_check_button_new_with_label("Waterfall Automatic");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (waterfall_automatic), rx->waterfall_automatic);
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_automatic,0,12,2,1);
  g_signal_connect(waterfall_automatic,"toggled",G_CALLBACK(waterfall_automatic_cb),rx);

  GtkWidget *waterfall_ft8_marker=gtk_check_button_new_with_label("Waterfall FT8 Marker");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (waterfall_ft8_marker), rx->waterfall_ft8_marker);
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_ft8_marker,0,13,2,1);
  g_signal_connect(waterfall_ft8_marker,"toggled",G_CALLBACK(waterfall_ft8_marker_cb),rx);

  GtkWidget *waterfall_theme_label=gtk_label_new("Color Theme:");
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_theme_label,0,14,1,1);

  GtkStringList *wt_sl=gtk_string_list_new(NULL);
  for(i=0; i<get_theme_count(); i++) {
    gtk_string_list_append(wt_sl, get_theme_name(i));
  }
  GtkWidget *waterfall_theme_combo=gtk_drop_down_new(G_LIST_MODEL(wt_sl),NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(waterfall_theme_combo),rx->waterfall_color_theme);
  gtk_grid_attach(GTK_GRID(panadapter_grid),waterfall_theme_combo,1,14,1,1);
  g_signal_connect(waterfall_theme_combo,"notify::selected",G_CALLBACK(waterfall_theme_cb),rx);

  // CAT sits directly UNDER the Panadapter in the same (middle) column, so it
  // takes the Panadapter's full width (col fill). row is 4 here: TX Freq (row 0)
  // + Panadapter (rows 1..3).
  GtkWidget *cat_frame=gtk_frame_new("CAT");
  GtkWidget *cat_grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(cat_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(cat_grid),FALSE);
  sui_style_group(cat_grid);
  gtk_frame_set_child(GTK_FRAME(cat_frame),cat_grid);
  // Top-align: without this the frame fills the full column height (as tall as
  // the left Audio/EQ/NR4/APF stack) and leaves a big empty area below its 3
  // rows of controls. START makes it hug its content height.
  // vexpand as well as FILL: valign only places the frame inside the slot the
  // box hands it, and without vexpand that slot is just its natural height.
  gtk_widget_set_valign(cat_frame,GTK_ALIGN_FILL);
  gtk_widget_set_vexpand(cat_frame,TRUE);
  // CAT sits in a vbox rather than straight in the grid cell: attached as a
  // plain grid row it was spread out by the tall panadapter row above (same
  // reason the left column stacks its Audio/EQ/NR4 frames in a box).
  GtkWidget *cat_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,5);
  gtk_widget_set_valign(cat_box,GTK_ALIGN_FILL);
  gtk_widget_set_vexpand(cat_box,TRUE);
  gtk_box_append(GTK_BOX(right_box),cat_box);
  gtk_box_append(GTK_BOX(cat_box),cat_frame);

  GtkWidget *cat_debug_b=gtk_check_button_new_with_label("CAT Debug");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (cat_debug_b), rx->rigctl_debug);
  gtk_grid_attach(GTK_GRID(cat_grid),cat_debug_b,0,0,3,1);
  g_signal_connect(cat_debug_b,"toggled",G_CALLBACK(cat_debug_cb),rx);
  // CAT fills the full Panadapter width above it via two groups: TCP/IP on the
  // left (cols 0-2), Serial on the right (cols 4-6, a 40px gap at col 3). The
  // serial-port entry hexpands to consume the remainder — no empty right half.


  GtkWidget *cat_enable_b=gtk_check_button_new_with_label("TCP/IP enable");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (cat_enable_b), rx->rigctl_enable);
  gtk_grid_attach(GTK_GRID(cat_grid),cat_enable_b,0,1,3,1);
  g_signal_connect(cat_enable_b,"toggled",G_CALLBACK(cat_enable_cb),rx);

  GtkWidget *rigctl_port_spinner =gtk_spin_button_new_with_range(18000,21000,1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(rigctl_port_spinner),(double)rx->rigctl_port);
  gtk_widget_set_visible(rigctl_port_spinner, TRUE);
  gtk_grid_attach(GTK_GRID(cat_grid),rigctl_port_spinner,0,2,3,1);
  g_signal_connect(rigctl_port_spinner,"value_changed",G_CALLBACK(rigctl_value_changed_cb),rx);



  GtkWidget *cat_serial_enable_b=gtk_check_button_new_with_label("Serial Port Enable");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (cat_serial_enable_b), rx->rigctl_serial_enable);
  gtk_widget_set_margin_start(cat_serial_enable_b,40);
  gtk_grid_attach(GTK_GRID(cat_grid),cat_serial_enable_b,4,0,3,1);
  g_signal_connect(cat_serial_enable_b,"toggled",G_CALLBACK(cat_serial_enable_cb),rx);

  GtkWidget *serial_text_label=gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(serial_text_label), "<b>Serial Port: </b>");
  gtk_widget_set_margin_start(serial_text_label,40);
  gtk_grid_attach(GTK_GRID(cat_grid),serial_text_label,4,1,1,1);

  rx->serial_port_entry=gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(rx->serial_port_entry),rx->rigctl_serial_port);
  gtk_widget_set_visible(rx->serial_port_entry, TRUE);
  // Sized to a device path, NOT hexpanding: expanding here made the entry eat
  // every spare pixel of the column and dragged the whole Configure window
  // wider than it needs to be.
  gtk_editable_set_width_chars(GTK_EDITABLE(rx->serial_port_entry),16);
  gtk_editable_set_max_width_chars(GTK_EDITABLE(rx->serial_port_entry),24);
  gtk_widget_set_halign(rx->serial_port_entry,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(cat_grid),rx->serial_port_entry,5,1,2,1);
  g_signal_connect(rx->serial_port_entry,"activate",G_CALLBACK(cat_serial_port_cb),rx);

  GtkWidget *serial_baudrate_label=gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(serial_baudrate_label), "<b>Baudrate: </b>");
  gtk_widget_set_margin_start(serial_baudrate_label,40);
  gtk_grid_attach(GTK_GRID(cat_grid),serial_baudrate_label,4,2,1,1);

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
  gtk_widget_set_halign(cat_serial_port_baudrate,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(cat_grid),cat_serial_port_baudrate,5,2,1,1);
  g_signal_connect(cat_serial_port_baudrate,"notify::selected",G_CALLBACK(cat_baudrate_cb),rx);

  // Squelch + Manual Notch sit SIDE BY SIDE in the page's bottom row, under
  // BOTH grid columns. Neither is tall, so stacking them wasted the bottom of
  // the page; and inside the right-hand CAT column they started at the middle
  // column's left edge (reading as right-aligned) with MNF running off the
  // right edge. halign START pins the pair to the page's left edge; the
  // children keep the default valign FILL so both frames take the row height
  // and come out the same height.
  GtkWidget *sq_mnf_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
  gtk_widget_set_valign(sq_mnf_row,GTK_ALIGN_START);
  gtk_widget_set_halign(sq_mnf_row,GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(page),sq_mnf_row);

  // Squelch calibration. Only the AMSQ (non-FM) path is exposed: the FM squelch
  // mapping is fixed and known-good, while the amplitude squelch has no
  // calibrated reference here.
  {
    GtkWidget *sq_frame=gtk_frame_new("Squelch (AM/SSB)");
    GtkWidget *sq_grid=gtk_grid_new();
    sui_style_group(sq_grid);
    gtk_frame_set_child(GTK_FRAME(sq_frame),sq_grid);
    gtk_box_append(GTK_BOX(sq_mnf_row),sq_frame);

    GtkWidget *l=gtk_label_new("The SQL bar spans this dB range on the pre-AGC signal (FM uses its own squelch).");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_label_set_wrap(GTK_LABEL(l),TRUE);
    // Half-width now, so cap the natural width or the wrapped label alone would
    // ask for the whole column back.
    gtk_label_set_max_width_chars(GTK_LABEL(l),40);
    gtk_grid_attach(GTK_GRID(sq_grid),l,0,0,4,1);

    l=gtk_label_new("Bar min (dB):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(sq_grid),l,0,1,1,1);
    GtkWidget *sp=gtk_spin_button_new_with_range(-160.0,0.0,1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp),rx->amsq_min_db);
    gtk_grid_attach(GTK_GRID(sq_grid),sp,1,1,1,1);
    g_signal_connect(sp,"value_changed",G_CALLBACK(amsq_min_cb),rx);

    l=gtk_label_new("Bar max (dB):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_widget_set_margin_start(l,20);
    gtk_grid_attach(GTK_GRID(sq_grid),l,2,1,1,1);
    sp=gtk_spin_button_new_with_range(-160.0,0.0,1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp),rx->amsq_max_db);
    gtk_grid_attach(GTK_GRID(sq_grid),sp,3,1,1,1);
    g_signal_connect(sp,"value_changed",G_CALLBACK(amsq_max_cb),rx);

    l=gtk_label_new("Max tail (s):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_grid_attach(GTK_GRID(sq_grid),l,0,2,1,1);
    sp=gtk_spin_button_new_with_range(0.0,3.0,0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(sp),2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp),rx->amsq_tail);
    gtk_widget_set_tooltip_text(sp,"How long the gate stays open after the signal drops");
    gtk_grid_attach(GTK_GRID(sq_grid),sp,1,2,1,1);
    g_signal_connect(sp,"value_changed",G_CALLBACK(amsq_tail_cb),rx);
  }

  // Manual notch list. The panadapter gestures (Ctrl+click to add/remove,
  // Ctrl+scroll to resize) stay the fast path; this is for exact values and for
  // the per-notch enable/AF switches, which have no gesture.
  {
    MNF_UI *ui=g_new0(MNF_UI,1);
    ui->rx=rx;

    GtkWidget *mnf_frame=gtk_frame_new("Manual Notch (MNF)");
    GtkWidget *mnf_grid=gtk_grid_new();
    sui_style_group(mnf_grid);
    gtk_frame_set_child(GTK_FRAME(mnf_frame),mnf_grid);
    // MNF sits left, under the Audio/EQ/NR4 column, and a size group pins it to
    // that column's exact width so the two line up; Squelch takes the rest.
    gtk_box_prepend(GTK_BOX(sq_mnf_row),mnf_frame);
    GtkSizeGroup *sg=gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    gtk_size_group_add_widget(sg,left_box);
    gtk_size_group_add_widget(sg,mnf_frame);
    g_object_set_data_full(G_OBJECT(mnf_frame),"mnf_sizegroup",sg,g_object_unref);
    // Freed with the frame, so the callbacks' data outlives every row widget.
    g_object_set_data_full(G_OBJECT(mnf_frame),"mnf_ui",ui,g_free);

    GtkWidget *add_b=gtk_button_new_with_label("Add at VFO");
    gtk_grid_attach(GTK_GRID(mnf_grid),add_b,0,0,1,1);
    g_signal_connect(add_b,"clicked",G_CALLBACK(notch_add_cb),ui);

    GtkWidget *l=gtk_label_new("New notch width (Hz):");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_widget_set_margin_start(l,20);
    gtk_grid_attach(GTK_GRID(mnf_grid),l,1,0,1,1);
    GtkWidget *dw=gtk_spin_button_new_with_range(NOTCH_MIN_WIDTH,NOTCH_MAX_WIDTH,10.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dw),rx->notch_default_width);
    gtk_widget_set_tooltip_text(dw,"Width a Ctrl+click notch is created with");
    gtk_grid_attach(GTK_GRID(mnf_grid),dw,2,0,1,1);
    g_signal_connect(dw,"value_changed",G_CALLBACK(notch_default_width_cb),rx);

    l=gtk_label_new("Ctrl+click the panadapter to add or remove a notch, Ctrl+scroll over one to resize it. AF makes a notch ride the dial instead of staying on the RF frequency.");
    gtk_label_set_xalign(GTK_LABEL(l),0.0);
    gtk_label_set_wrap(GTK_LABEL(l),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l),40);
    gtk_grid_attach(GTK_GRID(mnf_grid),l,0,1,5,1);

    const char *hdr[4]={"On","Frequency (MHz)","Width (Hz)","AF"};
    for(int h=0;h<4;h++) {
      GtkWidget *hl=gtk_label_new(hdr[h]);
      gtk_label_set_xalign(GTK_LABEL(hl),0.0);
      gtk_grid_attach(GTK_GRID(mnf_grid),hl,h,2,1,1);
    }

    for(int n=0;n<MAX_NOTCHES;n++) {
      int r=3+n;

      ui->active[n]=gtk_check_button_new();
      g_object_set_data(G_OBJECT(ui->active[n]),"notch_idx",GINT_TO_POINTER(n));
      gtk_grid_attach(GTK_GRID(mnf_grid),ui->active[n],0,r,1,1);
      g_signal_connect(ui->active[n],"toggled",G_CALLBACK(notch_active_cb),ui);

      ui->freq[n]=gtk_spin_button_new_with_range(0.0,6000.0,0.0001);
      gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ui->freq[n]),6);
      g_object_set_data(G_OBJECT(ui->freq[n]),"notch_idx",GINT_TO_POINTER(n));
      gtk_grid_attach(GTK_GRID(mnf_grid),ui->freq[n],1,r,1,1);
      g_signal_connect(ui->freq[n],"value_changed",G_CALLBACK(notch_freq_cb),ui);

      ui->width[n]=gtk_spin_button_new_with_range(NOTCH_MIN_WIDTH,NOTCH_MAX_WIDTH,10.0);
      g_object_set_data(G_OBJECT(ui->width[n]),"notch_idx",GINT_TO_POINTER(n));
      gtk_grid_attach(GTK_GRID(mnf_grid),ui->width[n],2,r,1,1);
      g_signal_connect(ui->width[n],"value_changed",G_CALLBACK(notch_width_cb),ui);

      ui->af[n]=gtk_check_button_new();
      g_object_set_data(G_OBJECT(ui->af[n]),"notch_idx",GINT_TO_POINTER(n));
      gtk_grid_attach(GTK_GRID(mnf_grid),ui->af[n],3,r,1,1);
      g_signal_connect(ui->af[n],"toggled",G_CALLBACK(notch_af_cb),ui);

      ui->del[n]=gtk_button_new_with_label("Delete");
      g_object_set_data(G_OBJECT(ui->del[n]),"notch_idx",GINT_TO_POINTER(n));
      gtk_grid_attach(GTK_GRID(mnf_grid),ui->del[n],4,r,1,1);
      g_signal_connect(ui->del[n],"clicked",G_CALLBACK(notch_delete_cb),ui);
    }

    ui->empty=gtk_label_new("No notches.");
    gtk_label_set_xalign(GTK_LABEL(ui->empty),0.0);
    gtk_grid_attach(GTK_GRID(mnf_grid),ui->empty,0,3+MAX_NOTCHES,5,1);

    mnf_refresh(ui);
  }

  return page;
}
