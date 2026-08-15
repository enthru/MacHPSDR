/* Copyright (C)
* 2016 - John Melton, G0ORX/N6LYT
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

/* Include AFTER radio.h: everything here takes a RADIO *. */
#ifndef VOX_H
#define VOX_H

extern void update_vox(RADIO *r);
extern void vox_cancel(RADIO *r);

/* The one setter for the VOX enable -- UI button, CAT (VX) and MIDI must all go
 * through it. Turning VOX off while it is holding the key has to release it:
 * isTransmitting() ORs in radio->vox, so clearing only vox_enabled strands the
 * transmitter keyed. GTK thread. */
extern void vox_set_enabled(RADIO *r, gboolean enabled);

extern void update_cwvox(RADIO *r);
extern void set_cwvox(RADIO *r, gboolean cw_key_state);

#endif /* VOX_H */
