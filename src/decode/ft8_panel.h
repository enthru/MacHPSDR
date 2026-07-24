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
 * Embedded FT8 QSO panel (Phase 2).
 *
 * A GtkWidget shaped like a receiver panel, slotted into the RX stack in place
 * of the second receiver while the active receiver is in DIGU.  Shows the live
 * decode list (double-click a row to work that station), the station config
 * (call / grid / TX offset / slot), and the auto-QSO status.  Drives the
 * ft8_qso state machine.
 */

#ifndef _FT8_PANEL_H
#define _FT8_PANEL_H

#include <gtk/gtk.h>

// Build the FT8 panel widget (self-refreshing while realized).
extern GtkWidget *ft8_panel_create(void);

#endif
