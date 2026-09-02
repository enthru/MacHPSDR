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

/* Rows a group occupies, counted from the action table (a group's actions are
   consecutive), plus the frame's own chrome -- the column split needs every
   height BEFORE a single frame is built. */
static int group_height(int first) {
  int n=0,i;
  for(i=first;i<keybind_action_count;i++) {
    if(strcmp(keybind_actions[i].group,keybind_actions[first].group)!=0) break;
    n++;
  }
  return n+1;
}

/* Index of the first action of the group that starts the SECOND column: the
   groups flow down one column and continue down the next (newspaper order, so
   the page still reads in table order), and the break is put where the two
   columns come out closest in height.  Choosing per group "whichever column is
   shorter right now" reads worse and balances no better -- the last group is
   the tallest one and lands wherever the greedy walk left it. */
static int column_break(void) {
  int start[KB_ACTIONS],height[KB_ACTIONS],n=0,total=0,i;
  int run=0,best=0,best_diff=-1;
  for(i=0;i<keybind_action_count;i++) {
    if(i>0 && strcmp(keybind_actions[i].group,keybind_actions[i-1].group)==0) continue;
    start[n]=i;
    height[n]=group_height(i);
    total+=height[n];
    n++;
  }
  for(i=1;i<n;i++) {
    int diff;
    run+=height[i-1];
    diff=run-(total-run);
    if(diff<0) diff=-diff;
    if(best_diff<0 || diff<best_diff) { best_diff=diff; best=start[i]; }
  }
  return best;
}

GtkWidget *create_keybind_dialog(RADIO *radio) {
  /* TWO INDEPENDENT COLUMNS, not a two-column grid: a grid gives every cell in
     a row the height of the tallest frame in that row, so the three-row "Mode"
     group sitting beside the seven-row "Tuning" one left a hole the height of
     four rows under it, and the page was mostly gaps.  Boxes pack each column
     tight, and column_break() puts the break where the two columns finish
     closest to level.  (Same reason the TX page is three boxes side by side.) */
  GtkWidget *page=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  GtkWidget *columns=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  GtkWidget *column[2];
  int second=column_break();
  GtkWidget *group=NULL;
  const char *current_group=NULL;
  /* The column a group goes in is its OWN variable: sharing the loop counter
     that builds column[] left it at 2 -- one past the end -- for every group
     before the break, and the garbage read as a GtkBox* was a bus error the
     moment Configure was opened. */
  int i,group_row=0,c,col=0;

  memset(accel_button,0,sizeof(accel_button));
  status_label=NULL;
  learning=-1;
  learn_controller=NULL;
  learn_widget=NULL;

  sui_style_page(page);
  sui_style_page(columns);
  for(c=0;c<2;c++) {
    column[c]=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    sui_style_page(column[c]);
    gtk_widget_set_valign(column[c],GTK_ALIGN_START);
    gtk_widget_set_hexpand(column[c],TRUE);
    gtk_box_append(GTK_BOX(columns),column[c]);
  }
  gtk_box_append(GTK_BOX(page),columns);

  for(i=0;i<keybind_action_count;i++) {
    GtkWidget *label,*button,*clear;

    if(current_group==NULL || strcmp(current_group,keybind_actions[i].group)!=0) {
      GtkWidget *frame;
      current_group=keybind_actions[i].group;
      if(i>=second) col=1;
      frame=gtk_frame_new(current_group);
      gtk_widget_set_valign(frame,GTK_ALIGN_START);
      group=gtk_grid_new();
      sui_style_group(group);
      gtk_frame_set_child(GTK_FRAME(frame),group);
      gtk_box_append(GTK_BOX(column[col]),frame);
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
    gtk_box_append(GTK_BOX(page),box);
  }

  g_signal_connect(page,"destroy",G_CALLBACK(page_destroyed),NULL);
  refresh_rows();
  return page;
}
