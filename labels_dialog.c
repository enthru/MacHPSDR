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

// Apply an entry's text to a stored label and update every widget that carries
// it live: the bottom-bar toolbar button and the check button on the Radio
// config page. Falls back to a default when the field is empty so nothing is
// ever left blank.
static void apply_label(const gchar *text, char *store, gsize store_size,
                        const char *fallback, GtkWidget *button, GtkWidget *check) {
  if(text==NULL || text[0]=='\0') text=fallback;
  g_strlcpy(store,text,store_size);
  if(button!=NULL) gtk_button_set_label(GTK_BUTTON(button),store);
  if(check!=NULL)  gtk_button_set_label(GTK_BUTTON(check),store);
}

static void att10_label_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  apply_label(gtk_entry_get_text(GTK_ENTRY(widget)),
              radio->att10_label,sizeof(radio->att10_label),"Att10",
              radio->att10_button,radio->att10_check);
}

static void att20_label_cb(GtkWidget *widget, gpointer data) {
  RADIO *radio=(RADIO *)data;
  apply_label(gtk_entry_get_text(GTK_ENTRY(widget)),
              radio->att20_label,sizeof(radio->att20_label),"Att20",
              radio->att20_button,radio->att20_check);
}

GtkWidget *create_labels_dialog(RADIO *r) {
  GtkWidget *frame=gtk_frame_new("Button Labels");
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid),5);
  gtk_grid_set_row_spacing(GTK_GRID(grid),5);
  sui_style_group(grid);
  gtk_container_add(GTK_CONTAINER(frame),grid);

  GtkWidget *info=gtk_label_new("Custom labels for the RX front-end attenuator buttons.\n"
                                "Leave a field empty to restore the default.");
  gtk_widget_set_halign(info,GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(info,12); // gap between the description and the fields below
  gtk_grid_attach(GTK_GRID(grid),info,0,0,2,1);

  GtkWidget *att10_lbl=gtk_label_new("Att10 button:");
  gtk_widget_set_halign(att10_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),att10_lbl,0,1,1,1);
  GtkWidget *att10_entry=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(att10_entry),sizeof(r->att10_label)-1);
  gtk_entry_set_text(GTK_ENTRY(att10_entry),r->att10_label);
  gtk_grid_attach(GTK_GRID(grid),att10_entry,1,1,1,1);
  g_signal_connect(att10_entry,"changed",G_CALLBACK(att10_label_cb),r);

  GtkWidget *att20_lbl=gtk_label_new("Att20 button:");
  gtk_widget_set_halign(att20_lbl,GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid),att20_lbl,0,2,1,1);
  GtkWidget *att20_entry=gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(att20_entry),sizeof(r->att20_label)-1);
  gtk_entry_set_text(GTK_ENTRY(att20_entry),r->att20_label);
  gtk_grid_attach(GTK_GRID(grid),att20_entry,1,2,1,1);
  g_signal_connect(att20_entry,"changed",G_CALLBACK(att20_label_cb),r);

  return frame;
}
