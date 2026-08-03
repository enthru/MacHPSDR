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
#include <math.h>

#include <wdsp.h>

#include "discovered.h"
#include "bpsk.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "meter.h"
#include "main.h"
#include "vfo.h"
#include "level_meter.h"
#include "pana_view.h"

typedef struct _choice {
  RECEIVER *rx;
  int selection;
} CHOICE;

// S-meter peak-hold ("smax"), decayed once per frame in update_meter() and read
// by the snapshot builder. Shared across meters as in the original (one meter
// updates at a time); main-thread only.
static double s_meter_smax=0;

static void meter_build(GtkSnapshot *snapshot,int meter_width,int meter_height,gpointer data);

// Append a stroked circular arc (cx,cy,r) from a0..a1 rad as a short polyline.
static void meter_arc(GtkSnapshot *snapshot,double cx,double cy,double r,double a0,double a1,double lw,const GdkRGBA *c) {
  int seg=(int)(fabs(a1-a0)/0.05)+2;   // ~0.05 rad segments -> smooth
  GskPathBuilder *b=gsk_path_builder_new();
  for(int k=0;k<=seg;k++) {
    double a=a0+(a1-a0)*(double)k/(double)seg;
    double px=cx+r*cos(a), py=cy+r*sin(a);
    if(k==0) gsk_path_builder_move_to(b,(float)px,(float)py);
    else     gsk_path_builder_line_to(b,(float)px,(float)py);
  }
  GskPath *p=gsk_path_builder_free_to_path(b);
  GskStroke *st=gsk_stroke_new((float)lw);
  gtk_snapshot_append_stroke(snapshot,p,st,c);
  gsk_stroke_free(st); gsk_path_unref(p);
}

// A menu-item button in the choice popover applies its selection and dismisses.
static void meter_choice_clicked(GtkButton *b,gpointer data) {
  CHOICE *choice=(CHOICE *)data;
  choice->rx->smeter=choice->selection;
  GtkWidget *pop=gtk_widget_get_ancestor(GTK_WIDGET(b),GTK_TYPE_POPOVER);
  if(pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void meter_add_choice(GtkWidget *box,RECEIVER *rx,const char *label,int sel) {
  GtkWidget *b=gtk_button_new_with_label(label);
  gtk_widget_add_css_class(b,"flat");
  CHOICE *choice=g_new0(CHOICE,1);
  choice->rx=rx;
  choice->selection=sel;
  g_object_set_data_full(G_OBJECT(b),"choice",choice,g_free);
  g_signal_connect(b,"clicked",G_CALLBACK(meter_choice_clicked),choice);
  gtk_box_append(GTK_BOX(box),b);
}

// GTK4: GtkMenu is gone — a small GtkPopover of flat buttons is the context menu.
static void meter_pressed_cb(GtkGestureClick *gesture,int n_press,double x,double y,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))!=1) return;
  GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  GtkWidget *pop=gtk_popover_new();
  gtk_widget_set_parent(pop,widget);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop),&(GdkRectangle){(int)x,(int)y,1,1});
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
  gtk_popover_set_child(GTK_POPOVER(pop),box);
  meter_add_choice(box,rx,"S Meter Peak",RXA_S_PK);
  meter_add_choice(box,rx,"S Meter AVERAGE",RXA_S_AV);
  g_signal_connect_swapped(pop,"closed",G_CALLBACK(gtk_widget_unparent),pop);
  gtk_popover_popup(GTK_POPOVER(pop));
}

GtkWidget *create_meter_visual(RECEIVER *rx) {

  GtkWidget *meter = pana_view_new(meter_build,(gpointer)rx);

  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);
  g_signal_connect(click,"pressed",G_CALLBACK(meter_pressed_cb),(gpointer)rx);
  gtk_widget_add_controller(meter,GTK_EVENT_CONTROLLER(click));

  return meter;

}

// Per-frame STATE (fps timer): update the needle ballistics + the S-meter peak
// hold, then queue a GPU redraw. All drawing is in meter_build().
void update_meter(RECEIVER *rx) {
  double attenuation = radio->adc[rx->adc].attenuation;
  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
      attenuation = attenuation * -1;
  }
  double level=rx->meter_db+attenuation;

  // --- Analog-meter ballistics -------------------------------------------
  // Ease the needle toward the measured level instead of snapping to it each
  // frame, so it moves like a mechanical S-meter. The strength is the per-RX
  // "Meter smoothing" setting (0 = off/instant .. 100 = max); it scales the
  // decay time constant (0..0.5 s), with a fast attack kept at 1/5 of decay so
  // rising signals catch up quickly while peaks linger and settle.
  int smoothing = rx->meter_smoothing;
  if(smoothing < 0)   smoothing = 0;
  if(smoothing > 100) smoothing = 100;
  if(smoothing == 0) {
    rx->meter_needle_db = level;     // no smoothing: track exactly
    rx->meter_needle_init = 1;
  } else {
    int fps = rx->fps > 0 ? rx->fps : 20;
    double t_decay  = 0.5 * ((double)smoothing / 100.0);  // seconds to ~63% falling
    double t_attack = t_decay / 5.0;                       // faster rise
    double a_attack = 1.0 - exp(-1.0 / ((double)fps * t_attack));
    double a_decay  = 1.0 - exp(-1.0 / ((double)fps * t_decay));
    if(!rx->meter_needle_init) {
      rx->meter_needle_db = level;   // seed on first draw: no wild sweep from 0
      rx->meter_needle_init = 1;
    } else {
      double a = (level > rx->meter_needle_db) ? a_attack : a_decay;
      rx->meter_needle_db += (level - rx->meter_needle_db) * a;
    }
  }

  // S-meter peak hold ("smax") with the same fps-scaled decay as before.
  double sl=level+127.0;
  if(sl<0) sl=0;
  if(sl>s_meter_smax)  s_meter_smax=sl;
  else if(sl>54)       s_meter_smax = s_meter_smax-((s_meter_smax-sl)/(3*rx->fps));
  else                 s_meter_smax = s_meter_smax-((s_meter_smax-sl)/(rx->fps/2));

  if(rx->meter!=NULL) gtk_widget_queue_draw(rx->meter);
}

// Snapshot builder: emit the analog S-meter as GSK nodes.
static void meter_build(GtkSnapshot *snapshot,int meter_width,int meter_height,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  GtkWidget *widget=rx->meter;
  char sf[32];
  int i;

  if(meter_width<=0 || meter_height<=0) return;

  GdkRGBA bg=skin_rgba(BACKGROUND,1.0);
  lm_fill(snapshot,0,0,meter_width,meter_height,&bg);

  double attenuation = radio->adc[rx->adc].attenuation;
  if(radio->discovered->device==DEVICE_HERMES_LITE2) attenuation = attenuation * -1;
  double level=rx->meter_db+attenuation;          // dBm readout
  double needle_level=rx->meter_needle_db;

  double offset=210.0;
  double cx=(double)meter_width-100.0;
  double cy=100.0;
  double radius=cy-20.0;

  GdkRGBA offwhite=skin_rgba(OFF_WHITE,1.0);
  GdkRGBA warn=skin_rgba(WARNING,1.0);
  GdkRGBA darktext=skin_rgba(DARK_TEXT,1.0);

  // --- moving-coil draw order -------------------------------------------
  // The printed scale (dial arc, S9+ red band, ticks, numbers) is laid down
  // FIRST; the needle sweeps OVER all of it, exactly like a real meter whose
  // pointer rides above the face; finally an opaque dial-face disc masks the
  // needle's inner two-thirds so it appears to emerge from the face. Drawing
  // the scale first is what lets the needle tip stay visible in the red S9+
  // band (it used to hide behind the band, which was painted on top).

  // dial arc + S9+ overload zone (outer scale)
  meter_arc(snapshot,cx,cy,radius,216.0*M_PI/180.0,324.0*M_PI/180.0,1.0,&offwhite);
  meter_arc(snapshot,cx,cy,radius+2,264.0*M_PI/180.0,324.0*M_PI/180.0,2.5,&warn);

  // ticks 1..9 (odd = major bright + number, even = minor dim)
  for(i=1;i<10;i++) {
    double angle=((double)i*6.0)+offset;
    double rad=angle*M_PI/180.0, ca=cos(rad), sa=sin(rad);
    if((i%2)==1) {
      lm_line(snapshot, cx+radius*ca, cy+radius*sa, cx+(radius+4)*ca, cy+(radius+4)*sa, 1.0, &offwhite);
      sprintf(sf,"%d",i);
      lm_text(snapshot,widget, cx+(radius+5)*ca-4.0, cy+(radius+5)*sa, 12, &offwhite, sf, FALSE);
    } else {
      lm_line(snapshot, cx+radius*ca, cy+radius*sa, cx+(radius+2)*ca, cy+(radius+2)*sa, 1.0, &darktext);
    }
  }

  // +20/+40/+60 major ticks + labels
  for(i=20;i<=60;i+=20) {
    double angle=((double)i+54.0)+offset;
    double rad=angle*M_PI/180.0, ca=cos(rad), sa=sin(rad);
    lm_line(snapshot, cx+radius*ca, cy+radius*sa, cx+(radius+4)*ca, cy+(radius+4)*sa, 1.0, &offwhite);
    sprintf(sf,"+%d",i);
    lm_text(snapshot,widget, cx+(radius+5)*ca-4.0, cy+(radius+5)*sa, 12, &offwhite, sf, FALSE);
  }

  // needle — drawn OVER the whole printed scale (incl. the red band) so the
  // tip stays visible everywhere.
  GdkRGBA tb=skin_rgba(TEXT_B,1.0);
  double nrad=(needle_level+127.0+offset)*M_PI/180.0;
  lm_line(snapshot, cx+(radius+8)*cos(nrad), cy+(radius+8)*sin(nrad), cx, cy, 2.0, &tb);

  // Dial face: an opaque disc (background subtly lightened so it reads as a
  // raised face). Opaque = it hides the needle root drawn just above. Kept
  // small (only the pivot/counterweight hides) so a good length of needle
  // still pokes out and stays readable at low S-levels.
  double face_r=radius-38.0;
  GdkRGBA face=bg;
  face.red  += 0.055f; if(face.red  >1.0f) face.red  =1.0f;
  face.green+= 0.055f; if(face.green>1.0f) face.green=1.0f;
  face.blue += 0.060f; if(face.blue >1.0f) face.blue =1.0f;
  {
    GskPathBuilder *b=gsk_path_builder_new();
    gsk_path_builder_add_circle(b,&GRAPHENE_POINT_INIT((float)cx,(float)cy),(float)face_r);
    GskPath *p=gsk_path_builder_free_to_path(b);
    gtk_snapshot_append_fill(snapshot,p,GSK_FILL_RULE_WINDING,&face);
    gsk_path_unref(p);
  }

  // the "second arc": the dial-face edge, mirroring the scale — the needle
  // pokes past this line and the rest hides behind the face below it.
  meter_arc(snapshot,cx,cy,face_r,216.0*M_PI/180.0,324.0*M_PI/180.0,1.5,&offwhite);

  // pivot hub, sitting on the face
  {
    GskPathBuilder *b=gsk_path_builder_new();
    gsk_path_builder_add_circle(b,&GRAPHENE_POINT_INIT((float)cx,(float)cy),3.0f);
    GskPath *p=gsk_path_builder_free_to_path(b);
    gtk_snapshot_append_fill(snapshot,p,GSK_FILL_RULE_WINDING,&tb);
    gsk_path_unref(p);
  }

  // dBm readout
  GdkRGBA ta=skin_rgba(TEXT_A,1.0);
  sprintf(sf,"%d dBm %s",(int)level,rx->smeter==RXA_S_AV?"Av":"Pk");
  lm_text(snapshot,widget, meter_width-130, meter_height-2, 12, &ta, sf, FALSE);

  // S-number + optional +dB, from the peak-hold smax
  GdkRGBA tc=skin_rgba(TEXT_C,1.0);
  double smax=s_meter_smax;
  i=(int)(smax/6); if(i>9) i=9;
  sprintf(sf,"S%d", i);
  lm_text(snapshot,widget, meter_width-250, meter_height-20, 36, &tc, sf, FALSE);

  i=(int)smax;
  if(i>54) {
    i=i-54;
    sprintf(sf,"+%d", i);
    lm_text(snapshot,widget, meter_width-210, (meter_height/2)+5, 20, &tc, sf, FALSE);
  }
}
