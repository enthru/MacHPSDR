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
#include "bandstack.h"
#include "band.h"
#include "radio.h"
#include "settings_ui.h"
#include "xvtr_dialog.h"
#include "qo100.h"
#include "qo100_dialog.h"

// Status label + poll id are static because only one Configure dialog exists at a
// time; the page's "destroy" handler nulls the label and cancels the poll so the
// timer can never fire against a freed widget (the same pattern as the DX-cluster
// and PPM status readouts).
static GtkWidget *qo100_status_label;
static GtkWidget *qo100_check_label;
static GtkWidget *qo100_setup_label;
static GtkWidget *qo100_xvtr_label;
static guint      qo100_poll_id;

static void status_refresh(void) {
  if(qo100_status_label==NULL) return;
  char st[128], buf[192];
  qo100_beacon_status(st,sizeof(st));
  snprintf(buf,sizeof(buf),"Beacon: %s",st);
  gtk_label_set_text(GTK_LABEL(qo100_status_label),buf);
  // The lock's own status says how STEADY it is, which it can be while being
  // 400 Hz wrong; this is the only line that can say otherwise, so it gets a
  // row of its own rather than being appended to that one.
  if(qo100_check_label!=NULL) {
    char ck[192];
    qo100_beacon_check(ck,sizeof(ck));
    gtk_label_set_text(GTK_LABEL(qo100_check_label),
                       (ck[0]!='\0')?ck:"Middle beacon: no check yet");
  }
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
  qo100_check_label=NULL;
  qo100_setup_label=NULL;
  qo100_xvtr_label=NULL;
}

static void lnb_lo_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->qo100_lnb_lo=(long long)llround(gtk_spin_button_get_value(GTK_SPIN_BUTTON(w))*1.0e6);
}

static void tx_lo_cb(GtkWidget *w, gpointer data) {
  RADIO *r=(RADIO *)data;
  r->qo100_tx_lo=(long long)llround(gtk_spin_button_get_value(GTK_SPIN_BUTTON(w))*1.0e6);
}

static void make_xvtr_cb(GtkWidget *w, gpointer data) {
  (void)w;
  RADIO *r=(RADIO *)data;
  char msg[160];
  gboolean ok=qo100_create_transverters(r,msg,sizeof(msg));
  if(ok) {
    // Every Configure page is built when the dialog opens, so the Transverters
    // rows already have their entry widgets filled from the old values and would
    // sit there showing stale text on the Bands tab. Refresh them; and push the
    // new LO into any receiver already sitting in one of the two bands.
    for(int b=BANDS;b<BANDS+XVTRS;b++) {
      BAND *band=band_get_band(b);
      if(band==NULL) continue;
      if(strcmp(band->title,QO100_XVTR_RX_TITLE)==0 ||
         strcmp(band->title,QO100_XVTR_TX_TITLE)==0) {
        xvtr_dialog_refresh_row(b);
        update_receiver(b);
      }
    }
  }
  if(qo100_xvtr_label!=NULL) gtk_label_set_text(GTK_LABEL(qo100_xvtr_label),msg);
}

static void transponder_cb(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  RADIO *r=(RADIO *)data;
  r->qo100_transponder=(gint)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
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
                       "Could not set up \342\200\224 no free transverter slots. Clear two rows in "
                       "Bands \342\206\222 Transverters.");
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

// Row -> QO100_BEACON_SEL_*, since the rows are in frequency order and the
// middle beacon's value was appended rather than inserted (it is persisted).
static const gint beacon_rows[]={QO100_BEACON_SEL_LOWER,QO100_BEACON_SEL_MIDDLE,
                                 QO100_BEACON_SEL_UPPER,QO100_BEACON_SEL_WB};

static void beacon_cb(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  RADIO *r=(RADIO *)data;
  guint row=gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  r->qo100_beacon_sel=(row<G_N_ELEMENTS(beacon_rows))?beacon_rows[row]
                                                     :QO100_BEACON_SEL_DEFAULT;
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
    "The geostationary transponders, both non-inverting and both translated by the\n"
    "same 8089.500 MHz:\n"
    "\n"
    "  narrow-band   uplink 2400.000\342\200\2232400.500, downlink 10489.500\342\200\22310490.000 MHz\n"
    "  wideband      uplink 2401.000\342\200\2232410.000, downlink 10490.500\342\200\22310499.500 MHz\n"
    "\n"
    "Receive and transmit go through different converters, so the satellite needs\n"
    "two entries under Bands \342\206\222 Transverters. Give the two local oscillators\n"
    "here and they will be written for you \342\200\224 one pair covering both transponders,\n"
    "since it is one dish. VFO A then follows the receive entry and VFO B the\n"
    "transmit one.\n"
    "\n"
    "The wideband transponder carries digital television, which this application\n"
    "does not demodulate. What it gives you there is a truthful dial, the channel\n"
    "plan drawn over the spectrum, and the matching uplink on VFO B.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12);
  gtk_grid_attach(GTK_GRID(grid),info,0,row++,2,1);

  // ---- converters ----------------------------------------------------------
  GtkWidget *lnb_lbl=gtk_label_new("Downlink converter (LNB) LO (MHz):");
  gtk_widget_set_halign(lnb_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),lnb_lbl,0,row,1,1);
  GtkWidget *lnb=gtk_spin_button_new_with_range(0.0,12000.0,0.001);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(lnb),3);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(lnb),
                            (double)(r->qo100_lnb_lo!=0?r->qo100_lnb_lo:QO100_DEFAULT_LNB_LO)/1.0e6);
  gtk_widget_set_halign(lnb,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),lnb,1,row++,1,1);
  g_signal_connect(lnb,"value-changed",G_CALLBACK(lnb_lo_cb),r);

  GtkWidget *txlo_lbl=gtk_label_new("Uplink converter LO (MHz, 0 = none):");
  gtk_widget_set_halign(txlo_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),txlo_lbl,0,row,1,1);
  GtkWidget *txlo=gtk_spin_button_new_with_range(0.0,2400.0,0.001);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(txlo),3);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(txlo),(double)r->qo100_tx_lo/1.0e6);
  gtk_widget_set_halign(txlo,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),txlo,1,row++,1,1);
  g_signal_connect(txlo,"value-changed",G_CALLBACK(tx_lo_cb),r);

  GtkWidget *mk=gtk_button_new_with_label("Create the two transverter entries");
  gtk_widget_set_halign(mk,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),mk,0,row++,2,1);
  g_signal_connect(mk,"clicked",G_CALLBACK(make_xvtr_cb),r);

  qo100_xvtr_label=gtk_label_new(
    "Writes \"" QO100_XVTR_RX_TITLE "\" and \"" QO100_XVTR_TX_TITLE "\" into Bands \342\206\222 Transverters.\n"
    "Pressing it again updates those two rows rather than using more slots,\nand keeps their LO error.");
  gtk_widget_set_halign(qo100_xvtr_label,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(qo100_xvtr_label,12);
  gtk_grid_attach(GTK_GRID(grid),qo100_xvtr_label,0,row++,2,1);

  // ---- transponder ---------------------------------------------------------
  GtkWidget *tp_lbl=gtk_label_new("Transponder:");
  gtk_widget_set_halign(tp_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),tp_lbl,0,row,1,1);
  const char *tp_opts[]={"Narrow-band, 10489.500\342\200\22310490.000 MHz (SSB/CW/digi)",
                         "Wideband, 10490.500\342\200\22310499.500 MHz (DATV)",NULL};
  GtkWidget *tp_dd=gtk_drop_down_new_from_strings(tp_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(tp_dd),
    (r->qo100_transponder==QO100_TRANSPONDER_WB)?QO100_TRANSPONDER_WB:QO100_TRANSPONDER_NB);
  gtk_widget_set_halign(tp_dd,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),tp_dd,1,row++,1,1);
  g_signal_connect(tp_dd,"notify::selected",G_CALLBACK(transponder_cb),r);

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

  qo100_setup_label=gtk_label_new(
    "Creates the converters if needed, tunes to the selected transponder's\n"
    "downlink, puts VFO B on the matching uplink and links the two (SAT split).\n"
    "If you are already on that transponder your own frequency is kept.");
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
  // All four beacons. The MIDDLE one is 400 bd BPSK, i.e. a suppressed carrier
  // with no line to peak-search — the loop makes one by squaring, and it is the
  // source that cannot be one 400 Hz shift out, since a BPSK spectrum carries no
  // tone convention. The wideband beacon is DVB-S2 and the lock refuses it in
  // words: it is here for the level reference line below, where it is the right
  // choice and the only beacon within reach of the span while the operator is on
  // that transponder (see qo100_beacon_frequency()).
  // The rows are in FREQUENCY order and the selection value is not the row:
  // QO100_BEACON_SEL_MIDDLE was appended (3) so an existing props file keeps
  // meaning what it meant, and a table is cheaper than migrating everyone's
  // settings. Same shape as radio_dialog.c's audio-backend rows.
  const char *b_opts[]={"Lower, 10489.500 MHz (CW)",
                        "Middle, 10489.750 MHz (BPSK \342\200\224 no tone convention)",
                        "Upper, 10490.000 MHz (CW)",
                        "Wideband, 10491.500 MHz (DVB-S2 \342\200\224 level reference only)",NULL};
  GtkWidget *b_dd=gtk_drop_down_new_from_strings(b_opts);
  guint b_row=0;
  for(guint i=0;i<G_N_ELEMENTS(beacon_rows);i++)
    if(beacon_rows[i]==r->qo100_beacon_sel) { b_row=i; break; }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(b_dd),b_row);
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

  qo100_check_label=gtk_label_new("Middle beacon: no check yet");
  gtk_widget_set_halign(qo100_check_label,GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(qo100_check_label),TRUE);
  gtk_grid_attach(GTK_GRID(grid),qo100_check_label,0,row++,2,1);

  GtkWidget *lk_hint=gtk_label_new(
    "The correction is written into the receive band's LO error, so it survives a\n"
    "restart. The uplink converter is a different box and is never touched by it.\n"
    "The selected beacon is always the correction source. For a CW selection the\n"
    "middle BPSK beacon confirms the acquisition, but never replaces it. Select\n"
    "Middle explicitly to correct from its unambiguous BPSK centre. The CW lock\n"
    "identifies the 400 Hz F1A pair and keeps its resting line as the anchor.\n"
    "Beacon correction requires a receiver span of at least 768 kHz. It is\n"
    "disabled at 192 kHz because one isolated carrier cannot be identified safely;\n"
    "acquisition is accepted only when another NB beacon is seen 250/500 kHz away.");
  gtk_widget_set_halign(lk_hint,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),lk_hint,0,row++,2,1);

  status_refresh();
  if(qo100_poll_id!=0) g_source_remove(qo100_poll_id);
  qo100_poll_id=g_timeout_add(500,status_poll,NULL);
  g_signal_connect(frame,"destroy",G_CALLBACK(qo100_dialog_destroy),NULL);

  return frame;
}
