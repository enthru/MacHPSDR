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
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>

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
#include "diversity_mixer.h"
#include "diversity_dialog.h"
#include "math.h"

// The Diversity page is always present in Configure (see configure_dialog.c).
// It carries the on/off control (a checkbox — enabling diversity adds a hidden
// receiver + WDSP mixer) plus the gain/phase controls. The controls act on the
// mixer of the active receiver, which only exists while diversity is on, so the
// page looks it up dynamically and greys the controls when there is none.
typedef struct _divpage {
  RADIO *radio;
  GtkWidget *enable_b;
  GtkWidget *gain_coarse;
  GtkWidget *phase_coarse;
  GtkWidget *gain_fine;
  GtkWidget *phase_fine;
  GtkWidget *adc_combo;
  GtkWidget *calibrate_b;
  GtkWidget *flip_b;
} DIVPAGE;

// Does this device support diversity at all? It combines two coherent ADC
// streams, so it needs Protocol 1 (the only path wired for it) — the fake
// device is allowed too so the UI can be exercised — and at least two receivers.
static gboolean diversity_supported(RADIO *r) {
  return r != NULL && r->discovered != NULL &&
         (r->discovered->protocol == PROTOCOL_1 || r->discovered->protocol == PROTOCOL_FAKE) &&
         r->discovered->supported_receivers >= 2;
}

// The mixer for the active receiver, or NULL when diversity is off.
static DIVMIXER *page_dmix(DIVPAGE *dp) {
  RECEIVER *rx = dp->radio->active_receiver;
  if(rx != NULL && rx->diversity &&
     rx->dmix_id >= 0 && rx->dmix_id < MAX_DIVERSITY_MIXERS) {
    return dp->radio->divmixer[rx->dmix_id];
  }
  return NULL;
}

// Enable/disable the gain/phase controls and, when a mixer exists, load its
// current values into them.
static void div_page_sync(DIVPAGE *dp) {
  DIVMIXER *d = page_dmix(dp);
  gboolean on = (d != NULL);
  gtk_widget_set_sensitive(dp->gain_coarse, on);
  gtk_widget_set_sensitive(dp->phase_coarse, on);
  gtk_widget_set_sensitive(dp->gain_fine, on);
  gtk_widget_set_sensitive(dp->phase_fine, on);
  gtk_widget_set_sensitive(dp->adc_combo, on);
  gtk_widget_set_sensitive(dp->calibrate_b, on);
  gtk_widget_set_sensitive(dp->flip_b, on);
  gtk_range_set_value(GTK_RANGE(dp->gain_coarse),  d ? d->gain      : 0.0);
  gtk_range_set_value(GTK_RANGE(dp->phase_coarse), d ? d->phase     : 0.0);
  gtk_range_set_value(GTK_RANGE(dp->gain_fine),    d ? d->gain_fine : 0.0);
  gtk_range_set_value(GTK_RANGE(dp->phase_fine),   d ? d->phase_fine: 0.0);
  if(d != NULL) gtk_drop_down_set_selected(GTK_DROP_DOWN(dp->adc_combo), d->num_streams);
}

static void gain_coarse_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix = page_dmix((DIVPAGE *)data);
  if(dmix == NULL) return;
  dmix->gain = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void phase_coarse_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix = page_dmix((DIVPAGE *)data);
  if(dmix == NULL) return;
  dmix->phase = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void gain_fine_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix = page_dmix((DIVPAGE *)data);
  if(dmix == NULL) return;
  dmix->gain_fine = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void phase_fine_changed_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix = page_dmix((DIVPAGE *)data);
  if(dmix == NULL) return;
  dmix->phase_fine = gtk_range_get_value(GTK_RANGE(widget));
  set_gain_phase(dmix);
}

static void dmix_adc_cb(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  DIVMIXER *dmix = page_dmix((DIVPAGE *)data);
  if(dmix == NULL) return;
  dmix->num_streams = gtk_drop_down_get_selected(dd);
  SetNumStreams(dmix);
}

static void calibrate_gain_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix = page_dmix((DIVPAGE *)data);
  if(dmix == NULL) return;
  dmix->calibrate_gain = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  diversity_mix_calibrate_gain_visuals(dmix);
}

static void dir_flip_cb(GtkWidget *widget, gpointer data) {
  DIVMIXER *dmix = page_dmix((DIVPAGE *)data);
  if(dmix == NULL) return;
  dmix->flip = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  if(dmix->flip) {
    dmix->phase += 180;
  } else {
    dmix->phase -= 180;
  }
  set_gain_phase(dmix);
}

// The on/off control: enabling diversity adds a hidden receiver and creates the
// WDSP mixer for the active receiver; disabling tears them down. (This is what
// the old VFO "DIV" button did.)
static void enable_cb(GtkWidget *widget, gpointer data) {
  DIVPAGE *dp = (DIVPAGE *)data;
  RADIO *r = dp->radio;
  RECEIVER *rx = r->active_receiver;
  if(rx == NULL) return;
  gboolean want = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));

  if(want && !rx->diversity) {
    int hidden = add_receiver(r, FALSE);
    if(hidden > 0) {
      int m = add_diversity_mixer(r, rx, r->receiver[hidden]);
      if(m > -1) rx->diversity = TRUE;
    }
  } else if(!want && rx->diversity) {
    if(rx->dmix_id >= 0 && rx->dmix_id < MAX_DIVERSITY_MIXERS &&
       r->divmixer[rx->dmix_id] != NULL) {
      delete_diversity_mixer(r->divmixer[rx->dmix_id]);
    }
    rx->diversity = FALSE;
  }

  // Enabling can fail (no free receiver slot) — reflect the real outcome.
  if(gtk_check_button_get_active(GTK_CHECK_BUTTON(widget)) != rx->diversity) {
    g_signal_handlers_block_by_func(widget, G_CALLBACK(enable_cb), dp);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widget), rx->diversity);
    g_signal_handlers_unblock_by_func(widget, G_CALLBACK(enable_cb), dp);
  }
  div_page_sync(dp);
}

GtkWidget *create_diversity_dialog(RADIO *radio) {
  DIVPAGE *dp = g_new0(DIVPAGE, 1);
  dp->radio = radio;

  GtkWidget *grid = gtk_grid_new();
  sui_style_page(grid);
  gtk_grid_set_row_homogeneous(GTK_GRID(grid), FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);

  // Row 0: the on/off checkbox (greyed out when the device can't do diversity).
  RECEIVER *arx = radio->active_receiver;
  dp->enable_b = gtk_check_button_new_with_label("Enable diversity");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(dp->enable_b),
                              arx != NULL && arx->diversity);
  gtk_widget_set_sensitive(dp->enable_b, diversity_supported(radio));
  gtk_grid_attach(GTK_GRID(grid), dp->enable_b, 0, 0, 2, 1);
  g_signal_connect(dp->enable_b, "toggled", G_CALLBACK(enable_cb), dp);

  GtkWidget *gain_coarse_label = gtk_label_new("Gain (dB, coarse):");
  gtk_label_set_xalign(GTK_LABEL(gain_coarse_label), 0.0);
  gtk_grid_attach(GTK_GRID(grid), gain_coarse_label, 0, 1, 1, 1);

  dp->gain_coarse = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -25.0, +25.0, 0.5);
  gtk_widget_set_size_request(dp->gain_coarse, 300, 25);
  sui_scale_show_value(dp->gain_coarse, 1);
  gtk_grid_attach(GTK_GRID(grid), dp->gain_coarse, 1, 1, 1, 1);
  g_signal_connect(G_OBJECT(dp->gain_coarse), "value_changed", G_CALLBACK(gain_coarse_changed_cb), dp);

  GtkWidget *phase_coarse_label = gtk_label_new("Phase (coarse):");
  gtk_label_set_xalign(GTK_LABEL(phase_coarse_label), 0.0);
  gtk_grid_attach(GTK_GRID(grid), phase_coarse_label, 2, 1, 1, 1);

  dp->phase_coarse = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -180.0, 180.0, 1.0);
  gtk_widget_set_size_request(dp->phase_coarse, 300, 25);
  sui_scale_show_value(dp->phase_coarse, 0);
  gtk_grid_attach(GTK_GRID(grid), dp->phase_coarse, 3, 1, 1, 1);
  g_signal_connect(G_OBJECT(dp->phase_coarse), "value_changed", G_CALLBACK(phase_coarse_changed_cb), dp);

  GtkWidget *gain_fine_label = gtk_label_new("Gain (dB, fine):");
  gtk_label_set_xalign(GTK_LABEL(gain_fine_label), 0.0);
  gtk_grid_attach(GTK_GRID(grid), gain_fine_label, 0, 2, 1, 1);

  dp->gain_fine = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -2.0, +2.0, 0.05);
  gtk_widget_set_size_request(dp->gain_fine, 300, 25);
  sui_scale_show_value(dp->gain_fine, 2);
  gtk_grid_attach(GTK_GRID(grid), dp->gain_fine, 1, 2, 1, 1);
  g_signal_connect(G_OBJECT(dp->gain_fine), "value_changed", G_CALLBACK(gain_fine_changed_cb), dp);

  GtkWidget *phase_fine_label = gtk_label_new("Phase (fine):");
  gtk_label_set_xalign(GTK_LABEL(phase_fine_label), 0.0);
  gtk_grid_attach(GTK_GRID(grid), phase_fine_label, 2, 2, 1, 1);

  dp->phase_fine = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -2.0, 2.0, 0.05);
  gtk_widget_set_size_request(dp->phase_fine, 300, 25);
  sui_scale_show_value(dp->phase_fine, 2);
  gtk_grid_attach(GTK_GRID(grid), dp->phase_fine, 3, 2, 1, 1);
  g_signal_connect(G_OBJECT(dp->phase_fine), "value_changed", G_CALLBACK(phase_fine_changed_cb), dp);

  // ADC1, ADC2 or ADC1+ADC2 (diversity mode)
  const char *dmix_adc_opts[] = {"ADC1", "ADC2", "ADC1+ADC2", NULL};
  dp->adc_combo = gtk_drop_down_new_from_strings(dmix_adc_opts);
  gtk_grid_attach(GTK_GRID(grid), dp->adc_combo, 0, 3, 2, 1);
  g_signal_connect(dp->adc_combo, "notify::selected", G_CALLBACK(dmix_adc_cb), dp);

  GtkWidget *calibrate_gain_label = gtk_label_new("Calibrate gain:");
  gtk_label_set_xalign(GTK_LABEL(calibrate_gain_label), 0.0);
  gtk_grid_attach(GTK_GRID(grid), calibrate_gain_label, 0, 4, 1, 1);

  dp->calibrate_b = gtk_check_button_new();
  gtk_grid_attach(GTK_GRID(grid), dp->calibrate_b, 1, 4, 1, 1);
  g_signal_connect(dp->calibrate_b, "toggled", G_CALLBACK(calibrate_gain_cb), dp);

  // 180 deg flip on phase
  GtkWidget *dir_flip_label = gtk_label_new("Flip:");
  gtk_label_set_xalign(GTK_LABEL(dir_flip_label), 0.0);
  gtk_grid_attach(GTK_GRID(grid), dir_flip_label, 0, 5, 1, 1);

  dp->flip_b = gtk_check_button_new();
  gtk_grid_attach(GTK_GRID(grid), dp->flip_b, 1, 5, 1, 1);
  g_signal_connect(dp->flip_b, "toggled", G_CALLBACK(dir_flip_cb), dp);

  // Honest disclaimer (mirrors the PureSignal note): the diversity path needs
  // two coherent ADC streams and has not been verified on hardware in this fork.
  GtkWidget *note = gtk_label_new(NULL);
  if(diversity_supported(radio)) {
    gtk_label_set_markup(GTK_LABEL(note),
      "<small><i>Note: diversity reception combines two coherent ADC streams to null "
      "interference / fight fading. Like PureSignal it is Protocol 1 only and has NOT "
      "been verified on hardware in this fork. Requires testing on real hardware; use at your own risk.</i></small>");
  } else {
    gtk_label_set_markup(GTK_LABEL(note),
      "<small><i>This device does not support diversity reception (it needs Protocol 1 "
      "and two receivers/ADCs), so it cannot be enabled here.</i></small>");
  }
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_label_set_xalign(GTK_LABEL(note), 0.0);
  gtk_widget_set_size_request(note, 600, -1);
  gtk_grid_attach(GTK_GRID(grid), note, 0, 6, 4, 1);

  // Load current state into the controls (and grey them if diversity is off).
  div_page_sync(dp);

  // The page owns the DIVPAGE; free it when the page widget is destroyed.
  g_object_set_data_full(G_OBJECT(grid), "divpage", dp, g_free);

  return grid;
}
