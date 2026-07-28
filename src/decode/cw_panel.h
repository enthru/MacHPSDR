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
 * Embedded CW (Morse) text panel.  A GtkWidget slotted into the RX stack in
 * place of the second receiver (mirroring the SSTV/WEFAX image panels): a
 * scrolling monospace text view of the decoded characters, a Clear button and
 * a status line (WPM / tracked tone). Its own refresh timer polls the
 * decoder; torn down on "destroy".
 */

#ifndef _CW_PANEL_H
#define _CW_PANEL_H

#include <gtk/gtk.h>

extern GtkWidget *cw_panel_create(void);

#endif
