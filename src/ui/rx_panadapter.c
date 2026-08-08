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

/*
 * RX panadapter — GPU-rendered spectrum display.
 *
 * The scene is emitted as GTK4 GSK render nodes (append_color / append_stroke /
 * append_fill / append_linear_gradient / append_layout / append_texture) from a
 * PanaView snapshot() builder, so the GPU rasterises it.  It used to be a
 * GtkDrawingArea whose draw-func rasterised the whole scene into a
 * CAIRO_FORMAT_RGB24 image surface every frame and blitted it — a cost
 * proportional to the window area (CPU raster + blit + full-area texture
 * upload), which is why maximising the window stuttered.  Only the optional
 * phosphor/persistence heatmap stays a bitmap; it is uploaded once per frame as
 * a GdkMemoryTexture, exactly like the waterfall.
 *
 * Threading split: the once-per-frame STATE that must stay frame-rate-locked
 * (peak-hold decay, phosphor occupancy EMA + colour-map) is updated in
 * update_rx_panadapter(), which the fps timer calls with fresh WDSP pixels; the
 * builder rx_pana_build() only DRAWS from the current state (pixel_samples, the
 * peak buffer, the phosphor surface, the vectorscope snapshot).  Both run on the
 * GTK main thread, so no extra lock is needed between them (the scope I/Q tap is
 * the one exception and keeps its scope_mutex).
 */

#include <gtk/gtk.h>
#include "log.h"
#include <epoxy/gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#ifdef SSTV
#include "apt_decoder.h"
#endif
#include "main.h"
#include "vfo.h"
#include "level_meter.h"
#include "css.h"
#include "dxcluster.h"
#include "pana_view.h"

#define LINE_WIDTH 1.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int signal_vertices_size=-1;
float *signal_vertices=NULL;

// ---------------------------------------------------------------------------
// Small GSK render-node helpers. These replace the cairo primitives 1:1; the
// geometry maths in the builder below is unchanged from the old cairo version.
// ---------------------------------------------------------------------------

static inline GdkRGBA nrgba(double r,double g,double b,double a) {
  GdkRGBA c={(float)r,(float)g,(float)b,(float)a}; return c;
}
// Skin palette colour by name, as a GdkRGBA (fallback if the name is absent).
static inline GdkRGBA css_rgba(const char *name,double r,double g,double b,double a) {
  css_rgb(name,&r,&g,&b); return nrgba(r,g,b,a);
}

// FPS / frame-time readout, gated by MACHPSDR_FPS (a diagnostic overlay drawn on
// the panadapter). Measures the real render rate (this builder runs once per
// presented frame) and how long the node-tree build takes on the CPU — if FPS is
// low but build is small, the cost is in the GSK/GL render of the nodes.
static inline double pan_now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (double)t.tv_sec*1000.0 + (double)t.tv_nsec/1e6;
}
static int pan_fps_show=-1;          // -1 = unread, else 0/1 from env
static double pan_fps_ema=0.0;       // smoothed frames/sec
static double pan_build_ema=0.0;     // smoothed node-build ms
static double pan_fps_last=0.0;      // last frame timestamp

// Solid/translucent filled rectangle.
static void n_rect(GtkSnapshot *s,double x,double y,double w,double h,const GdkRGBA *c) {
  if(w<=0.0 || h<=0.0) return;
  graphene_rect_t r=GRAPHENE_RECT_INIT((float)x,(float)y,(float)w,(float)h);
  gtk_snapshot_append_color(s,c,&r);
}

// Single stroked segment.
static void n_line(GtkSnapshot *s,double x1,double y1,double x2,double y2,double lw,const GdkRGBA *c) {
  GskPathBuilder *b=gsk_path_builder_new();
  gsk_path_builder_move_to(b,(float)x1,(float)y1);
  gsk_path_builder_line_to(b,(float)x2,(float)y2);
  GskPath *p=gsk_path_builder_free_to_path(b);
  GskStroke *st=gsk_stroke_new((float)lw);
  gtk_snapshot_append_stroke(s,p,st,c);
  gsk_stroke_free(st);
  gsk_path_unref(p);
}

// Build a fresh Noto Sans layout at an absolute pixel size (used only for the
// rare non-12px text — e.g. the 18px "no data" message). Caller unrefs.
static PangoLayout *n_layout(GtkWidget *widget,double size,const char *txt) {
  PangoLayout *l=gtk_widget_create_pango_layout(widget,txt);
  PangoFontDescription *fd=pango_font_description_new();
  pango_font_description_set_family(fd,"Noto Sans");
  pango_font_description_set_absolute_size(fd,size*PANGO_SCALE);
  pango_layout_set_font_description(l,fd);
  pango_font_description_free(fd);
  return l;
}

// Reused 12px Noto Sans label layout. Every per-frame panadapter label (dB /
// frequency numbers, "TX", the IQ readout, AGC "-H"/"-G") is 12px ASCII, so one
// cached layout + pango_layout_set_text() per label removes ~one PangoLayout
// allocation per label per frame from the render thread. Main-thread-only, so a
// file static shared across receivers (same font/context) is safe.
static PangoLayout *g_label_layout=NULL;
static PangoLayout *label12(GtkWidget *widget,const char *txt) {
  if(g_label_layout==NULL) {
    g_label_layout=gtk_widget_create_pango_layout(widget,NULL);
    PangoFontDescription *fd=pango_font_description_new();
    pango_font_description_set_family(fd,"Noto Sans");
    pango_font_description_set_absolute_size(fd,12*PANGO_SCALE);
    pango_layout_set_font_description(g_label_layout,fd);
    pango_font_description_free(fd);
  }
  pango_layout_set_text(g_label_layout,txt,-1);
  return g_label_layout;
}

// Draw 12px text with its BASELINE at (x, base_y) — matching cairo show_text.
// Returns the advance width in *out_w when non-NULL. Uses the cached layout.
static void n_text(GtkSnapshot *s,GtkWidget *widget,double x,double base_y,
                   const GdkRGBA *c,const char *txt,double *out_w) {
  PangoLayout *l=label12(widget,txt);
  int pw=0,ph=0; pango_layout_get_pixel_size(l,&pw,&ph);
  if(out_w) *out_w=(double)pw;
  double top=base_y-(double)pango_layout_get_baseline(l)/PANGO_SCALE;
  gtk_snapshot_save(s);
  graphene_point_t pt=GRAPHENE_POINT_INIT((float)x,(float)top);
  gtk_snapshot_translate(s,&pt);
  gtk_snapshot_append_layout(s,l,c);
  gtk_snapshot_restore(s);
}

// Draw 12px text over a solid background box (baseline at x, base_y) so the dB /
// frequency graticule lines don't strike through the label and make it hard to
// read. The box is padded 1px around the glyph bounds. Uses the cached layout.
static void n_text_boxed(GtkSnapshot *s,GtkWidget *widget,double x,double base_y,
                         const GdkRGBA *c,const GdkRGBA *bg,const char *txt,double *out_w) {
  PangoLayout *l=label12(widget,txt);
  int pw=0,ph=0; pango_layout_get_pixel_size(l,&pw,&ph);
  if(out_w) *out_w=(double)pw;
  double top=base_y-(double)pango_layout_get_baseline(l)/PANGO_SCALE;
  n_rect(s,x-1.0,top-1.0,(double)pw+2.0,(double)ph+2.0,bg);
  gtk_snapshot_save(s);
  graphene_point_t pt=GRAPHENE_POINT_INIT((float)x,(float)top);
  gtk_snapshot_translate(s,&pt);
  gtk_snapshot_append_layout(s,l,c);
  gtk_snapshot_restore(s);
}

// Draw text at an arbitrary size (ad-hoc layout, for the rare non-12px path).
static void n_text_sz(GtkSnapshot *s,GtkWidget *widget,double x,double base_y,double size,
                      const GdkRGBA *c,const char *txt) {
  PangoLayout *l=n_layout(widget,size,txt);
  double top=base_y-(double)pango_layout_get_baseline(l)/PANGO_SCALE;
  gtk_snapshot_save(s);
  graphene_point_t pt=GRAPHENE_POINT_INIT((float)x,(float)top);
  gtk_snapshot_translate(s,&pt);
  gtk_snapshot_append_layout(s,l,c);
  gtk_snapshot_restore(s);
  g_object_unref(l);
}

// Measure a 12px label's advance width without drawing it (cached layout).
static double n_measure(GtkWidget *widget,const char *txt) {
  PangoLayout *l=label12(widget,txt);
  int pw=0,ph=0; pango_layout_get_pixel_size(l,&pw,&ph);
  return (double)pw;
}

// The spectrum trace is drawn as per-column vertical `append_color` rects, NOT a
// stroked/filled GskPath. On the macOS GL renderer (the only GPU renderer in the
// stock gtk4 build) a stroked/filled path spanning the whole panadapter is
// rasterised on a slow path whose cost scales with the trace's bbox AREA — so the
// FPS drops as the window gets taller (measured: build stays ~0.3 ms but FPS
// 60->40 as the panadapter grows), and with the phosphor on (which suppresses the
// trace) it is smooth. append_color rects are the renderer's native fast path
// (batched instanced quads, no tessellation, no cairo fallback), so the trace
// cost stops scaling with area. Each column i draws a 1px-wide rect covering the
// vertical span between this sample and the previous (a connected line) or, when
// filled, down to the baseline.

// Amplitude t in [0,1] (0 = bottom/green .. S9 = red) -> the S9 green/orange/
// yellow/red gradient colour, for the per-column gradient trace.
static GdkRGBA pan_grad_rgba(double t, double S9) {
  if(t<0.0) t=0.0; if(t>1.0) t=1.0;
  double a=S9/3.0, c=2.0*S9/3.0, r,g,b;
  if(t<=a)       { double u=(a>0.0)?t/a:0.0;             r=u;   g=1.0-0.35*u; b=0.0; } // green->orange
  else if(t<=c)  { double u=((c-a)>0.0)?(t-a)/(c-a):0.0; r=1.0; g=0.65+0.35*u; b=0.0; } // orange->yellow
  else if(t<=S9) { double u=((S9-c)>0.0)?(t-c)/(S9-c):0.0;r=1.0; g=1.0-u;      b=0.0; } // yellow->red
  else           {                                       r=1.0; g=0.0;        b=0.0; } // red
  return nrgba(r,g,b,1.0);
}

// Draw a spectrum trace for `samples` as vertical color rects. base_y = the
// baseline (display_height-20), y_clamp = the bottom clamp (panadapter_height-20),
// add_db = attenuation + panadapter_calibration. gradient!=0 colours each column
// by amplitude (S9 in [0,1]); otherwise `solid` is used. filled draws each column
// down to base_y instead of connecting to the previous sample.
static void pan_trace_rects(GtkSnapshot *snapshot, const float *samples, int offset,
                            int display_width, double add_db, double pan_high,
                            double dbm_per_line, double base_y, double y_clamp,
                            gboolean gradient, double S9, GdkRGBA solid, gboolean filled) {
  double y_prev=0.0; gboolean have_prev=FALSE;
  for(int i=1;i<display_width;i++) {
    double s=(double)samples[i+offset]+add_db;
    double y=floor((pan_high - s)*dbm_per_line);
    if(y>=y_clamp) y=y_clamp;
    if(y<0.0) y=0.0;
    GdkRGBA c = gradient ? pan_grad_rgba(1.0 - y/base_y, S9) : solid;
    if(filled) {
      double h=base_y-y; if(h<0.0) h=0.0;
      n_rect(snapshot,(double)i,y,1.0,h,&c);
    } else {
      double top = have_prev ? (y<y_prev?y:y_prev) : y;
      double h   = have_prev ? fabs(y-y_prev) : 0.0;
      if(h<LINE_WIDTH) h=LINE_WIDTH;
      n_rect(snapshot,(double)i,top,1.0,h,&c);
    }
    y_prev=y; have_prev=TRUE;
  }
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

  // GSK render-node path: no off-screen panadapter_surface to (re)allocate — the
  // snapshot builder draws straight into the render tree.  receiver_init_analyzer
  // already dropped the phosphor buffers so they are re-made at the new size.
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
  glClearColor (0, 0, 0, 0);
  glClear (GL_COLOR_BUFFER_BIT);

  if(signal_vertices_size!=-1) {
    glLineWidth(2.0);
    glColor3f(1.0,1.0,0.0);
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, signal_vertices_size*sizeof(float)*2, signal_vertices, GL_STREAM_DRAW);

    glUseProgram(gl_program);
    glBindVertexArray(gl_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDrawArrays(GL_LINE_STRIP,0,signal_vertices_size);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
  }
  return TRUE;
}

static void rx_panadapter_realize (GtkGLArea *area)
{
  gtk_gl_area_make_current (area);
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

// PanaView "resize" signal (same name/signature as the old GtkDrawingArea one).
static void rx_panadapter_resize_cb(GtkWidget *area,int width,int height,gpointer data) {
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

static void rx_pana_build(GtkSnapshot *snapshot, int display_width, int display_height, gpointer data);

GtkWidget *create_rx_panadapter(RECEIVER *rx) {
  GtkWidget *panadapter;

  rx->panadapter_width=0;
  rx->panadapter_height=0;
  rx->panadapter_surface=NULL;
  rx->panadapter_resize_timer=-1;
  rx->pan_running=FALSE;

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
    // GPU render-node widget (see pana_view.c). The builder emits GSK nodes;
    // the existing gtk_widget_queue_draw() calls keep driving repaints.
    panadapter = pana_view_new(rx_pana_build,(gpointer)rx);
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

// Row-packed DX-cluster spot overlay, shared by the panadapter and the
// waterfall's cairo overlay. min_display is recomputed from rx so the same
// absolute-RF -> x mapping the trace uses applies on either widget; font size +
// label background colour come from the persisted RADIO settings. Kept on cairo
// (drawn through a small append_cairo node on the panadapter) so the panadapter
// and waterfall share one implementation — it is a tiny, label-only overlay, not
// an area-scaling cost.
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

    // Label + tick colour: by DXCC entity (default) or the operator's fixed
    // colour when "colour by DXCC entity" is off.
    double sr,sg,sb,sa;
    if(radio->cluster_spots_fg_dxcc) {
      cluster_spot_rgb(s->entity,&sr,&sg,&sb);
      sa=0.95;
    } else {
      sr=radio->cluster_spots_fg_r; sg=radio->cluster_spots_fg_g;
      sb=radio->cluster_spots_fg_b; sa=radio->cluster_spots_fg_a;
    }
    // tick reaching down to this spot's own label row
    cairo_set_source_rgba(cr, sr, sg, sb, sa*0.95);
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
    cairo_set_source_rgba(cr, sr, sg, sb, sa);
    cairo_move_to(cr, x+2.0, ty);
    cairo_show_text(cr, s->call);
  }
  #undef SPOT_LABEL_ROWS
  dxcluster_unlock();
}

// GSK-node version of the DX-cluster overlay, for the panadapter. Mirrors the
// cairo layout above but emits nodes (tick line, background rect, callsign text)
// so it costs no full-window append_cairo offscreen surface + CPU raster + upload
// per frame — the big per-frame cost on the GL renderer when the cluster is on
// at fullscreen. The waterfall keeps the cairo version (its GpuImage overlay is
// cairo). Uses lm_* helpers (level_meter.c) via label metrics from pango.
static void receiver_draw_cluster_spots_nodes(GtkSnapshot *snapshot, GtkWidget *widget,
                                              RECEIVER *rx, int display_width) {
  if(rx->hz_per_pixel==0.0) return;
  long long half=(long long)rx->sample_rate/2LL;
  long long min_display=(rx->frequency_a - half) + (long long)((double)rx->pan*rx->hz_per_pixel);

  dxcluster_lock();
  int ns=dxcluster_count();
  struct { double x; const DX_SPOT *s; } vis[DXCLUSTER_MAX_SPOTS];
  int nv=0;
  for(int i=0;i<ns;i++) {
    const DX_SPOT *s=dxcluster_spot(i);
    if(s==NULL) continue;
    double x=((double)s->freq - (double)min_display)/rx->hz_per_pixel;
    if(x<0.0 || x>(double)display_width) continue;
    vis[nv].x=x; vis[nv].s=s; nv++;
  }
  for(int a=1;a<nv;a++) {
    double kx=vis[a].x; const DX_SPOT *ks=vis[a].s; int b=a-1;
    while(b>=0 && vis[b].x>kx) { vis[b+1]=vis[b]; b--; }
    vis[b+1].x=kx; vis[b+1].s=ks;
  }

  double fs=(double)radio->cluster_spots_font;
  if(fs<7.0) fs=7.0; if(fs>28.0) fs=28.0;
  double bg_r=radio->cluster_spots_bg_r, bg_g=radio->cluster_spots_bg_g,
         bg_b=radio->cluster_spots_bg_b, bg_a=radio->cluster_spots_bg_a;
  #define SPOT_LABEL_ROWS 8
  double row_right[SPOT_LABEL_ROWS];
  for(int r=0;r<SPOT_LABEL_ROWS;r++) row_right[r]=-1e9;
  const double row_h=fs+1.0, base_y=fs+3.0;
  const double text_top=fs*0.80;   // approx ascent above the baseline (for tick end + bg box)

  for(int a=0;a<nv;a++) {
    double x=vis[a].x;
    const DX_SPOT *s=vis[a].s;
    double tw=lm_measure(widget, fs, s->call);

    int row=0, bestrow=0; double best=1e18;
    for(row=0;row<SPOT_LABEL_ROWS;row++) {
      if(row_right[row]<best) { best=row_right[row]; bestrow=row; }
      if(x > row_right[row]+3.0) break;
    }
    if(row>=SPOT_LABEL_ROWS) row=bestrow;
    double ty=base_y + row*row_h;
    row_right[row]=x+tw+2.0;

    double sr,sg,sb,sa;
    if(radio->cluster_spots_fg_dxcc) {
      cluster_spot_rgb(s->entity,&sr,&sg,&sb); sa=0.95;
    } else {
      sr=radio->cluster_spots_fg_r; sg=radio->cluster_spots_fg_g;
      sb=radio->cluster_spots_fg_b; sa=radio->cluster_spots_fg_a;
    }
    // tick from the top down to the top of this spot's own label row
    GdkRGBA tick=nrgba(sr,sg,sb,sa*0.95);
    lm_line(snapshot, x, 0.0, x, ty-text_top, 1.0, &tick);
    // background box behind the callsign
    if(bg_a>0.0) {
      GdkRGBA bg=nrgba(bg_r,bg_g,bg_b,bg_a);
      lm_fill(snapshot, x+2.0-1.0, ty-text_top-1.0, tw+2.0, fs+2.0, &bg);
    }
    // callsign text
    GdkRGBA txt=nrgba(sr,sg,sb,sa);
    lm_text(snapshot, widget, x+2.0, ty, fs, &txt, s->call, FALSE);
  }
  #undef SPOT_LABEL_ROWS
  dxcluster_unlock();
}

static gboolean first_time=TRUE;

// Phosphor / "digital phosphor" persistence heatmap. EMA occupancy per cell (the
// fraction of recent frames the trace was there), colour-mapped into a reusable
// RGB24 byte buffer that the builder uploads as a texture. Accumulated + mapped
// at HALF resolution (HW x HH); the builder lets the GPU upscale it with a linear
// filter. The two O(area) loops (decay + colour-map) then cost 1/4 as much — the
// only per-frame CPU work that scaled with the window area — and the soft cloud
// hides the interpolation. Frame-locked: called once per fresh frame from the
// fps timer (and from the headless render test).
static void phosphor_accumulate(RECEIVER *rx, int display_width, int display_height,
                                float *samples, int offset, double attenuation, double dbm_per_line) {
  if(!(rx->panadapter_histogram && rx->panadapter_histogram_bins!=NULL
       && display_width==rx->panadapter_histogram_w && display_height==rx->panadapter_histogram_h)) return;
  int HW=(display_width+1)/2, HH=(display_height+1)/2;   // half-res dims (match receiver.c alloc)
  float *bins=rx->panadapter_histogram_bins;            // sized HW*HH, column-major (stride HH)
  int fps = rx->fps>0 ? rx->fps : 15;
  float keep = expf(-(float)rx->panadapter_histogram_decay/(20.0f*(float)fps));
  float add = 1.0f - keep;   // EMA step toward occupancy = 1
  int total = HW*HH;
  for(int k=0;k<total;k++) bins[k]*=keep;
  for(int hx=0;hx<HW;hx++) {
    // Each half-column covers two screen columns; take the stronger so a narrow
    // carrier on either column still lights the cell.
    int c0=hx*2, c1=c0+1; if(c1>=display_width) c1=display_width-1;
    float sA=samples[c0+offset], sB=samples[c1+offset];
    double sm=(double)(sA>sB?sA:sB)+attenuation+radio->panadapter_calibration;
    int yr=(int)floor((rx->panadapter_high - sm)*dbm_per_line);
    if(yr<0) yr=0; if(yr>=display_height) yr=display_height-1;
    int hyc=yr/2;
    for(int dz=-1; dz<=1; dz++) {              // +/-1 half-row (~ +/-2 screen px after upscale)
      int hy=hyc+dz;
      if(hy<0 || hy>=HH) continue;
      float w = dz==0 ? 1.0f : 0.5f;
      bins[hx*HH+hy]+=w*add;
    }
  }
  // Heat-colour LUT (occupancy^0.4 -> 5-stop ramp), baked once. Each entry is the
  // little-endian B,G,R,x byte order of CAIRO_FORMAT_RGB24, wrapped as B8G8R8X8.
  static gboolean heat_lut_ready=FALSE;
  static unsigned char heat_lut[256][3];   // {B,G,R}
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
  cairo_surface_t *hs=rx->panadapter_histogram_surface;
  if(hs==NULL) {
    hs=cairo_image_surface_create(CAIRO_FORMAT_RGB24, HW, HH);
    rx->panadapter_histogram_surface=hs;
  }
  unsigned char *hd=cairo_image_surface_get_data(hs);
  int stride=cairo_image_surface_get_stride(hs);
  double bgr=0.09,bgg=0.09,bgb=0.10; css_rgb("SPECTRUM_BG",&bgr,&bgg,&bgb);
  unsigned char bgb8=(unsigned char)(bgb*255.0), bgg8=(unsigned char)(bgg*255.0), bgr8=(unsigned char)(bgr*255.0);
  for(int hy=0;hy<HH;hy++) {
    unsigned char *row=hd+hy*stride;
    for(int hx=0;hx<HW;hx++) {
      float d=bins[hx*HH+hy];   // absolute occupancy 0..1 (fraction of recent frames lit)
      if(d<=0.015f) {           // essentially unvisited -> background
        row[hx*4+0]=bgb8; row[hx*4+1]=bgg8; row[hx*4+2]=bgr8; row[hx*4+3]=255;
      } else {
        int li=(int)(d*255.0f); if(li>255) li=255;   // also clamps d>1
        row[hx*4+0]=heat_lut[li][0];
        row[hx*4+1]=heat_lut[li][1];
        row[hx*4+2]=heat_lut[li][2];
        row[hx*4+3]=255;
      }
    }
  }
  cairo_surface_mark_dirty(hs);
}

// ---------------------------------------------------------------------------
// Automatic dB scale ("Panadapter Automatic").
//
// Fits the vertical scale between the band noise floor and the strongest signal
// in view, so a quiet band and a loud one both fill the window without touching
// the High/Low sliders. Two decisions matter:
//
//  - the floor is a LOW PERCENTILE of the visible trace, not the minimum: the
//    minimum tracks the deepest notch of the moment and jitters, while the 30th
//    percentile is the noise the band actually sits at (on a crowded band most
//    pixels are still noise, so the percentile stays on the floor);
//  - the smoothing is ASYMMETRIC — the window opens quickly (a signal that
//    appears must fit within a fraction of a second) and closes slowly, so a
//    burst ending does not snap the whole display back and make the trace jump.
//
// Runs on the fps timer, writes rx->panadapter_low/high, so every existing
// drawing path (trace, graticule, peak hold, phosphor, S-meter marks) follows
// with no changes.
// ---------------------------------------------------------------------------
#define PAN_AUTO_BIN_MIN   (-200)   // histogram floor, dBm
#define PAN_AUTO_BINS      221      // 1 dB bins, -200..+20 dBm
#define PAN_AUTO_PCT       0.30     // percentile taken as the noise floor
#define PAN_AUTO_MARGIN    12.0     // dB of room kept below the noise floor
#define PAN_AUTO_HEADROOM  8.0      // dB of room kept above the strongest signal
#define PAN_AUTO_MIN_SPAN  40.0     // never squeeze the scale tighter than this
#define PAN_AUTO_TAU_OPEN  0.25     // seconds to widen the window
#define PAN_AUTO_TAU_CLOSE 3.0      // seconds to narrow it back down

static void pan_auto_levels(RECEIVER *rx, float *samples, int offset, int width, double corr) {
  int hist[PAN_AUTO_BINS];
  memset(hist,0,sizeof(hist));

  double peak=-1000.0;
  int n=0;
  for(int j=offset; j<offset+width && j<rx->pixels; j++) {
    double s=(double)samples[j]+corr;
    if(s>peak) peak=s;
    int b=(int)floor(s)-PAN_AUTO_BIN_MIN;
    if(b<0) b=0;
    if(b>=PAN_AUTO_BINS) b=PAN_AUTO_BINS-1;
    hist[b]++;
    n++;
  }
  if(n<8) return;

  int want=(int)(PAN_AUTO_PCT*(double)n);
  int cum=0, fb=0;
  for(fb=0; fb<PAN_AUTO_BINS; fb++) {
    cum+=hist[fb];
    if(cum>=want) break;
  }
  if(fb>=PAN_AUTO_BINS) fb=PAN_AUTO_BINS-1;
  double floor_db=(double)(fb+PAN_AUTO_BIN_MIN);

  double lo=floor_db-PAN_AUTO_MARGIN;
  double hi=peak+PAN_AUTO_HEADROOM;
  if(hi-lo<PAN_AUTO_MIN_SPAN) hi=lo+PAN_AUTO_MIN_SPAN;
  if(lo<-200.0) lo=-200.0;
  if(hi>20.0) hi=20.0;
  if(hi<=lo) hi=lo+PAN_AUTO_MIN_SPAN;

  if(!rx->pan_auto_seeded) {
    rx->pan_auto_low=lo;
    rx->pan_auto_high=hi;
    rx->pan_auto_seeded=TRUE;
  } else {
    double dt=1.0/(double)(rx->fps>0?rx->fps:10);
    double a_open=1.0-exp(-dt/PAN_AUTO_TAU_OPEN);
    double a_close=1.0-exp(-dt/PAN_AUTO_TAU_CLOSE);
    /* widening (low falls / high rises) is the fast direction */
    rx->pan_auto_low  += (lo-rx->pan_auto_low ) * (lo<rx->pan_auto_low  ? a_open : a_close);
    rx->pan_auto_high += (hi-rx->pan_auto_high) * (hi>rx->pan_auto_high ? a_open : a_close);
  }

  rx->panadapter_low=(gint)lround(rx->pan_auto_low);
  rx->panadapter_high=(gint)lround(rx->pan_auto_high);
  if(rx->panadapter_high<=rx->panadapter_low) rx->panadapter_high=rx->panadapter_low+1;
}

// ---------------------------------------------------------------------------
// Per-frame STATE update (fps timer). Only the frame-rate-locked buffers are
// touched here (peak-hold decay, phosphor occupancy EMA + colour-map); the
// actual drawing is done by rx_pana_build() at snapshot time. Ends by queuing a
// redraw of the PanaView.
// ---------------------------------------------------------------------------
void update_rx_panadapter(RECEIVER *rx,gboolean running) {
  int i;

  int display_width=gtk_widget_get_width (rx->panadapter);
  int display_height=gtk_widget_get_height (rx->panadapter);
  int offset=rx->pan;
  float *samples=rx->pixel_samples;

  rx->pan_running=running;

  if(samples==NULL || display_width<=0) {
    if(rx->panadapter!=NULL) gtk_widget_queue_draw(rx->panadapter);
    return;
  }

  samples[display_width-1+offset]=-200;

  double attenuation=radio->adc[rx->adc].attenuation;
  if (radio->divmixer[rx->dmix_id] != NULL) {
    if (radio->divmixer[rx->dmix_id]->calibrate_gain) {
      attenuation = 0;
      rx->panadapter_filled = FALSE;
    }
  }
  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
      attenuation = attenuation * -1;
  }

  if(display_height<=1) { gtk_widget_queue_draw(rx->panadapter); return; }

  // Auto dB scale, before anything derives from panadapter_low/high this frame.
  // Skipped in vectorscope mode, which draws no trace and has its own scaling.
  if(rx->panadapter_automatic && running && !rx->panadapter_phase) {
    pan_auto_levels(rx,samples,offset,display_width,attenuation+radio->panadapter_calibration);
  }
  double dbm_per_line=(double)display_height/((double)rx->panadapter_high-(double)rx->panadapter_low);

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
    return;
  }

  if(running && !rx->panadapter_phase) {
    // Peak-hold: per-pixel running maximum with a configurable dB/second decay.
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

    // Phosphor / "digital phosphor" persistence heatmap (half-res, GPU-upscaled).
    phosphor_accumulate(rx, display_width, display_height, samples, offset, attenuation, dbm_per_line);
  }

  gtk_widget_queue_draw (rx->panadapter);
}

// ---------------------------------------------------------------------------
// Snapshot builder (GTK main thread, at draw time). Emits the whole scene as GSK
// render nodes. Reads the current pixel_samples + peak/phosphor state produced by
// update_rx_panadapter(); does not mutate frame-locked buffers.
// ---------------------------------------------------------------------------
static void rx_pana_build(GtkSnapshot *snapshot, int display_width, int display_height, gpointer data) {
  RECEIVER *rx=(RECEIVER *)data;
  GtkWidget *widget=rx->panadapter;
  int i;
  int x1,x2;
  char temp[32];

  if(display_width<=0 || display_height<=1) return;

  // FPS/frame-time diagnostic (MACHPSDR_FPS). This builder runs once per presented
  // frame, so the delta between calls is the real render period.
  if(pan_fps_show<0) pan_fps_show = getenv("MACHPSDR_FPS")!=NULL;
  double _build_t0=0.0;
  if(pan_fps_show) {
    double t=pan_now_ms();
    if(pan_fps_last>0.0) {
      double dt=t-pan_fps_last;
      if(dt>0.0) { double inst=1000.0/dt; pan_fps_ema = pan_fps_ema>0.0 ? pan_fps_ema*0.9+inst*0.1 : inst; }
    }
    pan_fps_last=t; _build_t0=t;
  }

  int offset=rx->pan;
  float *samples=rx->pixel_samples;
  if(samples==NULL) return;

  float *samples_hidden_rx=NULL;
  int gain_cal_error=FALSE;
  int filled=rx->panadapter_filled;

  double dbm_per_line=(double)display_height/((double)rx->panadapter_high-(double)rx->panadapter_low);
  double attenuation=radio->adc[rx->adc].attenuation;
  if (radio->divmixer[rx->dmix_id] != NULL) {
    if (radio->divmixer[rx->dmix_id]->calibrate_gain) {
      attenuation = 0;
      filled = FALSE;
      if (radio->divmixer[rx->dmix_id]->rx_hidden->pixel_samples != NULL) {
        samples_hidden_rx = radio->divmixer[rx->dmix_id]->rx_hidden->pixel_samples;
        samples_hidden_rx[display_width-1 + offset] = -200;
      } else {
        gain_cal_error = TRUE;
      }
    }
  }
  if(radio->discovered->device==DEVICE_HERMES_LITE2) {
      attenuation = attenuation * -1;
  }

  const graphene_rect_t full=GRAPHENE_RECT_INIT(0,0,(float)display_width,(float)display_height);

  // ---- background ---------------------------------------------------------
  if(rx->panadapter_gradient) {
    // Radial wash matching the old cairo radial (centre below the panel, inner
    // 0.25 grey -> outer 0.1 grey).
    graphene_point_t ctr=GRAPHENE_POINT_INIT((float)(rx->panadapter_width/2),(float)(rx->panadapter_height+300));
    float radius=(float)(rx->panadapter_width/2); if(radius<1.0f) radius=1.0f;
    GskColorStop stops[2]={ {0.0f,nrgba(0.25,0.25,0.25,1.0)}, {1.0f,nrgba(0.1,0.1,0.1,1.0)} };
    gtk_snapshot_append_radial_gradient(snapshot,&full,&ctr,radius,radius,5.0f/radius,1.0f,stops,2);
  } else {
    GdkRGBA bg=css_rgba("SPECTRUM_BG",0.09,0.09,0.10,1.0);
    n_rect(snapshot,0,0,display_width,display_height,&bg);
  }

  // ---- receiver thread exited --------------------------------------------
  if(!rx->pan_running) {
    GdkRGBA warn=skin_rgba(WARNING,1.0);
    n_text_sz(snapshot,widget,display_width/2,display_height/2,18,&warn,"No data - receiver thread exited");
    return;
  }

  // ---- I/Q vectorscope (X-Y phase display) --------------------------------
  // Takes precedence over the spectrum path (which assumes a frequency-domain
  // trace), so draw it and return early, mirroring the old cairo early-exit.
  if(rx->panadapter_phase) {
    int cx=display_width/2;
    int cy=display_height/2;
    int min_dim = display_width<display_height ? display_width : display_height;
    double R=0.45*(double)min_dim;

    GdkRGBA grid=nrgba(0.5,0.5,0.5,0.35);
    n_line(snapshot,0,cy,display_width,cy,1.0,&grid);
    n_line(snapshot,cx,0,cx,display_height,1.0,&grid);
    // two concentric graticule circles
    for(int cc=0;cc<2;cc++) {
      double rr = cc==0 ? R : R/2.0;
      GskPathBuilder *b=gsk_path_builder_new();
      gsk_path_builder_add_circle(b,&GRAPHENE_POINT_INIT((float)cx,(float)cy),(float)rr);
      GskPath *p=gsk_path_builder_free_to_path(b);
      GskStroke *st=gsk_stroke_new(1.0f);
      gtk_snapshot_append_stroke(snapshot,p,st,&grid);
      gsk_stroke_free(st);
      gsk_path_unref(p);
    }

    // Diversity alignment target (main-I vs hidden-I -> y=x diagonal).
    if(rx->panadapter_phase_source==2) {
      GdkRGBA diag=nrgba(1.0,0.8,0.2,0.4);
      n_line(snapshot,cx-R,cy+R,cx+R,cy-R,1.0,&diag);
    }

    g_mutex_lock(&rx->scope_mutex);
    int n=rx->scope_iq_n;
    float *pts=NULL;
    if(n>0) {
      pts=g_new(float,2*n);
      memcpy(pts,rx->scope_iq,sizeof(float)*2*n);
    }
    g_mutex_unlock(&rx->scope_mutex);

    if(pts!=NULL) {
      double m=0.0;
      for(i=0;i<n;i++) {
        double ai=fabs((double)pts[i*2]);
        double aq=fabs((double)pts[i*2+1]);
        double a = ai>aq ? ai : aq;
        if(a>m) m=a;
      }
      rx->scope_ref = m > rx->scope_ref ? m : rx->scope_ref*0.95;
      if(rx->scope_ref<1e-6) rx->scope_ref=1e-6;
      double scale=(R/rx->scope_ref)*((double)rx->panadapter_phase_gain/100.0);

      GdkRGBA sc=nrgba(0.1,1.0,0.3,rx->panadapter_phase_mode==0 ? 0.5 : 0.6);
      if(rx->panadapter_phase_mode==0) {
        // Dots: one path of tiny filled squares.
        GskPathBuilder *b=gsk_path_builder_new();
        for(i=0;i<n;i++) {
          double x=cx+(double)pts[i*2]*scale;
          double y=cy-(double)pts[i*2+1]*scale;
          gsk_path_builder_add_rect(b,&GRAPHENE_RECT_INIT((float)(x-0.5),(float)(y-0.5),1.5f,1.5f));
        }
        GskPath *p=gsk_path_builder_free_to_path(b);
        gtk_snapshot_append_fill(snapshot,p,GSK_FILL_RULE_WINDING,&sc);
        gsk_path_unref(p);
      } else {
        // Lines: polyline through consecutive samples.
        GskPathBuilder *b=gsk_path_builder_new();
        gsk_path_builder_move_to(b,(float)(cx+(double)pts[0]*scale),(float)(cy-(double)pts[1]*scale));
        for(i=1;i<n;i++) {
          gsk_path_builder_line_to(b,(float)(cx+(double)pts[i*2]*scale),(float)(cy-(double)pts[i*2+1]*scale));
        }
        GskPath *p=gsk_path_builder_free_to_path(b);
        GskStroke *st=gsk_stroke_new(1.0f);
        gtk_snapshot_append_stroke(snapshot,p,st,&sc);
        gsk_stroke_free(st);
        gsk_path_unref(p);
      }
      g_free(pts);
    }

    {
      char scope_label[32];
      const char *src = rx->panadapter_phase_source==1 ? "Tuned" :
                        rx->panadapter_phase_source==2 ? "Diversity" : "Wideband";
      snprintf(scope_label,sizeof(scope_label),"%s (%s)",
               rx->panadapter_phase_mode==0 ? "PHASE" : "PHASE2", src);
      GdkRGBA warn=skin_rgba(WARNING,1.0);
      n_text(snapshot,widget,4,14,&warn,scope_label,NULL);
    }
    return;
  }

  // ---- phosphor texture (half-res, GPU-upscaled) --------------------------
  if(rx->panadapter_histogram && rx->panadapter_histogram_surface!=NULL
     && display_width==rx->panadapter_histogram_w && display_height==rx->panadapter_histogram_h) {
    int HW=(display_width+1)/2, HH=(display_height+1)/2;
    cairo_surface_t *hs=rx->panadapter_histogram_surface;
    unsigned char *hd=cairo_image_surface_get_data(hs);
    int stride=cairo_image_surface_get_stride(hs);
    GBytes *bytes=g_bytes_new(hd,(gsize)stride*(gsize)HH);
    GdkTexture *tex=gdk_memory_texture_new(HW,HH,GDK_MEMORY_B8G8R8X8,bytes,(gsize)stride);
    g_bytes_unref(bytes);
    // Upscale the half-res occupancy texture to the full panel with a linear
    // filter — the soft cloud hides the interpolation.
    gtk_snapshot_append_scaled_texture(snapshot,tex,GSK_SCALING_FILTER_LINEAR,&full);
    g_object_unref(tex);
  }

  long long frequency=rx->frequency_a;
  long long half=(long long)rx->sample_rate/2LL;
  long long min_display=(frequency - half) + (long long)((double)rx->pan*rx->hz_per_pixel);
  long long max_display=(frequency + half) + (long long)((double)rx->pan*rx->hz_per_pixel);
  BAND *band=band_get_band(rx->band_a);

  if(rx->band_a==band60) {
    GdkRGBA ch=nrgba(0.6,0.3,0.3,1.0);
    for(i=0;i<channel_entries;i++) {
      long long low_freq=band_channels_60m[i].frequency-(band_channels_60m[i].width/(long long)2);
      long long hi_freq=band_channels_60m[i].frequency+(band_channels_60m[i].width/(long long)2);
      x1=(low_freq-min_display)/rx->hz_per_pixel;
      x2=(hi_freq-min_display)/rx->hz_per_pixel;
      n_rect(snapshot,x1,0.0,x2-x1,(double)display_height,&ch);
    }
  }

  // ---- filter passband (vertical gradient band) ---------------------------
  double filter_left=((double)rx->pixels/2.0)-(double)rx->pan+(((double)rx->filter_low_a+rx->ctun_offset)/rx->hz_per_pixel);
  double filter_right=((double)rx->pixels/2.0)-(double)rx->pan+(((double)rx->filter_high_a+rx->ctun_offset)/rx->hz_per_pixel);
  if(filter_right>filter_left) {
    graphene_rect_t fr=GRAPHENE_RECT_INIT((float)filter_left,0.0f,(float)(filter_right-filter_left),(float)display_height);
    GskColorStop fstops[2]={ {0.1f,nrgba(0.5,0.5,0.5,0.75)}, {0.7f,nrgba(0.1,0.1,0.1,1.0)} };
    graphene_point_t fs0=GRAPHENE_POINT_INIT((float)filter_left,0.0f);
    graphene_point_t fs1=GRAPHENE_POINT_INIT((float)filter_left,(float)display_height);
    gtk_snapshot_append_linear_gradient(snapshot,&fr,&fs0,&fs1,fstops,2);
  }

#ifdef SSTV
  // ---- APT front-end window ----------------------------------------------
  // The APT decoder does not use the receive filter — it takes raw I/Q and runs
  // its own ~44 kHz FM front-end — so the passband drawn above says nothing
  // about whether the satellite fits.  Draw the window the decoder actually
  // accepts, around the frequency it is actually tuned to, so the operator can
  // SEE the signal sitting inside it instead of having to take it on trust.
  // Only while APT is the running decoder on this receiver.
  if(radio!=NULL && rx==radio->active_receiver && rx->mode_a==FMN &&
     radio->decode_mode==DECODE_APT) {
    apt_status_t ast; apt_decoder_get_status(&ast);
    double half=apt_decoder_get_bandwidth();
    if(ast.tuned_hz>0 && half>0.0) {
      double xl=(((double)ast.tuned_hz-half)-(double)min_display)/rx->hz_per_pixel;
      double xr=(((double)ast.tuned_hz+half)-(double)min_display)/rx->hz_per_pixel;
      if(xr>0.0 && xl<display_width) {
        // A wash rather than a solid band: it is wide (44 kHz), and it must not
        // bury the trace it exists to let you look at.
        GdkRGBA wash=nrgba(0.2,0.8,0.9,0.10);
        n_rect(snapshot,xl,0.0,xr-xl,(double)display_height,&wash);
        GdkRGBA edge=nrgba(0.3,0.9,1.0,0.55);
        n_rect(snapshot,xl,0.0,1.0,(double)display_height,&edge);
        n_rect(snapshot,xr-1.0,0.0,1.0,(double)display_height,&edge);
        char cap[48];
        snprintf(cap,sizeof(cap),"APT %.0f kHz",2.0*half/1000.0);
        double capw=0.0;
        // Measure first so the caption can be centred in the window, and keep it
        // on screen when the window runs off an edge.  label12 hands back the
        // SHARED cached layout — borrowed, never owned: unreffing it here would
        // destroy the cache and take the next frame with it.
        int cw=0,chh=0;
        pango_layout_get_pixel_size(label12(widget,cap),&cw,&chh);
        double cx=(xl+xr)/2.0-cw/2.0;
        if(cx<2.0) cx=2.0;
        if(cx>display_width-cw-2.0) cx=display_width-cw-2.0;
        GdkRGBA capc=nrgba(0.6,0.95,1.0,0.9), capbg=nrgba(0.0,0.0,0.0,0.5);
        n_text_boxed(snapshot,widget,cx,14.0,&capc,&capbg,cap,&capw);
      }
    }
  }
#endif

  double cw_offset = 0;
  if(rx->mode_a==CWL || rx->mode_a==CWU) {
    if(rx->mode_a==CWU) cw_offset=-radio->cw_keyer_sidetone_frequency;
    else                cw_offset=+radio->cw_keyer_sidetone_frequency;
  }
  // VFO B/sub rx filter
  if(rx->subrx!=NULL) {
    i=(int)(((double)rx->frequency_b-(double)min_display)/rx->hz_per_pixel);
    double fl = i + (rx->filter_low_a / rx->hz_per_pixel);
    double frr = i + (rx->filter_high_a / rx->hz_per_pixel);
    GdkRGBA sub=nrgba(0.5,0.5,0.5,0.75);
    n_rect(snapshot,fl,0.0,frr-fl,(double)display_height,&sub);
  }

  // ---- manual notch filters ----------------------------------------------
  for(i=0;i<rx->notches;i++) {
    double _fc=rx->notch[i].fcenter;
    double _w =rx->notch[i].fwidth;
    double _xl=((_fc-0.5*_w)-(double)min_display)/rx->hz_per_pixel;
    double _xr=((_fc+0.5*_w)-(double)min_display)/rx->hz_per_pixel;
    if(_xr<0 || _xl>display_width) continue;
    GdkRGBA band_c = rx->notch[i].active ? nrgba(0.9,0.2,0.2,0.30) : nrgba(0.6,0.6,0.6,0.20);
    n_rect(snapshot,_xl,0.0,(_xr-_xl)<1.0?1.0:(_xr-_xl),(double)display_height-20,&band_c);
    double _xc=(_fc-(double)min_display)/rx->hz_per_pixel;
    // Centre line follows the per-notch enable too, so a bypassed notch reads as
    // fully greyed out rather than a grey band with a live red line through it.
    GdkRGBA cl = rx->notch[i].active ? nrgba(0.9,0.2,0.2,0.8) : nrgba(0.6,0.6,0.6,0.5);
    n_line(snapshot,_xc,0.0,_xc,(double)display_height-20,1.0,&cl);
  }

#ifdef FT8
  // FT8 TX audio-offset marker (green vertical line + "TX").
  if(rx->mode_a==DIGU && radio->ft8_panel!=NULL) {
    double tx_x=((double)(frequency + (long long)radio->ft8_tx_offset)
                 - (double)min_display)/rx->hz_per_pixel;
    GdkRGBA g=nrgba(0.2,0.9,0.2,0.9);
    n_line(snapshot,tx_x,0.0,tx_x,(double)display_height,1.0,&g);
    n_text(snapshot,widget,tx_x+2.0,10.0,&g,"TX",NULL);
  }
#endif

  // ---- dB graticule + labels (left scale) ---------------------------------
  GdkRGBA bgstrip=css_rgba("SPECTRUM_BG",0.09,0.09,0.10,1.0);
  n_rect(snapshot,0,0,40,display_height,&bgstrip);

  GdkRGBA dark=skin_rgba(DARK_LINES,1.0);
  GdkRGBA text_b=skin_rgba(TEXT_B,1.0);
  GdkRGBA label_bg=bgstrip;  // same as the scale strip → box is seamless, just hides the graticule under each number
  int db_step = rx->panadapter_step>0 ? rx->panadapter_step : 20;
  {
    // Batch all dB gridlines into one dashed stroke, appended BEFORE the labels
    // so each boxed number renders on top of the graticule (no line through it).
    GskPathBuilder *b=gsk_path_builder_new();
    gboolean any=FALSE;
    for(i=rx->panadapter_high;i>=rx->panadapter_low;i--) {
      if(abs(i)%db_step==0) {
        double y=(double)(rx->panadapter_high-i)*dbm_per_line;
        gsk_path_builder_move_to(b,0.0f,(float)y);
        gsk_path_builder_line_to(b,(float)display_width,(float)y);
        any=TRUE;
      }
    }
    GskPath *p=gsk_path_builder_free_to_path(b);
    if(any) {
      GskStroke *st=gsk_stroke_new((float)LINE_WIDTH);
      float dash[2]={2.0f,2.0f}; gsk_stroke_set_dash(st,dash,2);
      gtk_snapshot_append_stroke(snapshot,p,st,&dark);
      gsk_stroke_free(st);
    }
    gsk_path_unref(p);
    // dB numbers on top, each over a solid background box.
    //
    // Label decimation: the gridline spacing in PIXELS is what decides whether
    // two numbers can coexist, and that shrinks with the panadapter's height —
    // on a short pane (waterfall dragged up, or the panadapter squeezed into a
    // sliver) consecutive labels landed a few pixels apart and smeared into an
    // unreadable stack. So keep every Nth label, N chosen from the actual pixel
    // pitch against the 12 px label font. This replaces a fixed "every 3rd when
    // the step is small" rule, which knew nothing about the pane height.
    const double db_label_min_px = 14.0;
    double px_per_step = (double)db_step * dbm_per_line;
    int label_nth = 1;
    if(px_per_step > 0.0) {
      label_nth = (int)ceil(db_label_min_px / px_per_step);
      if(label_nth < 1) label_nth = 1;
    }
    for(i=rx->panadapter_high;i>=rx->panadapter_low;i--) {
      if(abs(i)%db_step==0 && (abs(i)/db_step)%label_nth==0) {
        double y=(double)(rx->panadapter_high-i)*dbm_per_line;
        const GdkRGBA *lc = rx->panadapter_gradient ? &text_b : &dark;
        sprintf(temp," %d",i);
        n_text_boxed(snapshot,widget,5,y-4,lc,&label_bg,temp,NULL);
      }
    }
  }

  // ---- frequency scale (bottom) -------------------------------------------
  n_rect(snapshot,0,(rx->panadapter_height-20),display_width,20,&bgstrip);

  if(rx->zoom!=1) {
    int pan_x=(int)((double)rx->pan/(double)rx->zoom);
    int pan_width=(int)((double)rx->panadapter_width/(double)rx->zoom);
    GdkRGBA pc=nrgba(0.7,0.7,0.7,1.0);
    n_rect(snapshot,pan_x,(rx->panadapter_height-4),pan_width,4,&pc);
  }

  long long f1;
  long long f2;
  long long divisor1=20000;
  long long divisor2=5000;
  long long factor=(long long)(rx->sample_rate/48000);
  if(factor>10LL) factor=10LL;
  switch(rx->zoom) {
    case 1: case 2: case 3:
      divisor1=5000LL*factor; divisor2=1000LL*factor; break;
    case 4: case 5: case 6:
      divisor1=1000LL*factor; divisor2=500LL*factor; break;
    case 7: case 8: default:
      divisor1=1000LL*factor; divisor2=200LL*factor; break;
  }

  f1=frequency-half+(long long)(rx->hz_per_pixel*offset);
  if (rx->mode_a==CWU)      f1 -= radio->cw_keyer_sidetone_frequency;
  else if (rx->mode_a==CWL) f1 += radio->cw_keyer_sidetone_frequency;
  f2=(f1/divisor2)*divisor2;

  int x=0;
  double last_text_end = -1000.0;
  {
    GskPathBuilder *b=gsk_path_builder_new();
    gboolean any=FALSE;
    do {
      x=(int)(f2-f1)/rx->hz_per_pixel;
      if(x>70) {
        if((f2%divisor1)==0LL) {
          gsk_path_builder_move_to(b,(float)x,0.0f);
          gsk_path_builder_line_to(b,(float)x,(float)(display_height-20));
          any=TRUE;
          sprintf(temp,"%0lld.%03lld",f2/1000000,(f2%1000000)/1000);
          double tw=n_measure(widget,temp);
          double text_x=(double)x-(tw/2.0);
          if(text_x > last_text_end + 6.0) {
            n_text(snapshot,widget,text_x,(rx->panadapter_height-6),&text_b,temp,NULL);
            last_text_end = text_x + tw;
          }
        } else if((f2%divisor2)==00LL) {
          gsk_path_builder_move_to(b,(float)x,0.0f);
          gsk_path_builder_line_to(b,(float)x,(float)(display_height-20));
          any=TRUE;
        }
      }
      f2=f2+divisor2;
    } while(x<display_width);
    GskPath *p=gsk_path_builder_free_to_path(b);
    if(any) {
      GskStroke *st=gsk_stroke_new((float)LINE_WIDTH);
      gtk_snapshot_append_stroke(snapshot,p,st,&dark);
      gsk_stroke_free(st);
    }
    gsk_path_unref(p);
  }

  // ---- band edges ---------------------------------------------------------
  if(rx->band_a!=band60) {
    if(band->frequencyMin!=0LL) {
      GdkRGBA warn=skin_rgba(WARNING,1.0);
      if((min_display<band->frequencyMin)&&(max_display>band->frequencyMin)) {
        i=(int)(((double)band->frequencyMin-(double)min_display)/rx->hz_per_pixel);
        i -= cw_offset / rx->hz_per_pixel;
        n_line(snapshot,(double)i,0.0,(double)i,(double)display_height-20,2.0,&warn);
      }
      if((min_display<band->frequencyMax)&&(max_display>band->frequencyMax)) {
        i=(int)(((double)band->frequencyMax-(double)min_display)/rx->hz_per_pixel);
        i -= cw_offset / rx->hz_per_pixel;
        n_line(snapshot,(double)i,0.0,(double)i,(double)display_height-20,2.0,&warn);
      }
    }
  }

  // ---- AGC lines ----------------------------------------------------------
  if(rx->agc!=AGC_OFF && rx->panadapter_agc_line) {
    double ax=80.0;
    double hang=rx->agc_hang_level;
    double thresh=rx->agc_thresh_level;

    double knee_y=thresh+attenuation+radio->panadapter_calibration;
    knee_y = floor((rx->panadapter_high - knee_y)*dbm_per_line);
    double hang_y=hang+attenuation+radio->panadapter_calibration;
    hang_y = floor((rx->panadapter_high - hang_y)*dbm_per_line);

    if(rx->agc!=AGC_MEDIUM && rx->agc!=AGC_FAST) {
      GdkRGBA ca=skin_rgba(TEXT_A,1.0);
      n_rect(snapshot,ax,hang_y-8.0,8.0,8.0,&ca);
      n_line(snapshot,ax,hang_y,(double)display_width-ax,hang_y,LINE_WIDTH,&ca);
      n_text(snapshot,widget,ax+8.0,hang_y,&ca,"-H",NULL);
    }
    GdkRGBA cc=skin_rgba(TEXT_C,1.0);
    n_rect(snapshot,ax,knee_y-8.0,8.0,8.0,&cc);
    n_line(snapshot,ax,knee_y,(double)display_width-ax,knee_y,LINE_WIDTH,&cc);
    n_text(snapshot,widget,ax+8.0,knee_y,&cc,"-G",NULL);
  }

  // ---- cursor + VFO-B marker ---------------------------------------------
  {
    double cxp=(double)(rx->pixels/2.0)-(double)rx->pan+(rx->ctun_offset/rx->hz_per_pixel)-(cw_offset/rx->hz_per_pixel);
    n_line(snapshot,cxp,0.0,cxp,(double)display_height-20,LINE_WIDTH,&text_b);
  }
  if(rx->subrx!=NULL) {
    i=(int)(((double)rx->frequency_b-(double)min_display)/rx->hz_per_pixel);
    i -= cw_offset / rx->hz_per_pixel;
    GdkRGBA cc=skin_rgba(TEXT_C,1.0);
    n_line(snapshot,(double)i,0.0,(double)i,(double)display_height-20,LINE_WIDTH,&cc);
  }

  // ---- spectrum trace -----------------------------------------------------
  // When the phosphor is on it IS the spectrum display, so skip the ordinary
  // line/fill (it would paint a solid colour over the graded cloud).
  if(!rx->panadapter_histogram) {
    double add_db  = attenuation+radio->panadapter_calibration;
    double base_y  = (double)(display_height-20);
    double y_clamp = (double)(rx->panadapter_height-20);

    gboolean gradient=FALSE; double S9v=1.0;
    GdkRGBA solid;
    if(rx->panadapter_single_color == 0) {
      if(rx->panadapter_gradient) {
        gradient=TRUE;
        double S9=-73; if(rx->frequency_a>30000000LL) S9=-93;
        S9 = floor((rx->panadapter_high - S9)*(double)(rx->panadapter_height-20)
                   / (rx->panadapter_high - rx->panadapter_low));
        S9 = 1.0-(S9/(double)(rx->panadapter_height-20));
        if(S9<0.03) S9=0.03; if(S9>1.0) S9=1.0;
        S9v=S9;
        solid=nrgba(1.0,1.0,1.0,0.5);
      } else {
        solid=nrgba(1.0,1.0,1.0, filled?0.5:0.9);   // white
      }
    } else {
      // Single-colour trace (user's choice, or the skin accent for default 1).
      double sr=0.804, sg=0.635, sb=0.859;
      switch(rx->panadapter_single_color) {
        case 2: sr=0.769; sg=0.117; sb=0.227; break;
        case 3: sr=1.0;   sg=0.459; sb=0.095; break;
        case 4: sr=1.0;   sg=0.850; sb=0.0;   break;
        case 5: sr=0.133; sg=0.545; sb=0.133; break;
        case 6: sr=0.0;   sg=0.184; sb=0.655; break;
        case 7: sr=0.4;   sg=0.0;   sb=0.6;   break;
        case 8: sr=0.90;  sg=0.0;   sb=0.90;  break;
        case 9: sr=0.0;   sg=0.9;   sb=0.9;   break;
        case 1: default: css_rgb("ACCENT_A",&sr,&sg,&sb); break;
      }
      solid=nrgba(sr,sg,sb, filled?0.5:1.0);
    }

    pan_trace_rects(snapshot, samples, offset, display_width, add_db,
                    (double)rx->panadapter_high, dbm_per_line, base_y, y_clamp,
                    gradient, S9v, solid, filled);

    // Diversity gain-cal hidden-RX overlay (turquoise line).
    if (radio->divmixer[rx->dmix_id] != NULL) {
      if ((radio->divmixer[rx->dmix_id]->calibrate_gain) && (!gain_cal_error) && samples_hidden_rx!=NULL) {
        GdkRGBA tq=nrgba(0.259,0.960,0.950,1.0);
        pan_trace_rects(snapshot, samples_hidden_rx, offset, display_width, add_db,
                        (double)rx->panadapter_high, dbm_per_line, base_y, y_clamp,
                        FALSE, 1.0, tq, FALSE);
      }
    }
  }

  // ---- peak-hold trace (line only, on top) --------------------------------
  if(rx->panadapter_peak_hold && rx->panadapter_peaks!=NULL) {
    GdkRGBA pk=nrgba(0.95,0.95,0.95,0.85);
    pan_trace_rects(snapshot, rx->panadapter_peaks, offset, display_width,
                    attenuation+radio->panadapter_calibration, (double)rx->panadapter_high,
                    dbm_per_line, (double)(display_height-20), (double)(rx->panadapter_height-20),
                    FALSE, 1.0, pk, FALSE);
  }

  // ---- DX cluster spot overlay (GSK nodes — no full-window append_cairo) ----
  if(radio->cluster_enable && radio->cluster_spots_show &&
     (radio->cluster_spots_on==0 || radio->cluster_spots_on==2)) {
    receiver_draw_cluster_spots_nodes(snapshot, widget, rx, display_width);
  }

  // ---- I/Q Player readout (top-right, drawn ON TOP so the graticule doesn't
  //      strike through it — the black box only hides the ruling if it, and the
  //      text, come after the dB/frequency gridline strokes) -----------------
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
      double tw=n_measure(widget,pb);
      double px = (double)display_width - tw - 6.0;
      if(px < 42.0) px = 42.0;
      GdkRGBA cyan=nrgba(0.55,0.85,1.0,0.9);
      GdkRGBA blk=nrgba(0.0,0.0,0.0,1.0);
      n_text_boxed(snapshot,widget,px,14.0,&cyan,&blk,pb,NULL);
    }
  }

  // ---- FPS / build-time readout (diagnostic, drawn on top) ----------------
  if(pan_fps_show) {
    char fbuf[48];
    snprintf(fbuf,sizeof(fbuf),"FPS %.0f  build %.2fms", pan_fps_ema, pan_build_ema);
    GdkRGBA fc=nrgba(1.0,0.9,0.2,0.95);
    GdkRGBA fbg=nrgba(0.0,0.0,0.0,1.0);
    n_text_boxed(snapshot,widget,48,26,&fc,&fbg,fbuf,NULL);
    double _bt=pan_now_ms()-_build_t0;
    pan_build_ema = pan_build_ema>0.0 ? pan_build_ema*0.9+_bt*0.1 : _bt;
  }
}
