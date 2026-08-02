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

extern gboolean level_meter_draw(cairo_t *cr, double x, int width, int height, const int fill);

extern void SetColour(cairo_t *cr, const int colour);

// Skin-palette colour as a GdkRGBA (for GSK render-node code that has no cairo_t).
extern GdkRGBA skin_rgba(const int colour, const double alpha);

// GSK render-node meter helpers (GPU-rendered meters). lm_text centres on x when
// center is TRUE. level_meter_draw_node is the node version of level_meter_draw.
extern void lm_fill(GtkSnapshot *s,double x,double y,double w,double h,const GdkRGBA *c);
extern void lm_rrect(GtkSnapshot *s,double x,double y,double w,double h,double radius,const GdkRGBA *c);
extern void lm_line(GtkSnapshot *s,double x1,double y1,double x2,double y2,double lw,const GdkRGBA *c);
extern void lm_text(GtkSnapshot *s,GtkWidget *widget,double x,double base_y,double size,const GdkRGBA *c,const char *txt,gboolean center);
extern double lm_measure(GtkWidget *widget,double size,const char *txt);
extern void level_meter_draw_node(GtkSnapshot *s,double x,int width,int height,const int fill);

enum {
  BACKGROUND=0,
  OFF_WHITE=1,
  BOX_ON = 2,
  BOX_OFF = 3,
  TEXT_A = 4,
  TEXT_B = 5,
  TEXT_C = 6,
  WARNING = 7,
  DARK_LINES = 8,
  DARK_TEXT = 9,
  INFO_ON = 10,
  INFO_OFF = 11
};

enum {
  CLICK_ON=0,
  CLICK_OFF=1,
  INFO_TRUE = 2,
  INFO_FALSE = 3,
  WARNING_ON = 4
};
