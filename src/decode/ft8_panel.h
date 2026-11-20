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

// Decode-list colours (Configure -> FT8). A highlighted row carries its own
// two text colours (other / CQ), so the CQ green never lands on the gold/blue
// background and a CQ still stands apart there. Both backgrounds are mid-tone,
// so black and white both read on them (contrast >= 3.9:1).
#define FT8_COL_CQ_DEFAULT       "#40b840"
#define FT8_COL_NEW_DXCC_DEFAULT "#a87a00"
#define FT8_COL_NEW_BAND_DEFAULT "#3d7cc9"
#define FT8_COL_HL_TEXT_DEFAULT  "#000000"
#define FT8_COL_HL_CQ_DEFAULT    "#ffffff"

// Store `src` in dst as "#rrggbb" if GDK can parse it, else `fallback`.
extern void ft8_color_set(char dst[8], const char *src, const char *fallback);
// Re-read radio->ft8_col_* into the panel's stylesheet (live, no rebuild).
extern void ft8_panel_colors_changed(void);

#endif
