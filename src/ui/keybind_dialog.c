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

#include "log.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "keybind.h"
#include "settings_ui.h"
#include "keybind_dialog.h"

#define UNBOUND_TEXT "\xe2\x80\x94"          /* an em dash: "nothing bound" */
#define LEARN_TEXT   "Press a key\xe2\x80\xa6"

/* The Configure dialog is a singleton and this page is built once per open, so
   one set of statics is enough -- but every one of them is cleared by the page's
   own "destroy" (below), or a later open would set labels on finalised buttons
   and the capture controller would outlive the window it was added to. */
static GtkWidget *accel_button[KB_ACTIONS];
static GtkWidget *status_label;
static int learning=-1;                       /* row waiting for a key, or -1 */
static GtkEventController *learn_controller;  /* live only while learning */
static GtkWidget *learn_widget;               /* what it was added to */

static void set_status(const char *text) {
  if(status_label!=NULL) gtk_label_set_text(GTK_LABEL(status_label),text!=NULL?text:"");
}

/* Redraw every row from the store. A capture can clear a DIFFERENT row (one
   combination, one action), so a handler that only refreshed the row it touched
   would leave the page showing a shortcut that is no longer bound. */
static void refresh_rows(void) {
  int i;
  for(i=0;i<keybind_action_count;i++) {
    gchar *label;
    if(accel_button[i]==NULL) continue;
    if(i==learning) {
      gtk_button_set_label(GTK_BUTTON(accel_button[i]),LEARN_TEXT);
      continue;
    }
    label=keybind_accel_label(i);
    gtk_button_set_label(GTK_BUTTON(accel_button[i]),label!=NULL?label:UNBOUND_TEXT);
    g_free(label);
  }
}

static void learn_stop(void) {
  if(learn_widget!=NULL) {
    GtkWidget *w=learn_widget;
    /* The weak pointer is what makes this safe on the close path: the window
       can be gone before the page's own destroy runs, and removing a controller
       from a finalised widget is not a no-op. */
    g_object_remove_weak_pointer(G_OBJECT(w),(gpointer *)&learn_widget);
    learn_widget=NULL;
    if(learn_controller!=NULL) gtk_widget_remove_controller(w,learn_controller);
  }
  learn_controller=NULL;
  learning=-1;
}

static gboolean learn_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                  guint keycode, GdkModifierType state, gpointer data) {
  int index=learning;
  char text[160];

  if(index<0 || index>=keybind_action_count) { learn_stop(); return TRUE; }

  switch(keyval) {
    /* A modifier alone is not a shortcut: it is what the operator is holding
       down on the way to one, so keep waiting instead of binding Ctrl. */
    case GDK_KEY_Shift_L:   case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L: case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:     case GDK_KEY_Alt_R:
    case GDK_KEY_Meta_L:    case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L:   case GDK_KEY_Super_R:
    case GDK_KEY_Hyper_L:   case GDK_KEY_Hyper_R:
    case GDK_KEY_Caps_Lock: case GDK_KEY_Num_Lock:
    case GDK_KEY_ISO_Level3_Shift:
      return TRUE;
    case GDK_KEY_Escape:
      learn_stop();
      refresh_rows();
      set_status("Cancelled.");
      return TRUE;
    case GDK_KEY_BackSpace:
    case GDK_KEY_Delete:
      keybind_clear(index);
      learn_stop();
      refresh_rows();
      set_status("Shortcut cleared.");
      return TRUE;
  }

  {
    int previous=keybind_find(keyval,state);
    gchar *accel;
    keybind_set(index,keyval,state);
    accel=keybind_accel_label(index);
    if(previous>=0 && previous!=index) {
      /* keybind_set() took the combination away from that row; say so, or the
         operator only finds out when the old shortcut stops working. */
      g_snprintf(text,sizeof(text),"%s = %s (taken from \"%s\")",
                 keybind_actions[index].label,accel!=NULL?accel:"",
                 keybind_actions[previous].label);
    } else {
      g_snprintf(text,sizeof(text),"%s = %s",
                 keybind_actions[index].label,accel!=NULL?accel:"");
    }
    g_free(accel);
    learn_stop();
    refresh_rows();
    set_status(text);
  }
  return TRUE;
}

static void accel_clicked(GtkButton *button, gpointer data) {
  int index=GPOINTER_TO_INT(data);
  GtkRoot *root;

  if(learning==index) {           /* clicking again backs out */
    learn_stop();
    refresh_rows();
    set_status(NULL);
    return;
  }
  learn_stop();
  root=gtk_widget_get_root(GTK_WIDGET(button));
  if(root==NULL) return;
  learning=index;
  /* CAPTURE phase on the window: the Configure dialog has a key handler of its
     own (Cmd-W closes it) and the focused button would swallow space/Return, so
     a bubble-phase controller would never see half the keys worth binding. */
  learn_widget=GTK_WIDGET(root);
  g_object_add_weak_pointer(G_OBJECT(learn_widget),(gpointer *)&learn_widget);
  learn_controller=gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(learn_controller,GTK_PHASE_CAPTURE);
  g_signal_connect(learn_controller,"key-pressed",G_CALLBACK(learn_key_pressed),NULL);
  gtk_widget_add_controller(learn_widget,learn_controller);
  refresh_rows();
  set_status("Press a key or a combination. Esc cancels, Backspace clears.");
}

static void clear_clicked(GtkButton *button, gpointer data) {
  int index=GPOINTER_TO_INT(data);
  learn_stop();
  keybind_clear(index);
  refresh_rows();
  set_status(NULL);
}

static void clear_all_clicked(GtkButton *button, gpointer data) {
  learn_stop();
  keybind_clear_all();
  refresh_rows();
  set_status("All shortcuts cleared.");
}

static void page_destroyed(GtkWidget *widget, gpointer data) {
  /* The controller lives on the WINDOW, which outlives this page by a moment on
     some close paths -- so it is removed here rather than left to fire into a
     dead page. */
  learn_stop();
  memset(accel_button,0,sizeof(accel_button));
  status_label=NULL;
}

GtkWidget *create_keybind_dialog(RADIO *radio) {
  GtkWidget *grid=gtk_grid_new();
  GtkWidget *frame=NULL;
  GtkWidget *group=NULL;
  const char *current_group=NULL;
  int i,row=0,col=0,group_row=0;

  memset(accel_button,0,sizeof(accel_button));
  status_label=NULL;
  learning=-1;
  learn_controller=NULL;
  learn_widget=NULL;

  sui_style_page(grid);

  for(i=0;i<keybind_action_count;i++) {
    GtkWidget *label,*button,*clear;

    if(current_group==NULL || strcmp(current_group,keybind_actions[i].group)!=0) {
      current_group=keybind_actions[i].group;
      frame=gtk_frame_new(current_group);
      gtk_widget_set_valign(frame,GTK_ALIGN_START);
      group=gtk_grid_new();
      sui_style_group(group);
      gtk_frame_set_child(GTK_FRAME(frame),group);
      gtk_grid_attach(GTK_GRID(grid),frame,col,row,1,1);
      col++;
      if(col>1) { col=0; row++; }
      group_row=0;
    }

    label=gtk_label_new(keybind_actions[i].label);
    sui_label_left(label);
    gtk_grid_attach(GTK_GRID(group),label,0,group_row,1,1);

    button=gtk_button_new_with_label(UNBOUND_TEXT);
    gtk_widget_set_hexpand(button,FALSE);
    gtk_widget_set_size_request(button,140,-1);
    gtk_widget_set_tooltip_text(button,
      keybind_actions[i].tip!=NULL?keybind_actions[i].tip:
                                   "Click, then press the key combination to assign");
    g_signal_connect(button,"clicked",G_CALLBACK(accel_clicked),GINT_TO_POINTER(i));
    gtk_grid_attach(GTK_GRID(group),button,1,group_row,1,1);
    accel_button[i]=button;

    clear=gtk_button_new_with_label("x");
    gtk_widget_set_tooltip_text(clear,"Remove this shortcut");
    g_signal_connect(clear,"clicked",G_CALLBACK(clear_clicked),GINT_TO_POINTER(i));
    gtk_grid_attach(GTK_GRID(group),clear,2,group_row,1,1);

    group_row++;
  }

  if(col!=0) row++;

  {
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    GtkWidget *clear_all=gtk_button_new_with_label("Clear all");
    gtk_widget_set_tooltip_text(clear_all,"Remove every keyboard shortcut");
    g_signal_connect(clear_all,"clicked",G_CALLBACK(clear_all_clicked),NULL);
    gtk_box_append(GTK_BOX(box),clear_all);
    status_label=gtk_label_new("Shortcuts act on the ACTIVE receiver, and only "
                               "when the main window has the keyboard.");
    sui_label_left(status_label);
    gtk_box_append(GTK_BOX(box),status_label);
    gtk_grid_attach(GTK_GRID(grid),box,0,row,2,1);
  }

  g_signal_connect(grid,"destroy",G_CALLBACK(page_destroyed),NULL);
  refresh_rows();
  return grid;
}
