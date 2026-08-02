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
#include "level_meter.h"
#include "css.h"

// Map a level-meter colour enum onto the active skin's palette (see css.c).
// The fallback RGB (the original hard-coded Charcoal values) is used only if the
// skin has no such color, so behaviour is unchanged when a palette is missing.
static void skin_colour(const int colour, const char **name,
                        double *r, double *g, double *b) {
  switch(colour) {
    case BACKGROUND: *name="BACKGROUND"; *r=0.09;  *g=0.09;  *b=0.10;  break;
    case OFF_WHITE:  *name="OFF_WHITE";  *r=0.9;   *g=0.9;   *b=0.9;   break;
    case BOX_ON:     *name="ACCENT_ON";  *r=0.624; *g=0.427; *b=0.690; break;
    case BOX_OFF:    *name="SURFACE";    *r=0.2;   *g=0.2;   *b=0.2;   break;
    case TEXT_A:     *name="ACCENT_B";   *r=0.929; *g=0.616; *b=0.502; break;
    case TEXT_B:     *name="ACCENT_A";   *r=0.639; *g=0.800; *b=0.820; break;
    case TEXT_C:     *name="ACCENT_B";   *r=0.929; *g=0.616; *b=0.502; break;
    case WARNING:    *name="WARNING";    *r=0.851; *g=0.271; *b=0.271; break;
    case DARK_LINES: *name="BORDER";     *r=0.3;   *g=0.3;   *b=0.3;   break;
    case DARK_TEXT:  *name="DARK_TEXT";  *r=0.7;   *g=0.7;   *b=0.7;   break;
    case INFO_ON:    *name="INFO_ON";    *r=0.15;  *g=0.58;  *b=0.6;   break;
    case INFO_OFF:   *name="SURFACE";    *r=0.2;   *g=0.2;   *b=0.2;   break;
    default:         *name=NULL;         *r=0.0;   *g=0.0;   *b=0.0;   break;
  }
  if(*name!=NULL) css_rgb(*name,r,g,b);   // override fallback with skin color
}

void SetColour(cairo_t *cr, const int colour) {
  const char *name; double r,g,b;
  skin_colour(colour,&name,&r,&g,&b);
  cairo_set_source_rgb(cr,r,g,b);
}

// Same skin-palette lookup as SetColour(), but returned as a GdkRGBA for the
// GSK render-node code paths (panadapters) that no longer use a cairo_t.
GdkRGBA skin_rgba(const int colour, const double alpha) {
  const char *name; double r,g,b;
  skin_colour(colour,&name,&r,&g,&b);
  return (GdkRGBA){(float)r,(float)g,(float)b,(float)alpha};
}

void set_stop_pattern(cairo_pattern_t *pat, const int colour, const double pc) {
  const char *name; double r,g,b;
  skin_colour(colour,&name,&r,&g,&b);
  cairo_pattern_add_color_stop_rgb(pat,pc,r,g,b);
}

// rounded-rectangle sub-path helper
static void lm_rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
  if(w < 2.0*r) r = w/2.0;
  if(h < 2.0*r) r = h/2.0;
  if(r < 0.0) r = 0.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x+w-r, y+r,   r, -M_PI/2.0, 0.0);
  cairo_arc(cr, x+w-r, y+h-r, r, 0.0,       M_PI/2.0);
  cairo_arc(cr, x+r,   y+h-r, r, M_PI/2.0,  M_PI);
  cairo_arc(cr, x+r,   y+r,   r, M_PI,      3.0*M_PI/2.0);
  cairo_close_path(cr);
}

// Flat-dark horizontal meter: a dark rounded track with a solid accent fill.
// x is the fill-end position in pixels (0..width-10); fill is a colour enum.
gboolean level_meter_draw(cairo_t *cr, double x, int width, int height, const int fill) {
  const double pad = 5.0;
  const double th  = 9.0;                       // track height
  double tw = (double)width - 2.0*pad;          // track width
  double ty = (double)(height/2) - th - 1.0;    // bar in the upper half, label below
  if(ty < 2.0) ty = 2.0;

  // ground
  SetColour(cr, BACKGROUND);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // track: a recessed groove, slightly darker than the skin background so it
  // reads as sunken on both dark and light skins.
  lm_rounded(cr, pad, ty, tw, th, 3.0);
  double gr=0.06, gg=0.06, gb=0.07;
  if(css_rgb("BACKGROUND",&gr,&gg,&gb)) { gr*=0.72; gg*=0.72; gb*=0.72; }
  cairo_set_source_rgb(cr, gr, gg, gb);
  cairo_fill(cr);

  // accent fill up to x
  double fe = x;
  if(fe < pad)        fe = pad;
  if(fe > pad + tw)   fe = pad + tw;
  double fw = fe - pad;
  if(fw >= 1.0) {
    lm_rounded(cr, pad, ty, fw, th, 3.0);
    SetColour(cr, fill);
    cairo_fill(cr);
    // subtle top gloss
    lm_rounded(cr, pad, ty, fw, th/2.0, 2.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    cairo_fill(cr);
  }

  return TRUE;
}

