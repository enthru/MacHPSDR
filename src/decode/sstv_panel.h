/* Copyright (C)
* 2026 - MacHPSDR fork
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
 * Embedded SSTV image panel.  A GtkWidget slotted into the RX stack in place of
 * the second receiver (mirroring the FT8 QSO panel): it shows the incoming SSTV
 * image as it is decoded, a status line, a mode-override combo (Auto / the five
 * supported modes), slant trim, and Save / Clear buttons.  Its own refresh timer
 * polls the decoder; torn down on "destroy".
 */

#ifndef _SSTV_PANEL_H
#define _SSTV_PANEL_H

#include <gtk/gtk.h>

extern GtkWidget *sstv_panel_create(void);

#endif
