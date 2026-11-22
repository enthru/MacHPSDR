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

// Row widgets, indexed by band. NULL for a slot that has no row on screen
// (an empty slot is not shown) and for every slot while the page is closed.
static GtkWidget *title[BANDS+XVTRS];
static GtkWidget *min_frequency[BANDS+XVTRS];
static GtkWidget *max_frequency[BANDS+XVTRS];
static GtkWidget *lo_frequency[BANDS+XVTRS];
static GtkWidget *lo_error[BANDS+XVTRS];
static GtkWidget *disable_pa[BANDS+XVTRS];
static GtkWidget *pa_calibration[BANDS+XVTRS];
static GtkWidget *smeter_cal[BANDS+XVTRS];

// The page: the slots are a fixed table of XVTRS, but only the ones in use are
// shown, and "Add transverter" opens the next free one.  A slot is in use when
// it has a title -- or when it was added this session and is still being named
// (shown[]), so a row does not vanish under the operator's cursor while the
// title is empty.  Rows are rebuilt as a whole on add/delete.
static GtkWidget *xvtr_page=NULL;
static GtkWidget *xvtr_table=NULL;
static GtkWidget *xvtr_status=NULL;
static gboolean shown[XVTRS];
static guint rebuild_idle=0;

static void xvtr_table_build(void);

// What the page's edits leave behind when the dialog closes.  Every field is
// written live by its row's handler, so this only turns the result into bands:
// a slot whose title is empty is no transverter and is zeroed, and a named one
// gets its band stack laid out across its range.
void save_xvtr (void) {
  int i;
  int b;
  for(i=BANDS;i<BANDS+XVTRS;i++) {
    BAND *xvtr=band_get_band(i);
    BANDSTACK *bandstack=xvtr->bandstack;
    if(strlen(xvtr->title)!=0) {
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
    } else {
      xvtr->frequencyMin=0;
      xvtr->frequencyMax=0;
      xvtr->frequencyLO=0;
      xvtr->errorLO=0;
      xvtr->disablePA=0;
    }
  }
}

static gboolean rebuild_idle_cb(gpointer data) {
  rebuild_idle=0;
  if(xvtr_page==NULL) return G_SOURCE_REMOVE;
  if(xvtr_table!=NULL) gtk_box_remove(GTK_BOX(xvtr_page),xvtr_table);
  xvtr_table=NULL;
  xvtr_table_build();
  return G_SOURCE_REMOVE;
}

// Deferred: the request usually comes from a button INSIDE the table, and the
// table is what gets destroyed.
static void xvtr_rebuild(void) {
  if(xvtr_page==NULL || rebuild_idle!=0) return;
  rebuild_idle=g_idle_add(rebuild_idle_cb,NULL);
}

static void xvtr_set_status(const char *text) {
  if(xvtr_status==NULL) return;
  gtk_label_set_text(GTK_LABEL(xvtr_status),text);
  gtk_widget_set_visible(xvtr_status,text[0]!='\0');
}

void xvtr_dialog_refresh_row(int band) {
  if(radio==NULL || radio->dialog==NULL) return;      // widgets only exist while open
  if(xvtr_page==NULL) return;
  if(band<BANDS || band>=BANDS+XVTRS) return;
  BAND *xvtr=band_get_band(band);
  if(xvtr==NULL) return;
  if(title[band]==NULL) {
    // A slot something else just filled (the QO-100 page) gets its row.
    if(xvtr->title[0]!='\0') {
      shown[band-BANDS]=TRUE;
      xvtr_rebuild();
    }
    return;
  }
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

  if(radio->dialog!=NULL && lo_frequency[band]!=NULL) {
    char temp[32];
    g_ascii_formatd(temp,sizeof(temp),"%.6f",(double)new_lo/1000000.0);
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
  if(radio->dialog!=NULL && lo_error[band]!=NULL) {
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
    if(lo_frequency[i]==NULL) continue;     // no row for an unused slot
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

static void title_cb(GtkEditable *editable,gpointer user_data) {
  int band=GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  g_strlcpy(xvtr->title,gtk_editable_get_text(editable),sizeof(xvtr->title));
}

static void smeter_cal_cb(GtkSpinButton *spin,gpointer user_data) {
  int slot=GPOINTER_TO_INT(user_data)-BANDS;
  if(slot<0 || slot>=XVTRS) return;
  radio->smeter_cal.xvtr[slot]=smeter_cal_clamp(gtk_spin_button_get_value(spin));
}

// The receivers listening through this transverter, for the refusal below.
static int receivers_on_band(int band,char *names,size_t namesz) {
  int n=0;
  names[0]='\0';
  for(int i=0;i<MAX_RECEIVERS;i++) {
    RECEIVER *rx=radio->receiver[i];
    if(rx==NULL || !rx->show_rx) continue;
    if(rx->band_a!=band) continue;
    char one[16];
    snprintf(one,sizeof(one),"%sRX%d",n?", ":"",i+1);
    g_strlcat(names,one,namesz);
    n++;
  }
  return n;
}

static void delete_cb(GtkButton *button,gpointer user_data) {
  int band=GPOINTER_TO_INT(user_data);
  BAND *xvtr=band_get_band(band);
  char names[128];
  // Deleting the band a receiver is tuned through would leave it on a nameless
  // slot with no LO -- the radio commanded to the RF of a converter that is no
  // longer accounted for.  Say who is there instead.
  if(receivers_on_band(band,names,sizeof(names))>0) {
    char msg[256];
    snprintf(msg,sizeof(msg),"%s is on %s — change band before deleting it",
             names,xvtr->title[0]?xvtr->title:"this transverter");
    xvtr_set_status(msg);
    return;
  }
  xvtr->title[0]='\0';
  xvtr->frequencyMin=0;
  xvtr->frequencyMax=0;
  xvtr->frequencyLO=0;
  xvtr->errorLO=0;
  xvtr->disablePA=0;
  radio->smeter_cal.xvtr[band-BANDS]=radio_smeter_cal_default();
  shown[band-BANDS]=FALSE;
  update_receiver(band);
  xvtr_set_status("");
  xvtr_rebuild();
}

static void add_cb(GtkButton *button,gpointer user_data) {
  for(int i=0;i<XVTRS;i++) {
    BAND *xvtr=band_get_band(BANDS+i);
    if(xvtr->title[0]=='\0' && !shown[i]) {
      // A slot left empty may still hold numbers from a hand-edited file or a
      // title cleared without closing the dialog; a new transverter starts
      // clean, and with the device's uncalibrated S-meter offset.
      xvtr->frequencyMin=0;
      xvtr->frequencyMax=0;
      xvtr->frequencyLO=0;
      xvtr->errorLO=0;
      xvtr->disablePA=0;
      radio->smeter_cal.xvtr[i]=radio_smeter_cal_default();
      shown[i]=TRUE;
      xvtr_set_status("");
      xvtr_rebuild();
      return;
    }
  }
  xvtr_set_status("All transverter slots are in use");
}

static void xvtr_table_build(void) {
  int row=0;
  int col=0;
  char temp[32];

  for(int i=BANDS;i<BANDS+XVTRS;i++) {
    title[i]=min_frequency[i]=max_frequency[i]=lo_frequency[i]=NULL;
    lo_error[i]=disable_pa[i]=pa_calibration[i]=smeter_cal[i]=NULL;
  }

  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid),10);
  gtk_grid_set_row_spacing(GTK_GRID(grid),4);
  xvtr_table=grid;

  int used=0;
  for(int i=0;i<XVTRS;i++) if(shown[i]) used++;

  if(used>0) {
    const char *heads[]={"Title","Min Frequency(MHz)","Max Frequency(MHz)",
                         "LO Frequency(MHz)","LO Error(Hz)","S-Meter (dB)","Disable PA"};
    for(size_t h=0;h<G_N_ELEMENTS(heads);h++) {
      GtkWidget *label=gtk_label_new(heads[h]);
      if(h==5) gtk_widget_set_tooltip_text(label,
          "Added to the S-meter reading while a receiver is on this transverter's band "
          "(the converter's gain and loss). HF has its own offsets on the Radio page.");
      gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
    }
    if (radio->hl2 != NULL) {
      GtkWidget *label=gtk_label_new("PA Calibration");
      gtk_grid_attach(GTK_GRID(grid),label,col++,row,1,1);
    }
    row++;
  } else {
    GtkWidget *label=gtk_label_new("No transverters. Add one to give the band menu "
                                   "a band that is received through a converter.");
    gtk_widget_set_halign(label,GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid),label,0,row++,1,1);
  }

  for(int i=BANDS;i<BANDS+XVTRS;i++) {
    if(!shown[i-BANDS]) continue;
    BAND *xvtr=band_get_band(i);
    col=0;

    title[i]=gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(title[i]),7);
    gtk_entry_set_max_length(GTK_ENTRY(title[i]),sizeof(xvtr->title)-1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(title[i]),"Name");
    gtk_widget_set_tooltip_text(title[i],
        "The band's name in the band menu. A transverter with no name is not kept.");
    gtk_editable_set_text(GTK_EDITABLE(title[i]),xvtr->title);
    gtk_grid_attach(GTK_GRID(grid),title[i],col++,row,1,1);
    g_signal_connect(title[i],"changed",G_CALLBACK(title_cb),GINT_TO_POINTER(i));

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
    if(radio->hl2!=NULL && radio->hl2->cl2_integer_mode) {
      gtk_widget_set_sensitive(lo_frequency[i],FALSE);
    }

    lo_error[i]=gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(lo_error[i]),9);
    sprintf(temp,"%lld",xvtr->errorLO);
    gtk_editable_set_text(GTK_EDITABLE(lo_error[i]),temp);
    gtk_grid_attach(GTK_GRID(grid),lo_error[i],col++,row,1,1);
    g_signal_connect(lo_error[i],"changed",G_CALLBACK(lo_error_cb),GINT_TO_POINTER(i));

    smeter_cal[i]=gtk_spin_button_new_with_range(SMETER_CAL_MIN,SMETER_CAL_MAX,1.0);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(smeter_cal[i]),1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(smeter_cal[i]),radio->smeter_cal.xvtr[i-BANDS]);
    gtk_grid_attach(GTK_GRID(grid),smeter_cal[i],col++,row,1,1);
    g_signal_connect(smeter_cal[i],"value-changed",G_CALLBACK(smeter_cal_cb),GINT_TO_POINTER(i));

    disable_pa[i]=gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(disable_pa[i]),xvtr->disablePA);
    gtk_grid_attach(GTK_GRID(grid),disable_pa[i],col++,row,1,1);
    g_signal_connect(disable_pa[i], "toggled", G_CALLBACK(xvtr_pa_disable_changed_cb), GINT_TO_POINTER(i));

    if (radio->hl2 != NULL) {
      pa_calibration[i] = gtk_spin_button_new_with_range(38.8,100.0,0.1);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(pa_calibration[i]),(double)xvtr->pa_calibration);
      gtk_grid_attach(GTK_GRID(grid),pa_calibration[i],col++,row,1,1);
      g_signal_connect(pa_calibration[i],"changed",G_CALLBACK(pa_xvtr_value_changed_cb),GINT_TO_POINTER(i));

      if (radio->hl2->cl2_integer_mode) {
        gtk_widget_set_sensitive(disable_pa[i], FALSE);
      }
    }

    GtkWidget *del=gtk_button_new_from_icon_name("list-remove-symbolic");
    gtk_widget_set_tooltip_text(del,"Delete this transverter");
    gtk_grid_attach(GTK_GRID(grid),del,col++,row,1,1);
    g_signal_connect(del,"clicked",G_CALLBACK(delete_cb),GINT_TO_POINTER(i));

    row++;
  }

  GtkWidget *add=gtk_button_new_with_label("Add transverter");
  gtk_widget_set_halign(add,GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(add,"Open a new transverter row (up to 8)");
  gtk_grid_attach(GTK_GRID(grid),add,0,row,2,1);
  g_signal_connect(add,"clicked",G_CALLBACK(add_cb),NULL);
  // A full table hides the button rather than greying it: a tooltip on an
  // insensitive widget never appears, so it could not say why.
  gtk_widget_set_visible(add,used<XVTRS);

  // Before the status line, so the table keeps its place in the page.
  gtk_box_insert_child_after(GTK_BOX(xvtr_page),grid,
                             gtk_widget_get_prev_sibling(xvtr_status));
}

static void page_destroy_cb(GtkWidget *widget,gpointer data) {
  xvtr_page=NULL;
  xvtr_table=NULL;
  xvtr_status=NULL;
  if(rebuild_idle!=0) {
    g_source_remove(rebuild_idle);
    rebuild_idle=0;
  }
  for(int i=BANDS;i<BANDS+XVTRS;i++) {
    title[i]=min_frequency[i]=max_frequency[i]=lo_frequency[i]=NULL;
    lo_error[i]=disable_pa[i]=pa_calibration[i]=smeter_cal[i]=NULL;
  }
}

GtkWidget *create_xvtr_dialog(RADIO *radio) {
  GtkWidget *page=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
  sui_style_page(page);
  xvtr_page=page;
  g_signal_connect(page,"destroy",G_CALLBACK(page_destroy_cb),NULL);

  if (radio->hl2 != NULL) {
    GtkWidget *hbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    GtkWidget *cl2_label = gtk_label_new("Use CL2 in integer mode");
    gtk_box_append(GTK_BOX(hbox),cl2_label);
    GtkWidget *cl2 = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cl2), radio->hl2->cl2_integer_mode);
    gtk_box_append(GTK_BOX(hbox),cl2);
    g_signal_connect(cl2, "toggled", G_CALLBACK(cl2_mode_cb), radio->hl2);
    gtk_box_append(GTK_BOX(page),hbox);
  }

  // Shown: what has a name.  Every other slot is free for "Add transverter".
  for(int i=0;i<XVTRS;i++) shown[i]=(band_get_band(BANDS+i)->title[0]!='\0');

  xvtr_status=gtk_label_new("");
  gtk_widget_set_halign(xvtr_status,GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(xvtr_status),TRUE);
  gtk_box_append(GTK_BOX(page),xvtr_status);
  gtk_widget_set_visible(xvtr_status,FALSE);

  xvtr_table=NULL;
  xvtr_table_build();

  return page;
}
