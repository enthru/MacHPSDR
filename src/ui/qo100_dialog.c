/* Copyright (C)
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
#include <string.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "qo100.h"
#include "qo100_dialog.h"

// Status label + poll id are static because only one Configure dialog exists at a
// time; the page's "destroy" handler nulls the label and cancels the poll so the
// timer can never fire against a freed widget (the same pattern as the DX-cluster
// and PPM status readouts).
static GtkWidget *qo100_status_label;
static GtkWidget *qo100_setup_label;
static guint      qo100_poll_id;

static void status_refresh(void) {
  if(qo100_status_label==NULL) return;
  char st[128], buf[192];
  qo100_beacon_status(st,sizeof(st));
  snprintf(buf,sizeof(buf),"Beacon: %s",st);
  gtk_label_set_text(GTK_LABEL(qo100_status_label),buf);
}

static gboolean status_poll(gpointer data) {
  (void)data;
  status_refresh();
  return TRUE;
}

static void qo100_dialog_destroy(GtkWidget *widget, gpointer data) {
  (void)widget; (void)data;
  if(qo100_poll_id!=0) { g_source_remove(qo100_poll_id); qo100_poll_id=0; }
  qo100_status_label=NULL;
  qo100_setup_label=NULL;
}

static void offset_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  // Entered in MHz because the number is 8089.5 and nobody wants to type nine
  // digits; kept in Hz on RADIO because that is what the tuning arithmetic uses.
  double mhz=gtk_spin_button_get_value(GTK_SPIN_BUTTON(w));
  r->qo100_offset=(long long)llround(mhz*1.0e6);
}

static void setup_cb(GtkWidget *w, gpointer data) {
  (void)w;
  RADIO *r=(RADIO *)data;
  gboolean ok=qo100_transponder_setup(r);
  if(qo100_setup_label==NULL) return;
  if(ok) {
    RECEIVER *rx=r->active_receiver;
    char buf[128];
    snprintf(buf,sizeof(buf),"Uplink on VFO B: %.6f MHz, SAT split on",
             rx!=NULL ? (double)rx->frequency_b/1.0e6 : 0.0);
    gtk_label_set_text(GTK_LABEL(qo100_setup_label),buf);
  } else {
    gtk_label_set_text(GTK_LABEL(qo100_setup_label),
                       "Tune the receiver to the downlink (10489.500\342\200\22310490.000 MHz) first");
  }
}

static void bandplan_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->qo100_bandplan=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
}

static void ref_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->qo100_beacon_ref=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
  // Drop the remembered level: it was measured with the old setting/beacon and a
  // stale line is worse than none.
  if(r->active_receiver!=NULL) r->active_receiver->qo100_ref_dbm=-1000.0;
}

static void lock_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->qo100_beacon_lock=gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
  if(!r->qo100_beacon_lock) qo100_beacon_reset();
  status_refresh();
}

static void beacon_cb(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  RADIO *r=(RADIO *)data;
  r->qo100_beacon_sel=(gint)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  qo100_beacon_reset();                 // re-acquire against the new beacon
  if(r->active_receiver!=NULL) r->active_receiver->qo100_ref_dbm=-1000.0;
  status_refresh();
}

static void reacquire_cb(GtkWidget *w, gpointer data) {
  (void)w; (void)data;
  qo100_beacon_reset();
  status_refresh();
}

GtkWidget *create_qo100_dialog(RADIO *r) {
  GtkWidget *frame=gtk_frame_new("QO-100 (Es'hail-2)");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);

  // Running row counter so controls can be inserted without renumbering (the
  // same convention as the DX-cluster page).
  int row=0;

  GtkWidget *info=gtk_label_new(
    "The geostationary narrow-band transponder: uplink 2400.000\342\200\2232400.500 MHz,\n"
    "downlink 10489.500\342\200\22310490.000 MHz, non-inverting.\n"
    "\n"
    "Receive and transmit go through different converters, so define TWO entries\n"
    "under Transverters above \342\200\224 one covering the downlink with the LNB's LO\n"
    "(9750 MHz for a standard universal LNB), one covering the uplink with the\n"
    "2.4 GHz transverter's LO. VFO A then follows the receive entry and VFO B the\n"
    "transmit one.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12);
  gtk_grid_attach(GTK_GRID(grid),info,0,row++,2,1);

  // ---- transponder ---------------------------------------------------------
  GtkWidget *off_lbl=gtk_label_new("Transponder offset (MHz):");
  gtk_widget_set_halign(off_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),off_lbl,0,row,1,1);
  GtkWidget *off=gtk_spin_button_new_with_range(8000.0,8200.0,0.000001);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(off),6);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(off),
                            (double)(r->qo100_offset!=0?r->qo100_offset:QO100_TP_OFFSET)/1.0e6);
  gtk_widget_set_halign(off,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),off,1,row++,1,1);
  g_signal_connect(off,"value-changed",G_CALLBACK(offset_cb),r);

  GtkWidget *setup=gtk_button_new_with_label("Set up transponder mode");
  gtk_widget_set_halign(setup,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),setup,0,row++,2,1);
  g_signal_connect(setup,"clicked",G_CALLBACK(setup_cb),r);

  qo100_setup_label=gtk_label_new("Puts VFO B on the matching uplink and links the two (SAT split).");
  gtk_widget_set_halign(qo100_setup_label,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(qo100_setup_label,12);
  gtk_grid_attach(GTK_GRID(grid),qo100_setup_label,0,row++,2,1);

  // ---- display -------------------------------------------------------------
  GtkWidget *bp=gtk_check_button_new_with_label("Show the transponder band plan on the panadapter");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(bp),r->qo100_bandplan);
  gtk_grid_attach(GTK_GRID(grid),bp,0,row++,2,1);
  g_signal_connect(bp,"toggled",G_CALLBACK(bandplan_cb),r);

  GtkWidget *rf=gtk_check_button_new_with_label("Show the beacon level as a reference line");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(rf),r->qo100_beacon_ref);
  gtk_grid_attach(GTK_GRID(grid),rf,0,row++,2,1);
  g_signal_connect(rf,"toggled",G_CALLBACK(ref_cb),r);

  GtkWidget *rf_hint=gtk_label_new("Keep your own downlink below that line \342\200\224 the transponder is shared.");
  gtk_widget_set_halign(rf_hint,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(rf_hint,12);
  gtk_grid_attach(GTK_GRID(grid),rf_hint,0,row++,2,1);

  // ---- beacon lock ---------------------------------------------------------
  GtkWidget *lk=gtk_check_button_new_with_label("Correct the LNB's drift against a beacon");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(lk),r->qo100_beacon_lock);
  gtk_grid_attach(GTK_GRID(grid),lk,0,row++,2,1);
  g_signal_connect(lk,"toggled",G_CALLBACK(lock_cb),r);

  GtkWidget *b_lbl=gtk_label_new("Reference beacon:");
  gtk_widget_set_halign(b_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),b_lbl,0,row,1,1);
  // Only the two CW beacons: the middle one is 400 bd BPSK and has no carrier to
  // measure (see qo100_beacon_frequency()).
  const char *b_opts[]={"Lower, 10489.500 MHz (CW)","Upper, 10490.000 MHz (CW)",NULL};
  GtkWidget *b_dd=gtk_drop_down_new_from_strings(b_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(b_dd),
                             (r->qo100_beacon_sel>=0 && r->qo100_beacon_sel<=1)?r->qo100_beacon_sel:0);
  gtk_widget_set_halign(b_dd,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),b_dd,1,row++,1,1);
  g_signal_connect(b_dd,"notify::selected",G_CALLBACK(beacon_cb),r);

  GtkWidget *re=gtk_button_new_with_label("Re-acquire");
  gtk_widget_set_halign(re,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),re,0,row++,2,1);
  g_signal_connect(re,"clicked",G_CALLBACK(reacquire_cb),r);

  qo100_status_label=gtk_label_new("Beacon: Off");
  gtk_widget_set_halign(qo100_status_label,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),qo100_status_label,0,row++,2,1);

  GtkWidget *lk_hint=gtk_label_new(
    "The correction is written into the receive band's LO error, so it survives a\n"
    "restart. The uplink converter is a different box and is never touched by it.");
  gtk_widget_set_halign(lk_hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),lk_hint,0,row++,2,1);

  status_refresh();
  if(qo100_poll_id!=0) g_source_remove(qo100_poll_id);
  qo100_poll_id=g_timeout_add(500,status_poll,NULL);
  g_signal_connect(frame,"destroy",G_CALLBACK(qo100_dialog_destroy),NULL);

  return frame;
}
