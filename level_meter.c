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

void SetColour(cairo_t *cr, const int colour) {

  switch(colour) {
    case BACKGROUND: {
      cairo_set_source_rgb(cr, 0.09, 0.09, 0.10);   // match CSS @BACKGROUND
      break;
    }
    case OFF_WHITE: {
      cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
      break;
    }
    case BOX_ON: {
      cairo_set_source_rgb(cr, 0.624, 0.427, 0.690);
      break;
    }
    case BOX_OFF: {
      cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
      break;
    }
    case TEXT_A: {
      cairo_set_source_rgb(cr, 0.929, 0.616, 0.502);
      break;
    }
    case TEXT_B: {
      //light blue
      cairo_set_source_rgb(cr, 0.639, 0.800, 0.820);
      break;
    }
    case TEXT_C: {
      // Pale orange
      cairo_set_source_rgb(cr, 0.929, 0.616, 0.502);
      break;
    }
    case WARNING: {
      // Pale red
        cairo_set_source_rgb(cr, 0.851, 0.271, 0.271);
      break;
    }
    case DARK_LINES: {
      // Dark grey
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
      break;
    }
    case DARK_TEXT: {
      cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
      break;
    }
    case INFO_ON: {
      cairo_set_source_rgb(cr, 0.15, 0.58, 0.6);
      break;
    }
    case INFO_OFF: {
      cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
      break;
    }
  }
}

void set_stop_pattern(cairo_pattern_t *pat, const int colour, const double pc) {
  switch(colour) {
    case BACKGROUND: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.09, 0.09, 0.10);
      break;
    }
    case OFF_WHITE: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.9, 0.9, 0.9);
      break;
    }
    case BOX_ON: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.624, 0.427, 0.690);
      break;
    }
    case BOX_OFF: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.2, 0.2, 0.2);
      break;
    }
    case TEXT_A: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.929, 0.616, 0.502);
      break;
    }
    case TEXT_B: {
      //light blue
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.639, 0.800, 0.820);
      break;
    }
    case TEXT_C: {
      // Pale orange
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.929, 0.616, 0.502);
      break;
    }
    case WARNING: {
      // Pale red
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.851, 0.271, 0.271);
      break;
    }
    case DARK_LINES: {
      // Dark grey
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.3, 0.3, 0.3);
      break;
    }
    case DARK_TEXT: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.7, 0.7, 0.7);
      break;
    }
    case INFO_ON: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.15, 0.58, 0.6);
      break;
    }
    case INFO_OFF: {
      cairo_pattern_add_color_stop_rgb(pat, pc, 0.2, 0.2, 0.2);
      break;
    }
  }
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

  // track
  lm_rounded(cr, pad, ty, tw, th, 3.0);
  cairo_set_source_rgb(cr, 0.06, 0.06, 0.07);
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

