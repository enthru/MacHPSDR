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
#include <epoxy/gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wdsp.h>
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr

#include "bpsk.h"
#include "agc.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "receiver.h"
#include "rx_panadapter.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "fake_protocol.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "vfo.h"
#include "level_meter.h"
#include "css.h"
#include "dxcluster.h"

#define LINE_WIDTH 1.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int signal_vertices_size=-1;
float *signal_vertices=NULL;

// Set the Cairo source to the active skin's spectrum backdrop. Kept dark on
// every skin (light skins map it to a dark tone of their own palette) so the
// trace/waterfall stay readable and the panel does not read as a black box.
static void pan_background(cairo_t *cr) {
  double r=0.09, g=0.09, b=0.10;
  css_rgb("SPECTRUM_BG",&r,&g,&b);
  cairo_set_source_rgb(cr, r, g, b);
}

static gboolean resize_timeout(void *data) {
  RECEIVER *rx=(RECEIVER *)data;

  g_mutex_lock(&rx->mutex);
  rx->panadapter_width=rx->panadapter_resize_width;
  rx->panadapter_height=rx->panadapter_resize_height;
  // A wider window at a high zoom could push pixels past WDSP's dMAX_PIXELS
  // ceiling and corrupt the analyzer, so re-clamp the zoom to the new width.
  if(rx->panadapter_width>0) {
    int max_zoom=16384/rx->panadapter_width;
    if(max_zoom<1) max_zoom=1;
    if(rx->zoom>max_zoom) rx->zoom=max_zoom;
  }
  rx->pixels=rx->panadapter_width*rx->zoom;

  receiver_init_analyzer(rx);

  // Hidden rx may be associated with this display
  if (radio->divmixer[rx->dmix_id] != NULL) {
    if (radio->divmixer[rx->dmix_id]->calibrate_gain) {    
      radio->divmixer[rx->dmix_id]->rx_hidden->panadapter_width = rx->panadapter_width;
      radio->divmixer[rx->dmix_id]->rx_hidden->panadapter_height = rx->panadapter_height;   
      radio->divmixer[rx->dmix_id]->rx_hidden->pixels = rx->pixels;
      radio->divmixer[rx->dmix_id]->rx_hidden->fps = rx->fps;
      radio->divmixer[rx->dmix_id]->rx_hidden->display_average_time = rx->display_average_time;
      receiver_init_analyzer(radio->divmixer[rx->dmix_id]->rx_hidden);    
    }
  }  

  if (rx->panadapter_surface) {
    cairo_surface_destroy (rx->panadapter_surface);
    rx->panadapter_surface=NULL;
  }

  if(rx->panadapter!=NULL && rx->panadapter_width>0 && rx->panadapter_height>0) {
    // GTK4: no GdkWindow to back a similar surface; draw into an off-screen
    // image surface that the draw-func blits each frame.
    rx->panadapter_surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24,
                                       rx->panadapter_width,
                                       rx->panadapter_height);

    /* Initialize the surface to black */
    cairo_t *cr;
    cr = cairo_create (rx->panadapter_surface);

    if(rx->panadapter_gradient) {
      cairo_pattern_t *pat=cairo_pattern_create_linear(0.2, 0.2, 0.2, rx->panadapter_height);
      cairo_pattern_add_color_stop_rgba(pat,0.0, (48/255), (48/255), (48/255), 1);
      cairo_pattern_add_color_stop_rgba(pat,0.0, (80/255), (80/255), (80/255), 1);
      cairo_rectangle(cr, 0,0,rx->panadapter_width,rx->panadapter_height);
      cairo_set_source (cr, pat);
      cairo_fill(cr);
      cairo_pattern_destroy(pat);
    } else {
      cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
      cairo_paint (cr);
    }
    cairo_destroy(cr);
  }
  rx->panadapter_resize_timer=-1;
  update_vfo(rx);
  g_mutex_unlock(&rx->mutex);
  return FALSE;
}

#ifdef OPENGL

GLuint gl_program, gl_vao;

const GLchar *vert_src ="\n" \
"#version 330                                  \n" \
"#extension GL_ARB_explicit_attrib_location: enable  \n" \
"                                              \n" \
"layout(location = 0) in vec2 in_position;     \n" \
"                                              \n" \
"void main()                                   \n" \
"{                                             \n" \
"  gl_Position = ftransform;                   \n" \
"}                                             \n";

const GLchar *frag_src ="\n" \
"void main (void)                              \n" \
"{                                             \n" \
"  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);    \n" \
"}                                             \n";

static gboolean rx_panadapter_render(GtkGLArea *area, GdkGLContext *context)
{
  //g_mutex_lock(&rx->mutex);
  // inside this function it's safe to use GL; the given
  // #GdkGLContext has been made current to the drawable
  // surface used by the #GtkGLArea and the viewport has
  // already been set to be the size of the allocation

  // we can start by clearing the buffer
  glClearColor (0, 0, 0, 0);
  glClear (GL_COLOR_BUFFER_BIT);

  // draw the object
  if(signal_vertices_size!=-1) {
    glLineWidth(2.0);
    glColor3f(1.0,1.0,0.0);
    GLuint vbo;
    glGenBuffers(1, &vbo); // Generate 1 buffer
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, signal_vertices_size*sizeof(float)*2, signal_vertices, GL_STREAM_DRAW);

    glUseProgram(gl_program);
    glBindVertexArray(gl_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDrawArrays(GL_LINE_STRIP,0,signal_vertices_size);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); //Unbind
    glBindVertexArray(0);
    glUseProgram(0);
  }

  // we completed our drawing; the draw commands will be
  // flushed at the end of the signal emission chain, and
  // the buffers will be drawn on the window
  //g_mutex_unlock(&rx->mutex);
  return TRUE;
}

static void rx_panadapter_realize (GtkGLArea *area)
{
  // We need to make the context current if we want to
  // call GL API
  gtk_gl_area_make_current (area);

  // If there were errors during the initialization or
  // when trying to make the context current, this
  // function will return a #GError for you to catch
  if (gtk_gl_area_get_error (area) != NULL)
    return;

  GLuint frag_shader, vert_shader;
  frag_shader = glCreateShader(GL_FRAGMENT_SHADER);
  vert_shader = glCreateShader(GL_VERTEX_SHADER);

  glShaderSource(frag_shader, 1, &frag_src, NULL);
  glShaderSource(vert_shader, 1, &vert_src, NULL);

  glCompileShader(frag_shader);
  glCompileShader(vert_shader);

  gl_program = glCreateProgram();
  glAttachShader(gl_program, frag_shader);
  glAttachShader(gl_program, vert_shader);
  glLinkProgram(gl_program);

  glGenVertexArrays(1, &gl_vao);
  glBindVertexArray(gl_vao);


}
#endif

void rx_panadapter_resize(GtkGLArea *area, gint width, gint height, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(width!=rx->panadapter_width || height!=rx->panadapter_height) {
    rx->panadapter_resize_width=width;
    rx->panadapter_resize_height=height;
    if(rx->panadapter_resize_timer!=-1) {
      g_source_remove(rx->panadapter_resize_timer);
    }
    rx->panadapter_resize_timer=g_timeout_add(250,resize_timeout,(gpointer)rx);
  }
}

// GTK4: GtkDrawingArea "resize" signal replaces GTK3 "configure-event".
static void rx_panadapter_resize_cb(GtkDrawingArea *area,int width,int height,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(width!=rx->panadapter_width || height!=rx->panadapter_height) {
    rx->panadapter_resize_width=width;
    rx->panadapter_resize_height=height;
    if(rx->panadapter_resize_timer!=-1) {
      g_source_remove(rx->panadapter_resize_timer);
    }
    rx->panadapter_resize_timer=g_timeout_add(250,resize_timeout,(gpointer)rx);
  }
}


// GTK4: draw func signature is (area, cr, width, height, data).
static void rx_panadapter_draw_cb(GtkDrawingArea *area,cairo_t *cr,int width,int height,gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  if(rx->panadapter_surface!=NULL) {
    cairo_set_source_surface (cr, rx->panadapter_surface, 0.0, 0.0);
    cairo_paint (cr);
  }
}

GtkWidget *create_rx_panadapter(RECEIVER *rx) {
  GtkWidget *panadapter;

  rx->panadapter_width=0;
  rx->panadapter_height=0;
  rx->panadapter_surface=NULL;
  rx->panadapter_resize_timer=-1;

  panadapter=NULL;
#ifdef OPENGL
  if(opengl) {
    panadapter=gtk_gl_area_new();
  }

  if(panadapter!=NULL) {
    log_info("rx_panadapter: using opengl\n");
    g_signal_connect (panadapter,"render",G_CALLBACK(rx_panadapter_render),rx);
    g_signal_connect (panadapter,"realize",G_CALLBACK(rx_panadapter_realize),rx);
    g_signal_connect (panadapter,"resize",G_CALLBACK(rx_panadapter_resize),rx);
  } else {
#endif
    panadapter = gtk_drawing_area_new ();

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(panadapter),rx_panadapter_draw_cb,(gpointer)rx,NULL);
    g_signal_connect(panadapter,"resize",G_CALLBACK(rx_panadapter_resize_cb),(gpointer)rx);

#ifdef OPENGL
  }
#endif

  // GTK4: pointer input via event controllers (button masks are gone).
  GtkGesture *click=gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),0); // any button
  g_signal_connect(click,"pressed",G_CALLBACK(receiver_pressed_cb),rx);
  g_signal_connect(click,"released",G_CALLBACK(receiver_released_cb),rx);
  gtk_widget_add_controller(panadapter,GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion=gtk_event_controller_motion_new();
  g_signal_connect(motion,"motion",G_CALLBACK(receiver_motion_cb),rx);
  gtk_widget_add_controller(panadapter,motion);

  GtkEventController *scroll=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll,"scroll",G_CALLBACK(receiver_scroll_cb),rx);
  gtk_widget_add_controller(panadapter,scroll);

  return panadapter;
}

// Heatmap for the persistence display: t in [0,1] -> black->blue->cyan->green->yellow->red.
static void hist_heat_rgb(float t, double *r, double *g, double *b) {
  if(t<0.0f) t=0.0f; if(t>1.0f) t=1.0f;
  // 5-stop ramp
  if(t<0.25f)      { double u=t/0.25f;        *r=0;          *g=0;          *b=0.15+0.85*u; }
  else if(t<0.5f)  { double u=(t-0.25f)/0.25f;*r=0;          *g=u;          *b=1.0; }
  else if(t<0.7f)  { double u=(t-0.5f)/0.2f;  *r=0;          *g=1.0;        *b=1.0-u; }
  else if(t<0.85f) { double u=(t-0.7f)/0.15f; *r=u;          *g=1.0;        *b=0; }
  else             { double u=(t-0.85f)/0.15f;*r=1.0;        *g=1.0-u;      *b=0; }
}

static gboolean first_time=TRUE;

// Deterministic colour for a DX cluster spot's DXCC entity: unresolved (-1)
// draws grey, otherwise a stable hue spread (HSV, s=0.7 v=1.0) so repeat
// entities are visually distinguishable without a lookup table.
static void cluster_spot_rgb(int entity, double *r, double *g, double *b) {
  if(entity<0) { *r=0.6; *g=0.6; *b=0.6; return; }
  double hue=(double)((entity*47)%360);
  double s=0.7, v=1.0;
  double c=v*s;
  double hp=hue/60.0;
  double x=c*(1.0-fabs(fmod(hp,2.0)-1.0));
  double r1,g1,b1;
  if(hp<1.0)      { r1=c; g1=x; b1=0; }
  else if(hp<2.0) { r1=x; g1=c; b1=0; }
  else if(hp<3.0) { r1=0; g1=c; b1=x; }
  else if(hp<4.0) { r1=0; g1=x; b1=c; }
  else if(hp<5.0) { r1=x; g1=0; b1=c; }
  else            { r1=c; g1=0; b1=x; }
  double m=v-c;
  *r=r1+m; *g=g1+m; *b=b1+m;
}

// Row-packed DX-cluster spot overlay, shared by the panadapter surface and the
// waterfall's cairo overlay. min_display is recomputed from rx so the same
// absolute-RF -> x mapping the trace uses applies on either widget; font size +
// label background colour come from the persisted RADIO settings.
void receiver_draw_cluster_spots(cairo_t *cr, RECEIVER *rx, int display_width) {
  if(rx->hz_per_pixel==0.0) return;
  long long half=(long long)rx->sample_rate/2LL;
  long long min_display=(rx->frequency_a - half) + (long long)((double)rx->pan*rx->hz_per_pixel);

  dxcluster_lock();
  int ns=dxcluster_count();

  // Collect the in-span spots and sort them left-to-right so callsign labels can
  // be packed into stacked rows deterministically. Without this, several spots
  // at nearly the same frequency (a pileup, or an FT8 watering hole) overprint
  // their labels at one y and smear into an unreadable blob.
  struct { double x; const DX_SPOT *s; } vis[DXCLUSTER_MAX_SPOTS];
  int nv=0;
  for(int i=0;i<ns;i++) {
    const DX_SPOT *s=dxcluster_spot(i);
    if(s==NULL) continue;
    double x=((double)s->freq - (double)min_display)/rx->hz_per_pixel;
    if(x<0.0 || x>(double)display_width) continue;
    vis[nv].x=x; vis[nv].s=s; nv++;
  }
  for(int a=1;a<nv;a++) {                       // insertion sort by x
    double kx=vis[a].x; const DX_SPOT *ks=vis[a].s; int b=a-1;
    while(b>=0 && vis[b].x>kx) { vis[b+1]=vis[b]; b--; }
    vis[b+1].x=kx; vis[b+1].s=ks;
  }

  cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  double fs=(double)radio->cluster_spots_font;
  if(fs<7.0) fs=7.0; if(fs>28.0) fs=28.0;       // clamp to a sane range
  cairo_set_font_size(cr, fs);
  double bg_r=radio->cluster_spots_bg_r, bg_g=radio->cluster_spots_bg_g,
         bg_b=radio->cluster_spots_bg_b, bg_a=radio->cluster_spots_bg_a;
  #define SPOT_LABEL_ROWS 8
  double row_right[SPOT_LABEL_ROWS];
  for(int r=0;r<SPOT_LABEL_ROWS;r++) row_right[r]=-1e9;
  const double row_h=fs+1.0, base_y=fs+3.0;

  for(int a=0;a<nv;a++) {
    double x=vis[a].x;
    const DX_SPOT *s=vis[a].s;
    cairo_text_extents_t te;
    cairo_text_extents(cr, s->call, &te);

    int row=0, bestrow=0; double best=1e18;
    for(row=0;row<SPOT_LABEL_ROWS;row++) {
      if(row_right[row]<best) { best=row_right[row]; bestrow=row; }
      if(x > row_right[row]+3.0) break;
    }
    if(row>=SPOT_LABEL_ROWS) row=bestrow;       // all rows busy: least-bad
    double ty=base_y + row*row_h;
    row_right[row]=x+te.width+2.0;

    double sr,sg,sb;
    cluster_spot_rgb(s->entity,&sr,&sg,&sb);
    // tick reaching down to this spot's own label row
    cairo_set_source_rgba(cr, sr, sg, sb, 0.9);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x, 0.0);
    cairo_line_to(cr, x, ty+te.y_bearing);
    cairo_stroke(cr);
    // background box behind the callsign (skip if fully transparent)
    if(bg_a>0.0) {
      cairo_set_source_rgba(cr, bg_r, bg_g, bg_b, bg_a);
      cairo_rectangle(cr, x+2.0+te.x_bearing-1.0, ty+te.y_bearing-1.0,
                      te.width+2.0, te.height+2.0);
      cairo_fill(cr);
    }
    // callsign text on top
    cairo_set_source_rgba(cr, sr, sg, sb, 0.95);
    cairo_move_to(cr, x+2.0, ty);
    cairo_show_text(cr, s->call);
  }
  #undef SPOT_LABEL_ROWS
  dxcluster_unlock();
}

void update_rx_panadapter(RECEIVER *rx,gboolean running) {
  int i;
  int x1,x2;
  
  int gain_cal_error = FALSE;
  
  float *samples;
  float *samples_hidden_rx;
  cairo_text_extents_t extents;
  char temp[32];

  int display_width=gtk_widget_get_width (rx->panadapter);
  int display_height=gtk_widget_get_height (rx->panadapter);
  //int offset=((rx->zoom-1)/2)*display_width;
  int offset=rx->pan;
  samples=rx->pixel_samples;
  samples[display_width-1+offset]=-200;
  double dbm_per_line=(double)display_height/((double)rx->panadapter_high-(double)rx->panadapter_low);
  
  
  double attenuation=radio->adc[rx->adc].attenuation;
  // With diversity mixers, for calibration of gain between 2 RX, don't
  // want adjustment of panadapter with gain
  if (radio->divmixer[rx->dmix_id] != NULL) {
    if (radio->divmixer[rx->dmix_id]->calibrate_gain) {
      attenuation = 0;
      rx->panadapter_filled = FALSE;
      
      if (radio->divmixer[rx->dmix_id]->rx_hidden->pixel_samples != NULL) {
        samples_hidden_rx = radio->divmixer[rx->dmix_id]->rx_hidden->pixel_samples;
        samples_hidden_rx[display_width-1 + offset] = -200;        
      }
      else {
        gain_cal_error = TRUE;
        log_info("Dmix gain cal error\n");
      }

    }
  }
  
  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
      attenuation = attenuation * -1;
  }
  

  if(display_height<=1) return;

  if(opengl) {
    if(signal_vertices_size!=display_width) {
      if(signal_vertices!=NULL) {
        g_free(signal_vertices);
      }
      signal_vertices=g_new(float,display_width*2);
      signal_vertices_size=display_width;
    }
    float h_half=(float)display_width/2.0;
    float v_half=(float)rx->panadapter_low+(((float)rx->panadapter_high-(float)rx->panadapter_low)/2.0);
    for(i=0;i<display_width;i++) {
      float x=((float)i-h_half)/h_half;
      double s2=(double)samples[i+offset]+attenuation+radio->panadapter_calibration;
      float y=((float)s2-v_half)/v_half;
      if(y>1.0) y=1.0;
      if(y<-1.0) y=-1.0;
      signal_vertices[i*2]=x;
      signal_vertices[(i*2)+1]=y;
      if(first_time) {
        log_info("i=%d x=%f y=%f\n",i,x,y);
      }
    }
    first_time=FALSE;
    gtk_widget_queue_draw (rx->panadapter);
  } else {

    if(rx->panadapter_surface==NULL) {
      return;
    }

    //clear_panadater_surface();
    cairo_t *cr;
    cr = cairo_create (rx->panadapter_surface);
    cairo_select_font_face(cr, "Noto Sans", CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_line_width(cr, LINE_WIDTH);

    if(!running) {
      SetColour(cr, WARNING);
      cairo_set_font_size(cr, 18);
      cairo_move_to(cr, display_width/2, display_height/2);  
      cairo_show_text(cr, "No data - receiver thread exited");
      cairo_destroy (cr);
      gtk_widget_queue_draw (rx->panadapter);
      return;
    }

    if(rx->panadapter_gradient) {
      // Set fill
      cairo_pattern_t *pat = cairo_pattern_create_radial((rx->panadapter_width / 2),
                             rx->panadapter_height + 300,
                             5,
                             (rx->panadapter_width / 2),
                             rx->panadapter_height + 300,
                             rx->panadapter_width/2);
      
      cairo_pattern_add_color_stop_rgba(pat, 1, 0.1, 0.1, 0.1, 1);
      cairo_pattern_add_color_stop_rgba(pat, 0, 0.25, 0.25, 0.25, 1);      
      
      cairo_rectangle(cr, 0,0,rx->panadapter_width,rx->panadapter_height);
      cairo_set_source (cr, pat);
      cairo_fill(cr);
      cairo_pattern_destroy(pat);
    } else {
      pan_background(cr);
      //cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
      cairo_rectangle(cr,0,0,display_width,display_height);
      cairo_fill(cr);
    }

    // I/Q vectorscope (X-Y phase display). Takes precedence over the whole
    // spectrum path below (graticule/dB+freq scales/trace/histogram/peak
    // hold/AGC/filter/cursor/cluster overlay all assume a frequency-domain
    // trace, which the scope has none of), so draw it and return early right
    // after the background, mirroring the histogram/peak-hold early-exit
    // style used elsewhere in this function.
    if(rx->panadapter_phase) {
      int cx=display_width/2;
      int cy=display_height/2;
      int min_dim = display_width<display_height ? display_width : display_height;
      double R=0.45*(double)min_dim;

      // Graticule: faint crosshair + two concentric circles.
      cairo_set_source_rgba(cr,0.5,0.5,0.5,0.35);
      cairo_set_line_width(cr,1.0);
      cairo_move_to(cr,0,cy);
      cairo_line_to(cr,display_width,cy);
      cairo_stroke(cr);
      cairo_move_to(cr,cx,0);
      cairo_line_to(cr,cx,display_height);
      cairo_stroke(cr);
      cairo_arc(cr,cx,cy,R,0,2*M_PI);
      cairo_stroke(cr);
      cairo_arc(cr,cx,cy,R/2.0,0,2*M_PI);
      cairo_stroke(cr);

      // Diversity source plots main-I (X) vs hidden-I (Y): a common signal that
      // is in-phase and equal-gain across the two antennas lands on the y=x
      // diagonal (math coords; lower-left -> upper-right on screen). Draw it as
      // the alignment target so the operator collapses the ellipse onto this
      // line with the diversity Phase (line vs ellipse) then Gain (45 deg slope)
      // controls.
      if(rx->panadapter_phase_source==2) {
        cairo_set_source_rgba(cr,1.0,0.8,0.2,0.4);
        cairo_move_to(cr,cx-R,cy+R);
        cairo_line_to(cr,cx+R,cy-R);
        cairo_stroke(cr);
      }

      // Snapshot the tapped I/Q under the lock, then draw outside it so the
      // audio thread is never blocked on cairo work.
      g_mutex_lock(&rx->scope_mutex);
      int n=rx->scope_iq_n;
      float *pts=NULL;
      if(n>0) {
        pts=g_new(float,2*n);
        memcpy(pts,rx->scope_iq,sizeof(float)*2*n);
      }
      g_mutex_unlock(&rx->scope_mutex);

      if(pts!=NULL) {
        // Auto-scale: track the smoothed peak magnitude so the cloud fills
        // the graticule without needing a manual reference; the gain slider
        // is a multiplier on top of that auto-fit.
        double m=0.0;
        for(int i=0;i<n;i++) {
          double ai=fabs((double)pts[i*2]);
          double aq=fabs((double)pts[i*2+1]);
          double a = ai>aq ? ai : aq;
          if(a>m) m=a;
        }
        rx->scope_ref = m > rx->scope_ref ? m : rx->scope_ref*0.95;
        if(rx->scope_ref<1e-6) rx->scope_ref=1e-6;
        double scale=(R/rx->scope_ref)*((double)rx->panadapter_phase_gain/100.0);

        cairo_set_line_width(cr,1.0);
        cairo_set_source_rgba(cr,0.1,1.0,0.3,rx->panadapter_phase_mode==0 ? 0.5 : 0.6);

        if(rx->panadapter_phase_mode==0) {
          // Dots: batch every sample into one path, single fill.
          for(int i=0;i<n;i++) {
            double x=cx+(double)pts[i*2]*scale;
            double y=cy-(double)pts[i*2+1]*scale;
            cairo_rectangle(cr,x-0.5,y-0.5,1.5,1.5);
          }
          cairo_fill(cr);
        } else {
          // Lines: polyline through consecutive samples, single stroke.
          double x0=cx+(double)pts[0]*scale;
          double y0=cy-(double)pts[1]*scale;
          cairo_move_to(cr,x0,y0);
          for(int i=1;i<n;i++) {
            double x=cx+(double)pts[i*2]*scale;
            double y=cy-(double)pts[i*2+1]*scale;
            cairo_line_to(cr,x,y);
          }
          cairo_stroke(cr);
        }
        g_free(pts);
      }

      {
        char scope_label[32];
        const char *src = rx->panadapter_phase_source==1 ? "Tuned" :
                          rx->panadapter_phase_source==2 ? "Diversity" : "Wideband";
        snprintf(scope_label,sizeof(scope_label),"%s (%s)",
                 rx->panadapter_phase_mode==0 ? "PHASE" : "PHASE2", src);
        SetColour(cr, WARNING);
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, 4, 14);
        cairo_show_text(cr, scope_label);
      }

      cairo_destroy(cr);
      gtk_widget_queue_draw(rx->panadapter);
      return;
    }

    // Persistence / "digital phosphor" heatmap (Siglent/Rigol style). Drawn here,
    // right after the background and BEFORE the graticule / dB+frequency scales /
    // trace, so all of those redraw cleanly on top (the scales must not get
    // speckled). Each cell of a screen-coord buffer is an EMA of a "was the trace
    // here this frame?" indicator, so it holds the fraction of recent frames the
    // pixel was lit (occupancy in [0,1]): the dense noise floor / steady signals
    // grade up to red, rarer high excursions stay blue - the whole visited cloud
    // is colour-graded, not just the hottest baseline. No peak normalisation (a
    // lone outlier would wash the rest to blue). Accumulate + render are all on
    // this GTK-main-thread timer, same as the peak-hold buffer, so no lock.
    if(rx->panadapter_histogram && rx->panadapter_histogram_bins!=NULL
       && display_width==rx->panadapter_histogram_w && display_height==rx->panadapter_histogram_h) {
      int H=rx->panadapter_histogram_h;
      float *bins=rx->panadapter_histogram_bins;
      // frame-rate-independent exponential fade; /20 makes the default (decay=20)
      // persist ~1 s of frames so the cloud builds up, slider spans slow..fast.
      int fps = rx->fps>0 ? rx->fps : 15;
      float keep = expf(-(float)rx->panadapter_histogram_decay/(20.0f*(float)fps));
      float add = 1.0f - keep;   // EMA step toward occupancy = 1
      int total = display_width*H;
      for(int k=0;k<total;k++) bins[k]*=keep;
      for(i=0;i<display_width;i++) {
        double s2h=(double)samples[i+offset]+attenuation+radio->panadapter_calibration;
        int yr=(int)floor((rx->panadapter_high - s2h)*dbm_per_line);
        if(yr<0) yr=0; if(yr>=H) yr=H-1;
        // Deposit over +/-3 rows (tapered) so a level that dithers across several
        // pixels on a wide dB span still concentrates into one warm band instead
        // of splitting its dwell thinly across many rows (which reads as cold blue).
        for(int dz=-3; dz<=3; dz++) {
          int yy=yr+dz;
          if(yy<0 || yy>=H) continue;
          float w=1.0f-0.18f*(float)(dz<0?-dz:dz);   // 1.0 at centre -> 0.46 at +/-3
          bins[i*H+yy]+=w*add;
        }
      }
      // Heat-colour lookup table: the per-pixel colour is a pure function of the
      // occupancy (gamma-lifted, then the 5-stop ramp), so bake both into a
      // 256-entry LUT built once. This replaces a powf()+hist_heat_rgb() per
      // pixel (~display_width*display_height calls every frame), which was the
      // heatmap's dominant cost. Each entry is the CAIRO_FORMAT_RGB24 byte order
      // B,G,R for that occupancy bucket. Gamma note: lift the occupancy so even a
      // modestly-dwelt pixel (noise floor ~0.2-0.4) reads green/yellow while a
      // steady signal (->1) burns red; NO peak normalisation (a lone outlier
      // would wash the cloud back to blue).
      static gboolean heat_lut_ready=FALSE;
      static unsigned char heat_lut[256][3];   // [occupancy bucket] = {B,G,R}
      if(!heat_lut_ready) {
        for(int li=0;li<256;li++) {
          double rr,gg,bb;
          hist_heat_rgb(powf((float)li/255.0f,0.40f),&rr,&gg,&bb);
          heat_lut[li][0]=(unsigned char)(bb*255.0);
          heat_lut[li][1]=(unsigned char)(gg*255.0);
          heat_lut[li][2]=(unsigned char)(rr*255.0);
        }
        heat_lut_ready=TRUE;
      }
      // Reuse a cached blit surface across frames (matches the guarded dims);
      // receiver_init_analyzer drops it on resize so it is re-made once here.
      cairo_surface_t *hs=rx->panadapter_histogram_surface;
      if(hs==NULL) {
        hs=cairo_image_surface_create(CAIRO_FORMAT_RGB24, display_width, display_height);
        rx->panadapter_histogram_surface=hs;
      }
      unsigned char *hd=cairo_image_surface_get_data(hs);
      int stride=cairo_image_surface_get_stride(hs);
      double bgr=0.09,bgg=0.09,bgb=0.10; css_rgb("SPECTRUM_BG",&bgr,&bgg,&bgb);
      unsigned char bgb8=(unsigned char)(bgb*255.0), bgg8=(unsigned char)(bgg*255.0), bgr8=(unsigned char)(bgr*255.0);
      for(int y=0;y<display_height;y++) {
        unsigned char *row=hd+y*stride;
        for(int xx=0;xx<display_width;xx++) {
          float d=bins[xx*H+y];   // absolute occupancy 0..1 (fraction of recent frames lit)
          if(d<=0.015f) {         // essentially unvisited -> background
            row[xx*4+0]=bgb8; row[xx*4+1]=bgg8; row[xx*4+2]=bgr8; row[xx*4+3]=0;
          } else {
            int li=(int)(d*255.0f); if(li>255) li=255;   // also clamps d>1
            row[xx*4+0]=heat_lut[li][0];
            row[xx*4+1]=heat_lut[li][1];
            row[xx*4+2]=heat_lut[li][2];
            row[xx*4+3]=0;
          }
        }
      }
      cairo_surface_mark_dirty(hs);
      cairo_set_source_surface(cr, hs, 0.0, 0.0);
      cairo_paint(cr);
    }

    long long frequency=rx->frequency_a;
    long long half=(long long)rx->sample_rate/2LL;
    long long min_display=(frequency - half) + (long long)((double)rx->pan*rx->hz_per_pixel);
    long long max_display=(frequency + half) + (long long)((double)rx->pan*rx->hz_per_pixel);
    BAND *band=band_get_band(rx->band_a);

    if(rx->band_a==band60) {
      for(i=0;i<channel_entries;i++) {
        long long low_freq=band_channels_60m[i].frequency-(band_channels_60m[i].width/(long long)2);
        long long hi_freq=band_channels_60m[i].frequency+(band_channels_60m[i].width/(long long)2);
        x1=(low_freq-min_display)/rx->hz_per_pixel;
        x2=(hi_freq-min_display)/rx->hz_per_pixel;
        cairo_set_source_rgb (cr, 0.6, 0.3, 0.3);
        cairo_rectangle(cr, x1, 0.0, x2-x1, (double)display_height);
        cairo_fill(cr);
      }
    }

    // filter
    //cairo_set_source_rgba (cr, 0.25, 0.25, 0.25, 0.75);
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.75);
    double filter_left=((double)rx->pixels/2.0)-(double)rx->pan+(((double)rx->filter_low_a+rx->ctun_offset)/rx->hz_per_pixel);
    double filter_right=((double)rx->pixels/2.0)-(double)rx->pan+(((double)rx->filter_high_a+rx->ctun_offset)/rx->hz_per_pixel);
    
    
    cairo_pattern_t *pat3 = cairo_pattern_create_linear(filter_left, 0, filter_left, (double)display_height);
  
    //cairo_pattern_add_color_stop_rgb(pat3, 0.01, 0.1, 0.1, 0.1);
    cairo_pattern_add_color_stop_rgba(pat3, 0.1, 0.5, 0.5, 0.5, 0.75);
    cairo_pattern_add_color_stop_rgb(pat3, 0.7, 0.1, 0.1, 0.1);
  
    cairo_rectangle(cr, filter_left, 0.0, filter_right-filter_left, (double)display_height);
    
    cairo_set_source(cr, pat3);
    
    cairo_fill(cr);
    
    // Show VFO B (tx) for split mode
    
    double cw_offset = 0;
    if(rx->mode_a==CWL || rx->mode_a==CWU) {  
      if(rx->mode_a==CWU) {
        cw_offset=-radio->cw_keyer_sidetone_frequency;
      } else {
        cw_offset=+radio->cw_keyer_sidetone_frequency;
      }  
    }
    // VFO B/sub rx filter
    if(rx->subrx!=NULL) {
      i=(int)(((double)rx->frequency_b-(double)min_display)/rx->hz_per_pixel);
      filter_left = i + (rx->filter_low_a / rx->hz_per_pixel);
      filter_right = i + (rx->filter_high_a / rx->hz_per_pixel);
      cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.75);
      cairo_rectangle(cr, filter_left, 0.0, filter_right-filter_left, (double)display_height);
      cairo_fill(cr);
    }

    // Manual notch filters: translucent band + centre line, drawn in the same
    // absolute-RF-to-pixel coordinate system as the filter passband above so
    // they sit on top of it and track the dial (Ctrl+click in receiver.c adds
    // or removes one).
    for(i=0;i<rx->notches;i++) {
      double _fc=rx->notch[i].fcenter;
      double _w =rx->notch[i].fwidth;
      double _xl=((_fc-0.5*_w)-(double)min_display)/rx->hz_per_pixel;
      double _xr=((_fc+0.5*_w)-(double)min_display)/rx->hz_per_pixel;
      if(_xr<0 || _xl>display_width) continue;
      if(rx->notch[i].active) {
        cairo_set_source_rgba(cr, 0.9, 0.2, 0.2, 0.30);   // active: translucent red band
      } else {
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 0.20);   // inactive: grey
      }
      cairo_rectangle(cr, _xl, 0.0, (_xr-_xl)<1.0?1.0:(_xr-_xl), (double)display_height-20);
      cairo_fill(cr);
      // centre line
      cairo_set_source_rgba(cr, 0.9, 0.2, 0.2, 0.8);
      cairo_set_line_width(cr, 1.0);
      double _xc=(_fc-(double)min_display)/rx->hz_per_pixel;
      cairo_move_to(cr, _xc, 0.0);
      cairo_line_to(cr, _xc, (double)display_height-20);
      cairo_stroke(cr);
    }

#ifdef FT8
    // FT8 TX audio-offset marker: a green vertical line where our transmission
    // will land (dial + tx offset, USB). Only meaningful while the FT8 panel is
    // open (radio->ft8_panel != NULL) — showing it in plain DIGU without the
    // panel is just clutter.
    if(rx->mode_a==DIGU && radio->ft8_panel!=NULL) {
      double tx_x=((double)(frequency + (long long)radio->ft8_tx_offset)
                   - (double)min_display)/rx->hz_per_pixel;
      cairo_set_source_rgba(cr, 0.2, 0.9, 0.2, 0.9);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, tx_x, 0.0);
      cairo_line_to(cr, tx_x, (double)display_height);
      cairo_stroke(cr);
      cairo_move_to(cr, tx_x+2.0, 10.0);
      cairo_show_text(cr, "TX");
    }
#endif

    // DX cluster spot overlay (shared with the waterfall): a short tick +
    // callsign for each in-span spot, colour-keyed by DXCC entity. Drawn here on
    // the panadapter when cluster_spots_on is 0 (panadapter) or 2 (both).
    if(radio->cluster_enable && radio->cluster_spots_show &&
       (radio->cluster_spots_on==0 || radio->cluster_spots_on==2)) {
      receiver_draw_cluster_spots(cr, rx, display_width);
    }

    // I/Q Player readout: while the fake device is looping a recording, print the
    // elapsed/total playback time and the recording's bandwidth in the top-right
    // corner. Drawn through the same overlay path as the DX spots (a display-only
    // read of state the feed thread publishes, repainted by the fps timer).
    if(radio->discovered->protocol==PROTOCOL_FAKE) {
      double elapsed_s, total_s, bw_hz;
      if(fake_protocol_playback(&elapsed_s, &total_s, &bw_hz)) {
        char pb[64];
        int e=(int)elapsed_s, t=(int)total_s;
        if(bw_hz>=1e6)
          snprintf(pb,sizeof(pb),"IQ %d:%02d / %d:%02d   BW %.2f MHz",
                   e/60,e%60,t/60,t%60,bw_hz/1e6);
        else
          snprintf(pb,sizeof(pb),"IQ %d:%02d / %d:%02d   BW %.1f kHz",
                   e/60,e%60,t/60,t%60,bw_hz/1e3);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, pb, &ext);
        double px = (double)display_width - ext.width - 6.0;
        if(px < 42.0) px = 42.0;   // keep clear of the left dB scale strip
        cairo_set_source_rgba(cr, 0.55, 0.85, 1.0, 0.9);   // light cyan
        cairo_move_to(cr, px, 14.0);
        cairo_show_text(cr, pb);
      }
    }

    cairo_set_line_width (cr, LINE_WIDTH);
    // plot the levels
    
    pan_background(cr);
    //cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
    cairo_rectangle(cr,0,0,40,display_height);
    cairo_fill(cr);    
    
    // dB graticule spacing follows the "Step" slider (rx->panadapter_step),
    // not a hardcoded 20 dB - otherwise the slider is inert. Guard against a
    // 0/negative step (would be a %0 divide) by falling back to 20.
    int db_step = rx->panadapter_step>0 ? rx->panadapter_step : 20;
    for(i=rx->panadapter_high;i>=rx->panadapter_low;i--) {
      SetColour(cr, DARK_LINES);
      int mod=abs(i)%db_step;
      if(mod==0) {
        double y = (double)(rx->panadapter_high-i)*dbm_per_line;
        cairo_move_to(cr,0.0,y);

        static const double dashed2[] = {2.0, 2.0};
        static int len2  = sizeof(dashed2) / sizeof(dashed2[0]);
        cairo_set_dash(cr, dashed2, len2, 0);

        cairo_line_to(cr,(double)display_width,y);
        // With a very small step the labels would collide; at step<=3 dB print
        // the number on every third gridline (the lines are still all drawn).
        if(db_step>3 || (abs(i)/db_step)%3==0) {
          if(rx->panadapter_gradient) SetColour(cr, TEXT_B);
          sprintf(temp," %d",i);
          cairo_move_to(cr, 5, y-4);  // lift the label clear of the graticule line
          cairo_show_text(cr, temp);
        }
      }
    }
    SetColour(cr, DARK_LINES);      
    cairo_stroke(cr);



    // plot frequency markers
    
    pan_background(cr);
    cairo_rectangle(cr,0, (rx->panadapter_height-20), display_width, (rx->panadapter_height));
    cairo_fill(cr);        

    // if zoom > 1 - show the pan position
    if(rx->zoom!=1) {
      int pan_x=(int)((double)rx->pan/(double)rx->zoom);
      int pan_width=(int)((double)rx->panadapter_width/(double)rx->zoom);
      cairo_set_source_rgb (cr, 0.7, 0.7, 0.7);
      cairo_rectangle(cr,pan_x, (rx->panadapter_height-4), pan_width, (rx->panadapter_height));
      cairo_fill(cr);        
    }

    long long f1;
    long long f2;
    long long divisor1=20000;
    long long divisor2=5000;
    long long factor=(long long)(rx->sample_rate/48000);
    if(factor>10LL) factor=10LL;
    switch(rx->zoom) {
      case 1:
      case 2:
      case 3:
        divisor1=5000LL*factor;
        divisor2=1000LL*factor;
        break;
      case 4:
      case 5:
      case 6:
        divisor1=1000LL*factor;
        divisor2=500LL*factor;
        break;
      case 7:
      case 8:
      default:   // 7, 8 and the higher FT8 zoom levels (up to 16x)
        divisor1=1000LL*factor;
        divisor2=200LL*factor;
        break;
    }
    cairo_set_line_width(cr, LINE_WIDTH);

    f1=frequency-half+(long long)(rx->hz_per_pixel*offset);
    if (rx->mode_a==CWU) {
      f1 -= radio->cw_keyer_sidetone_frequency;
    }
    else if (rx->mode_a==CWL) {
      f1 += radio->cw_keyer_sidetone_frequency;
    }    
    f2=(f1/divisor2)*divisor2;

    int x=0;
    double last_text_end = -1000.0;  // right edge of the last frequency label drawn
    do {
      x=(int)(f2-f1)/rx->hz_per_pixel;
      if(x>70) {
        if((f2%divisor1)==0LL) {
          SetColour(cr, DARK_LINES);
          cairo_move_to(cr,(double)x,0);
          cairo_line_to(cr,(double)x,(double)display_height-20);
          cairo_stroke(cr);
          SetColour(cr, TEXT_B);
          cairo_select_font_face(cr, "Noto Sans",
                              CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
          cairo_set_font_size(cr, 12);
          sprintf(temp,"%0lld.%03lld",f2/1000000,(f2%1000000)/1000);
          cairo_text_extents(cr, temp, &extents);
          double text_x = (double)x-(extents.width/2.0);
          // Draw the label only if it clears the previous one; the gridline is
          // still drawn above. Prevents labels colliding when they get dense
          // (wide labels / small pixel spacing at high freqs and wide spans).
          if(text_x > last_text_end + 6.0) {
            cairo_move_to(cr, text_x, (rx->panadapter_height - 6));
            cairo_show_text(cr, temp);
            last_text_end = text_x + extents.width;
          }
        } else if((f2%divisor2)==00LL) {
          SetColour(cr, DARK_LINES);
          cairo_move_to(cr,(double)x,0);
          cairo_line_to(cr,(double)x,(double)display_height-20);
          cairo_stroke(cr);
        }
      }
      f2=f2+divisor2;
    } while(x<display_width);
    


    if(rx->band_a!=band60) {
      // band edges
      if(band->frequencyMin!=0LL) {
        SetColour(cr, WARNING);
        cairo_set_line_width(cr, 2.0);
        if((min_display<band->frequencyMin)&&(max_display>band->frequencyMin)) {
          i=(int)(((double)band->frequencyMin-(double)min_display)/rx->hz_per_pixel);
          i -= cw_offset / rx->hz_per_pixel;
          cairo_move_to(cr,(double)i,0.0);
          cairo_line_to(cr,(double)i,(double)display_height-20);
          cairo_stroke(cr);
        }
        if((min_display<band->frequencyMax)&&(max_display>band->frequencyMax)) {
          i=(int)(((double)band->frequencyMax-(double)min_display)/rx->hz_per_pixel);
          i -= cw_offset / rx->hz_per_pixel;
          cairo_move_to(cr,(double)i,0.0);
          cairo_line_to(cr,(double)i,(double)display_height-20);
          cairo_stroke(cr);
        }
        cairo_set_line_width(cr, LINE_WIDTH);
      }
    }
    
    cairo_set_dash(cr, 0, 0, 0);
    // agc
    if(rx->agc!=AGC_OFF) {
      double x=80.0;

      // Use the values cached by the display timer under rx->mutex; calling
      // WDSP here would race with the RX thread (this runs unlocked).
      double hang=rx->agc_hang_level;
      double thresh=rx->agc_thresh_level;
    
      if(rx->panadapter_agc_line) {
        double knee_y=thresh+attenuation+radio->panadapter_calibration;      
        knee_y = floor((rx->panadapter_high - knee_y)*dbm_per_line);
  
        double hang_y=hang+attenuation+radio->panadapter_calibration;    
        hang_y = floor((rx->panadapter_high - hang_y)*dbm_per_line);
  
        if(rx->agc!=AGC_MEDIUM && rx->agc!=AGC_FAST) {
          SetColour(cr, TEXT_A);
          cairo_move_to(cr,x,hang_y-8.0);
          cairo_rectangle(cr, x, hang_y-8.0,8.0,8.0);
          cairo_fill(cr);
          cairo_move_to(cr,x,hang_y);
          cairo_line_to(cr,(double)display_width-x,hang_y);
          cairo_stroke(cr);
          cairo_move_to(cr,x+8.0,hang_y);
          cairo_show_text(cr, "-H");
        }
  
        SetColour(cr, TEXT_C);
        cairo_move_to(cr,x,knee_y-8.0);
        cairo_rectangle(cr, x, knee_y-8.0,8.0,8.0);
        cairo_fill(cr);
        cairo_move_to(cr,x,knee_y);
        cairo_line_to(cr,(double)display_width-x,knee_y);
        cairo_stroke(cr);
        cairo_move_to(cr,x+8.0,knee_y);
        cairo_show_text(cr, "-G");      
      }
    }


    // cursor
    SetColour(cr, TEXT_B);  
    cairo_move_to(cr,(double)(rx->pixels/2.0)-(double)rx->pan+(rx->ctun_offset/rx->hz_per_pixel) - (cw_offset/rx->hz_per_pixel),0.0);
    cairo_line_to(cr,(double)(rx->pixels/2.0)-(double)rx->pan+(rx->ctun_offset/rx->hz_per_pixel) - (cw_offset/rx->hz_per_pixel),(double)display_height-20);
    cairo_stroke(cr);    
    
    // Frequency marker vfo b    
    if(rx->subrx!=NULL) {   
      i=(int)(((double)rx->frequency_b-(double)min_display)/rx->hz_per_pixel);
      i -= cw_offset / rx->hz_per_pixel;
      SetColour(cr, TEXT_C);        
      cairo_move_to(cr,(double)i,0.0);
      cairo_line_to(cr,(double)i,(double)display_height-20);
      cairo_stroke(cr);     
    }
    
    // signal
    
    
    double s2;
    
    samples[display_width-1+offset]=-200;

    // Peak-hold overlay: per-pixel running maximum with a configurable
    // dB/second decay, tracked once per frame before the main trace draws.
    if(rx->panadapter_peak_hold && rx->panadapter_peaks!=NULL) {
      double dec = (rx->panadapter_peak_decay<=0) ? 0.0 : ((double)rx->panadapter_peak_decay/(double)rx->fps);
      for(int j=offset; j<offset+display_width && j<rx->pixels; j++) {
        float s = samples[j];
        if(dec>0.0) {
          float d=rx->panadapter_peaks[j]-(float)dec;
          rx->panadapter_peaks[j] = s>d ? s : d;
        } else {
          if(s>rx->panadapter_peaks[j]) rx->panadapter_peaks[j]=s;
        }
      }
    }

    // When the phosphor/histogram is on it IS the spectrum display, so skip the
    // ordinary trace line/fill - it would just paint a solid colour over the
    // graded cloud and hide it. The dB/frequency scales already drew above and
    // the phosphor was drawn behind them, so everything stays clean.
    if(!rx->panadapter_histogram) {
    cairo_move_to(cr, 0.0, display_height-20);

    for(i=1;i<display_width;i++) {
      s2=(double)samples[i+offset]+attenuation+radio->panadapter_calibration;
      s2 = floor((rx->panadapter_high - s2) *dbm_per_line);
      if (s2 >= rx->panadapter_height-20) {
        s2 = rx->panadapter_height-20;
      }
      cairo_line_to(cr, (double)i, s2);
    }
  
    
      
    if(rx->panadapter_single_color == 0) {
        cairo_pattern_t *gradient;
        if(rx->panadapter_gradient) {
            gradient = cairo_pattern_create_linear(0.0, rx->panadapter_height-20, 0.0, 0.0);
            // calculate where S9 is
            double S9=-73;
            if(rx->frequency_a>30000000LL) {
                S9=-93;
            }
            S9 = floor((rx->panadapter_high - S9)
                       * (double)(rx->panadapter_height-20)
                            / (rx->panadapter_high - rx->panadapter_low));
            S9 = 1.0-(S9/(double)(rx->panadapter_height-20));

            cairo_pattern_add_color_stop_rgb (gradient,0.0,0.0,1.0,0.0); // Green
            cairo_pattern_add_color_stop_rgb (gradient,S9/3.0,1.0,0.65,0.0); // Orange
            cairo_pattern_add_color_stop_rgb (gradient,(S9/3.0)*2.0,1.0,1.0,0.0); // Yellow
            cairo_pattern_add_color_stop_rgb (gradient,S9,1.0,0.0,0.0); // Red
            cairo_set_source(cr, gradient);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0,0.5);
        }

        if(rx->panadapter_filled) {
            cairo_close_path (cr);
            /*
              cairo_pattern_t *pat=cairo_pattern_create_linear(0.624,	0.427,	0.690,(rx->panadapter_height-20));     
              cairo_pattern_add_color_stop_rgba(pat,0.0,0.804,	0.635,	0.859,0.5);
              cairo_pattern_add_color_stop_rgba(pat,1.0,0.804,	0.635,	0.859,0.5);
              cairo_set_source (cr, pat);
            */
            cairo_fill_preserve(cr);
            //cairo_pattern_destroy(pat);
        }
        //cairo_set_source_rgb(cr, 0.804,	0.635,	0.859);
        cairo_stroke(cr);
        if(rx->panadapter_gradient) {
            cairo_pattern_destroy(gradient);
        }
        
   }
    else {
      // Base trace colour: the user's single-colour choice, or -- for the
      // default -- the active skin's accent, so the spectrum matches the theme.
      double sr=0.804, sg=0.635, sb=0.859;
      switch(rx->panadapter_single_color) {
        case 2: sr=0.769; sg=0.117; sb=0.227; break; // red
        case 3: sr=1.0;   sg=0.459; sb=0.095; break; // orange
        case 4: sr=1.0;   sg=0.850; sb=0.0;   break; // yellow
        case 5: sr=0.133; sg=0.545; sb=0.133; break; // green
        case 6: sr=0.0;   sg=0.184; sb=0.655; break; // blue
        case 7: sr=0.4;   sg=0.0;   sb=0.6;   break; // violet
        case 8: sr=0.90;  sg=0.0;   sb=0.90;  break; // magenta
        case 9: sr=0.0;   sg=0.9;   sb=0.9;   break; // cyan
        case 1:
        default: css_rgb("ACCENT_A",&sr,&sg,&sb); break; // follow the skin
      }

      if(rx->panadapter_filled) {
        cairo_close_path (cr);
        // Solid, constant-alpha fill in the trace colour. A vertical alpha
        // gradient ("fade under the curve") was tried but a *varying* gradient
        // forces per-pixel compositing over the whole fill area every frame --
        // and this runs inline on the RX/audio thread, so on a large panadapter
        // it stole enough time to stutter the stream. A constant fill is the
        // cheap path (cairo treats it as solid).
        cairo_set_source_rgba(cr, sr,sg,sb, 0.5);
        cairo_fill_preserve(cr);
      }
      cairo_set_line_width(cr, LINE_WIDTH);
      cairo_set_source_rgb(cr, sr,sg,sb);
      cairo_stroke(cr);
                if (radio->divmixer[rx->dmix_id] != NULL) {
            if ((radio->divmixer[rx->dmix_id]->calibrate_gain) && (!gain_cal_error)) {

                // signal - hidden_rx
                double s2_hidden_rx;

                samples_hidden_rx[display_width-1+offset]=-200;

                cairo_move_to(cr, 0.0, display_height-20);

                for(i = 1; i < display_width; i++) {
                    s2_hidden_rx = (double)samples_hidden_rx[i+offset]+attenuation+radio->panadapter_calibration;
                    s2_hidden_rx = floor((rx->panadapter_high - s2_hidden_rx) *dbm_per_line);
                    if (s2_hidden_rx >= rx->panadapter_height-20) {
                        s2_hidden_rx = rx->panadapter_height-20;
                    }
                    cairo_line_to(cr, (double)i, s2_hidden_rx);
                }
                // turquoise
                cairo_set_source_rgb(cr, 0.259, 0.960, 0.950);
                cairo_stroke(cr);
            }
        }
    }
    }  // end if(!panadapter_histogram): ordinary trace suppressed under phosphor

    // Peak-hold trace: same mapping as the main trace, line only (no fill),
    // drawn last so it sits on top.
    if(rx->panadapter_peak_hold && rx->panadapter_peaks!=NULL) {
      cairo_set_line_width(cr, LINE_WIDTH);
      cairo_set_source_rgba(cr, 0.95, 0.95, 0.95, 0.85);   // light grey/white, distinct from any trace colour
      cairo_move_to(cr, 0.0, display_height-20);
      for(i=1;i<display_width;i++) {
        double ph=(double)rx->panadapter_peaks[i+offset]+attenuation+radio->panadapter_calibration;
        ph=floor((rx->panadapter_high - ph)*dbm_per_line);
        if(ph >= rx->panadapter_height-20) ph=rx->panadapter_height-20;
        cairo_line_to(cr, (double)i, ph);
      }
      cairo_stroke(cr);
    }

    cairo_destroy (cr);
    gtk_widget_queue_draw (rx->panadapter);
  }
}
