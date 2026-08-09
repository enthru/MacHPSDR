/* Copyright (C)
* 201h - John Melton, G0ORX/N6LYT
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

extern GtkWidget *create_xvtr_dialog(RADIO *radio);
extern void save_xvtr(void);
extern void lo_error_update(int band,long long offset);
/* Re-fill one transverter row's entry widgets from the bands table. Needed when
   something other than this page writes a band (the QO-100 page creates the two
   converters the satellite needs), since the entries are only populated when the
   dialog is built and would otherwise show stale text on the very same page.
   No-op when the Configure dialog is not open. */
extern void xvtr_dialog_refresh_row(int band);
extern void update_receiver(int band);
