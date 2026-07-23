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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "mode.h"
#include "filter.h"
#include "bookmark_dialog.h"
#include "property.h"

static GtkWidget *name_text;

static BOOKMARK *bookmark_head=NULL;
static BOOKMARK *bookmark_tail=NULL;

enum {
  NAME_COLUMN,
  FREQUENCY_A_COLUMN,
  FREQUENCY_B_COLUMN,
  CTUN_FREQUENCY_COLUMN,
  CTUN_COLUMN,
  MODE_COLUMN,
  FILTER_COLUMN,
  SPLIT_COLUMN,
  N_COLUMNS
};

// GTK4: GtkTreeView/GtkListStore are deprecated. The bookmark list is a
// GtkColumnView over a GListStore of BookmarkItem GObjects; each item carries a
// BOOKMARK* so selection/edit/delete/activate map straight to the bookmark (no
// string re-matching against bookmark_head). Column sorting uses a per-column
// GtkCustomSorter (g_utf8_collate on the display string), same as the old
// compare_func.
enum { N_BM_COLS = N_COLUMNS };
#define BM_TYPE_ITEM (bm_item_get_type())
G_DECLARE_FINAL_TYPE(BmItem, bm_item, BM, ITEM, GObject)
struct _BmItem { GObject parent_instance; char *col[N_BM_COLS]; BOOKMARK *bmk; };
G_DEFINE_TYPE(BmItem, bm_item, G_TYPE_OBJECT)
static void bm_item_finalize(GObject *o) {
  BmItem *it = BM_ITEM(o);
  for(int c=0;c<N_BM_COLS;c++) g_free(it->col[c]);
  G_OBJECT_CLASS(bm_item_parent_class)->finalize(o);
}
static void bm_item_class_init(BmItemClass *k){ G_OBJECT_CLASS(k)->finalize = bm_item_finalize; }
static void bm_item_init(BmItem *it){ }

static GListStore *store;
static GtkWidget *view;
static GtkSingleSelection *bm_selection;   // over the sorted model

static void bm_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_list_item_set_child(li, lbl);
}
static void bm_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  int c = GPOINTER_TO_INT(u);
  BmItem *it = gtk_list_item_get_item(li);
  const char *s = (it && it->col[c]) ? it->col[c] : "";
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), s);
}
static int bm_sort(const void *a, const void *b, gpointer u) {
  int c = GPOINTER_TO_INT(u);
  const char *as = ((BmItem*)a)->col[c], *bs = ((BmItem*)b)->col[c];
  if(as==NULL || bs==NULL) return (as==bs) ? 0 : (as==NULL ? -1 : 1);
  return g_utf8_collate(as, bs);
}
static GtkColumnViewColumn *bm_col(const char *title, int colid) {
  GtkListItemFactory *f = gtk_signal_list_item_factory_new();
  g_signal_connect(f,"setup",G_CALLBACK(bm_setup),NULL);
  g_signal_connect(f,"bind",G_CALLBACK(bm_bind),GINT_TO_POINTER(colid));
  GtkColumnViewColumn *col = gtk_column_view_column_new(title, f);
  GtkSorter *s = GTK_SORTER(gtk_custom_sorter_new(bm_sort, GINT_TO_POINTER(colid), NULL));
  gtk_column_view_column_set_sorter(col, s);
  g_object_unref(s);
  return col;
}

static char *split_char[] = {"OFF","SPLIT","SAT","RSAT"};

static void save_bookmarks() {
  char filename[128];
  sprintf(filename,"%s/.local/share/machpsdr/bookmarks",
                        g_get_home_dir());
  initProperties();

  int i=0;
  char name[80];
  char value[128];

  BOOKMARK *bookmark=bookmark_head;
  while(bookmark!=NULL) {
    sprintf(name,"bookmark[%d].name",i);
    setProperty(name,bookmark->name);
    sprintf(name,"bookmark[%d].frequency_a",i);
    sprintf(value,"%lld",bookmark->frequency_a);
    setProperty(name,value);
    sprintf(name,"bookmark[%d].frequency_b",i);
    sprintf(value,"%lld",bookmark->frequency_b);
    setProperty(name,value);
    sprintf(name,"bookmark[%d].ctun_frequency",i);
    sprintf(value,"%lld",bookmark->ctun_frequency);
    setProperty(name,value);
    sprintf(name,"bookmark[%d].ctun",i);
    sprintf(value,"%d",bookmark->ctun);
    setProperty(name,value);
    sprintf(name,"bookmark[%d].band",i);
    sprintf(value,"%d",bookmark->band);
    setProperty(name,value);
    sprintf(name,"bookmark[%d].mode",i);
    sprintf(value,"%d",bookmark->mode);
    setProperty(name,value);
    sprintf(name,"bookmark[%d].filter",i);
    sprintf(value,"%d",bookmark->filter);
    setProperty(name,value);
    sprintf(name,"bookmark[%d].split",i);
    sprintf(value,"%d",bookmark->split);
    setProperty(name,value);
    i++;
    bookmark=(BOOKMARK *)bookmark->next;
  }
  saveProperties(filename);
}

static void restore_bookmarks() {
  char filename[128];
  sprintf(filename,"%s/.local/share/machpsdr/bookmarks",
                        g_get_home_dir());
  int i=0;
  char name[80];
  char *value;

  loadProperties(filename);

  while(1) {
    sprintf(name,"bookmark[%d].name",i);
    value=getProperty(name);
    if(value==NULL) {
      break;
    }
    BOOKMARK *bookmark=g_new0(BOOKMARK,1);
    bookmark->previous=NULL;
    bookmark->next=NULL;
    bookmark->name=g_new0(gchar,strlen(value)+1);
    strcpy(bookmark->name,value);
    // Only the .name key is used as the end-of-list sentinel above; every other
    // key may be missing on a truncated or hand-edited bookmarks file, in which
    // case getProperty() returns NULL. atoll(NULL)/atoi(NULL) is a NULL deref,
    // so default each missing field to 0 instead of crashing at start-up.
    sprintf(name,"bookmark[%d].frequency_a",i);
    value=getProperty(name);
    bookmark->frequency_a=value?atoll(value):0;
    sprintf(name,"bookmark[%d].frequency_b",i);
    value=getProperty(name);
    bookmark->frequency_b=value?atoll(value):0;
    sprintf(name,"bookmark[%d].ctun_frequency",i);
    value=getProperty(name);
    bookmark->ctun_frequency=value?atoll(value):0;
    sprintf(name,"bookmark[%d].ctun",i);
    value=getProperty(name);
    bookmark->ctun=value?atoi(value):0;
    sprintf(name,"bookmark[%d].band",i);
    value=getProperty(name);
    bookmark->band=value?atoi(value):0;
    sprintf(name,"bookmark[%d].mode",i);
    value=getProperty(name);
    bookmark->mode=value?atoi(value):0;
    sprintf(name,"bookmark[%d].filter",i);
    value=getProperty(name);
    bookmark->filter=value?atoi(value):0;
    sprintf(name,"bookmark[%d].split",i);
    value=getProperty(name);
    bookmark->split=value?atoi(value):0;

    if(bookmark_tail==NULL) {
      bookmark_head=bookmark;
      bookmark_tail=bookmark;
    } else {
      bookmark_tail->next=(void *)bookmark;
      bookmark->previous=bookmark_tail;
      bookmark_tail=bookmark;
    }
    i++;
  }
}

// GTK4: window "close-request" replaces "delete-event".
static gboolean delete_event(GtkWindow *widget, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  rx->bookmark_dialog=NULL;
  return FALSE;
}

static gboolean add_cb(GtkWidget *widget,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;

  BOOKMARK *bookmark=g_new0(BOOKMARK,1);
  bookmark->name=g_new0(gchar,strlen(gtk_editable_get_text(GTK_EDITABLE(name_text)))+1);
  strcpy(bookmark->name,gtk_editable_get_text(GTK_EDITABLE(name_text)));
  bookmark->frequency_a=rx->frequency_a;
  bookmark->frequency_b=rx->frequency_b;
  bookmark->ctun_frequency=rx->ctun_frequency;
  bookmark->ctun=rx->ctun;
  bookmark->band=rx->band_a;
  bookmark->mode=rx->mode_a;
  bookmark->filter=rx->filter_a;
  bookmark->split=rx->split;
  bookmark->next=NULL;

  if(bookmark_tail==NULL) {
    bookmark_head=bookmark;
    bookmark_tail=bookmark;
  } else {
    bookmark_tail->next=(void *)bookmark;
    bookmark_tail=bookmark;
  }

  gtk_window_destroy(GTK_WINDOW(rx->bookmark_dialog));
  rx->bookmark_dialog=NULL;
  save_bookmarks();
  return TRUE;
}

static gboolean update_cb(GtkWidget *widget,gpointer data) {
  BOOKMARK_INFO *info=(BOOKMARK_INFO *)data;

  g_free(info->bookmark->name);
  info->bookmark->name=g_new0(gchar,strlen(gtk_editable_get_text(GTK_EDITABLE(name_text)))+1);
  strcpy(info->bookmark->name,gtk_editable_get_text(GTK_EDITABLE(name_text)));

  gtk_window_destroy(GTK_WINDOW(info->rx->bookmark_dialog));
  info->rx->bookmark_dialog=NULL;
  g_free(info);
  save_bookmarks();
  return TRUE;
}

// The currently-selected bookmark, or NULL.
static BOOKMARK *bm_selected(void) {
  if(bm_selection==NULL) return NULL;
  BmItem *it = gtk_single_selection_get_selected_item(bm_selection);
  return it ? it->bmk : NULL;
}

void edit_cb(GtkWidget *menuitem,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  BOOKMARK *bookmark=bm_selected();
  if(bookmark!=NULL) {
    // edit this one
    gtk_window_destroy(GTK_WINDOW(rx->bookmark_dialog));
    rx->bookmark_dialog=create_bookmark_dialog(rx,EDIT_BOOKMARK,bookmark);
  }
}

void delete_cb(GtkWidget *menuitem,gpointer data) {
  BmItem *it = bm_selection ? gtk_single_selection_get_selected_item(bm_selection) : NULL;
  BOOKMARK *bookmark = it ? it->bmk : NULL;
  if(bookmark!=NULL) {
    // unlink from the bookmark list
    if(bookmark->previous==NULL) {
      bookmark_head=bookmark->next;
    } else {
      BOOKMARK *p=(BOOKMARK *)bookmark->previous;
      p->next=bookmark->next;
    }
    if(bookmark->next==NULL) {
      bookmark_tail=bookmark->previous;
    } else {
      BOOKMARK *n=(BOOKMARK *)bookmark->next;
      n->previous=bookmark->previous;
    }
    // remove the row from the store (found by identity)
    guint pos;
    if(g_list_store_find(store, it, &pos)) g_list_store_remove(store, pos);
    save_bookmarks();
  }
}

void cancel_cb(GtkWidget *menuitem,gpointer userdata) {
}

// Add a flat "menu item" button to the context popover.
static void bm_add_item(GtkWidget *box,GtkWidget *pop,const char *label,GCallback cb,gpointer rx) {
  GtkWidget *b=gtk_button_new_with_label(label);
  gtk_widget_add_css_class(b,"flat");
  gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)),GTK_ALIGN_START);
  if(cb) g_signal_connect(b,"clicked",cb,rx);
  g_signal_connect_swapped(b,"clicked",G_CALLBACK(gtk_popover_popdown),pop);
  gtk_box_append(GTK_BOX(box),b);
}

// GTK4: right-click context menu on the bookmark list → a GtkPopover of buttons
// (GtkMenu is gone).  Driven by a GtkGestureClick attached to the tree view.
void bookmark_pressed_cb(GtkGestureClick *gesture,int n_press,double px,double py,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))!=3) return;
  GtkWidget *view=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  BOOKMARK *bookmark=bm_selected();
  if(bookmark==NULL) return;
  const char *name=bookmark->name;
  char label[128];

  GtkWidget *pop=gtk_popover_new();
  gtk_widget_set_parent(pop,view);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop),&(GdkRectangle){(int)px,(int)py,1,1});
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  gtk_popover_set_child(GTK_POPOVER(pop),box);

  snprintf(label,sizeof(label),"Edit: %s",name?name:"");
  bm_add_item(box,pop,label,G_CALLBACK(edit_cb),rx);
  snprintf(label,sizeof(label),"Delete: %s",name?name:"");
  bm_add_item(box,pop,label,G_CALLBACK(delete_cb),rx);
  bm_add_item(box,pop,"Cancel",G_CALLBACK(cancel_cb),rx);

  g_signal_connect_swapped(pop,"closed",G_CALLBACK(gtk_widget_unparent),pop);
  gtk_popover_popup(GTK_POPOVER(pop));
}

// GtkColumnView "activate" (double-click): pos indexes the (sorted) model.
void tree_selection_activated_cb(GtkColumnView *cv,guint pos,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  BmItem *it = g_list_model_get_item(G_LIST_MODEL(bm_selection), pos);   // owned
  if(it != NULL) {
    BOOKMARK *bookmark = it->bmk;
    g_object_unref(it);
    if(bookmark!=NULL) {
      rx->frequency_a=bookmark->frequency_a;
      rx->frequency_b=bookmark->frequency_b;
      rx->ctun_frequency=bookmark->ctun_frequency;
      rx->ctun=bookmark->ctun;
      rx->mode_a=bookmark->mode;
      rx->filter_a=bookmark->filter;
      rx->split=bookmark->split;
      rx->band_a=bookmark->band;
      receiver_mode_changed(rx,rx->mode_a);
      receiver_filter_changed(rx,rx->filter_a);
      frequency_changed(rx);
      if(radio->transmitter) {
      if(radio->transmitter->rx==rx) {
          if(rx->split) {
            transmitter_set_mode(radio->transmitter,rx->mode_b);
          } else {
            transmitter_set_mode(radio->transmitter,rx->mode_a);
          }
        }
      }
    }
  }
}

GtkWidget *create_bookmark_dialog(RECEIVER *rx,gint function,BOOKMARK *bookmark) {
  int x;
  int y;
  gchar temp[128];
  gchar temp_a[128];
  gchar temp_b[128];
  gchar temp_ctun_frequency[128];
  gchar temp_ctun[128];

  if(bookmark_head==NULL) {
    restore_bookmarks();
  }
  // GTK4: GtkDialog is deprecated; use a plain GtkWindow with a content box.
  GtkWidget *dialog=gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(dialog),GTK_WINDOW(main_window));
  g_signal_connect (dialog,"close-request",G_CALLBACK(delete_event),(gpointer)rx);
  GtkWidget *content=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  gtk_window_set_child(GTK_WINDOW(dialog),content);

  GtkWidget *grid=gtk_grid_new();
  gtk_box_append(GTK_BOX(content),grid);

  switch(function) {
    case ADD_BOOKMARK:
      g_snprintf((gchar *)&temp,sizeof(temp),"Linux HPSDR: Bookmarks");
      gtk_window_set_title(GTK_WINDOW(dialog),temp);

      x=0;
      y=0;
      g_snprintf((gchar *)&temp_a,sizeof(temp_a),"%4lld.%03lld.%03lld",rx->frequency_a/(long int)1000000,(rx->frequency_a%(long int)1000000)/(long int)1000,rx->frequency_a%(long int)1000);
      g_snprintf((gchar *)&temp_b,sizeof(temp_b),"%4lld.%03lld.%03lld",rx->frequency_b/(long int)1000000,(rx->frequency_b%(long int)1000000)/(long int)1000,rx->frequency_b%(long int)1000);
      g_snprintf((gchar *)&temp_ctun_frequency,sizeof(temp),"%4lld.%03lld.%03lld",rx->ctun_frequency/(long int)1000000,(rx->ctun_frequency%(long int)1000000)/(long int)1000,rx->ctun_frequency%(long int)1000);
      GtkWidget *name_title=gtk_label_new("Name: ");
      gtk_grid_attach(GTK_GRID(grid),name_title,x,y,1,1);
      x++;
      name_text=gtk_entry_new();
      if(rx->ctun) {
        gtk_editable_set_text(GTK_EDITABLE(name_text),temp_ctun_frequency);
      } else {
        gtk_editable_set_text(GTK_EDITABLE(name_text),temp_a);
      }
      gtk_grid_attach(GTK_GRID(grid),name_text,x,y,1,1);
      y++;
      x=0;
      GtkWidget *frequency_title=gtk_label_new("Frequency A: ");
      gtk_grid_attach(GTK_GRID(grid),frequency_title,x,y,1,1);
      x++;
      GtkWidget *frequency_text=gtk_label_new(temp_a);
      gtk_grid_attach(GTK_GRID(grid),frequency_text,x,y,1,1);
      y++;
      x=0;
      frequency_title=gtk_label_new("Frequency B: ");
      gtk_grid_attach(GTK_GRID(grid),frequency_title,x,y,1,1);
      x++;
      frequency_text=gtk_label_new(temp_b);
      gtk_grid_attach(GTK_GRID(grid),frequency_text,x,y,1,1);
      y++;
      x=0;
      GtkWidget *ctun_frequency_title=gtk_label_new("CTUN Frequency: ");
      gtk_grid_attach(GTK_GRID(grid),ctun_frequency_title,x,y,1,1);
      x++;
      frequency_text=gtk_label_new(temp_ctun_frequency);
      gtk_grid_attach(GTK_GRID(grid),frequency_text,x,y,1,1);
      y++;
      x=0;
      GtkWidget *ctun_title=gtk_label_new("CTUN: ");
      gtk_grid_attach(GTK_GRID(grid),ctun_title,x,y,1,1);
      x++;
      g_snprintf((gchar *)&temp,sizeof(temp),"%d",rx->ctun);
      GtkWidget *ctun_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),ctun_text,x,y,1,1);
      y++;
      x=0;
      GtkWidget *mode_title=gtk_label_new("Mode: ");
      gtk_grid_attach(GTK_GRID(grid),mode_title,x,y,1,1);
      x++;
      g_snprintf((gchar *)&temp,sizeof(temp),"%s",mode_string[rx->mode_a]);
      GtkWidget *mode_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),mode_text,x,y,1,1);
      y++;
      x=0;
      GtkWidget *filter_title=gtk_label_new("Filter: ");
      gtk_grid_attach(GTK_GRID(grid),filter_title,x,y,1,1);
      x++;
      FILTER* band_filters=filters[rx->mode_a];
      g_snprintf((gchar *)&temp,sizeof(temp),"%s",band_filters[rx->filter_a].title);
      GtkWidget *filter_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),filter_text,x,y,1,1);

      y++;
      x=0; 
      GtkWidget *split_title=gtk_label_new("Split: ");
      gtk_grid_attach(GTK_GRID(grid),split_title,x,y,1,1);
      x++;
      GtkWidget *split_text=gtk_label_new(split_char[rx->split]);
      gtk_grid_attach(GTK_GRID(grid),split_text,x,y,1,1);

      y++;
      x=0; 
      GtkWidget* button=gtk_button_new_with_label("Add Bookmark");
      g_signal_connect(button,"clicked",G_CALLBACK(add_cb),(gpointer)rx);
      gtk_grid_attach(GTK_GRID(grid),button,x,y,1,1);

      break;
    case VIEW_BOOKMARKS:
      g_snprintf((gchar *)&temp,sizeof(temp),"Linux HPSDR: RX-%d: Bookmarks",rx->channel);
      gtk_window_set_title(GTK_WINDOW(dialog),temp);

      store=g_list_store_new(BM_TYPE_ITEM);
      BOOKMARK *bmk=bookmark_head;
      while(bmk!=NULL) {
        FILTER* band_filters=filters[bmk->mode];
        g_snprintf((gchar *)&temp_a,sizeof(temp),"%4lld.%03lld.%03lld",bmk->frequency_a/(long int)1000000,(bmk->frequency_a%(long int)1000000)/(long int)1000,bmk->frequency_a%(long int)1000);
        g_snprintf((gchar *)&temp_b,sizeof(temp),"%4lld.%03lld.%03lld",bmk->frequency_b/(long int)1000000,(bmk->frequency_b%(long int)1000000)/(long int)1000,bmk->frequency_b%(long int)1000);
        g_snprintf((gchar *)&temp_ctun_frequency,sizeof(temp_ctun_frequency),"%4lld.%03lld.%03lld",bmk->ctun_frequency/(long int)1000000,(bmk->ctun_frequency%(long int)1000000)/(long int)1000,bmk->ctun_frequency%(long int)1000);
        g_snprintf((gchar *)&temp_ctun,sizeof(temp_ctun),"%d",bmk->ctun);

        BmItem *it = g_object_new(BM_TYPE_ITEM, NULL);
        it->bmk = bmk;
        it->col[NAME_COLUMN]           = g_strdup(bmk->name);
        it->col[FREQUENCY_A_COLUMN]    = g_strdup(temp_a);
        it->col[FREQUENCY_B_COLUMN]    = g_strdup(temp_b);
        it->col[CTUN_FREQUENCY_COLUMN] = g_strdup(temp_ctun_frequency);
        it->col[CTUN_COLUMN]           = g_strdup(temp_ctun);
        it->col[MODE_COLUMN]           = g_strdup(mode_string[bmk->mode]);
        it->col[FILTER_COLUMN]         = g_strdup(band_filters[bmk->filter].title);
        it->col[SPLIT_COLUMN]          = g_strdup(split_char[bmk->split]);
        g_list_store_append(store, it);
        g_object_unref(it);
        bmk=(BOOKMARK *)bmk->next;
      }

      // sorted model driven by the column view's own sorter, single selection
      GtkSortListModel *sort_model=gtk_sort_list_model_new(G_LIST_MODEL(store),NULL);
      bm_selection=gtk_single_selection_new(G_LIST_MODEL(sort_model));  // takes sort_model ref
      gtk_single_selection_set_autoselect(bm_selection,FALSE);
      gtk_single_selection_set_can_unselect(bm_selection,TRUE);
      view=gtk_column_view_new(GTK_SELECTION_MODEL(bm_selection));      // takes selection ref
      gtk_sort_list_model_set_sorter(sort_model, gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view)));

      GtkColumnViewColumn *name_col=bm_col("Name",NAME_COLUMN);
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), name_col);
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), bm_col("Frequency A",FREQUENCY_A_COLUMN));
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), bm_col("Frequency B",FREQUENCY_B_COLUMN));
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), bm_col("CTUN Frequency",CTUN_FREQUENCY_COLUMN));
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), bm_col("CTUN",CTUN_COLUMN));
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), bm_col("Mode",MODE_COLUMN));
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), bm_col("Filter",FILTER_COLUMN));
      gtk_column_view_append_column(GTK_COLUMN_VIEW(view), bm_col("Split",SPLIT_COLUMN));
      gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(view), name_col, GTK_SORT_ASCENDING);

      gtk_grid_attach(GTK_GRID(grid), view, 0, 0, 4, 1);
      g_signal_connect(view,"activate", G_CALLBACK(tree_selection_activated_cb), rx);
      // GTK4: right-click via a gesture controller (button-press-event is gone).
      GtkGesture *bm_click=gtk_gesture_click_new();
      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bm_click),3);
      g_signal_connect(bm_click,"pressed",G_CALLBACK(bookmark_pressed_cb),rx);
      gtk_widget_add_controller(view,GTK_EVENT_CONTROLLER(bm_click));
      break;
    case EDIT_BOOKMARK:
      g_snprintf((gchar *)&temp,sizeof(temp),"Linux HPSDR: Bookmark");
      gtk_window_set_title(GTK_WINDOW(dialog),temp);

      x=0;
      y=0;
      name_title=gtk_label_new("Name: ");
      gtk_grid_attach(GTK_GRID(grid),name_title,x,y,1,1);
      x++;
      name_text=gtk_entry_new();
      gtk_editable_set_text(GTK_EDITABLE(name_text),bookmark->name);
      gtk_grid_attach(GTK_GRID(grid),name_text,x,y,1,1);
      y++;
      x=0;
      g_snprintf((gchar *)&temp_a,sizeof(temp_a),"%4lld.%03lld.%03lld",bookmark->frequency_a/(long int)1000000,(bookmark->frequency_a%(long int)1000000)/(long int)1000,bookmark->frequency_a%(long int)1000);
      frequency_title=gtk_label_new("Frequency A: ");
      gtk_grid_attach(GTK_GRID(grid),frequency_title,x,y,1,1);
      x++;
      frequency_text=gtk_label_new(temp_a);
      gtk_grid_attach(GTK_GRID(grid),frequency_text,x,y,1,1);
      y++;
      x=0;
      g_snprintf((gchar *)&temp_b,sizeof(temp_b),"%4lld.%03lld.%03lld",bookmark->frequency_b/(long int)1000000,(bookmark->frequency_b%(long int)1000000)/(long int)1000,bookmark->frequency_b%(long int)1000);
      frequency_title=gtk_label_new("Frequency B: ");
      gtk_grid_attach(GTK_GRID(grid),frequency_title,x,y,1,1);
      x++;
      frequency_text=gtk_label_new(temp_b);
      gtk_grid_attach(GTK_GRID(grid),frequency_text,x,y,1,1);
      y++;
      x=0;
      g_snprintf((gchar *)&temp,sizeof(temp),"%4lld.%03lld.%03lld",bookmark->ctun_frequency/(long int)1000000,(bookmark->ctun_frequency%(long int)1000000)/(long int)1000,bookmark->ctun_frequency%(long int)1000);
      frequency_title=gtk_label_new("CTUN Frequency: ");
      gtk_grid_attach(GTK_GRID(grid),frequency_title,x,y,1,1);
      x++;
      frequency_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),frequency_text,x,y,1,1);
      y++;
      x=0;
      g_snprintf((gchar *)&temp,sizeof(temp),"%d",bookmark->ctun);
      frequency_title=gtk_label_new("CTUN: ");
      gtk_grid_attach(GTK_GRID(grid),frequency_title,x,y,1,1);
      x++;
      frequency_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),frequency_text,x,y,1,1);
      y++;
      x=0;
      mode_title=gtk_label_new("Mode: ");
      gtk_grid_attach(GTK_GRID(grid),mode_title,x,y,1,1);
      x++;
      g_snprintf((gchar *)&temp,sizeof(temp),"%s",mode_string[bookmark->mode]);
      mode_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),mode_text,x,y,1,1);
      y++;
      x=0;
      filter_title=gtk_label_new("Filter: ");
      gtk_grid_attach(GTK_GRID(grid),filter_title,x,y,1,1);
      x++;
      band_filters=filters[rx->mode_a];
      //band_filter=&band_filters[bookmark->filter]; // TO REMOVE
      g_snprintf((gchar *)&temp,sizeof(temp),"%s",band_filters[bookmark->filter].title);
      filter_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),filter_text,x,y,1,1);

      y++;
      x=0;
      filter_title=gtk_label_new("Split: ");
      gtk_grid_attach(GTK_GRID(grid),filter_title,x,y,1,1);
      x++;
      g_snprintf((gchar *)&temp,sizeof(temp),"%s",split_char[bookmark->split]);
      filter_text=gtk_label_new(temp);
      gtk_grid_attach(GTK_GRID(grid),filter_text,x,y,1,1);

      y++;
      x=0;
      button=gtk_button_new_with_label("Update Bookmark");
      BOOKMARK_INFO *info=g_new0(BOOKMARK_INFO,1);
      info->rx=rx;
      info->bookmark=bookmark;
      g_signal_connect(button,"clicked",G_CALLBACK(update_cb),(gpointer)info);
      gtk_grid_attach(GTK_GRID(grid),button,x,y,1,1);

      break;
  }

  gtk_widget_set_visible(dialog, TRUE);
  return dialog;
}
