/* Copyright (C)
* 2020 - John Melton, G0ORX/N6LYT
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
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "receiver.h"
#include "transmitter.h"
#include "receiver.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "ext.h"
#include "settings_ui.h"
#include "midi.h"
#include "alsa_midi.h"
#include "midi_dialog.h"
#include "property.h"

enum {
  EVENT_COLUMN,
  CHANNEL_COLUMN,
  NOTE_COLUMN,
  TYPE_COLUMN,
  ACTION_COLUMN,
  N_COLUMNS
};

static GtkWidget *midi_enable_b;

// GTK4: GtkTreeView/GtkListStore are deprecated. The mapping table is a
// GtkColumnView over a GListStore of MidiItem GObjects (5 display strings,
// indexed by *_COLUMN). The store mirrors MidiCommandsTable, so edits/deletes
// mutate that table and rebuild via load_store().
#define MIDI_TYPE_ITEM (midi_item_get_type())
G_DECLARE_FINAL_TYPE(MidiItem, midi_item, MIDI, ITEM, GObject)
struct _MidiItem { GObject parent_instance; char *col[N_COLUMNS]; };
G_DEFINE_TYPE(MidiItem, midi_item, G_TYPE_OBJECT)
static void midi_item_finalize(GObject *o) {
  MidiItem *it = MIDI_ITEM(o);
  for(int c=0;c<N_COLUMNS;c++) g_free(it->col[c]);
  G_OBJECT_CLASS(midi_item_parent_class)->finalize(o);
}
static void midi_item_class_init(MidiItemClass *k){ G_OBJECT_CLASS(k)->finalize = midi_item_finalize; }
static void midi_item_init(MidiItem *it){ }

static void midi_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_list_item_set_child(li, lbl);
}
static void midi_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  int c = GPOINTER_TO_INT(u);
  MidiItem *it = gtk_list_item_get_item(li);
  const char *s = (it && it->col[c]) ? it->col[c] : "";
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), s);
}
static GtkColumnViewColumn *midi_col(const char *title, int colid) {
  GtkListItemFactory *f = gtk_signal_list_item_factory_new();
  g_signal_connect(f,"setup",G_CALLBACK(midi_setup),NULL);
  g_signal_connect(f,"bind",G_CALLBACK(midi_bind),GINT_TO_POINTER(colid));
  return gtk_column_view_column_new(title, f);
}

static GListStore *store;
static GtkWidget *view;
static GtkWidget *scrolled_window=NULL;
static gulong selection_signal_id;
static GtkSingleSelection *midi_selection;
struct desc *current_cmd;

static GtkWidget *filename;

static GtkWidget *newEvent;
static GtkWidget *newChannel;
static GtkWidget *newNote;
static GtkWidget *newVal;
static GtkWidget *newType;
static GtkWidget *newMin;
static GtkWidget *newMax;
static GtkWidget *newAction;
static GtkWidget *configure_b;
static GtkWidget *add_b;
static GtkWidget *update_b;
static GtkWidget *delete_b;

static enum MIDIevent thisEvent=EVENT_NONE;
static int thisChannel;
static int thisNote;
static int thisVal;
static int thisMin;
static int thisMax;
static enum MIDItype thisType;
static enum MIDIaction thisAction;

static gint device=-1;
gchar *midi_device_name=NULL;


enum {
  UPDATE_NEW,
  UPDATE_CURRENT,
  UPDATE_EXISTING
};

static int update(void *data);
static void load_store(void);

static gboolean midi_enable_cb(GtkWidget *widget,gpointer data) {
  RADIO *r=(RADIO *)data;
  if(r->midi_enabled) {
    close_midi_device();
  }
  r->midi_enabled=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  if(r->midi_enabled && midi_device_name!=NULL) {
    if(register_midi_device(midi_device_name)<0) {
      r->midi_enabled=false;
      gtk_check_button_set_active(GTK_CHECK_BUTTON (widget), r->midi_enabled);
    }
  }
  return TRUE;
}

static void configure_cb(GtkWidget *widget, gpointer data) {
  gboolean conf=gtk_check_button_get_active(GTK_CHECK_BUTTON (widget));
  configure_midi_device(conf);
}

// Read the selected row's text from a GtkStringList-backed GtkDropDown.
static const char *md_selected_text(GtkWidget *dd) {
  GtkStringObject *o=GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dd)));
  return o ? gtk_string_object_get_string(o) : NULL;
}

static void device_changed_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  RADIO *r=(RADIO *)data;
  guint sel = gtk_drop_down_get_selected(widget);
  // GTK_INVALID_LIST_POSITION is what a drop-down reports with nothing
  // selected -- which is also what it reports the moment its model changes.
  // Taken as an int that is -1, and midi_devices[-1].name was then strlen'd.
  if(sel==GTK_INVALID_LIST_POSITION || (int)sel>=n_midi_devices) return;
  device = (int)sel;
  if(midi_device_name!=NULL) {
    g_free(midi_device_name);
  }
  midi_device_name=g_new(gchar,strlen(midi_devices[device].name)+1);
  strcpy(midi_device_name,midi_devices[device].name);
  if(r->midi_enabled) {
    close_midi_device();
    if(register_midi_device(midi_device_name)) {
      r->midi_enabled=false;
      gtk_check_button_set_active(GTK_CHECK_BUTTON(midi_enable_b), r->midi_enabled);
    }
  }
}

static void type_changed_cb(GtkDropDown *widget, GParamSpec *ps, gpointer data) {
  int i=1;
  int j=1;

  // update actions available for the type
  const char *type=md_selected_text(GTK_WIDGET(widget));

  //g_print("%s: type=%s\n",__FUNCTION__,type);
  gtk_string_list_splice(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),0,g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),NULL);
  gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),ActionTable[0].str);
  if(type==NULL || strcmp(type,"NONE")==0) {
    // leave empty
    gtk_drop_down_set_selected(GTK_DROP_DOWN(newAction),0);
  } else if(strcmp(type,"KEY")==0) {
    // add all the Key actions
    while(ActionTable[i].action!=ACTION_NONE) {
      if(ActionTable[i].type&MIDI_KEY) {
        gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),ActionTable[i].str);
	if(ActionTable[i].action==thisAction) {
          gtk_drop_down_set_selected(GTK_DROP_DOWN(newAction),j);
	}
	j++;
      }
      i++;
    }
  } else if(strcmp(type,"KNOB/SLIDER")==0) {
    // add all the Knob actions
    while(ActionTable[i].action!=ACTION_NONE) {
      if(ActionTable[i].type&MIDI_KNOB) {
        gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),ActionTable[i].str);
	if(ActionTable[i].action==thisAction) {
          gtk_drop_down_set_selected(GTK_DROP_DOWN(newAction),j);
	}
	j++;
      }
      i++;
    }
  } else if(strcmp(type,"WHEEL")==0) {
    // add all the Wheel actions
    while(ActionTable[i].action!=ACTION_NONE) {
      if(ActionTable[i].type&MIDI_WHEEL || ActionTable[i].type&MIDI_KNOB) {
        gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),ActionTable[i].str);
	if(ActionTable[i].action==thisAction) {
          gtk_drop_down_set_selected(GTK_DROP_DOWN(newAction),j);
	}
	j++;
      }
      i++;
    }
  }
}



static void tree_selection_changed_cb (GObject *sel, GParamSpec *ps, gpointer data) {
    MidiItem *it = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(sel));
    if(it != NULL) {
      const char *str_event   = it->col[EVENT_COLUMN];
      const char *str_channel = it->col[CHANNEL_COLUMN];
      const char *str_note    = it->col[NOTE_COLUMN];
      const char *str_type    = it->col[TYPE_COLUMN];
      const char *str_action  = it->col[ACTION_COLUMN];

      if(str_event!=NULL && str_channel!=NULL && str_note!=NULL && str_type!=NULL && str_action!=NULL) {

        if(strcmp(str_event,"CTRL")==0) {
          thisEvent=MIDI_CTRL;
        } else if(strcmp(str_event,"PITCH")==0) {
          thisEvent=MIDI_PITCH;
        } else if(strcmp(str_event,"NOTE")==0) {
          thisEvent=MIDI_NOTE;
        } else {
          thisEvent=EVENT_NONE;
        }
        thisChannel=atoi(str_channel);
        thisNote=atoi(str_note);
        thisVal=0;
        thisMin=0;
        thisMax=0;
        if(strcmp(str_type,"KEY")==0) {
          thisType=MIDI_KEY;
        } else if(strcmp(str_type,"KNOB/SLIDER")==0) {
          thisType=MIDI_KNOB;
        } else if(strcmp(str_type,"WHEEL")==0) {
          thisType=MIDI_WHEEL;
        } else {
          thisType=TYPE_NONE;
        }
        thisAction=ACTION_NONE;
        int i=1;
        while(ActionTable[i].action!=ACTION_NONE) {
          if(strcmp(ActionTable[i].str,str_action)==0) {
            thisAction=ActionTable[i].action;
            break;
          }
          i++;
        }
        g_idle_add(update,GINT_TO_POINTER(UPDATE_EXISTING));
      }
    }
  //}
}

static void find_current_cmd(void) {
  struct desc *cmd;
  //g_print("%s:\n",__FUNCTION__);
  cmd=MidiCommandsTable.desc[thisNote];
  while(cmd!=NULL) {
    if((cmd->channel==thisChannel || cmd->channel==-1) && cmd->type==thisType && cmd->action==thisAction) {
  //g_print("%s: found cmd\n",__FUNCTION__);
      break;
    }
    cmd=cmd->next;
  }
  current_cmd=cmd;
}

static void clear_cb(GtkWidget *widget,gpointer user_data) {
  struct desc *cmd;
  struct desc *next;
  for(int i=0;i<128;i++) {
    cmd=MidiCommandsTable.desc[i];
    while(cmd!=NULL) {
      next=cmd->next;
      g_free(cmd);
      cmd=next;
    }
    MidiCommandsTable.desc[i]=NULL;
  }
  g_list_store_remove_all(store);
}

// GTK4: GtkFileChooserDialog is deprecated; GtkFileDialog is async — the accept
// path runs in each *_done finish callback.
static gchar *midi_default_filename(void) {
  if(midi_device_name==NULL) return g_strdup("midi.midi");
  return g_strdup_printf("%s.midi",midi_device_name);
}

static void save_done(GObject *src,GAsyncResult *res,gpointer user_data) {
  GFile *gf=gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),res,NULL);
  if(gf==NULL) return;
  char *savefilename=g_file_get_path(gf);
  g_object_unref(gf);
  // A MIDI file is not the radio's props, and the store is global: park the
  // live settings or this wipes them (see pushPropertyStore in property.c).
  pushPropertyStore();
  initProperties();
  midi_save_state();
  saveProperties(savefilename);
  popPropertyStore();
  g_free(savefilename);
}

static void save_cb(GtkWidget *widget,gpointer user_data) {
  RADIO *r=(RADIO *)user_data;
  GtkFileDialog *dialog=gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog,"Save File");
  gchar *filename=midi_default_filename();
  gtk_file_dialog_set_initial_name(dialog,filename);
  g_free(filename);
  gtk_file_dialog_save(dialog,GTK_WINDOW(r->dialog),NULL,save_done,NULL);
  g_object_unref(dialog);
}

static void load_done(GObject *src,GAsyncResult *res,gpointer user_data) {
  GFile *gf=gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),res,NULL);
  if(gf==NULL) return;
  char *loadfilename=g_file_get_path(gf);
  g_object_unref(gf);
  clear_cb(NULL,NULL);
  pushPropertyStore();
  initProperties();
  loadProperties(loadfilename);
  midi_restore_state();
  popPropertyStore();
  load_store();
  g_free(loadfilename);
}

static void load_cb(GtkWidget *widget,gpointer user_data) {
  RADIO *r=(RADIO *)user_data;
  GtkFileDialog *dialog=gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog,"Open MIDI File");
  gchar *filename=midi_default_filename();
  gtk_file_dialog_set_initial_name(dialog,filename);
  g_free(filename);
  gtk_file_dialog_open(dialog,GTK_WINDOW(r->dialog),NULL,load_done,NULL);
  g_object_unref(dialog);
}

static void load_original_done(GObject *src,GAsyncResult *res,gpointer user_data) {
  GFile *gf=gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),res,NULL);
  if(gf==NULL) return;
  char *loadfilename=g_file_get_path(gf);
  g_object_unref(gf);
  clear_cb(NULL,NULL);
  MIDIstartup(loadfilename);
  load_store();
  g_free(loadfilename);
}

static void load_original_cb(GtkWidget *widget,gpointer user_data) {
  RADIO *r=(RADIO *)user_data;
  GtkFileDialog *dialog=gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog,"Open ORIGINAL MIDI File");
  gchar *filename=g_strdup_printf("%s.midi",midi_device_name);
  gtk_file_dialog_set_initial_name(dialog,filename);
  g_free(filename);
  gtk_file_dialog_open(dialog,GTK_WINDOW(r->dialog),NULL,load_original_done,NULL);
  g_object_unref(dialog);
}

static void add_store(int key,struct desc *cmd) {
  char str_event[16];
  char str_channel[16];
  char str_note[16];
  char str_type[32];
  char str_action[32];

  switch(cmd->event) {
    case EVENT_NONE:
      strcpy(str_event,"NONE");
      break;
    case MIDI_NOTE:
      strcpy(str_event,"NOTE");
      break;
    case MIDI_CTRL:
      strcpy(str_event,"CTRL");
      break;
    case MIDI_PITCH:
      strcpy(str_event,"PITCH");
      break;
  }
  sprintf(str_channel,"%d",cmd->channel);
  sprintf(str_note,"%d",key);
  switch(cmd->type) {
    case TYPE_NONE:
      strcpy(str_type,"NONE");
      break;
    case MIDI_KEY:
      strcpy(str_type,"KEY");
      break;
    case MIDI_KNOB:
      strcpy(str_type,"KNOB/SLIDER");
      break;
    case MIDI_WHEEL:
      strcpy(str_type,"WHEEL");
      break;
  }
  strcpy(str_action,ActionTable[cmd->action].str);
  MidiItem *it = g_object_new(MIDI_TYPE_ITEM, NULL);
  it->col[EVENT_COLUMN]   = g_strdup(str_event);
  it->col[CHANNEL_COLUMN] = g_strdup(str_channel);
  it->col[NOTE_COLUMN]    = g_strdup(str_note);
  it->col[TYPE_COLUMN]    = g_strdup(str_type);
  it->col[ACTION_COLUMN]  = g_strdup(str_action);
  g_list_store_insert(store, 0, it);   // prepend (newest first)
  g_object_unref(it);

  if(scrolled_window!=NULL) {
    GtkAdjustment *adjustment=gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW(scrolled_window));
    //g_print("%s: adjustment=%f lower=%f upper=%f\n",__FUNCTION__,gtk_adjustment_get_value(adjustment),gtk_adjustment_get_lower(adjustment),gtk_adjustment_get_upper(adjustment));
    if(gtk_adjustment_get_value(adjustment)!=0.0) {
      gtk_adjustment_set_value(adjustment,0.0);
    }
  }
}

static void load_store(void) {
  struct desc *cmd;
  g_list_store_remove_all(store);
  for(int i=0;i<128;i++) {
    cmd=MidiCommandsTable.desc[i];
    while(cmd!=NULL) {
      add_store(i,cmd);
      cmd=cmd->next;
    }
  }
}

static void add_cb(GtkButton *widget,gpointer user_data) {

  const gchar *str_type=md_selected_text(newType);
  const gchar *str_action=md_selected_text(newAction);
;

  gint i;
  gint type;
  gint action;

  if(strcmp(str_type,"KEY")==0) {
    type=MIDI_KEY;
  } else if(strcmp(str_type,"KNOB/SLIDER")==0) {
    type=MIDI_KNOB;
  } else if(strcmp(str_type,"WHEEL")==0) {
    type=MIDI_WHEEL;
  } else {
    type=TYPE_NONE;
  }

  action=ACTION_NONE;
  i=1;
  while(ActionTable[i].action!=ACTION_NONE) {
    if(strcmp(ActionTable[i].str,str_action)==0) {
      action=ActionTable[i].action;
      break;
    }
    i++;
  }

  //g_print("%s: type=%s (%d) action=%s (%d)\n",__FUNCTION__,str_type,type,str_action,action);

  struct desc *desc;
  desc = (struct desc *) malloc(sizeof(struct desc));
  desc->next = NULL;
  desc->action = action; // MIDIaction
  desc->type = type; // MIDItype
  desc->event = thisEvent; // MIDevent
  desc->onoff = 0;
  if(type==MIDI_KEY && (action==CWLEFT || action==CWRIGHT || action==MIDI_PTT)) {
    desc->onoff = 1;
  }
  desc->delay = 0;
  desc->vfl1  = -1;
  desc->vfl2  = 57;
  desc->fl1   = 57;
  desc->fl2   = 60;
  desc->lft1  = 60;
  desc->lft2  = 63;
  desc->rgt1  = 64;
  desc->rgt2  = 67;
  desc->fr1   = 67;
  desc->fr2   = 73;
  desc->vfr1  = 73;
  desc->vfr2  = 128;
  desc->channel  = thisChannel;

  gint key=thisNote;
  if(key<0) key=0;
  if(key>127) key=0;


  if(MidiCommandsTable.desc[key]!=NULL) {
    desc->next=MidiCommandsTable.desc[key];
  }
  MidiCommandsTable.desc[key]=desc;
    
  add_store(key,desc);

  gtk_widget_set_sensitive(add_b,false);
  gtk_widget_set_sensitive(update_b,true);
  gtk_widget_set_sensitive(delete_b,true);

}

static void update_cb(GtkButton *widget,gpointer user_data) {
  int i;

  const gchar *str_type=md_selected_text(newType);
  const gchar *str_action=md_selected_text(newAction);
;
  //g_print("%s: type=%s action=%s\n",__FUNCTION__,str_type,str_action);

  if(strcmp(str_type,"KEY")==0) {
    thisType=MIDI_KEY;
  } else if(strcmp(str_type,"KNOB/SLIDER")==0) {
    thisType=MIDI_KNOB;
  } else if(strcmp(str_type,"WHEEL")==0) {
    thisType=MIDI_WHEEL;
  } else {
    thisType=TYPE_NONE;
  }

  thisAction=ACTION_NONE;
  i=1;
  while(ActionTable[i].action!=ACTION_NONE) {
    if(strcmp(ActionTable[i].str,str_action)==0) {
      thisAction=ActionTable[i].action;
      break;
    }
    i++;
  }

  current_cmd->channel=thisChannel;
  current_cmd->type=thisType;
  current_cmd->action=thisAction;

  // The store mirrors MidiCommandsTable; rebuild it from the (now edited) data.
  load_store();
}

static void delete_cb(GtkButton *widget,gpointer user_data) {
  struct desc *previous_cmd;
  struct desc *next_cmd;

  //g_print("%s\n",__FUNCTION__);

  // remove from MidiCommandsTable
  if(MidiCommandsTable.desc[thisNote]==current_cmd) {
    MidiCommandsTable.desc[thisNote]=current_cmd->next;
    g_free(current_cmd);
  } else {
    previous_cmd=MidiCommandsTable.desc[thisNote];
    while(previous_cmd->next!=NULL) {
      next_cmd=previous_cmd->next;
      if(next_cmd==current_cmd) {
	previous_cmd->next=next_cmd->next;
	g_free(next_cmd);
	break;
      }
      previous_cmd=next_cmd;
    }
  }

  // remove from list store (rebuild from the table)
  load_store();

  gtk_widget_set_sensitive(add_b,true);
  gtk_widget_set_sensitive(update_b,false);
  gtk_widget_set_sensitive(delete_b,false);

}

GtkWidget *create_midi_dialog(RADIO *r) {

  GtkWidget *page=gtk_grid_new();
  sui_style_page(page);
  int prow=0;

  /* ---- MIDI Device ---------------------------------------------------- */
  GtkWidget *dev_frame=gtk_frame_new("MIDI Device");
  GtkWidget *dev_grid=gtk_grid_new();
  sui_style_group(dev_grid);
  gtk_frame_set_child(GTK_FRAME(dev_frame),dev_grid);
  gtk_grid_attach(GTK_GRID(page),dev_frame,0,prow++,1,1);

  get_midi_devices();
  if(n_midi_devices>0) {
    GtkWidget *devices_label=gtk_label_new("Device:");
    gtk_widget_set_halign(devices_label,GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(dev_grid),devices_label,0,0,1,1);

    GtkWidget *devices=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
    for(int i=0;i<n_midi_devices;i++) {
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(devices))),midi_devices[i].name);
      if(midi_device_name!=NULL) {
        if(strcmp(midi_device_name,midi_devices[i].name)==0) {
          device=i;
        }
      }
    }
    gtk_widget_set_hexpand(devices,TRUE);
    gtk_grid_attach(GTK_GRID(dev_grid),devices,1,0,1,1);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(devices),device);
    g_signal_connect(devices,"notify::selected",G_CALLBACK(device_changed_cb),r);
  } else {
    GtkWidget *message=gtk_label_new("No MIDI devices found!");
    gtk_widget_set_halign(message,GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(dev_grid),message,0,0,2,1);
  }

  midi_enable_b=gtk_check_button_new_with_label("MIDI Enable");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (midi_enable_b), r->midi_enabled);
  gtk_grid_attach(GTK_GRID(dev_grid),midi_enable_b,0,1,2,1);
  g_signal_connect(midi_enable_b,"toggled",G_CALLBACK(midi_enable_cb),r);

  configure_b=gtk_check_button_new_with_label("MIDI Configure");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (configure_b), false);
  gtk_grid_attach(GTK_GRID(dev_grid),configure_b,0,2,2,1);
  g_signal_connect(configure_b,"toggled",G_CALLBACK(configure_cb),r);

  /* ---- New Mapping ---------------------------------------------------- */
  // Header labels and the entry widgets live in their own grid, so their
  // column widths only depend on each other and the header lines up over
  // the fields (the old single grid shared widths with the treeview).
  GtkWidget *map_frame=gtk_frame_new("New Mapping");
  GtkWidget *map_grid=gtk_grid_new();
  sui_style_group(map_grid);
  gtk_frame_set_child(GTK_FRAME(map_frame),map_grid);
  gtk_grid_attach(GTK_GRID(page),map_frame,0,prow++,1,1);

  const char *hdr[8]={"Event","Channel","Note","Type","Value","Min","Max","Action"};
  for(int c=0;c<8;c++) {
    GtkWidget *label=gtk_label_new(hdr[c]);
    gtk_widget_set_halign(label,GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(map_grid),label,c,0,1,1);
  }

  newEvent=gtk_label_new("");
  gtk_grid_attach(GTK_GRID(map_grid),newEvent,0,1,1,1);
  newChannel=gtk_label_new("");
  gtk_grid_attach(GTK_GRID(map_grid),newChannel,1,1,1,1);
  newNote=gtk_label_new("");
  gtk_grid_attach(GTK_GRID(map_grid),newNote,2,1,1,1);
  newType=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
  gtk_grid_attach(GTK_GRID(map_grid),newType,3,1,1,1);
  g_signal_connect(newType,"notify::selected",G_CALLBACK(type_changed_cb),NULL);
  newVal=gtk_label_new("");
  gtk_grid_attach(GTK_GRID(map_grid),newVal,4,1,1,1);
  newMin=gtk_label_new("");
  gtk_grid_attach(GTK_GRID(map_grid),newMin,5,1,1,1);
  newMax=gtk_label_new("");
  gtk_grid_attach(GTK_GRID(map_grid),newMax,6,1,1,1);
  newAction=gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),NULL);
  gtk_grid_attach(GTK_GRID(map_grid),newAction,7,1,1,1);

  add_b=gtk_button_new_with_label("Add");
  g_signal_connect(add_b, "clicked", G_CALLBACK(add_cb),NULL);
  gtk_grid_attach(GTK_GRID(map_grid),add_b,8,1,1,1);
  gtk_widget_set_sensitive(add_b,false);

  update_b=gtk_button_new_with_label("Update");
  g_signal_connect(update_b, "clicked", G_CALLBACK(update_cb),NULL);
  gtk_grid_attach(GTK_GRID(map_grid),update_b,9,1,1,1);
  gtk_widget_set_sensitive(update_b,false);

  delete_b=gtk_button_new_with_label("Delete");
  g_signal_connect(delete_b, "clicked", G_CALLBACK(delete_cb),NULL);
  gtk_grid_attach(GTK_GRID(map_grid),delete_b,10,1,1,1);
  gtk_widget_set_sensitive(delete_b,false);

  /* ---- Mappings ------------------------------------------------------- */
  GtkWidget *table_frame=gtk_frame_new("Mappings");
  GtkWidget *table_grid=gtk_grid_new();
  sui_style_group(table_grid);
  gtk_frame_set_child(GTK_FRAME(table_frame),table_grid);
  gtk_grid_attach(GTK_GRID(page),table_frame,0,prow++,1,1);

  GtkWidget *clear_b=gtk_button_new_with_label("Clear");
  gtk_grid_attach(GTK_GRID(table_grid),clear_b,0,0,1,1);
  g_signal_connect(clear_b,"clicked",G_CALLBACK(clear_cb),r);
  GtkWidget *save_b=gtk_button_new_with_label("Save");
  gtk_grid_attach(GTK_GRID(table_grid),save_b,1,0,1,1);
  g_signal_connect(save_b,"clicked",G_CALLBACK(save_cb),r);
  GtkWidget *load_b=gtk_button_new_with_label("Load");
  gtk_grid_attach(GTK_GRID(table_grid),load_b,2,0,1,1);
  g_signal_connect(load_b,"clicked",G_CALLBACK(load_cb),r);
  GtkWidget *load_original_b=gtk_button_new_with_label("Load Original");
  gtk_grid_attach(GTK_GRID(table_grid),load_original_b,3,0,1,1);
  g_signal_connect(load_original_b,"clicked",G_CALLBACK(load_original_cb),r);

  scrolled_window=gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),GTK_POLICY_AUTOMATIC,GTK_POLICY_ALWAYS);
  gtk_widget_set_size_request(scrolled_window,400,400);
  gtk_widget_set_hexpand(scrolled_window,TRUE);
  gtk_widget_set_vexpand(scrolled_window,TRUE);

  store=g_list_store_new(MIDI_TYPE_ITEM);
  midi_selection=gtk_single_selection_new(G_LIST_MODEL(store));   // takes the store ref
  gtk_single_selection_set_autoselect(midi_selection, FALSE);
  gtk_single_selection_set_can_unselect(midi_selection, TRUE);
  view=gtk_column_view_new(GTK_SELECTION_MODEL(midi_selection));  // takes the selection ref
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), midi_col("Event",   EVENT_COLUMN));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), midi_col("CHANNEL", CHANNEL_COLUMN));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), midi_col("NOTE",    NOTE_COLUMN));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), midi_col("TYPE",    TYPE_COLUMN));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), midi_col("ACTION",  ACTION_COLUMN));

  load_store();

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window),view);

  gtk_grid_attach(GTK_GRID(table_grid), scrolled_window, 0, 1, 4, 1);

  gtk_single_selection_set_selected(midi_selection, GTK_INVALID_LIST_POSITION);   // start unselected
  selection_signal_id=g_signal_connect(midi_selection,"notify::selected",
                                       G_CALLBACK(tree_selection_changed_cb),NULL);

  return page;
}

static int update(void *data) {
  int state=GPOINTER_TO_INT(data);
  gchar text[32];
  gint i=1;

  //g_print("%s\n",__FUNCTION__);
  switch(state) {
    case UPDATE_NEW:
      switch(thisEvent) {
        case EVENT_NONE:
          gtk_label_set_text(GTK_LABEL(newEvent),"NONE");
          break;
        case MIDI_NOTE:
          gtk_label_set_text(GTK_LABEL(newEvent),"NOTE");
          break;
        case MIDI_CTRL:
          gtk_label_set_text(GTK_LABEL(newEvent),"CTRL");
          break;
        case MIDI_PITCH:
          gtk_label_set_text(GTK_LABEL(newEvent),"PITCH");
          break;
      }
      sprintf(text,"%d",thisChannel);
      gtk_label_set_text(GTK_LABEL(newChannel),text);
      sprintf(text,"%d",thisNote);
      gtk_label_set_text(GTK_LABEL(newNote),text);
      gtk_string_list_splice(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),0,g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"NONE");
      switch(thisEvent) {
        case EVENT_NONE:
          gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),0);
          break;
        case MIDI_NOTE:
        case MIDI_PITCH:
          gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"KEY");
          gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),0);
          break;
        case MIDI_CTRL:
          gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"KNOB/SLIDER");
          gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"WHEEL");
          gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),0);
          break;
      }
      gtk_string_list_splice(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),0,g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newAction))),"NONE");
      gtk_drop_down_set_selected(GTK_DROP_DOWN(newAction),0);
      sprintf(text,"%d",thisVal);
      gtk_label_set_text(GTK_LABEL(newVal),text);
      sprintf(text,"%d",thisMin);
      gtk_label_set_text(GTK_LABEL(newMin),text);
      sprintf(text,"%d",thisMax);
      gtk_label_set_text(GTK_LABEL(newMax),text);

      gtk_widget_set_sensitive(add_b,true);
      gtk_widget_set_sensitive(update_b,false);
      gtk_widget_set_sensitive(delete_b,false);
      break;

    case UPDATE_CURRENT:
      sprintf(text,"%d",thisVal);
      gtk_label_set_text(GTK_LABEL(newVal),text);
      sprintf(text,"%d",thisMin);
      gtk_label_set_text(GTK_LABEL(newMin),text);
      sprintf(text,"%d",thisMax);
      gtk_label_set_text(GTK_LABEL(newMax),text);
      break;

    case UPDATE_EXISTING:
      switch(thisEvent) {
        case EVENT_NONE:
          gtk_label_set_text(GTK_LABEL(newEvent),"NONE");
          break;
        case MIDI_NOTE:
          gtk_label_set_text(GTK_LABEL(newEvent),"NOTE");
          break;
        case MIDI_CTRL:
          gtk_label_set_text(GTK_LABEL(newEvent),"CTRL");
          break;
        case MIDI_PITCH:
          gtk_label_set_text(GTK_LABEL(newEvent),"PITCH");
          break;
      }
      sprintf(text,"%d",thisChannel);
      gtk_label_set_text(GTK_LABEL(newChannel),text);
      sprintf(text,"%d",thisNote);
      gtk_label_set_text(GTK_LABEL(newNote),text);
      gtk_string_list_splice(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),0,g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),NULL);
      gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"NONE");
      switch(thisEvent) {
        case EVENT_NONE:
	  gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),0);
          break;
        case MIDI_NOTE:
        case MIDI_PITCH:
          gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"KEY");
	  if(thisType==TYPE_NONE) {
	    gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),0);
	  } else if(thisType==MIDI_KEY) {
	    gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),1);
	  }
          break;
        case MIDI_CTRL:
          gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"KNOB/SLIDER");
          gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(newType))),"WHEEL");
	  if(thisType==TYPE_NONE) {
	    gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),0);
	  } else if(thisType==MIDI_KNOB) {
	    gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),1);
	  } else if(thisType==MIDI_WHEEL) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(newType),2);
          }
          break;
      }
      sprintf(text,"%d",thisVal);
      gtk_label_set_text(GTK_LABEL(newVal),text);
      sprintf(text,"%d",thisMin);
      gtk_label_set_text(GTK_LABEL(newMin),text);
      sprintf(text,"%d",thisMax);
      gtk_label_set_text(GTK_LABEL(newMax),text);
  
      find_current_cmd();

      gtk_widget_set_sensitive(add_b,false);
      gtk_widget_set_sensitive(update_b,true);
      gtk_widget_set_sensitive(delete_b,true);
      break;

  }

  return 0;
}

// MIDI-learn, ON THE GTK THREAD (see NewMidiConfigureEvent below).
static void midi_configure_apply(enum MIDIevent event, int channel, int note, int val) {

  const char *str_event;
  const char *str_channel;
  const char *str_note;
  const char *str_type;
  const char *str_action;

  gint tree_event;
  gint tree_channel;
  gint tree_note;

  //g_print("%s: event=%d channel=%d note=%d val=%d\n", __FUNCTION__,event,channel,note,val);

  // Same range test as NewMidiEvent(): learn mode stores the note and it ends
  // up subscripting MidiCommandsTable.desc[].
  if(note<0 || note>127) return;

  if(event==thisEvent && channel==thisChannel && note==thisNote) {
    //g_print("%s: current event\n",__FUNCTION__);
    thisVal=val;
    if(val<thisMin) thisMin=val;
    if(val>thisMax) thisMax=val;
    g_idle_add(update,GINT_TO_POINTER(UPDATE_CURRENT));
  } else {
    //g_print("%s: new or existing event\n",__FUNCTION__);
    thisEvent=event;
    thisChannel=channel;
    thisNote=note;
    thisVal=val;
    thisMin=val;
    thisMax=val;
    thisType=TYPE_NONE;
    thisAction=ACTION_NONE;

    // search the list to see if it is an existing event
    guint n_rows = g_list_model_get_n_items(G_LIST_MODEL(store));
    for(guint row=0; row<n_rows; row++) {
      MidiItem *it = g_list_model_get_item(G_LIST_MODEL(store), row);   // owned
      str_event   = it->col[EVENT_COLUMN];
      str_channel = it->col[CHANNEL_COLUMN];
      str_note    = it->col[NOTE_COLUMN];
      str_type    = it->col[TYPE_COLUMN];
      str_action  = it->col[ACTION_COLUMN];

      if(str_event!=NULL && str_channel!=NULL && str_note!=NULL && str_type!=NULL && str_action!=NULL) {
        if(strcmp(str_event,"CTRL")==0) {
          tree_event=MIDI_CTRL;
        } else if(strcmp(str_event,"PITCH")==0) {
          tree_event=MIDI_PITCH;
        } else if(strcmp(str_event,"NOTE")==0) {
          tree_event=MIDI_NOTE;
        } else {
          tree_event=EVENT_NONE;
        }
        tree_channel=atoi(str_channel);
        tree_note=atoi(str_note);

	if(thisEvent==tree_event && (thisChannel==tree_channel || tree_channel==-1) && thisNote==tree_note) {
          thisVal=0;
          thisMin=0;
          thisMax=0;
          if(strcmp(str_type,"KEY")==0) {
            thisType=MIDI_KEY;
          } else if(strcmp(str_type,"KNOB/SLIDER")==0) {
            thisType=MIDI_KNOB;
          } else if(strcmp(str_type,"WHEEL")==0) {
            thisType=MIDI_WHEEL;
          } else {
            thisType=TYPE_NONE;
          }
          thisAction=ACTION_NONE;
          int i=1;
          while(ActionTable[i].action!=ACTION_NONE) {
            if(strcmp(ActionTable[i].str,str_action)==0) {
              thisAction=ActionTable[i].action;
              break;
            }
            i++;
          }
	  if(midi_selection!=NULL) gtk_single_selection_set_selected(midi_selection, row);
          g_object_unref(it);
          g_idle_add(update,GINT_TO_POINTER(UPDATE_EXISTING));
          return;
	}
      }
      g_object_unref(it);
    }

    g_idle_add(update,GINT_TO_POINTER(UPDATE_NEW));
  }
}

// Called from the platform MIDI reader thread, so it does nothing but carry the
// event across.  The body above walks the GtkListStore the GTK thread is adding
// rows to and calls gtk_single_selection_set_selected() -- reading a GListModel
// while another thread mutates it is not a stale read, it is a race on the
// model's own array, and the selection call is GTK from a foreign thread
// outright.  The note range test stays here: it costs nothing and keeps a wild
// subscript out of the queue.
typedef struct { enum MIDIevent event; int channel, note, val; } MIDI_CFG_EV;

static int midi_configure_idle(void *data) {
  MIDI_CFG_EV *e = (MIDI_CFG_EV *)data;
  midi_configure_apply(e->event, e->channel, e->note, e->val);
  g_free(e);
  return G_SOURCE_REMOVE;
}

void NewMidiConfigureEvent(enum MIDIevent event, int channel, int note, int val) {
  if(note<0 || note>127) return;
  MIDI_CFG_EV *e = g_new(MIDI_CFG_EV, 1);
  e->event = event;
  e->channel = channel;
  e->note = note;
  e->val = val;
  g_idle_add(midi_configure_idle, e);
}

/* Whether midi_save_state() will write anything.  `device` is set only when the
   saved device name matches one that is CONNECTED, so running once with the
   controller unplugged writes no midi[...] keys at all -- and radio_save_state
   has already wiped the old ones, which is how a whole mapping disappears from
   the props file.  radio_state.c asks this and retains them instead. */
gboolean midi_has_state(void) {
  return device!=-1;
}

void midi_save_state(void) {
  char name[80];
  char value[80];
  struct desc *cmd;
  gint channels;

  if(device!=-1) {
    setProperty("midi_device",midi_devices[device].name);
    for(int i=0;i<128;i++) {
      channels=0;
      cmd=MidiCommandsTable.desc[i];
      while(cmd!=NULL) {
        //g_print("%s:  channel=%d key=%d event=%s onoff=%d type=%s action=%s\n",__FUNCTION__,cmd->channel,i,midi_events[cmd->event],cmd->onoff,midi_types[cmd->type],ActionTable[cmd->action].str);

        sprintf(name,"midi[%d].channel[%d]",i,channels);
        sprintf(value,"%d",cmd->channel);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].event",i,cmd->channel);
        setProperty(name,midi_events[cmd->event]);
        sprintf(name,"midi[%d].channel[%d].onoff",i,cmd->channel);
        sprintf(value,"%d",cmd->onoff);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].type",i,cmd->channel);
        setProperty(name,midi_types[cmd->type]);
        sprintf(name,"midi[%d].channel[%d].action",i,cmd->channel);
	sprintf(value,"%s",ActionTable[cmd->action].str);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].vfl1",i,cmd->channel);
	sprintf(value,"%d",cmd->vfl1);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].vfl2",i,cmd->channel);
	sprintf(value,"%d",cmd->vfl2);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].fl1",i,cmd->channel);
	sprintf(value,"%d",cmd->fl1);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].fl2",i,cmd->channel);
	sprintf(value,"%d",cmd->fl2);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].lft1",i,cmd->channel);
	sprintf(value,"%d",cmd->lft1);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].lft2",i,cmd->channel);
	sprintf(value,"%d",cmd->lft2);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].vfr1",i,cmd->channel);
	sprintf(value,"%d",cmd->vfr1);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].vfr2",i,cmd->channel);
	sprintf(value,"%d",cmd->vfr2);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].fr1",i,cmd->channel);
	sprintf(value,"%d",cmd->fr1);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].fr2",i,cmd->channel);
	sprintf(value,"%d",cmd->fr2);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].rgt1",i,cmd->channel);
	sprintf(value,"%d",cmd->rgt1);
        setProperty(name,value);
        sprintf(name,"midi[%d].channel[%d].rgt2",i,cmd->channel);
	sprintf(value,"%d",cmd->rgt2);
        setProperty(name,value);
        cmd=cmd->next;
	channels++;
      }

      if(channels!=0) {
        sprintf(name,"midi[%d].channels",i);
        sprintf(value,"%d",channels);
        setProperty(name,value);
      }

    }
  }
}

void midi_restore_state(void) {
  char name[80];
  char *value;
  gint channels;
  gint channel;
  gint event;
  gint onoff;
  gint type;
  gint action;
  gint vfl1 = -1;
  gint vfl2 = 57;
  gint fl1  = 57;
  gint fl2  = 60;
  gint lft1 = 60;
  gint lft2 = 63;
  gint rgt1 = 64;
  gint rgt2 = 67;
  gint fr1  = 67;
  gint fr2  = 73;
  gint vfr1 = 73;
  gint vfr2 = 128;

  struct desc *cmd;

  get_midi_devices();

  //g_print("%s\n",__FUNCTION__);
  value=getProperty("midi_device");
  if(value) {
    //g_print("%s: device=%s\n",__FUNCTION__,value);
    midi_device_name=g_new(gchar,strlen(value)+1);
    strcpy(midi_device_name,value);

    
    for(int i=0;i<n_midi_devices;i++) {
      if(strcmp(midi_devices[i].name,value)==0) {
        device=i;
        log_info("%s: found device at %d\n",__FUNCTION__,i);
        break;
      }
    }
  }

  for(int i=0;i<128;i++) {
    sprintf(name,"midi[%d].channels",i);
    value=getProperty(name);
    if(value) {
      channels=atoi(value);
      // A count out of a props file is a claim, not a bound (see property.c).
      // Nothing here indexes an array with it, but the loop it drives does a
      // hash lookup per iteration, so a hand-edited or corrupted "2000000000"
      // is a start-up that never finishes.  128 is the size of the table these
      // entries hang off, i.e. past any list a save can produce.
      if(channels<0) channels=0;
      if(channels>128) channels=128;
      for(int c=0;c<channels;c++) {
        sprintf(name,"midi[%d].channel[%d]",i,c);
        value=getProperty(name);
        if(value) {
	  channel=atoi(value);
          sprintf(name,"midi[%d].channel[%d].event",i,channel);
          value=getProperty(name);
	  event=EVENT_NONE;
          if(value) {
            for(int j=0;j<4;j++) {
	      if(strcmp(value,midi_events[j])==0) {
		event=j;
		break;
              }
	    }
	  }
          sprintf(name,"midi[%d].channel[%d].onoff",i,channel);
          value=getProperty(name);
          if(value) onoff=atoi(value);
          sprintf(name,"midi[%d].channel[%d].type",i,channel);
          value=getProperty(name);
	  type=TYPE_NONE;
          if(value) {
            for(int j=0;j<5;j++) {
              if(strcmp(value,midi_types[j])==0) {
                type=j;
                break;
              }
            }
	  }
          sprintf(name,"midi[%d].channel[%d].action",i,channel);
          value=getProperty(name);
	  action=ACTION_NONE;
          if(value) {
	    int j=1;
	    while(ActionTable[j].action!=ACTION_NONE) {
              if(strcmp(value,ActionTable[j].str)==0) {
                action=ActionTable[j].action;
		break;
              }
	      j++;
	    }
	  }

          sprintf(name,"midi[%d].channel[%d].vfl1",i,channel);
          value=getProperty(name);
	  if(value) vfl1=atoi(value);
          sprintf(name,"midi[%d].channel[%d].vfl2",i,channel);
          value=getProperty(name);
	  if(value) vfl2=atoi(value);
          sprintf(name,"midi[%d].channel[%d].fl1",i,channel);
          value=getProperty(name);
	  if(value) fl1=atoi(value);
          sprintf(name,"midi[%d].channel[%d].fl2",i,channel);
          value=getProperty(name);
	  if(value) fl2=atoi(value);
          sprintf(name,"midi[%d].channel[%d].lft1",i,channel);
          value=getProperty(name);
	  if(value) lft1=atoi(value);
          sprintf(name,"midi[%d].channel[%d].lft2",i,channel);
          value=getProperty(name);
	  if(value) lft2=atoi(value);
          sprintf(name,"midi[%d].channel[%d].rgt1",i,channel);
          value=getProperty(name);
	  if(value) rgt1=atoi(value);
          sprintf(name,"midi[%d].channel[%d].rgt2",i,channel);
          value=getProperty(name);
	  if(value) rgt2=atoi(value);
          sprintf(name,"midi[%d].channel[%d].fr1",i,channel);
          value=getProperty(name);
	  if(value) fr1=atoi(value);
          sprintf(name,"midi[%d].channel[%d].fr2",i,channel);
          value=getProperty(name);
	  if(value) fr2=atoi(value);
          sprintf(name,"midi[%d].channel[%d].vfr1",i,channel);
          value=getProperty(name);
	  if(value) vfr1=atoi(value);
          sprintf(name,"midi[%d].channel[%d].vfr2",i,channel);
          value=getProperty(name);
	  if(value) vfr2=atoi(value);


	  struct desc *desc;
          desc = (struct desc *) malloc(sizeof(struct desc));
          desc->next = NULL;
          desc->action = action; // MIDIaction
          desc->type = type; // MIDItype
          desc->event = event; // MIDevent
          desc->onoff = onoff;
          desc->delay = 0;
	  desc->delay = 0;
          desc->vfl1  = vfl1;
          desc->vfl2  = vfl2;
          desc->fl1   = fl1;
          desc->fl2   = fl2;
          desc->lft1  = lft1;
          desc->lft2  = lft2;
          desc->rgt1  = rgt1;
          desc->rgt2  = rgt2;
          desc->fr1   = fr1;
          desc->fr2   = fr2;
          desc->vfr1  = vfr1;
          desc->vfr2  = vfr2;
          desc->channel  = channel;

	  if(MidiCommandsTable.desc[i]!=NULL) {
            desc->next=MidiCommandsTable.desc[i];
          }
          MidiCommandsTable.desc[i]=desc;
        }
      }
    }
  }

}

