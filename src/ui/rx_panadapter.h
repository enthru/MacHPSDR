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

extern GtkWidget *create_rx_panadapter(RECEIVER *rx);
extern void update_rx_panadapter(RECEIVER *rx,gboolean running);
// Draw the DX-cluster spot overlay (row-packed ticks + labels) onto cr, mapping
// absolute-RF spot frequencies to x with the same formula the panadapter/
// waterfall use. Shared so both the spectrum surface and the waterfall overlay
// can render it. Caller decides whether to call (per radio->cluster_spots_on).
extern void receiver_draw_cluster_spots(cairo_t *cr, RECEIVER *rx, int display_width);
