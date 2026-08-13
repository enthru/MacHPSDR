/* 2018 - John Melton, G0ORX/N6LYT
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
#include <wdsp.h>

#include "agc.h"
#include "mode.h"
#include "filter.h"
#include "discovered.h"
#include "bpsk.h"
#include "receiver.h"
#include "receiver_dialog.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "css.h"
#include "band.h"
#include "main.h"
#include "vfo.h"
#include "bookmark_dialog.h"
#include "rigctl.h"
#include "bpsk.h"
#include "subrx.h"

typedef struct _choice {
  RECEIVER *rx;
  int selection;
  int sub_selection;
  GtkWidget *button;
} CHOICE;

// ---------------------------------------------------------------------------
// GTK4 popup-menu shim.  GtkMenu/GtkMenuItem are gone; the many right-click and
// button menus in this file are reproduced as a GtkPopover containing a vertical
// box of flat buttons.  The macros below let the existing gtk_menu_* call sites
// compile unchanged: gtk_menu_new() builds a popover parented to `widget` (which
// every menu handler here has in scope), gtk_menu_item_new_with_label() makes a
// flat button, gtk_menu_shell_append() adds it (and wires popdown-on-click), and
// gtk_menu_popup_at_pointer() shows it.  Menu-item callbacks are connected to
// "clicked" (see the "activate"->"clicked" migration).
static GtkWidget *vfo_pop_new(GtkWidget *relto) {
  GtkWidget *pop=gtk_popover_new();
  gtk_widget_set_parent(pop,relto);
  gtk_popover_set_has_arrow(GTK_POPOVER(pop),FALSE);
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  gtk_popover_set_child(GTK_POPOVER(pop),box);
  g_signal_connect_swapped(pop,"closed",G_CALLBACK(gtk_widget_unparent),pop);
  return pop;
}
static GtkWidget *vfo_menu_button(const char *label) {
  GtkWidget *b=gtk_button_new_with_label(label);
  gtk_widget_add_css_class(b,"flat");
  GtkWidget *lbl=gtk_button_get_child(GTK_BUTTON(b));
  if(lbl) gtk_widget_set_halign(lbl,GTK_ALIGN_START);
  return b;
}
static void vfo_menu_append(GtkWidget *pop, GtkWidget *item) {
  GtkWidget *box=gtk_popover_get_child(GTK_POPOVER(pop));
  gtk_box_append(GTK_BOX(box),item);
  g_signal_connect_swapped(item,"clicked",G_CALLBACK(gtk_popover_popdown),pop);
}
static void vfo_menu_popup(GtkWidget *pop) { gtk_popover_popup(GTK_POPOVER(pop)); }

// GTK4 replacement for the old GtkEventBox + event masks: attach the requested
// gesture/motion/scroll controllers straight onto a widget.  Any callback may be
// NULL.  Press/release share one GtkGestureClick (any button).
static void vfo_attach_ctl(GtkWidget *w, gpointer rx,
                           GCallback press, GCallback release,
                           GCallback motion, GCallback scroll) {
  if(press||release) {
    GtkGesture *g=gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g),0);
    if(press)   g_signal_connect(g,"pressed",press,rx);
    if(release) g_signal_connect(g,"released",release,rx);
    gtk_widget_add_controller(w,GTK_EVENT_CONTROLLER(g));
  }
  if(motion) {
    GtkEventController *m=gtk_event_controller_motion_new();
    g_signal_connect(m,"motion",motion,rx);
    gtk_widget_add_controller(w,m);
  }
  if(scroll) {
    GtkEventController *s=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(s,"scroll",scroll,rx);
    gtk_widget_add_controller(w,s);
  }
}

#define gtk_menu_new()                   vfo_pop_new(widget)
#define gtk_menu_item_new_with_label(l)  vfo_menu_button(l)
#define GTK_MENU_SHELL(m)                (m)
#define GTK_MENU(m)                      (m)
#define gtk_menu_shell_append(m,i)       vfo_menu_append((m),(i))
#define gtk_menu_popup_at_pointer(m,e)   vfo_menu_popup(m)

typedef struct _step {
  gint64 step;
  char *label;
} STEP;

const long long ll_step[13]= {
   10000000000LL,
   1000000000LL,
   100000000LL,
   10000000LL,
   1000000LL,
   0LL,
   100000LL,
   10000LL,
   1000LL,
   0LL,
   100LL,
   10LL,
   1LL
};

gint64 steps[STEPS]={1,10,25,50,100,250,500,1000,5000,9000,10000,12500,100000,250000,500000,1000000};
char *step_labels[STEPS]={"1 Hz","10 Hz","25 Hz","50 Hz","100 Hz","250 Hz","500 Hz","1 kHz","5 kHz","9 kHz","10 kHz","12.5 kHz", "100 kHz","250 kHz","500 kHz","1 MHz"};

static gboolean pressed=FALSE;
static gdouble last_x;
static gboolean has_moved=FALSE;
// GTK4: scroll controllers carry no pointer position, so the freq-label motion
// handler stashes the hover x here for the scroll handler's digit-under-cursor.
static double freq_hover_x=0;

// SDR#-style digit typing: the freq-label motion handlers also record which
// receiver / VFO / digit the cursor is over, so a numeric key press from the
// global key handler can overwrite that exact digit. Cleared on pointer leave.
static RECEIVER *freq_hover_rx=NULL;
static int       freq_hover_digit=-1;   // digit index under cursor, -1 = none
static gboolean  freq_hover_is_b=FALSE; // FALSE = VFO A, TRUE = VFO B

// Map a pointer x (widget coords) to the digit index under it, by hit-testing
// the label's actual Pango layout rather than assuming the text fills the whole
// widget with uniform character cells. This stays accurate regardless of label
// alignment/padding or proportional glyph widths. The frequency string is pure
// ASCII ("00014.074.000") so the returned byte index is also the digit index.
// Returns -1 when the pointer is outside the drawn text.
static int freq_digit_at(GtkWidget *label, double x) {
  PangoLayout *layout=gtk_label_get_layout(GTK_LABEL(label));
  if(layout==NULL) return -1;
  int lx=0,ly=0;
  gtk_label_get_layout_offsets(GTK_LABEL(label),&lx,&ly);
  int index=0,trailing=0;
  int px=(int)((x-(double)lx)*PANGO_SCALE);
  gboolean inside=pango_layout_xy_to_index(layout,px,0,&index,&trailing);
  if(!inside) return -1;   // in the padding to the left/right of the text
  return index;
}

static int get_step(gint64 step) {
  int i;
  for(i=0;i<STEPS;i++) {
    if(steps[i]==step) {
      return i;
    }
  }
  return 4; // 100Hz
}

void vfo_a2b(RECEIVER *rx) {
  int m=rx->mode_b;
  int f=rx->filter_b;
  rx->band_b=rx->band_a;
  rx->frequency_b=rx->frequency_a;
  rx->mode_b=rx->mode_a;
  rx->filter_b=rx->filter_a;
  rx->filter_low_b=rx->filter_low_a;
  rx->filter_high_b=rx->filter_high_a;
  rx->lo_b=rx->lo_a;
  rx->error_b=rx->error_a;
  frequency_changed(rx);
  if(rx->subrx_enable) {
    if(m!=rx->mode_b) {
      subrx_mode_changed(rx);
    } else if(f!=rx->filter_b) {
      subrx_filter_changed(rx);
    }
  }
  update_vfo(rx);
}

static void a2b_cb(GtkButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  vfo_a2b(rx);
}

void vfo_b2a(RECEIVER *rx) {
  int m=rx->mode_a;
  int f=rx->filter_a;
  rx->band_a=rx->band_b;
  rx->frequency_a=rx->frequency_b;
  rx->mode_a=rx->mode_b;
  rx->filter_a=rx->filter_b;
  rx->filter_low_a=rx->filter_low_b;
  rx->filter_high_a=rx->filter_high_b;
  rx->lo_a=rx->lo_b;
  rx->error_a=rx->error_b;
  frequency_changed(rx);
  // m/f are the *old* A values, kept only to detect what changed; the mode/filter
  // to apply is the new A (copied from B), as in vfo_aswapb.  Passing m/f here
  // reverted A to its old mode/filter, so B>A did nothing when the modes differed.
  if(m!=rx->mode_a) {
    receiver_mode_changed(rx,rx->mode_a);
  } else if(f!=rx->filter_a) {
    receiver_filter_changed(rx,rx->filter_a);
  }
  update_vfo(rx);
}

static void b2a_cb(GtkButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  vfo_b2a(rx);
}

void vfo_aswapb(RECEIVER *rx) {
  gint temp_band;
  gint64 temp_frequency;
  gint temp_mode;
  gint temp_filter_low;
  gint temp_filter_high;
  gint temp_filter;
  gint64 temp_lo;
  gint64 temp_error;

  temp_band=rx->band_a;
  temp_frequency=rx->frequency_a;
  temp_mode=rx->mode_a;
  temp_filter=rx->filter_a;
  temp_filter_low=rx->filter_low_a;
  temp_filter_high=rx->filter_high_a;
  temp_lo=rx->lo_a;
  temp_error=rx->error_a;

  rx->band_a=rx->band_b;
  rx->frequency_a=rx->frequency_b;
  rx->mode_a=rx->mode_b;
  rx->filter_a=rx->filter_b;
  rx->filter_low_a=rx->filter_low_b;
  rx->filter_high_a=rx->filter_high_b;
  rx->lo_a=rx->lo_b;
  rx->error_a=rx->error_b;

  rx->band_b=temp_band;
  rx->frequency_b=temp_frequency;
  rx->mode_b=temp_mode;
  rx->filter_b=temp_filter;
  rx->filter_low_b=temp_filter_low;
  rx->filter_high_b=temp_filter_high;
  rx->lo_b=temp_lo;
  rx->lo_b=temp_lo;
  rx->lo_b=temp_lo;
  rx->error_b=temp_error;

  frequency_changed(rx);
  receiver_mode_changed(rx,rx->mode_a);
  if(radio->transmitter!=NULL && radio->transmitter->rx==rx) {
    if(rx->split!=SPLIT_OFF) {
      transmitter_set_mode(radio->transmitter,rx->mode_b);
    } else {
      transmitter_set_mode(radio->transmitter,rx->mode_a);
    }
  }

  if(rx->subrx_enable) {
    if(rx->mode_b!=rx->mode_a) {
      subrx_mode_changed(rx);
    } else if(rx->filter_b!=rx->filter_a) {
      subrx_filter_changed(rx);
    }
  }

  update_vfo(rx);
}

static void aswapb_cb(GtkButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  vfo_aswapb(rx);
}

static void EnableSplitSubRX(gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  vfo_a2b(rx);

  // Split mode in CW, RX on VFO A, TX on VFO B.
  // When mode turned on, default to VFO A +1 kHz
  if (rx->mode_a == CWL || rx->mode_a == CWU) {
    // Most pile-ups start with UP 1
    rx->frequency_b = rx->frequency_a + 1000;
  }
  else if (rx->mode_a == LSB || rx->mode_a == USB) {
    rx->frequency_b = rx->frequency_a + 5000;
  }
  else {
    return;
  }
  rx->mode_b=rx->mode_a;
  if(!rx->subrx_enable) {
    create_subrx(rx);
    rx->subrx_enable=TRUE;
  }
}

static void DisableSplitSubRX(gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  if (rx->mode_a == CWL || rx->mode_a ==CWU ||
      rx->mode_a == LSB || rx->mode_a == USB) {
    if(rx->subrx_enable) {
      rx->subrx_enable=FALSE;
      destroy_subrx(rx);
      rx->subrx=NULL;
      log_info("Destroy subrx subrx\n");
    }
  }
}

static void split_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->split=rx->split==SPLIT_OFF?SPLIT_ON:SPLIT_OFF;
  g_signal_handlers_block_by_func(widget,G_CALLBACK(split_b_cb),user_data);
  gtk_toggle_button_set_active(widget,rx->split!=SPLIT_OFF);
  g_signal_handlers_unblock_by_func(widget,G_CALLBACK(split_b_cb),user_data);
  gtk_button_set_label(GTK_BUTTON(widget),"SPLIT");
  update_vfo(rx);
}

void split_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  RECEIVER *rx=choice->rx;
  rx->split=choice->selection;
  switch(rx->split) {
     case SPLIT_OFF:
     case SPLIT_ON:
       gtk_button_set_label(GTK_BUTTON(choice->button),"SPLIT");
       break;
     case SPLIT_SAT:
       gtk_button_set_label(GTK_BUTTON(choice->button),"SAT");
       break;
     case SPLIT_RSAT:
       gtk_button_set_label(GTK_BUTTON(choice->button),"RSAT");
       break;
  }
  g_signal_handlers_block_by_func(choice->button,G_CALLBACK(split_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(choice->button),rx->split!=SPLIT_OFF);
  g_signal_handlers_unblock_by_func(choice->button,G_CALLBACK(split_b_cb),rx);

  if(radio->transmitter && radio->transmitter->rx==rx) {
    switch(rx->split) {
      case SPLIT_OFF:
        transmitter_set_mode(radio->transmitter,rx->mode_a);
        DisableSplitSubRX(rx);
        break;
      case SPLIT_ON:
        transmitter_set_mode(radio->transmitter,rx->mode_b);
        EnableSplitSubRX(rx);
        break;
      case SPLIT_SAT:
      case SPLIT_RSAT:
        transmitter_set_mode(radio->transmitter,rx->mode_b);
        break;
    }
  }
  frequency_changed(rx);
  update_vfo(rx);
  g_free(choice);
}

static gboolean split_b_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *menu_item;
  CHOICE *choice;

  switch(button) {
    case 1:  // LEFT
      if(rx->split!=SPLIT_OFF) {
        transmitter_set_mode(radio->transmitter,rx->mode_a);
        DisableSplitSubRX(rx);
        rx->split=SPLIT_OFF;
      } else {
        EnableSplitSubRX(rx);
        transmitter_set_mode(radio->transmitter,rx->mode_b);
        rx->split=SPLIT_ON;
      }
      frequency_changed(rx);
      update_vfo(rx);

      return TRUE;
      break;

    case 3:  // RIGHT
      menu=gtk_menu_new();
      menu_item=gtk_menu_item_new_with_label("Off");
      choice=g_new0(CHOICE,1);
      choice->rx=rx;
      choice->selection=SPLIT_OFF;
      choice->button=widget;
      g_signal_connect(menu_item,"clicked",G_CALLBACK(split_cb),choice);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
      menu_item=gtk_menu_item_new_with_label("SPLIT");
      choice=g_new0(CHOICE,1);
      choice->rx=rx;
      choice->selection=SPLIT_ON;
      choice->button=widget;
      g_signal_connect(menu_item,"clicked",G_CALLBACK(split_cb),choice);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
      menu_item=gtk_menu_item_new_with_label("SAT");
      choice=g_new0(CHOICE,1);
      choice->rx=rx;
      choice->selection=SPLIT_SAT;
      choice->button=widget;
      g_signal_connect(menu_item,"clicked",G_CALLBACK(split_cb),choice);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
      menu_item=gtk_menu_item_new_with_label("RSAT");
      choice=g_new0(CHOICE,1);
      choice->rx=rx;
      choice->selection=SPLIT_RSAT;
      choice->button=widget;
      g_signal_connect(menu_item,"clicked",G_CALLBACK(split_cb),choice);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
#if GTK_CHECK_VERSION(3,22,0)
      gtk_menu_popup_at_pointer(GTK_MENU(menu),(GdkEvent *)event);
#else
      gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,event->time);
#endif
      frequency_changed(rx);
      return TRUE;
      break;
  }
  return FALSE;
}

void zoom_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  RECEIVER *rx=choice->rx;
  char temp[16];
  receiver_change_zoom(rx,choice->selection);
  sprintf(temp,"ZOOM x%d",rx->zoom);
  gtk_button_set_label(GTK_BUTTON(choice->button),temp);
  g_free(choice);
}

static void zoom_b_cb(GtkWidget *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *menu_item;
  CHOICE *choice;
  menu=gtk_menu_new();
  menu_item=gtk_menu_item_new_with_label("x1");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=1;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("x2");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=2;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("x3");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=3;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("x4");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=4;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("x5");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=5;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("x6");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=6;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("x7");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=7;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("x8");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=8;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  // Deep zoom levels (handy for FT8 / narrow digital signals).
  int deep_zoom[]={10,12,16,32};
  for(unsigned dz=0;dz<sizeof(deep_zoom)/sizeof(deep_zoom[0]);dz++) {
    char lbl[8];
    snprintf(lbl,sizeof(lbl),"x%d",deep_zoom[dz]);
    menu_item=gtk_menu_item_new_with_label(lbl);
    choice=g_new0(CHOICE,1);
    choice->rx=rx;
    choice->selection=deep_zoom[dz];
    choice->button=widget;
    g_signal_connect(menu_item,"clicked",G_CALLBACK(zoom_cb),choice);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  }
#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu),NULL);
#else
  gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,NULL);
#endif
}

void step_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  choice->rx->step=steps[choice->selection];
  char temp[16];
  sprintf(temp,"STEP %s",step_labels[choice->selection]);
  gtk_button_set_label(GTK_BUTTON(choice->button),temp);
  g_free(choice);
}

static void step_b_cb(GtkWidget *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *menu_item;
  CHOICE *choice;
  int i;

  menu=gtk_menu_new();
  for(i=0;i<STEPS;i++) {
    menu_item=gtk_menu_item_new_with_label(step_labels[i]);
    choice=g_new0(CHOICE,1);
    choice->rx=rx;
    choice->selection=i;
    choice->button=widget;
    g_signal_connect(menu_item,"clicked",G_CALLBACK(step_cb),choice);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  }
#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu),NULL);
#else
  gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,NULL);
#endif
}

static void subrx_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  if(rx->subrx_enable) {
    rx->subrx_enable=FALSE;
    destroy_subrx(rx);
    rx->subrx=NULL;
  } else {
    create_subrx(rx);
    rx->subrx_enable=TRUE;
  }
  update_vfo(rx);
}


static void lock_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->locked=gtk_toggle_button_get_active(widget);
}

static void vfo_set_mute_icon(GtkWidget *button,gboolean muted) {
  // GTK4: gtk_image_new_from_icon_name takes no size; button child replaces set_image.
  GtkWidget *img=gtk_image_new_from_icon_name(muted?"audio-volume-muted-symbolic":"audio-volume-high-symbolic");
  gtk_button_set_child(GTK_BUTTON(button),img);
}

static void mute_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->mute=gtk_toggle_button_get_active(widget);
  vfo_set_mute_icon(GTK_WIDGET(widget),rx->mute);
  receiver_set_volume(rx);
}

void mode_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  receiver_mode_changed(choice->rx,choice->selection);
  if(choice->rx->split!=SPLIT_OFF) {
    choice->rx->mode_b=choice->selection;
  }
  if(radio->transmitter!=NULL && radio->transmitter->rx==choice->rx) {
    if(choice->rx->split!=SPLIT_OFF) {
      transmitter_set_mode(radio->transmitter,choice->rx->mode_b);
    } else {
      transmitter_set_mode(radio->transmitter,choice->rx->mode_a);
    }
  }
  gtk_button_set_label(GTK_BUTTON(choice->button),mode_string[choice->selection]);
  g_free(choice);
}

static void mode_b_cb(GtkWidget *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *menu_item;
  CHOICE *choice;
  int i;

  for(i=0;i<MODES;i++) {
    menu_item=gtk_menu_item_new_with_label(mode_string[i]);
    choice=g_new0(CHOICE,1);
    choice->rx=rx;
    choice->selection=i;
    choice->button=(GtkWidget *)widget;
    g_signal_connect(menu_item,"clicked",G_CALLBACK(mode_cb),choice);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
    }
#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu),NULL);
#else
  gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,NULL);
#endif
}

void filter_cb(GtkWidget *menu_item,gpointer data) {
  FILTER *mode_filters;
  CHOICE *choice=(CHOICE *)data;
  receiver_filter_changed(choice->rx,choice->selection);
  mode_filters=filters[choice->rx->mode_a];
  gtk_button_set_label(GTK_BUTTON(choice->button),mode_filters[choice->selection].title);
  g_free(choice);
}

static void filter_b_cb(GtkWidget *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *menu_item;
  CHOICE *choice;
  FILTER *mode_filters;
  char text[32];
  int i;
  if(rx->mode_a==FMN) {
    menu=gtk_menu_new();
    menu_item=gtk_menu_item_new_with_label("2.5k Dev");
    choice=g_new0(CHOICE,1);
    choice->rx=rx;
    choice->selection=0;
    choice->button=(GtkWidget *)widget;
    g_signal_connect(menu_item,"clicked",G_CALLBACK(filter_cb),choice);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
    menu_item=gtk_menu_item_new_with_label("5.0k Dev");
    choice=g_new0(CHOICE,1);
    choice->rx=rx;
    choice->selection=1;
    choice->button=(GtkWidget *)widget;
    g_signal_connect(menu_item,"clicked",G_CALLBACK(filter_cb),choice);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  } else {
    mode_filters=filters[rx->mode_a];
    menu=gtk_menu_new();
    for(i=0;i<FILTERS;i++) {
      if(i>=FVar1) {
        sprintf(text,"%s (%d..%d)",mode_filters[i].title,mode_filters[i].low,mode_filters[i].high);
        menu_item=gtk_menu_item_new_with_label(text);
      } else {
        menu_item=gtk_menu_item_new_with_label(mode_filters[i].title);
      }
      choice=g_new0(CHOICE,1);
      choice->rx=rx;
      choice->selection=i;
      choice->button=(GtkWidget *)widget;
      g_signal_connect(menu_item,"clicked",G_CALLBACK(filter_cb),choice);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
    }
  }
#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu),NULL);
#else
  gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,NULL);
#endif
}

static gboolean nb_b_pressed_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data);

void nb_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)choice->rx->vfo,"vfo_data");

  switch(choice->selection) {
    case 0:
      choice->rx->nb=FALSE;
      choice->rx->nb2=FALSE;
      gtk_button_set_label(GTK_BUTTON(v->nb_b),"NB");
      break;
    case 1:
      choice->rx->nb=TRUE;
      choice->rx->nb2=FALSE;
      gtk_button_set_label(GTK_BUTTON(v->nb_b),"NB");
      break;
    case 2:
      choice->rx->nb=FALSE;
      choice->rx->nb2=TRUE;
      gtk_button_set_label(GTK_BUTTON(v->nb_b),"NB2");
      break;
  }
  g_signal_handlers_block_by_func(v->nb_b,G_CALLBACK(nb_b_pressed_cb),data);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->nb_b),choice->rx->nb|choice->rx->nb2);
  g_signal_handlers_unblock_by_func(v->nb_b,G_CALLBACK(nb_b_pressed_cb),data);

  update_noise(choice->rx);

  g_free(choice);
}

static gboolean nb_b_pressed_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu;
  GtkWidget *menu_item;
  CHOICE *choice;

  menu=gtk_menu_new();
  menu_item=gtk_menu_item_new_with_label("OFF");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=0;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nb_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("NB");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=1;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nb_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("NB2");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=2;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nb_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu),(GdkEvent *)event);
#else
  gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,event->time);
#endif
  return TRUE;
}

static gboolean nr_b_pressed_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data);

void nr_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)choice->rx->vfo,"vfo_data");

  // choice->selection encodes the NR mode directly: 0=off,1=NR,2=NR2,3=NR3,4=NR4
  receiver_set_nr_mode(choice->rx,choice->selection);
  switch(choice->selection) {
    case 0:
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR");
      break;
    case 1:
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR");
      break;
    case 2:
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR2");
      break;
    case 3:
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR3");
      break;
    case 4:
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR4");
      break;
  }
  g_signal_handlers_block_by_func(v->nr_b,G_CALLBACK(nr_b_pressed_cb),data);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->nr_b),choice->selection!=0);
  g_signal_handlers_unblock_by_func(v->nr_b,G_CALLBACK(nr_b_pressed_cb),data);

  g_free(choice);
}

static gboolean nr_b_pressed_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu;
  GtkWidget *menu_item;
  CHOICE *choice;

  menu=gtk_menu_new();
  menu_item=gtk_menu_item_new_with_label("OFF");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=0;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nr_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("NR");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=1;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nr_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("NR2");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=2;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nr_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("NR3");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=3;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nr_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("NR4");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=4;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(nr_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu),(GdkEvent *)event);
#else
  gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,event->time);
#endif
  return TRUE;
}

static void snb_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->snb=gtk_toggle_button_get_active(widget);
  update_noise(rx);
}

static void anf_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->anf=gtk_toggle_button_get_active(widget);
  update_noise(rx);
}

static void bpsk_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->bpsk_enable=gtk_toggle_button_get_active(widget);
  if(rx->bpsk_enable) {
    rx->bpsk=create_bpsk(BPSK_CHANNEL,rx->band_a);
    rx->bpsk_enable=TRUE;
  } else {
    destroy_bpsk(rx->bpsk);
    rx->bpsk=NULL;
    rx->bpsk_enable=FALSE;
  }
}

static void ant_b_cb(GtkToggleButton *widget,gpointer user_data) {

  if (radio->adc[0].antenna == 0) {
    radio->adc[0].antenna = 3;
  }
  else {
    radio->adc[0].antenna = 0;
  }
}

static void rit_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->rit_enabled=gtk_toggle_button_get_active(widget);
  frequency_changed(rx);
  update_frequency(rx);
}

static gboolean rit_b_scroll_event_cb(GtkEventControllerScroll *ctrl,double dx,double dy,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  char text[16];
  if((dy<0.0)) {
    rx->rit=rx->rit+rx->rit_step;
    if(rx->rit>10000) rx->rit=10000;
  } else {
    rx->rit=rx->rit-rx->rit_step;
    if(rx->rit<-10000) rx->rit=-10000;
  }
  sprintf(text,"%+05lld",rx->rit);
  gtk_label_set_text(GTK_LABEL(v->rit_value),text);
  frequency_changed(rx);
  update_frequency(rx);
  return TRUE;
}

void rit_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  choice->rx->rit_step=choice->selection;
  g_free(choice);
}

static gboolean rit_b_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *menu_item;
  CHOICE *choice;
  switch(button) {
    case 3:  // RIGHT
     menu=gtk_menu_new();

     menu_item=gtk_menu_item_new_with_label("1Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=1;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(rit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("5Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=5;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(rit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("10Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=10;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(rit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("100Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=100;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(rit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("1000Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=1000;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(rit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

#if GTK_CHECK_VERSION(3,22,0)
     gtk_menu_popup_at_pointer(GTK_MENU(menu),(GdkEvent *)event);
#else
     gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,event->time);
#endif
      return TRUE;
      break;
  }
  return FALSE;
}

static void xit_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  if(radio->transmitter!=NULL && radio->transmitter->rx==rx) {
    radio->transmitter->xit_enabled=gtk_toggle_button_get_active(widget);
  }
}

static gboolean xit_b_scroll_event_cb(GtkEventControllerScroll *ctrl,double dx,double dy,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  char text[16];
  if(radio->transmitter!=NULL && radio->transmitter->rx==rx) {
    if((dy<0.0)) {
      radio->transmitter->xit=radio->transmitter->xit+radio->transmitter->xit_step;
      if(radio->transmitter->xit>10000) radio->transmitter->xit=10000;
    } else {
      radio->transmitter->xit=radio->transmitter->xit-radio->transmitter->xit_step;
      if(radio->transmitter->xit<-10000) radio->transmitter->xit=-10000;
    }
    sprintf(text,"%+05lld",radio->transmitter->xit);
    gtk_label_set_text(GTK_LABEL(v->xit_value),text);
  }
  return TRUE;
}

void xit_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  radio->transmitter->xit_step=choice->selection;
  g_free(choice);
}

static gboolean xit_b_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *menu_item;
  CHOICE *choice;
  switch(button) {
    case 3:  // RIGHT
     menu=gtk_menu_new();

     menu_item=gtk_menu_item_new_with_label("1Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=1;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(xit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("5Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=5;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(xit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("10Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=10;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(xit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("100Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=100;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(xit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

     menu_item=gtk_menu_item_new_with_label("1000Hz");
     choice=g_new0(CHOICE,1);
     choice->rx=rx;
     choice->selection=1000;
     g_signal_connect(menu_item,"clicked",G_CALLBACK(xit_cb),choice);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

#if GTK_CHECK_VERSION(3,22,0)
     gtk_menu_popup_at_pointer(GTK_MENU(menu),(GdkEvent *)event);
#else
     gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,event->time);
#endif
      return TRUE;
      break;
  }
  return FALSE;
}

static void dup_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->duplex=gtk_toggle_button_get_active(widget);
}

static void ctun_b_cb(GtkToggleButton *widget,gpointer user_data) {
  RECEIVER *rx=(RECEIVER *)user_data;
  rx->ctun=!rx->ctun;
  receiver_set_ctun(rx);
}

static gboolean bmk_b_pressed_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  switch(button) {
    case 1:  // LEFT
      if(rx->bookmark_dialog==NULL) {
        rx->bookmark_dialog=create_bookmark_dialog(rx,VIEW_BOOKMARKS,NULL);
      }
      break;
    case 3:  // RIGHT
      if(rx->bookmark_dialog==NULL) {
        rx->bookmark_dialog=create_bookmark_dialog(rx,ADD_BOOKMARK,NULL);
      }
      break;
  }
  return TRUE;
}

static gboolean agc_b_pressed_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data);

void agc_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)choice->rx->vfo,"vfo_data");

  choice->rx->agc=choice->selection;
  if(choice->rx->mode_a>=0 && choice->rx->mode_a<MODES) choice->rx->mode_agc[choice->rx->mode_a]=choice->rx->agc;
  set_agc(choice->rx);
  switch(choice->selection) {
    case AGC_OFF:
      gtk_button_set_label(GTK_BUTTON(choice->button),"AGC");
      break;
    case AGC_LONG:
      gtk_button_set_label(GTK_BUTTON(choice->button),"AGC LONG");
      break;
    case AGC_SLOW:
      gtk_button_set_label(GTK_BUTTON(choice->button),"AGC SLOW");
      break;
    case AGC_MEDIUM:
      gtk_button_set_label(GTK_BUTTON(choice->button),"AGC MED");
      break;
    case AGC_FAST:
      gtk_button_set_label(GTK_BUTTON(choice->button),"AGC FAST");
      break;
  }
  g_signal_handlers_block_by_func(v->agc_b,G_CALLBACK(agc_b_pressed_cb),data);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->agc_b),choice->rx->agc!=AGC_OFF);
  g_signal_handlers_unblock_by_func(v->agc_b,G_CALLBACK(agc_b_pressed_cb),data);

  update_noise(choice->rx);

  g_free(choice);
}

static gboolean agc_b_pressed_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  GtkWidget *menu;
  GtkWidget *menu_item;
  CHOICE *choice;

  menu=gtk_menu_new();
  menu_item=gtk_menu_item_new_with_label("OFF");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=AGC_OFF;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(agc_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("LONG");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=AGC_LONG;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(agc_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("SLOW");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=AGC_SLOW;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(agc_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("MEDIUM");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=AGC_MEDIUM;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(agc_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
  menu_item=gtk_menu_item_new_with_label("FAST");
  choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=AGC_FAST;
  choice->button=widget;
  g_signal_connect(menu_item,"clicked",G_CALLBACK(agc_cb),choice);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);
#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu),(GdkEvent *)event);
#else
  gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,button,event->time);
#endif
  return TRUE;
}

static gboolean afgain_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)data;
  pressed=TRUE;
  last_x=ex;
  has_moved=FALSE;
  return TRUE;
}

static gboolean afgain_scale_motion_notify_event_cb(GtkEventControllerMotion *ctrl, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(pressed) {
    gdouble moved=ex-last_x;
    if (moved > 1 && moved < -1) {
      has_moved=TRUE;
    }
    rx->volume=rx->volume+(moved/100.0);
    if(rx->volume>1.0) rx->volume=1.0;
    if(rx->volume<0.0) rx->volume=0.0;
    receiver_set_volume(rx);
    last_x=ex;
    update_vfo(rx);
  }
  return TRUE;
}

static gboolean afgain_release_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)data;
  if(has_moved) {
    gdouble moved=ex-last_x;
    rx->volume=rx->volume+(moved/100.0);
    if(rx->volume>1.0) rx->volume=1.0;
    if(rx->volume<0.0) rx->volume=0.0;
    receiver_set_volume(rx);
  } else {
    rx->volume=ex/100.0;
    if(rx->volume>1.0) rx->volume=1.0;
    if(rx->volume<0.0) rx->volume=0.0;
    receiver_set_volume(rx);
  }
  pressed=FALSE;
  has_moved=FALSE;
  update_vfo(rx);
  return TRUE;
}

static gboolean afgain_scale_scroll_event_cb(GtkEventControllerScroll *ctrl,double dx,double dy,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  if((dy>0.0)) {
    if(rx->volume>0.0) {
      rx->volume=rx->volume-0.01;
    }
  } else if((dy<0.0)) {
    if(rx->volume<1.0) {
      rx->volume=rx->volume+0.01;
    }
  }
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->afgain_scale),rx->volume);
  receiver_set_volume(rx);
  return TRUE;
}

static gboolean agcgain_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)data;
  pressed=TRUE;
  last_x=ex;
  has_moved=FALSE;
  return TRUE;
}

static gboolean agcgain_scale_motion_notify_event_cb(GtkEventControllerMotion *ctrl, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(pressed) {
    gdouble moved=ex-last_x;
    if (moved > 1 && moved < -1) {
      has_moved=TRUE;
    }
    rx->agc_gain=rx->agc_gain+moved*1.4;
    if(rx->agc_gain>120.0) rx->agc_gain=120.0;
    if(rx->agc_gain<-20.0) rx->agc_gain=-20.0;
    receiver_set_agc_gain(rx);
    last_x=ex;
    update_vfo(rx);
  }
  return TRUE;
}

static gboolean agcgain_release_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)data;
  if(has_moved) {
    gdouble moved=ex-last_x;
    rx->agc_gain=rx->agc_gain+moved*1.4;
    if(rx->agc_gain>120.0) rx->agc_gain=120.0;
    if(rx->agc_gain<-20.0) rx->agc_gain=-20.0;
    receiver_set_agc_gain(rx);
  } else {
    rx->agc_gain=ex*1.4-20.0;
    if(rx->agc_gain>120.0) rx->agc_gain=120.0;
    if(rx->agc_gain<-20.0) rx->agc_gain=-20.0;
    receiver_set_agc_gain(rx);
  }
  pressed=FALSE;
  has_moved=FALSE;
  update_vfo(rx);
  return TRUE;
}

static gboolean agcgain_scale_scroll_event_cb(GtkEventControllerScroll *ctrl,double dx,double dy,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  if((dy>0.0)) {
    if(rx->agc_gain>-20.0) {
      rx->agc_gain=rx->agc_gain-1.0;
    }
  } if((dy<0.0)) {
    if(rx->agc_gain<120.0) {
      rx->agc_gain=rx->agc_gain+1.0;
    }
  }
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->agcgain_scale),rx->agc_gain+20.0);
  receiver_set_agc_gain(rx);
  return TRUE;
}

//**********************************************************************************
//**********************************************************************************
static gboolean squelch_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)data;
  pressed=TRUE;
  last_x=ex;
  has_moved=FALSE;
  return TRUE;
}

static gboolean squelch_scale_motion_notify_event_cb(GtkEventControllerMotion *ctrl, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(pressed) {
    gdouble moved=ex-last_x;
    if (moved > 1 && moved < -1) {
      has_moved=TRUE;
    }
    rx->squelch = rx->squelch + (moved/100.0);
    if(rx->squelch > 1.0) rx->squelch= 1.0;
    if(rx->squelch < 0) rx->squelch = 0;
    set_squelch(rx);
    last_x=ex;
    update_vfo(rx);
  }
  return TRUE;
}

static gboolean squelch_release_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)data;
  if(has_moved) {
    gdouble moved=ex-last_x;
    rx->squelch = rx->squelch + (moved/100);
    if(rx->squelch>1.0) rx->squelch = 1.0;
    if(rx->squelch<0.0) rx->squelch = 0.0;
    set_squelch(rx);
  } else {
    rx->squelch = ex/100;
    if(rx->squelch>1.0) rx->squelch = 1.0;
    if(rx->squelch<0.0) rx->squelch = 0.0;
    set_squelch(rx);
  }
  pressed=FALSE;
  has_moved=FALSE;
  update_vfo(rx);
  return TRUE;
}

static gboolean squelch_scale_scroll_event_cb(GtkEventControllerScroll *ctrl,double dx,double dy,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  if((dy>0.0)) {
    if(rx->squelch>0.0) {
      rx->squelch=rx->squelch-0.01;
    }
  } else if((dy<0.0)) {
    if(rx->squelch<1.0) {
      rx->squelch = rx->squelch+0.01;
    }
  }
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->squelch_scale),rx->squelch);
  set_squelch(rx);
  return TRUE;
}

//**********************************************************************************
//**********************************************************************************
void band_cb(GtkWidget *menu_item,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  set_band(choice->rx,choice->selection,choice->sub_selection);
  update_vfo(choice->rx);
  g_free(choice);
}

// Set an absolute tune frequency (Hz), respecting the current tuning mode: in
// ctun/freetune the moved value is ctun_frequency (+hz) and for a normal VFO it
// is frequency_a (-hz); VFO B always adds. Reuses receiver_move[_b] so the
// [0, 6 GHz] guards apply. Computes the delta from the currently displayed value.
static void vfo_apply_frequency(RECEIVER *rx, long long target, gboolean is_b) {
  if(rx==NULL || rx->locked) return;
  if(is_b) {
    long long want=target-rx->frequency_b;
    receiver_move_b(rx,want,FALSE,FALSE);
    frequency_changed(rx);
  } else {
    long long f=(rx->ctun || rx->freetune)?rx->ctun_frequency:rx->frequency_a;
    long long want=target-f;
    if(rx->ctun || rx->freetune) receiver_move(rx,want,FALSE);
    else                         receiver_move(rx,-want,FALSE);
  }
  update_vfo(rx);
}

// Parse a typed frequency in MHz into Hz. Accepts '.' or ',' as the decimal
// point and tolerates the VFO's grouped "14.074.000" style (only the first
// separator is treated as the decimal point; later ones are dropped but their
// digits kept, so "14.074.000" -> 14.074 MHz). Returns -1 on no usable number.
static long long vfo_parse_freq(const char *s) {
  char buf[64]; int j=0; gboolean have_dot=FALSE;
  for(int i=0; s[i] && j<63; i++) {
    char c=s[i];
    if(c=='.' || c==',') { if(!have_dot){ buf[j++]='.'; have_dot=TRUE; } }
    else if(c>='0' && c<='9') buf[j++]=c;
    /* skip spaces and any other characters */
  }
  buf[j]='\0';
  if(j==0) return -1;
  double mhz=g_ascii_strtod(buf,NULL);
  if(mhz<=0.0) return -1;
  return (long long)(mhz*1e6 + 0.5);
}

// GtkEntry "activate" (Enter): apply the typed frequency and close the popover.
static void freq_entry_activate_cb(GtkEntry *entry, gpointer data) {
  RECEIVER *rx=(RECEIVER *)g_object_get_data(G_OBJECT(entry),"rx");
  GtkWidget *pop=(GtkWidget *)g_object_get_data(G_OBJECT(entry),"pop");
  gboolean is_b=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry),"is_b"));
  long long hz=vfo_parse_freq(gtk_editable_get_text(GTK_EDITABLE(entry)));
  if(hz>0) vfo_apply_frequency(rx,hz,is_b);
  if(pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

// Small pop-up panel over a VFO frequency label: a text field (pre-focused) to
// type a full frequency in MHz, applied on Enter. The popover is modal, so it
// grabs the keyboard, closes on Esc / click-outside, and unparents itself.
static void vfo_show_freq_entry(RECEIVER *rx, GtkWidget *relto, gboolean is_b) {
  GtkWidget *pop=gtk_popover_new();
  gtk_widget_set_parent(pop,relto);
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
  gtk_widget_set_margin_start(box,6);  gtk_widget_set_margin_end(box,6);
  gtk_widget_set_margin_top(box,6);    gtk_widget_set_margin_bottom(box,6);
  GtkWidget *entry=gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry),"14.074");
  gtk_editable_set_max_width_chars(GTK_EDITABLE(entry),12);
  gtk_entry_set_input_purpose(GTK_ENTRY(entry),GTK_INPUT_PURPOSE_NUMBER);

  // Pre-fill with the current frequency (MHz), matching what the VFO shows:
  // ctun_frequency in ctun/freetune, frequency_a otherwise, frequency_b for B.
  // Trim trailing zeros (and a bare trailing dot) for a clean "14.074".
  long long cur = is_b ? rx->frequency_b
                       : ((rx->ctun || rx->freetune) ? rx->ctun_frequency
                                                     : rx->frequency_a);
  char txt[32];
  snprintf(txt,sizeof(txt),"%.6f",(double)cur/1e6);
  { int e=(int)strlen(txt);
    while(e>0 && txt[e-1]=='0') e--;
    if(e>0 && txt[e-1]=='.') e--;
    txt[e]='\0'; }
  gtk_editable_set_text(GTK_EDITABLE(entry),txt);

  gtk_box_append(GTK_BOX(box),entry);
  gtk_box_append(GTK_BOX(box),gtk_label_new("MHz"));
  gtk_popover_set_child(GTK_POPOVER(pop),box);
  g_object_set_data(G_OBJECT(entry),"rx",rx);
  g_object_set_data(G_OBJECT(entry),"pop",pop);
  g_object_set_data(G_OBJECT(entry),"is_b",GINT_TO_POINTER(is_b));
  g_signal_connect(entry,"activate",G_CALLBACK(freq_entry_activate_cb),NULL);
  g_signal_connect_swapped(pop,"closed",G_CALLBACK(gtk_widget_unparent),pop);
  gtk_popover_popup(GTK_POPOVER(pop));
  gtk_widget_grab_focus(entry);
  gtk_editable_select_region(GTK_EDITABLE(entry),0,-1);  // typing replaces it
}

static gboolean frequency_a_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  // Primary click opens the type-in frequency panel; the band-stack menu moves
  // to the secondary (right) button.
  if(button==GDK_BUTTON_PRIMARY) {
    if(!rx->locked) vfo_show_freq_entry(rx,widget,FALSE);
    return TRUE;
  }
  GtkWidget *menu=gtk_menu_new();
  GtkWidget *band_menu;
  GtkWidget *menu_item;
  CHOICE *choice;
  BAND *band;
  BANDSTACK *bandstack;
  BANDSTACK_ENTRY *entry;
  char temp[64];
  int i,j;

  if(!rx->locked) {
    menu=gtk_menu_new();
    for(i=0;i<BANDS+XVTRS;i++) {
#ifdef SOAPYSDR
      if(radio->discovered->protocol!=PROTOCOL_SOAPYSDR) {
        if(i>=band70 && i<=bandAIR) {
          continue;
        }
      }
#endif
      band=(BAND*)band_get_band(i);
      bandstack=band->bandstack;

      if(strlen(band->title)>0) {
        // GTK4: no submenus in this flat popover — the band title is a
        // non-clickable header and its entries follow it in the same popover.
        GtkWidget *band_menu=menu;
        menu_item=gtk_menu_item_new_with_label(band->title);
        gtk_widget_set_sensitive(menu_item,FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),menu_item);

	for(j=0;j<bandstack->entries;j++) {
          entry=&bandstack->entry[j];
	  sprintf(temp,"%05lld.%03lld.%03lld",entry->frequency/(long long)1000000,(entry->frequency%(long long)1000000)/(long long)1000,entry->frequency%(long long)1000);
          menu_item=gtk_menu_item_new_with_label(temp);
          choice=g_new0(CHOICE,1);
          choice->rx=rx;
          choice->selection=i;
          choice->sub_selection=j;
          g_signal_connect(menu_item,"clicked",G_CALLBACK(band_cb),choice);
          gtk_menu_shell_append(GTK_MENU_SHELL(band_menu),menu_item);
	}
      }
    }
#if GTK_CHECK_VERSION(3,22,0)
    gtk_menu_popup_at_pointer(GTK_MENU(menu),(GdkEvent *)event);
#else
    gtk_menu_popup(GTK_MENU(menu),NULL,NULL,NULL,NULL,NULL,NULL);
#endif
  }
  return TRUE;
}

static gboolean frequency_a_scroll_event_cb(GtkEventControllerScroll *ctrl,double dx,double dy,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  int digit;

  // Trackpad-aware: 1 notch per wheel detent, threshold-accumulated notches for
  // a trackpad so smooth scroll doesn't over-tune (n>1 on a fast flick).
  int n=scroll_notches(ctrl,dy);
  if(!rx->locked && n!=0) {
    digit=freq_digit_at(v->frequency_a_text,freq_hover_x);
    long long step=0LL;
    if(digit>=0 && digit<13) {
      step=ll_step[digit]*(long long)(n<0?-n:n);
    }
    // Notch up (n<0) tunes up, down (n>0) tunes down. receiver_move SUBTRACTS
    // its argument for a normal VFO but ADDS it in ctun/freetune, so the sign to
    // negate depends on the mode to keep the direction consistent.
    gboolean adds = rx->ctun || rx->freetune;
    if((n>0 && !adds) || (n<0 && adds)) {
      step=-step;
    }
//g_print("%s: digit=%d step=%lld\n",__FUNCTION__,digit,step);
    receiver_move(rx,step,FALSE);
  }
  update_vfo(rx);
  return TRUE;
}

static gboolean frequency_a_motion_notify_event_cb(GtkEventControllerMotion *ctrl, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  freq_hover_x=ex;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  int digit;

  if(!rx->locked) {
    digit=freq_digit_at(v->frequency_a_text,freq_hover_x);
    freq_hover_rx=rx; freq_hover_is_b=FALSE;
    freq_hover_digit=(digit>=0 && digit<13)?digit:-1;
    if(digit>=0 && digit<13) {
      long long step=ll_step[digit];
      if(step==0LL) {
        gtk_widget_set_cursor_from_name(v->frequency_a_text,"default");
      } else {
        gtk_widget_set_cursor_from_name(v->frequency_a_text,"ew-resize");
      }
    } else {
      gtk_widget_set_cursor_from_name(v->frequency_a_text,"default");
    }
 }
 return TRUE;
}

// Pointer left a frequency label: forget the hover so a stray digit key press
// doesn't retarget the last-hovered digit.
static void frequency_leave_cb(GtkEventControllerMotion *ctrl,gpointer data) {
  freq_hover_rx=NULL;
  freq_hover_digit=-1;
}

// Same, for a receiver that is being freed rather than merely left: the pointer
// can still be sitting over a digit of the panel the operator just closed, and
// the next digit key press would type into freed memory. Called from
// receiver_destroy(); the "leave" event does not necessarily arrive first.
void vfo_forget_receiver(RECEIVER *rx) {
  if(freq_hover_rx==rx) {
    freq_hover_rx=NULL;
    freq_hover_digit=-1;
  }
}

static gboolean frequency_b_press_cb(GtkGestureClick *gesture,int n_press,double ex,double ey,gpointer user_data) { GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)); guint button=gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)); (void)widget;(void)button;(void)ex;(void)ey;
  RECEIVER *rx=(RECEIVER *)user_data;
  if(button==GDK_BUTTON_PRIMARY && !rx->locked) {
    vfo_show_freq_entry(rx,widget,TRUE);
  }
  return TRUE;
}

static gboolean frequency_b_scroll_event_cb(GtkEventControllerScroll *ctrl,double dx,double dy,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  int digit;

  int n=scroll_notches(ctrl,dy);
  if(!rx->locked && n!=0) {
    digit=freq_digit_at(v->frequency_b_text,freq_hover_x);
    long long step=0LL;
    if(digit>=0 && digit<13) {
      step=ll_step[digit]*(long long)(n<0?-n:n);
    }
    // receiver_move_b ADDS its argument (VFO A's receiver_move subtracts), so
    // to move the same way as VFO A for a given scroll direction the sign is
    // flipped on notch-up (n<0): up tunes up, down tunes down.
    if((n<0)) {
      step=-step;
    }
    switch(rx->split) {
      case SPLIT_OFF:
      case SPLIT_ON:
        receiver_move_b(rx,step,FALSE,FALSE);
        break;
      case SPLIT_SAT:
      case SPLIT_RSAT:
        receiver_move_b(rx,step,FALSE,FALSE);
        break;
    }
  }
  frequency_changed(rx);
  update_vfo(rx);
  return TRUE;
}

static gboolean frequency_b_motion_notify_event_cb(GtkEventControllerMotion *ctrl, double ex, double ey, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  freq_hover_x=ex;
  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");
  int digit;

  if(!rx->locked) {
    digit=freq_digit_at(v->frequency_b_text,freq_hover_x);
    freq_hover_rx=rx; freq_hover_is_b=TRUE;
    freq_hover_digit=(digit>=0 && digit<13)?digit:-1;
    if(digit>=0 && digit<13) {
      long long step=ll_step[digit];
      if(step==0LL) {
        gtk_widget_set_cursor_from_name(v->frequency_b_text,"default");
      } else {
        gtk_widget_set_cursor_from_name(v->frequency_b_text,"ew-resize");
      }
    } else {
      gtk_widget_set_cursor_from_name(v->frequency_b_text,"default");
    }
 }
 return TRUE;
}

// SDR#-style digit entry: if a numeric key is pressed while the cursor hovers a
// VFO digit, overwrite that digit in place and advance the "cursor" one digit to
// the right so consecutive keystrokes fill left-to-right. Returns TRUE if the
// key was consumed. Called from the global key handler (receiver_key_pressed).
gboolean vfo_type_digit(guint keyval) {
  int n;
  if(keyval>=GDK_KEY_0 && keyval<=GDK_KEY_9)        n=(int)(keyval-GDK_KEY_0);
  else if(keyval>=GDK_KEY_KP_0 && keyval<=GDK_KEY_KP_9) n=(int)(keyval-GDK_KEY_KP_0);
  else return FALSE;

  RECEIVER *rx=freq_hover_rx;
  int digit=freq_hover_digit;
  if(rx==NULL || rx->locked || digit<0 || digit>=13) return FALSE;

  long long place=ll_step[digit];
  if(place==0LL) return FALSE;   // cursor is over a '.' separator

  // Current displayed value for the hovered VFO (mirrors update_vfo()).
  long long f;
  if(freq_hover_is_b) {
    f=rx->frequency_b;
  } else {
    f=(rx->ctun || rx->freetune)?rx->ctun_frequency:rx->frequency_a;
  }

  long long cur=(f/place)%10;
  long long want=((long long)n-cur)*place;  // desired change in displayed freq

  // Sign to pass depends on how the mover applies its argument:
  //   receiver_move_b (VFO B) ADDS hz
  //   receiver_move   (VFO A) SUBTRACTS hz for a normal VFO, but ADDS it in
  //                   ctun/freetune (it moves ctun_frequency there).
  if(freq_hover_is_b) {
    receiver_move_b(rx,want,FALSE,FALSE);
    frequency_changed(rx);
  } else if(rx->ctun || rx->freetune) {
    receiver_move(rx,want,FALSE);
  } else {
    receiver_move(rx,-want,FALSE);
  }

  // Advance to the next editable digit on the right (skip '.' separators).
  int nd=digit+1;
  while(nd<13 && ll_step[nd]==0LL) nd++;
  if(nd<13) freq_hover_digit=nd;

  update_vfo(rx);
  return TRUE;
}

GtkWidget *create_vfo(RECEIVER *rx) {
  char temp[32];

  log_info("%s: rx=%d\n",__FUNCTION__,rx->channel);

  VFO_DATA *v=g_new(VFO_DATA,1);

  v->vfo=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
  gtk_widget_set_margin_start(v->vfo,10);

  GtkWidget *vfo_row_top=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
  GtkWidget *vfo_row_freq=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
  GtkWidget *vfo_row_ctl=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,3);
  gtk_box_append(GTK_BOX(v->vfo),vfo_row_top);
  gtk_box_append(GTK_BOX(v->vfo),vfo_row_freq);
  gtk_box_append(GTK_BOX(v->vfo),vfo_row_ctl);

  gtk_widget_set_name(v->vfo,"vfo");

  v->vfo_a_text=gtk_label_new("VFO A");
  gtk_widget_set_name(v->vfo_a_text,"vfo-a-text");
  gtk_box_append(GTK_BOX(vfo_row_top),v->vfo_a_text);

  v->a2b=gtk_button_new_with_label("A>B");
  gtk_widget_set_name(v->a2b,"vfo-button");
  g_signal_connect(v->a2b, "clicked", G_CALLBACK(a2b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_top),v->a2b);

  v->b2a=gtk_button_new_with_label("A<B");
  gtk_widget_set_name(v->b2a,"vfo-button");
  g_signal_connect(v->b2a, "clicked", G_CALLBACK(b2a_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_top),v->b2a);

  v->aswapb=gtk_button_new_with_label("A<>B");
  gtk_widget_set_name(v->aswapb,"vfo-button");
  g_signal_connect(v->aswapb, "clicked", G_CALLBACK(aswapb_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_top),v->aswapb);

  switch(rx->split) {
    case SPLIT_OFF:
    case SPLIT_ON:
      strcpy(temp, "SPLIT");
      break;
    case SPLIT_SAT:
      strcpy(temp, "SAT");
      break;
    case SPLIT_RSAT:
      strcpy(temp, "RSAT");
      break;
  }
  v->split_b=gtk_toggle_button_new_with_label(temp);
  gtk_widget_set_name(v->split_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->split_b),rx->split!=SPLIT_OFF);
  g_signal_connect(v->split_b, "toggled", G_CALLBACK(split_b_cb),rx);
  // Right-click opens the SPLIT/SAT/RSAT menu (left-click is the "toggled"
  // handler above). GtkButton has no "button_press_event" in GTK4, so use a
  // secondary-button gesture — restricting to button 3 avoids double-handling
  // the primary click that already drives "toggled".
  { GtkGesture *_g=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(_g),GDK_BUTTON_SECONDARY); g_signal_connect(_g,"pressed",G_CALLBACK(split_b_press_cb),rx); gtk_widget_add_controller(v->split_b,GTK_EVENT_CONTROLLER(_g)); }
  gtk_box_append(GTK_BOX(vfo_row_top),v->split_b);

  v->vfo_b_text=gtk_label_new("VFO B");
  gtk_widget_set_name(v->vfo_b_text,"vfo-b-text");
  gtk_box_append(GTK_BOX(vfo_row_top),v->vfo_b_text);

  sprintf(temp,"ZOOM x%d",rx->zoom);
  v->zoom_b=gtk_button_new_with_label(temp);
  gtk_widget_set_name(v->zoom_b,"vfo-button");
  g_signal_connect(v->zoom_b, "clicked",G_CALLBACK(zoom_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_top),v->zoom_b);

  sprintf(temp,"STEP %s",step_labels[get_step(rx->step)]);
  v->step_b=gtk_button_new_with_label(temp);
  gtk_widget_set_name(v->step_b,"vfo-button");
  g_signal_connect(v->step_b, "clicked",G_CALLBACK(step_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_top),v->step_b);

  v->tx_label=gtk_label_new("");
  gtk_widget_set_name(v->tx_label,"warning-label");
  gtk_box_append(GTK_BOX(vfo_row_top),v->tx_label);
  if(radio!=NULL && radio->transmitter!=NULL) {
    if(radio->transmitter->rx==rx) {
      gtk_label_set_text(GTK_LABEL(v->tx_label),"ASSIGNED TX");
    }
  }

  /* ... */

  long long af=rx->frequency_a;
  if(rx->ctun) af=rx->ctun_frequency;
  if(rx->entering_frequency) af=rx->entered_frequency;

  sprintf(temp,"%05lld.%03lld.%03lld",af/(long long)1000000,(af%(long long)1000000)/(long long)1000,af%(long long)1000);
  v->frequency_a_text=gtk_label_new(temp);
  rx->vfo_a_digits=strlen(temp);
  gtk_widget_set_name(v->frequency_a_text,"frequency-a-text");
  gtk_label_set_width_chars(GTK_LABEL(v->frequency_a_text),rx->vfo_a_digits);

  gtk_box_append(GTK_BOX(vfo_row_freq),v->frequency_a_text);
  vfo_attach_ctl(v->frequency_a_text, rx, G_CALLBACK(frequency_a_press_cb), NULL,
                 G_CALLBACK(frequency_a_motion_notify_event_cb), G_CALLBACK(frequency_a_scroll_event_cb));
  { GtkEventController *lm=gtk_event_controller_motion_new();
    g_signal_connect(lm,"leave",G_CALLBACK(frequency_leave_cb),rx);
    gtk_widget_add_controller(v->frequency_a_text,lm); }

  long long bf=rx->frequency_b;
  sprintf(temp,"%05lld.%03lld.%03lld",bf/(long long)1000000,(bf%(long long)1000000)/(long long)1000,bf%(long long)1000);
  v->frequency_b_text=gtk_label_new(temp);
  rx->vfo_b_digits=strlen(temp);
  gtk_widget_set_name(v->frequency_b_text,"frequency-b-text");
  gtk_label_set_width_chars(GTK_LABEL(v->frequency_b_text),rx->vfo_b_digits);

  gtk_box_append(GTK_BOX(vfo_row_freq),v->frequency_b_text);
  vfo_attach_ctl(v->frequency_b_text, rx, G_CALLBACK(frequency_b_press_cb), NULL,
                 G_CALLBACK(frequency_b_motion_notify_event_cb), G_CALLBACK(frequency_b_scroll_event_cb));
  { GtkEventController *lm=gtk_event_controller_motion_new();
    g_signal_connect(lm,"leave",G_CALLBACK(frequency_leave_cb),rx);
    gtk_widget_add_controller(v->frequency_b_text,lm); }

  v->subrx_b=gtk_toggle_button_new_with_label("SUBRX");
  gtk_widget_set_name(v->subrx_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->subrx_b),FALSE);
  g_signal_connect(v->subrx_b, "toggled", G_CALLBACK(subrx_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_freq),v->subrx_b);

  GtkWidget *afgain_label=gtk_label_new("AF GAIN");
  gtk_widget_set_name(afgain_label,"afgain-text");
  gtk_box_append(GTK_BOX(vfo_row_freq),afgain_label);

  v->afgain_scale=gtk_level_bar_new();
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->afgain_scale),GTK_LEVEL_BAR_OFFSET_LOW);
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->afgain_scale),GTK_LEVEL_BAR_OFFSET_HIGH);
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->afgain_scale),GTK_LEVEL_BAR_OFFSET_FULL);
  gtk_widget_set_name(v->afgain_scale,"afgain-scale");
  gtk_widget_set_size_request(v->afgain_scale,100,15);
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->afgain_scale),rx->volume);

  gtk_box_append(GTK_BOX(vfo_row_freq),v->afgain_scale);
  vfo_attach_ctl(v->afgain_scale, rx, G_CALLBACK(afgain_press_cb), G_CALLBACK(afgain_release_cb),
                 G_CALLBACK(afgain_scale_motion_notify_event_cb), G_CALLBACK(afgain_scale_scroll_event_cb));

  v->mute_b=gtk_toggle_button_new();
  gtk_widget_set_name(v->mute_b,"vfo-toggle");
  gtk_widget_set_tooltip_text(v->mute_b,"Mute this receiver");
  vfo_set_mute_icon(v->mute_b,rx->mute);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->mute_b),rx->mute);
  g_signal_connect(v->mute_b, "toggled", G_CALLBACK(mute_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_freq),v->mute_b);

  v->squelch_label=gtk_label_new("SQL");
  gtk_widget_set_name(v->squelch_label,"squelch-text");
  gtk_box_append(GTK_BOX(vfo_row_freq),v->squelch_label);

  v->squelch_scale=gtk_level_bar_new();
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->squelch_scale),GTK_LEVEL_BAR_OFFSET_LOW);
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->squelch_scale),GTK_LEVEL_BAR_OFFSET_HIGH);
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->squelch_scale),GTK_LEVEL_BAR_OFFSET_FULL);
  gtk_widget_set_name(v->squelch_scale,"squelch-scale");
  gtk_widget_set_size_request(v->squelch_scale,100,15);
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->squelch_scale),rx->squelch);

  gtk_box_append(GTK_BOX(vfo_row_freq),v->squelch_scale);
  vfo_attach_ctl(v->squelch_scale, rx, G_CALLBACK(squelch_press_cb), G_CALLBACK(squelch_release_cb),
                 G_CALLBACK(squelch_scale_motion_notify_event_cb), G_CALLBACK(squelch_scale_scroll_event_cb));

  GtkWidget *agcgain_label=gtk_label_new("AGC GAIN");
  gtk_widget_set_name(agcgain_label,"agcgain-text");
  gtk_box_append(GTK_BOX(vfo_row_freq),agcgain_label);

  v->agcgain_scale=gtk_level_bar_new_for_interval(0.0,140.0);
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->agcgain_scale),GTK_LEVEL_BAR_OFFSET_LOW);
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->agcgain_scale),GTK_LEVEL_BAR_OFFSET_HIGH);
  gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(v->agcgain_scale),GTK_LEVEL_BAR_OFFSET_FULL);
  gtk_widget_set_name(v->agcgain_scale,"agcgain-scale");
  gtk_widget_set_size_request(v->agcgain_scale,100,15);
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->agcgain_scale),rx->agc_gain+20.0);

  gtk_box_append(GTK_BOX(vfo_row_freq),v->agcgain_scale);
  vfo_attach_ctl(v->agcgain_scale, rx, G_CALLBACK(agcgain_press_cb), G_CALLBACK(agcgain_release_cb),
                 G_CALLBACK(agcgain_scale_motion_notify_event_cb), G_CALLBACK(agcgain_scale_scroll_event_cb));

  v->lock_b=gtk_toggle_button_new_with_label("LOCK");
  gtk_widget_set_name(v->lock_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->lock_b),FALSE);
  g_signal_connect(v->lock_b, "toggled", G_CALLBACK(lock_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->lock_b);

  v->mode_b=gtk_button_new_with_label(mode_string[rx->mode_a]);
  gtk_widget_set_name(v->mode_b,"vfo-mode-filter-button");
  g_signal_connect(v->mode_b, "clicked", G_CALLBACK(mode_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->mode_b);

  FILTER *band_filters=filters[rx->mode_a];

  if(rx->mode_a==FMN) {
    if(rx->deviation==2500) {
      strcpy(temp,"8000");
    } else {
      strcpy(temp,"16000");
    }
  } else {
    strcpy(temp,band_filters[rx->filter_a].title);
  }

  v->filter_b=gtk_button_new_with_label(temp);
  gtk_widget_set_name(v->filter_b,"vfo-mode-filter-button");
  g_signal_connect(v->filter_b, "clicked", G_CALLBACK(filter_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->filter_b);

  strcpy(temp,"NB");
  if(rx->nb2) {
    strcpy(temp,"NB2");
  }
  v->nb_b=gtk_toggle_button_new_with_label(temp);
  gtk_widget_set_name(v->nb_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->nb_b),rx->nb|rx->nb2);
  { GtkGesture *_g=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(_g),0); g_signal_connect(_g,"pressed",G_CALLBACK(nb_b_pressed_cb),rx); gtk_widget_add_controller(v->nb_b,GTK_EVENT_CONTROLLER(_g)); }
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->nb_b);

  v->nr_b=gtk_toggle_button_new_with_label("NR");
  gtk_widget_set_name(v->nr_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->nr_b),rx->nr|rx->nr2|rx->nr3|rx->nr4);
  { GtkGesture *_g=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(_g),0); g_signal_connect(_g,"pressed",G_CALLBACK(nr_b_pressed_cb),rx); gtk_widget_add_controller(v->nr_b,GTK_EVENT_CONTROLLER(_g)); }
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->nr_b);

  v->snb_b=gtk_toggle_button_new_with_label("SNB");
  gtk_widget_set_name(v->snb_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->snb_b),rx->snb);
  g_signal_connect(v->snb_b, "toggled", G_CALLBACK(snb_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->snb_b);

  v->anf_b=gtk_toggle_button_new_with_label("ANF");
  gtk_widget_set_name(v->anf_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->anf_b),rx->anf);
  g_signal_connect(v->anf_b, "toggled", G_CALLBACK(anf_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->anf_b);

  switch(rx->agc) {
    case AGC_OFF:
      strcpy(temp,"AGC");
      break;
    case AGC_LONG:
      strcpy(temp,"AGC LONG");
      break;
    case AGC_SLOW:
      strcpy(temp,"AGC SLOW");
      break;
    case AGC_MEDIUM:
      strcpy(temp,"AGC MED");
      break;
    case AGC_FAST:
      strcpy(temp,"AGC FAST");
      break;
  }
  v->agc_b=gtk_toggle_button_new_with_label(temp);
  gtk_widget_set_name(v->agc_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->agc_b),rx->agc!=AGC_OFF);
  { GtkGesture *_g=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(_g),0); g_signal_connect(_g,"pressed",G_CALLBACK(agc_b_pressed_cb),rx); gtk_widget_add_controller(v->agc_b,GTK_EVENT_CONTROLLER(_g)); }
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->agc_b);

  // RIT button + value packed into one bordered "value group" (spacing 0): the
  // toggle button (left-rounded, flush to the group's top/left/bottom edges)
  // and the offset value share a single outer rounded rectangle.
  GtkWidget *rit_group=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_name(rit_group,"vfo-value-group");
  gtk_box_append(GTK_BOX(vfo_row_ctl),rit_group);

  v->rit_b=gtk_toggle_button_new_with_label("RIT");
  gtk_widget_set_name(v->rit_b,"vfo-toggle-seg");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->rit_b),rx->rit_enabled);
  g_signal_connect(v->rit_b, "toggled", G_CALLBACK(rit_b_cb),rx);
  { GtkGesture *_g=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(_g),0); g_signal_connect(_g,"pressed",G_CALLBACK(rit_b_press_cb),rx); gtk_widget_add_controller(v->rit_b,GTK_EVENT_CONTROLLER(_g)); }
  gtk_box_append(GTK_BOX(rit_group),v->rit_b);

  sprintf(temp,"%+05lld",rx->rit);
  v->rit_value=gtk_label_new(temp);
  gtk_widget_set_name(v->rit_value,"rit-value");

  gtk_box_append(GTK_BOX(rit_group),v->rit_value);
  vfo_attach_ctl(v->rit_value, rx, NULL, NULL, NULL, G_CALLBACK(rit_b_scroll_event_cb));

  GtkWidget *xit_group=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_name(xit_group,"vfo-value-group");
  gtk_box_append(GTK_BOX(vfo_row_ctl),xit_group);

  v->xit_b=gtk_toggle_button_new_with_label("XIT");
  gtk_widget_set_name(v->xit_b,"vfo-toggle-seg");
  if(radio->transmitter!=NULL && radio->transmitter->rx==rx) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->xit_b),radio->transmitter->xit_enabled);
  }
  g_signal_connect(v->xit_b, "toggled", G_CALLBACK(xit_b_cb),rx);
  { GtkGesture *_g=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(_g),0); g_signal_connect(_g,"pressed",G_CALLBACK(xit_b_press_cb),rx); gtk_widget_add_controller(v->xit_b,GTK_EVENT_CONTROLLER(_g)); }
  gtk_box_append(GTK_BOX(xit_group),v->xit_b);


  if(radio->transmitter!=NULL) {
    sprintf(temp,"%+05lld",radio->transmitter->xit);
  } else {
    sprintf(temp,"%+05ld",0L);
  }
  v->xit_value=gtk_label_new(temp);
  gtk_widget_set_name(v->xit_value,"xit-value");

  gtk_box_append(GTK_BOX(xit_group),v->xit_value);
  vfo_attach_ctl(v->xit_value, rx, NULL, NULL, NULL, G_CALLBACK(xit_b_scroll_event_cb));

  v->ctun_b=gtk_toggle_button_new_with_label("CTUN");
  gtk_widget_set_name(v->ctun_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->ctun_b),rx->ctun);
  g_signal_connect(v->ctun_b, "toggled", G_CALLBACK(ctun_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->ctun_b);


  v->dup_b=gtk_toggle_button_new_with_label("DUP");
  gtk_widget_set_name(v->dup_b,"vfo-toggle");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->dup_b),rx->duplex);
  g_signal_connect(v->dup_b, "toggled", G_CALLBACK(dup_b_cb),rx);
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->dup_b);


  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
    v->ant_b=gtk_toggle_button_new_with_label("RXANT");
    gtk_widget_set_name(v->ant_b,"vfo-toggle");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->ant_b),radio->adc[0].antenna!=0);
    g_signal_connect(v->ant_b, "toggled", G_CALLBACK(ant_b_cb),rx);
    gtk_box_append(GTK_BOX(vfo_row_ctl),v->ant_b);
  }
  else {
    v->bpsk_b=gtk_toggle_button_new_with_label("BPSK");
    gtk_widget_set_name(v->bpsk_b,"vfo-toggle");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->bpsk_b),rx->bpsk_enable);
    g_signal_connect(v->bpsk_b, "toggled", G_CALLBACK(bpsk_b_cb),rx);
    gtk_box_append(GTK_BOX(vfo_row_ctl),v->bpsk_b);
  }

  v->bmk_b=gtk_button_new_with_label("BMK");
  gtk_widget_set_name(v->bmk_b,"vfo-button");
  { GtkGesture *_g=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(_g),0); g_signal_connect(_g,"pressed",G_CALLBACK(bmk_b_pressed_cb),rx); gtk_widget_add_controller(v->bmk_b,GTK_EVENT_CONTROLLER(_g)); }
  gtk_box_append(GTK_BOX(vfo_row_ctl),v->bmk_b);

  // Diversity on/off lives in Configure -> Diversity now (a checkbox), not on the
  // VFO — you open settings to tune its gain/phase anyway. See diversity_dialog.c.

  gtk_widget_set_visible(v->vfo, TRUE);

  g_object_set_data ((GObject *)v->vfo,"vfo_data",v);
  return v->vfo;
}

// Format an active-skin palette color as "#rrggbb" (fallback if name unknown).
static void skin_hex(const char *name, char *out, const char *fallback) {
  double r,g,b;
  if(css_rgb(name,&r,&g,&b)) {
    sprintf(out,"#%02x%02x%02x",(int)(r*255.0+0.5),(int)(g*255.0+0.5),(int)(b*255.0+0.5));
  } else {
    g_strlcpy(out,fallback,8);
  }
}

// Pango markup for a frequency string with its leading zeros drawn in a dim
// colour, so the eye lands on the significant digits (a classic radio look).
// s contains only digits and '.', so no markup escaping is needed.
static char *freq_markup(const char *s, const char *bright, const char *dim) {
  int n=(int)strlen(s), cut=0;
  while(cut<n && (s[cut]=='0' || s[cut]=='.')) cut++;
  if(cut>=n) cut=0;   // all-zero frequency: don't dim everything
  char pre[32];
  g_strlcpy(pre,s,cut+1);
  return g_markup_printf_escaped(
    "<span foreground=\"%s\">%s</span><span foreground=\"%s\">%s</span>",
    dim,pre,bright,s+cut);
}

void update_vfo(RECEIVER *rx) {
  char temp[32];
  char *markup;
  char accent_a[8],accent_b[8],dim_col[8],tx_col[8];
  skin_hex("ACCENT_A",accent_a,"#A3CCD1");
  skin_hex("ACCENT_B",accent_b,"#ED9D80");
  skin_hex("DARK_TEXT",dim_col,"#5A5A5A");
  skin_hex("WARNING",tx_col,"#D94545");

  if(rx->vfo==NULL) return;

  VFO_DATA *v=(VFO_DATA *)g_object_get_data((GObject *)rx->vfo,"vfo_data");

  if(v==NULL) return;

  // VFO A
  long long af=rx->frequency_a;
  if(rx->ctun || rx->freetune) af=rx->ctun_frequency;
  if(rx->entering_frequency) af=rx->entered_frequency;

  sprintf(temp,"%05lld.%03lld.%03lld",af/(long long)1000000,(af%(long long)1000000)/(long long)1000,af%(long long)1000);
  if(radio!=NULL && radio->transmitter!=NULL && rx==radio->transmitter->rx && radio->transmitter->rx->split==SPLIT_OFF && isTransmitting(radio)) {
    markup=freq_markup(temp,tx_col,dim_col);
  } else {
    markup=freq_markup(temp,accent_a,dim_col);
  }
  gtk_label_set_markup(GTK_LABEL(v->frequency_a_text),markup);

  // VFO B
  if(radio!=NULL && radio->transmitter!=NULL && rx==radio->transmitter->rx && radio->transmitter->rx->split!=SPLIT_OFF && isTransmitting(radio)) {
    markup=g_markup_printf_escaped("<span foreground=\"#D94545\">%s</span>","VFO B");
  } else {
    markup=g_markup_printf_escaped("<span foreground=\"#ED9D80\">%s</span>","VFO B");
  }
  gtk_label_set_markup(GTK_LABEL(v->vfo_b_text),markup);

  long long bf=rx->frequency_b;
  sprintf(temp,"%05lld.%03lld.%03lld",bf/(long long)1000000,(bf%(long long)1000000)/(long long)1000,bf%(long long)1000);
  if(radio!=NULL && radio->transmitter!=NULL && rx==radio->transmitter->rx && radio->transmitter->rx->split!=SPLIT_OFF && isTransmitting(radio)) {
    markup=freq_markup(temp,tx_col,dim_col);
  } else {
    markup=freq_markup(temp,accent_b,dim_col);
  }
  gtk_label_set_markup(GTK_LABEL(v->frequency_b_text),markup);

  // ASSIGNED TX
  if(radio!=NULL && radio->transmitter!=NULL) {
    TRANSMITTER *tx=radio->transmitter;

    if(tx->rx==rx) {
      gtk_label_set_text(GTK_LABEL(v->tx_label),"ASSIGNED TX");
    } else {
      gtk_label_set_text(GTK_LABEL(v->tx_label),"");
    }

    g_signal_handlers_block_by_func(v->xit_b,G_CALLBACK(xit_b_cb),rx);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->xit_b),tx->xit_enabled);
    g_signal_handlers_unblock_by_func(v->xit_b,G_CALLBACK(xit_b_cb),rx);
  }

  // update AF Gain scale
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->afgain_scale),rx->volume);

  // update AGC Gain scale
  gtk_level_bar_set_value(GTK_LEVEL_BAR(v->agcgain_scale),rx->agc_gain+20.0);

  // update FM squelch
  if(rx->mode_a==FMN) {
    gtk_level_bar_set_value(GTK_LEVEL_BAR(v->squelch_scale),rx->squelch);
    gtk_label_set_text(GTK_LABEL(v->squelch_label),"SQL");
    gtk_widget_set_visible(v->squelch_scale, TRUE);
  }
  else {
      gtk_label_set_text(GTK_LABEL(v->squelch_label),"");
      gtk_widget_set_visible(v->squelch_scale, FALSE);
  }
  // update Lock button
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->lock_b),rx->locked);

  // update mode button
  gtk_button_set_label(GTK_BUTTON(v->mode_b),mode_string[rx->mode_a]);

  // update filter button
  FILTER *band_filters=filters[rx->mode_a];
  if(rx->mode_a==FMN) {
    if(rx->deviation==2500) {
      strcpy(temp,"8000");
    } else {
      strcpy(temp,"16000");
    }
  } else {
    strcpy(temp,band_filters[rx->filter_a].title);
  }
  gtk_button_set_label(GTK_BUTTON(v->filter_b),temp);

  // update NB button
  if(rx->nb) {
      gtk_button_set_label(GTK_BUTTON(v->nb_b),"NB");
  } else if(rx->nb2) {
      gtk_button_set_label(GTK_BUTTON(v->nb_b),"NB2");
  } else {
      gtk_button_set_label(GTK_BUTTON(v->nb_b),"NB");
  }
  g_signal_handlers_block_by_func(v->nb_b,G_CALLBACK(nb_b_pressed_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->nb_b),rx->nb|rx->nb2);
  g_signal_handlers_unblock_by_func(v->nb_b,G_CALLBACK(nb_b_pressed_cb),rx);

  // update NR button
  if(rx->nr) {
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR");
  } else if(rx->nr2) {
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR2");
  } else if(rx->nr3) {
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR3");
  } else if(rx->nr4) {
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR4");
  } else {
      gtk_button_set_label(GTK_BUTTON(v->nr_b),"NR");
  }
  g_signal_handlers_block_by_func(v->nr_b,G_CALLBACK(nr_b_pressed_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->nr_b),rx->nr|rx->nr2|rx->nr3|rx->nr4);
  g_signal_handlers_unblock_by_func(v->nr_b,G_CALLBACK(nr_b_pressed_cb),rx);

  // update SNB button
  g_signal_handlers_block_by_func(v->snb_b,G_CALLBACK(snb_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->snb_b),rx->snb);
  g_signal_handlers_unblock_by_func(v->snb_b,G_CALLBACK(snb_b_cb),rx);

  // update ANF button
  g_signal_handlers_block_by_func(v->anf_b,G_CALLBACK(anf_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->anf_b),rx->anf);
  g_signal_handlers_unblock_by_func(v->anf_b,G_CALLBACK(anf_b_cb),rx);

  // update AGC button
  switch(rx->agc) {
    case AGC_OFF:
      strcpy(temp,"AGC");
      break;
    case AGC_LONG:
      strcpy(temp,"AGC LONG");
      break;
    case AGC_SLOW:
      strcpy(temp,"AGC SLOW");
      break;
    case AGC_MEDIUM:
      strcpy(temp,"AGC MED");
      break;
    case AGC_FAST:
      strcpy(temp,"AGC FAST");
      break;
  }
  gtk_button_set_label(GTK_BUTTON(v->agc_b),temp);
  g_signal_handlers_block_by_func(v->agc_b,G_CALLBACK(agc_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->agc_b),rx->agc!=AGC_OFF);
  g_signal_handlers_unblock_by_func(v->agc_b,G_CALLBACK(agc_cb),rx);

  // update RIT button
  g_signal_handlers_block_by_func(v->rit_b,G_CALLBACK(rit_b_press_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->rit_b),rx->rit_enabled);
  g_signal_handlers_unblock_by_func(v->rit_b,G_CALLBACK(rit_b_press_cb),rx);

  // update XIT button
  if(radio->transmitter!=NULL && radio->transmitter->rx==rx) {
    g_signal_handlers_block_by_func(v->xit_b,G_CALLBACK(xit_b_press_cb),rx);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->xit_b),radio->transmitter->xit_enabled);
    g_signal_handlers_unblock_by_func(v->xit_b,G_CALLBACK(xit_b_press_cb),rx);
  }

  // update CTUN button
  g_signal_handlers_block_by_func(v->ctun_b,G_CALLBACK(ctun_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->ctun_b),rx->ctun);
  g_signal_handlers_unblock_by_func(v->ctun_b,G_CALLBACK(ctun_b_cb),rx);

  // update DUP button
  g_signal_handlers_block_by_func(v->dup_b,G_CALLBACK(dup_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->dup_b),rx->duplex);
  g_signal_handlers_unblock_by_func(v->dup_b,G_CALLBACK(dup_b_cb),rx);

  // update RXANT button
  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
    g_signal_handlers_block_by_func(v->ant_b,G_CALLBACK(ant_b_cb),rx);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->ant_b),radio->adc[0].antenna!=0);
    g_signal_handlers_unblock_by_func(v->ant_b,G_CALLBACK(ant_b_cb),rx);
  }
  // update BPSK button
  //g_signal_handlers_block_by_func(v->bpsk_b,G_CALLBACK(bpsk_b_cb),rx);
  //gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->bpsk_b),rx->bpsk_enable);
  //g_signal_handlers_unblock_by_func(v->bpsk_b,G_CALLBACK(bpsk_b_cb),rx);

  // update ZOOM button
  sprintf(temp,"ZOOM x%d",rx->zoom);
  gtk_button_set_label(GTK_BUTTON(v->zoom_b),temp);

  // update STEP button
  sprintf(temp,"STEP %s",step_labels[get_step(rx->step)]);
  gtk_button_set_label(GTK_BUTTON(v->step_b),temp);

  // update SPLIT button
  switch(rx->split) {
     case SPLIT_OFF:
     case SPLIT_ON:
       gtk_button_set_label(GTK_BUTTON(v->split_b),"SPLIT");
       break;
     case SPLIT_SAT:
       gtk_button_set_label(GTK_BUTTON(v->split_b),"SAT");
       break;
     case SPLIT_RSAT:
       gtk_button_set_label(GTK_BUTTON(v->split_b),"RSAT");
       break;
  }
  g_signal_handlers_block_by_func(v->split_b,G_CALLBACK(split_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->split_b),rx->split!=SPLIT_OFF);
  g_signal_handlers_unblock_by_func(v->split_b,G_CALLBACK(split_b_cb),rx);

  // SUBRX button
  g_signal_handlers_block_by_func(v->subrx_b,G_CALLBACK(subrx_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->subrx_b),rx->subrx!=NULL);
  g_signal_handlers_unblock_by_func(v->subrx_b,G_CALLBACK(subrx_b_cb),rx);

  // Mute button + icon. Synced here (blocked, so the sync cannot re-enter the
  // handler) because rx->mute is now settable from outside the UI - the TCI
  // mute/rx_mute commands - and a mute the button does not show reads as a dead
  // receiver.
  g_signal_handlers_block_by_func(v->mute_b,G_CALLBACK(mute_b_cb),rx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->mute_b),rx->mute);
  vfo_set_mute_icon(v->mute_b,rx->mute);
  g_signal_handlers_unblock_by_func(v->mute_b,G_CALLBACK(mute_b_cb),rx);
}
