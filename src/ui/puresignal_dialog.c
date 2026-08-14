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
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <wdsp.h>

#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "ext.h"
#include "settings_ui.h"
#include "css.h"
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#include "audio.h"
#include "band.h"
#include "level_meter.h"
#include "pana_view.h"

static int deltadb=0;
static gboolean running=FALSE;

#define BLDR_RX 0
#define BLDR_CM 1
#define BLDR_CC 2
#define BLDR_CS 3
#define FEEDBACK 4
#define COR_CNT 5
#define SLN_CHK 6
#define DG_CNT 13
#define STATUS 15

#define INFO_SIZE 16
static int info[INFO_SIZE];
static int save_ps_auto;
static int save_ps_single;

// Skin palette colour by name -> GdkRGBA (fallback to the given RGB).
static GdkRGBA ps_rgba(const char *name, double fr, double fg, double fb) {
  double r=fr,g=fg,b=fb;
  css_rgb(name,&r,&g,&b);
  return (GdkRGBA){(float)r,(float)g,(float)b,1.0f};
}

static double ps_last_pk=0;   // last peak, published by update_ps() for the builder
static void ps_build(GtkSnapshot *snapshot,int width,int height,gpointer data);

// Per-update STATE: publish the peak reading, then queue the GPU redraw. The
// info[] block is a file static the builder reads directly.
static void update_ps(TRANSMITTER *tx,double pk) {
  ps_last_pk=pk;
  if(tx->ps!=NULL) gtk_widget_queue_draw(tx->ps);
}

// GPU render-node builder (PanaView) — the PureSignal feedback readout (text only).
static void ps_build(GtkSnapshot *snapshot,int width,int height,gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  GtkWidget *widget=tx->ps;
  char text[32];

  GdkRGBA surf=ps_rgba("SURFACE",0.16,0.16,0.19);
  lm_fill(snapshot,0,0,width,height,&surf);

  GdkRGBA fb;
  if(info[FEEDBACK]>181)       fb=ps_rgba("OFF_WHITE",0.89,0.89,0.91); // good feedback level
  else if(info[FEEDBACK]>128)  fb=ps_rgba("ACCENT_A",0.0,1.0,0.0);
  else if(info[FEEDBACK]>90)   fb=ps_rgba("INFO_ON",0.0,1.0,1.0);
  else                         fb=ps_rgba("WARNING",1.0,0.0,0.0);      // too low
  sprintf(text,"Feedback Level: %d",info[FEEDBACK]);
  lm_text(snapshot,widget,5,12,12,&fb,text,FALSE);

  GdkRGBA ow=ps_rgba("OFF_WHITE",0.89,0.89,0.91);

  sprintf(text,"Correction Count: %d",info[COR_CNT]);
  lm_text(snapshot,widget,5,24,12,&ow,text,FALSE);

  sprintf(text,"Sln Chk: %d",info[SLN_CHK]);
  lm_text(snapshot,widget,5,36,12,&ow,text,FALSE);

  sprintf(text,"Dg Cnt: %d",info[DG_CNT]);
  lm_text(snapshot,widget,5,48,12,&ow,text,FALSE);

  const char *st=NULL;
  switch(info[STATUS]) {
    case 0: st="STATUS: RESET"; break;
    case 1: st="STATUS: WAIT"; break;
    case 2: st="STATUS: MOXDELAY"; break;
    case 3: st="STATUS: SETUP"; break;
    case 4: st="STATUS: COLLECT"; break;
    case 5: st="STATUS: MOXCHECK"; break;
    case 6: st="STATUS: CALC"; break;
    case 7: st="STATUS: DELAY"; break;
    case 8: st="STATUS: STAY ON"; break;
    case 9: st="STATUS: TURN ON"; break;
    default:
      sprintf(text,"STATUS: UNKNOWN %d",info[STATUS]);
      st=text;
      break;
  }
  lm_text(snapshot,widget,5,60,12,&ow,st,FALSE);

  sprintf(text,"Peak: %f",ps_last_pk);
  lm_text(snapshot,widget,5,72,12,&ow,text,FALSE);
}

static gboolean info_timeout(gpointer arg) {
#ifdef PURESIGNAL
  TRANSMITTER *tx=(TRANSMITTER *)arg;
  double pk;
  
  GetPSInfo(tx->channel,&info[0]);
  
  // Clear the stored id on every path that ends the timer: g_source_remove()
  // on an id GLib has already dropped is a warning, and the page-destroy
  // handler cannot tell the two apart otherwise.
  if (tx->puresignal == NULL) { tx->ps_timer_id=0; return FALSE; }

  if (tx->puresignal->auto_on) {
    double fbk_db;
    int newcal = info[COR_CNT] != tx->puresignal->old_cor_cnt;
    tx->puresignal->old_cor_cnt = info[COR_CNT];

    int att = ps_get_tx_attenuation(tx->puresignal);
    
    switch(tx->puresignal->state) {
      case 0:
        if(newcal && ((info[FEEDBACK] > 175 && att < 31) || (info[FEEDBACK]<= 132 && att > 0))) {
          if(info[FEEDBACK] > 256) {
            fbk_db = 100;
          }
          else if (info[FEEDBACK] > 0) {
            fbk_db = 20.0 * log10((double)info[FEEDBACK] / 152.293);
          }
          else {
            fbk_db = -100.0;
          }
          fbk_db = (int)lround(fbk_db);
          ps_change_tx_attenuation(tx->puresignal, fbk_db);
        }
        break;
      case 1:
        tx->puresignal->state = 2;
        SetPSControl(tx->channel, 1, 0, 0, 0);
        break;
      case 2:
        tx->puresignal->state = 0;
        SetPSControl(tx->channel, 0, 0, 1, 0);
        break;
    }
  }
  GetPSMaxTX(tx->channel,&pk);
  update_ps(tx, pk);

  if(!running) { tx->ps_timer_id=0; return FALSE; }
  return TRUE;
#else
  return FALSE;
#endif
}

static void enable_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  gboolean want=gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  // Already on the GTK thread, so call the shared applier straight rather than
  // through g_idle_add. It carries the free-slot precondition and the
  // puresignal_enabled bookkeeping, so this handler and the CAT/MIDI paths
  // cannot drift apart.
  ext_tx_set_ps(GINT_TO_POINTER(want));

  // Enabling can fail (too many open receivers) — reflect the real outcome
  // without re-entering this callback.
  if(gtk_check_button_get_active(GTK_CHECK_BUTTON(widget)) != tx->puresignal_enabled) {
    g_signal_handlers_block_by_func(widget,G_CALLBACK(enable_cb),tx);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widget),tx->puresignal_enabled);
    g_signal_handlers_unblock_by_func(widget,G_CALLBACK(enable_cb),tx);
  }
}

/* The Configure page is going away: stop the 100 ms readout timer and drop the
   widget pointer it draws through, so nothing is left pointing at a destroyed
   PanaView.  The transmitter itself outlives the page, hence the fields being
   cleared rather than the struct. */
static void ps_page_destroy(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  running=FALSE;
  if(tx->ps_timer_id>0) {
    g_source_remove(tx->ps_timer_id);
    tx->ps_timer_id=0;
  }
  tx->ps=NULL;
}

static void twotone_cb(GtkWidget *widget, gpointer data) {
  TRANSMITTER *tx=(TRANSMITTER *)data;
  transmitter_set_twotone(tx,gtk_check_button_get_active(GTK_CHECK_BUTTON (widget)));
  if(tx->ps_twotone && (tx->puresignal != NULL)) {
    running=TRUE;
    if(tx->ps_timer_id==0) tx->ps_timer_id=g_timeout_add(100,info_timeout,(gpointer)tx);
  } else {
    running=FALSE;
    if(tx->ps_timer_id>0) { g_source_remove(tx->ps_timer_id); tx->ps_timer_id=0; }
  }
}


GtkWidget *create_puresignal_dialog(TRANSMITTER *tx) {
  GtkWidget *grid=gtk_grid_new();
  sui_style_page(grid);
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);

  int row=0;
  int col=0;

  GtkWidget *ps_frame=gtk_frame_new("Pure Signal");
  GtkWidget *ps_grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(ps_grid),10);
  // NOT homogeneous: the plot spans 8 columns, so homogeneous columns forced
  // every column to the width of the "Enable PureSignal" checkbox (~150px each
  // → ~1200px total, stretching the whole tab). Content-sized columns let the
  // plot keep its bounded 480px and the frame stays compact.
  gtk_grid_set_row_homogeneous(GTK_GRID(ps_grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(ps_grid),FALSE);
  sui_style_group(ps_grid);
  gtk_frame_set_child(GTK_FRAME(ps_frame),ps_grid);
  // Stretch the Pure Signal frame to the tab width (== PA Calibration width, the
  // widest sibling in the merged PA/Linearity page) instead of hugging the plot.
  gtk_widget_set_hexpand(ps_frame,TRUE);
  gtk_widget_set_halign(ps_frame,GTK_ALIGN_FILL);
  gtk_grid_attach(GTK_GRID(grid),ps_frame,col,row++,2,1);

  GtkWidget *enable_b=gtk_check_button_new_with_label("Enable PureSignal");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (enable_b), tx->puresignal_enabled);
  g_signal_connect(enable_b,"toggled",G_CALLBACK(enable_cb),tx);
  gtk_grid_attach(GTK_GRID(ps_grid),enable_b,0,0,1,1);

  GtkWidget *twotone_b=gtk_check_button_new_with_label("Two Tone");
  g_signal_connect(twotone_b,"toggled",G_CALLBACK(twotone_cb),tx);
  gtk_grid_attach(GTK_GRID(ps_grid),twotone_b,1,0,1,1);

  tx->ps=pana_view_new(ps_build,(gpointer)tx);
  // Bound the feedback plot to a modest fixed size (it had none, so it ballooned
  // to fill the tab). This is an unfinished prototype graph.
  gtk_widget_set_size_request(tx->ps,480,180);
  gtk_widget_set_hexpand(tx->ps,TRUE);   // grow with the frame (PA-Calibration width)
  gtk_widget_set_vexpand(tx->ps,FALSE);  // but keep the fixed 180px height
  gtk_widget_set_halign(tx->ps,GTK_ALIGN_FILL);
  gtk_grid_attach(GTK_GRID(ps_grid),tx->ps,0,1,8,8);

  // Honest disclaimer: this PureSignal path is an unfinished prototype.
  GtkWidget *note=gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(note),
    "<small><i>Note: PureSignal here is an unfinished prototype. Protocol 1 is the "
    "tested code path (peak calibration tuned mainly for the Hermes-Lite 2); "
#ifdef PURESIGNAL_P2
    "Protocol 2 support is now wired but experimental and completely unverified "
    "against real hardware — the closed correction loop has never run on a P2 "
    "radio with a feedback ADC. "
#else
    "Protocol 2 is not built in (enable PURESIGNAL_P2 to try the experimental path). "
#endif
    "None of this has been verified on hardware in this fork; use at your own risk.</i></small>");
  gtk_label_set_wrap(GTK_LABEL(note),TRUE);
  // Bound the wrap width so this long sentence doesn't stretch the whole page
  // (its single-line natural width was driving the tab wide).
  gtk_label_set_max_width_chars(GTK_LABEL(note),64);
  gtk_widget_set_halign(note,GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(note),0.0);
  gtk_grid_attach(GTK_GRID(grid),note,0,row++,2,1);

  // PureSignal (adaptive predistortion) needs a feedback ADC. Protocol 1 is the
  // live path; Protocol 2 is only wired when built with PURESIGNAL_P2. Grey the
  // interactive frame on any protocol the build can't drive, but leave the note
  // above sensitive so it stays readable and explains why.
  gboolean ps_supported = (radio->discovered->protocol == PROTOCOL_1);
#ifdef PURESIGNAL_P2
  if(radio->discovered->protocol == PROTOCOL_2) {
    ps_supported = TRUE;
  }
#endif
  if(!ps_supported) {
    gtk_widget_set_sensitive(ps_frame, FALSE);
  }

  // The Two Tone timer redraws tx->ps every 100 ms and nothing ever stopped it:
  // close Configure with Two Tone on and it went on calling
  // gtk_widget_queue_draw() on a finalised PanaView for the rest of the
  // process.  Stop it with the page, the shape every decoder panel uses.
  g_signal_connect(grid,"destroy",G_CALLBACK(ps_page_destroy),(gpointer)tx);

  return grid;
}
