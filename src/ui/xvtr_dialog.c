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
#include <semaphore.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "band.h"
// A transverter's edges and LO are typed in MHz with a decimal point, and this
// dialog used sprintf("%f") to show them and atof() to read them back -- both of
// which follow LC_NUMERIC. GTK calls setlocale(LC_ALL,"") at start-up, so on a
// comma-decimal machine the entry displayed "144,500000" and an operator typing
// the dot form got atof() stopping at the '.': 144.5 MHz silently became 144, a
// 500 kHz error in the one number that has to agree with the hardware or the
// radio is commanded to a nonsense IF with no clue why. Written with
// g_ascii_formatd (always the dot, so it matches the props file), read with
// propToDouble (which accepts either spelling) -- the pair property.c already
// uses, and the rule CLAUDE.md states for every ASCII format this tree parses.
#include "property.h"
#include "bandstack.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "settings_ui.h"
#include "vfo.h"
#include "main.h"
#include "protocol1.h"
#include "protocol2.h"
#include "tx_panadapter.h"
#include "xvtr_dialog.h"

static GtkWidget *title[BANDS+XVTRS];
static GtkWidget *min_frequency[BANDS+XVTRS];
static GtkWidget *max_frequency[BANDS+XVTRS];
static GtkWidget *lo_frequency[BANDS+XVTRS];
static GtkWidget *lo_error[BANDS+XVTRS];
static GtkWidget *disable_pa[BANDS+XVTRS];
static GtkWidget *pa_calibration[BANDS+XVTRS];

void save_xvtr (void) {
  int i;
  int b;
  const char *t;
  const char *minf;
  const char *maxf;
  const char *lof;
  const char *loerr;
  const char *txlof;
  const char *txloerr;
  for(i=BANDS;i<BANDS+XVTRS;i++) {
    BAND *xvtr=band_get_band(i);
    BANDSTACK *bandstack=xvtr->bandstack;
    t=gtk_editable_get_text(GTK_EDITABLE(title[i]));
    g_strlcpy(xvtr->title,t,sizeof(xvtr->title));
    if(strlen(t)!=0) {
      minf=gtk_editable_get_text(GTK_EDITABLE(min_frequency[i]));
      xvtr->frequencyMin=(long long)(propToDouble(minf)*1000000.0);
      maxf=gtk_editable_get_text(GTK_EDITABLE(max_frequency[i]));
      xvtr->frequencyMax=(long long)(propToDouble(maxf)*1000000.0);
      lof=gtk_editable_get_text(GTK_EDITABLE(lo_frequency[i]));
      xvtr->frequencyLO=(long long)(propToDouble(lof)*1000000.0);
      loerr=gtk_editable_get_text(GTK_EDITABLE(lo_error[i]));
      xvtr->errorLO=atoll(loerr);
      xvtr->disablePA=gtk_check_button_get_active(GTK_CHECK_BUTTON(disable_pa[i]));
      for(b=0;b<bandstack->entries;b++) {
        BANDSTACK_ENTRY *entry=&bandstack->entry[b];
	switch(b) {
          case 0:
            entry->frequency=xvtr->frequencyMin;
	    break;
          case 1:
            entry->frequency=xvtr->frequencyMin + ((xvtr->frequencyMax-xvtr->frequencyMin)/2LL);
	    break;
          case 2:
            entry->frequency=xvtr->frequencyMax;
	    break;
        }
        entry->mode=USB;
        entry->filter=F6;
      }
//g_print("min=%s:%lld max=%s:%lld lo=%s:%lld\n",minf,xvtr->frequencyMin,maxf,xvtr->frequencyMax,lof,xvtr->frequencyLO);
    } else {
      xvtr->frequencyMin=0;
      xvtr->frequencyMax=0;
      xvtr->frequencyLO=0;
      xvtr->errorLO=0;
      xvtr->disablePA=0;
    }
  }
}

void xvtr_dialog_refresh_row(int band) {
  if(radio==NULL || radio->dialog==NULL) return;      // widgets only exist while open
  if(band<BANDS || band>=BANDS+XVTRS) return;
  if(title[band]==NULL) return;
  BAND *xvtr=band_get_band(band);
  if(xvtr==NULL) return;
  char temp[32];
  gtk_editable_set_text(GTK_EDITABLE(title[band]),xvtr->title);
  g_ascii_formatd(temp,sizeof(temp),"%f",(double)xvtr->frequencyMin/1000000.0);
  gtk_editable_set_text(GTK_EDITABLE(min_frequency[band]),temp);
  g_ascii_formatd(temp,sizeof(temp),"%f",(double)xvtr->frequencyMax/1000000.0);
  gtk_editable_set_text(GTK_EDITABLE(max_frequency[band]),temp);
  g_ascii_formatd(temp,sizeof(temp),"%f",(double)xvtr->frequencyLO/1000000.0);
  gtk_editable_set_text(GTK_EDITABLE(lo_frequency[band]),temp);
  sprintf(temp,"%lld",(long long)xvtr->errorLO);
  gtk_editable_set_text(GTK_EDITABLE(lo_error[band]),temp);
}

void update_receiver(int band) {
  int i;
  RECEIVER *rx;
  for(i=0;i<MAX_RECEIVERS;i++) {
    rx=radio->receiver[i];
    if(rx!=NULL) {
      if(rx->band_a==band) {
        BAND *xvtr=band_get_band(band);
        rx->lo_a=xvtr->frequencyLO;
        rx->error_a=xvtr->errorLO;
        // This used to switch ctun off around the call and back on afterwards,
        // because frequency_changed() only retuned the hardware in its non-ctun
        // branch -- and paid for it by clearing the tuning shift, so an operator
        // who edited an LO under ctun was left listening at the span centre
        // instead of at their cursor. The branch now follows the commanded
        // frequency, which is exactly what an LO edit moves.
        frequency_changed(rx);

        if(radio->transmitter!=NULL) {
          if(radio->transmitter->rx==rx) {
            update_tx_panadapter(radio);
          }
        }
      } else if(rx->band_b==band) {
        BAND *xvtr=band_get_band(band);
        rx->lo_b=xvtr->frequencyLO;
        rx->error_b=xvtr->errorLO;
        if(radio->transmitter!=NULL) {
          if(radio->transmitter->rx==rx) {
            update_tx_panadapter(radio);
          }
        }
      }
    }
  }
}

void min_frequency_cb(GtkEditable *editable,gpointer user_data) {
  int band=GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  const char* minf=gtk_editable_get_text(GTK_EDITABLE(min_frequency[band]));
  xvtr->frequencyMin=(long long)(propToDouble(minf)*1000000.0);
  update_receiver(band);
}

void max_frequency_cb(GtkEditable *editable,gpointer user_data) {
  int band=GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  const char* maxf=gtk_editable_get_text(GTK_EDITABLE(max_frequency[band]));
  // frequencyMax.  This wrote frequencyMin -- on every keystroke, since the
  // handler is on "changed" -- so typing the upper band edge overwrote the
  // LOWER one with each partial value and the maximum never reached the live
  // band at all.
  xvtr->frequencyMax=(long long)(propToDouble(maxf)*1000000.0);
  update_receiver(band);
}

void lo_frequency_cb(GtkEditable *editable,gpointer user_data) {
  int band=GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  const char* lof=gtk_editable_get_text(GTK_EDITABLE(lo_frequency[band]));
  xvtr->frequencyLO=(long long)(propToDouble(lof)*1000000.0);
  update_receiver(band);
}

void update_lo_frequency(int band, long long new_lo) {
  BAND *xvtr=band_get_band(band);

  if(radio->dialog!=NULL) {
    char temp[32];
    sprintf(temp,"%.6f", (double)(new_lo / 1000000.0));
    gtk_editable_set_text(GTK_EDITABLE(lo_frequency[band]), temp);
  }
}

void lo_error_cb(GtkEditable *editable,gpointer user_data) {
  int band=GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  const char* errorf=gtk_editable_get_text(GTK_EDITABLE(lo_error[band]));
  xvtr->errorLO=atoll(errorf);
  update_receiver(band);
}

void lo_error_update(int band,long long offset) {
  BAND *xvtr=band_get_band(band);
  xvtr->errorLO=xvtr->errorLO+offset;
  update_receiver(band);
  if(radio->dialog!=NULL) {
    char temp[32];
    sprintf(temp,"%lld",xvtr->errorLO);
    gtk_editable_set_text(GTK_EDITABLE(lo_error[band]),temp);
  }
}

static void pa_xvtr_value_changed_cb(GtkWidget *widget, gpointer user_data) {
  int band = GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  xvtr->pa_calibration = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  update_receiver(band);    
}
static void xvtr_pa_disable_changed_cb(GtkWidget *widget, gpointer user_data) {
  int band = GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  xvtr->disablePA = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
  update_receiver(band);    
}

// HL2 clock 2 output can be used for transverters
// However, testing has shown that if the Versaclock IC sets the 
// frequency NOT in integer mode, spurs occur. This allows the option
// for MacHPSDR to calculate the LO frequency based in an integer divisor
// for the versaclock PLL.
static void cl2_mode_cb(GtkWidget *widget, gpointer data) {
  HERMESLITE2 *hl2 = (HERMESLITE2 *)data;
  hl2->cl2_integer_mode = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
 
  // Update display and disable input of LO freq while in 
  // integer mode for CL2
  for(int i= BANDS; i < BANDS+XVTRS; i++) {
    gtk_widget_set_sensitive(lo_frequency[i], !hl2->cl2_integer_mode);
    BAND *xvtr=band_get_band(i);

    char temp[32];
    long long new_lo_freq = 0;
    if ((xvtr->frequencyLO > 0) && (hl2->cl2_integer_mode)) {
      // Calculate nearest LO value for integer divisor
      new_lo_freq = HL2cl2CalculateNearest(hl2, xvtr->frequencyLO);
      update_lo_frequency(i, new_lo_freq);
    }
  }
}

GtkWidget *create_xvtr_dialog(RADIO *radio) {
  int row;
  int col;
  int i;
  char temp[32];

  GtkWidget *grid=gtk_grid_new();
  sui_style_page(grid);
  gtk_grid_set_column_spacing (GTK_GRID(grid),10);

  row=0;
  col=0;

  if (radio->hl2 != NULL) {
    GtkWidget *cl2_label = gtk_label_new("Use CL2 in integer mode");
    gtk_grid_attach(GTK_GRID(grid), cl2_label, col++, row, 1, 1);
    GtkWidget *cl2 = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cl2), radio->hl2->cl2_integer_mode);
    gtk_grid_attach(GTK_GRID(grid), cl2, col++, row, 1, 1);
    g_signal_connect(cl2, "toggled", G_CALLBACK(cl2_mode_cb), radio->hl2);
  }

  row++;
  col=0;

  GtkWidget *label=gtk_label_new("Title");
  gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
  label=gtk_label_new("Min Frequency(MHz)");
  gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
  label=gtk_label_new("Max Frequency(MHz)");
  gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
  label=gtk_label_new("LO Frequency(MHz)");
  gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
  label=gtk_label_new("LO Error(Hz)");
  gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
  label=gtk_label_new("Disable PA");
  gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);

  if (radio->hl2 != NULL) {
    label=gtk_label_new("PA Calibration");
    gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
  }

  row++;
  col=0;

  for(i=BANDS;i<BANDS+XVTRS;i++) {
    BAND *xvtr=band_get_band(i);

    title[i]=gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(title[i]),7);
    gtk_editable_set_text(GTK_EDITABLE(title[i]),xvtr->title);
    gtk_grid_attach(GTK_GRID(grid),title[i],col++,row,1,1);

    min_frequency[i]=gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(min_frequency[i]),7);
    g_ascii_formatd(temp,sizeof(temp),"%f",(double)xvtr->frequencyMin/1000000.0);
    gtk_editable_set_text(GTK_EDITABLE(min_frequency[i]),temp);
    gtk_grid_attach(GTK_GRID(grid),min_frequency[i],col++,row,1,1);
    g_signal_connect(min_frequency[i],"changed",G_CALLBACK(min_frequency_cb),GINT_TO_POINTER(i));

    max_frequency[i]=gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(max_frequency[i]),7);
    g_ascii_formatd(temp,sizeof(temp),"%f",(double)xvtr->frequencyMax/1000000.0);
    gtk_editable_set_text(GTK_EDITABLE(max_frequency[i]),temp);
    gtk_grid_attach(GTK_GRID(grid),max_frequency[i],col++,row,1,1);
    g_signal_connect(max_frequency[i],"changed",G_CALLBACK(max_frequency_cb),GINT_TO_POINTER(i));

    lo_frequency[i]=gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(lo_frequency[i]),7);
    g_ascii_formatd(temp,sizeof(temp),"%f",(double)xvtr->frequencyLO/1000000.0);
    gtk_editable_set_text(GTK_EDITABLE(lo_frequency[i]),temp);
    gtk_grid_attach(GTK_GRID(grid),lo_frequency[i],col++,row,1,1);
    g_signal_connect(lo_frequency[i],"changed",G_CALLBACK(lo_frequency_cb),GINT_TO_POINTER(i));

    lo_error[i]=gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(lo_error[i]),9);
    sprintf(temp,"%lld",xvtr->errorLO);
    gtk_editable_set_text(GTK_EDITABLE(lo_error[i]),temp);
    gtk_grid_attach(GTK_GRID(grid),lo_error[i],col++,row,1,1);
    g_signal_connect(lo_error[i],"changed",G_CALLBACK(lo_error_cb),GINT_TO_POINTER(i));

    disable_pa[i]=gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(disable_pa[i]),xvtr->disablePA);
    gtk_grid_attach(GTK_GRID(grid),disable_pa[i],col++,row,1,1);
    g_signal_connect(disable_pa[i], "toggled", G_CALLBACK(xvtr_pa_disable_changed_cb), GINT_TO_POINTER(i));

    if (radio->hl2 != NULL) {
      pa_calibration[i] = gtk_spin_button_new_with_range(38.8,100.0,0.1);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(pa_calibration[i]),(double)xvtr->pa_calibration);
      gtk_widget_set_visible(pa_calibration[i], TRUE);    
      gtk_grid_attach(GTK_GRID(grid),pa_calibration[i],col++,row,1,1);
      g_signal_connect(pa_calibration[i],"changed",G_CALLBACK(pa_xvtr_value_changed_cb),GINT_TO_POINTER(i));

      if (radio->hl2->cl2_integer_mode) {
        log_info("Set disable pa false\n");
        gtk_widget_set_sensitive(disable_pa[i], FALSE);
      }
    }

    row++;
    col=0;

  }
  
  gtk_widget_set_visible(grid, TRUE);

  return grid;

}

