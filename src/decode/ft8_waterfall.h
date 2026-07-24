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
 * Dedicated FT8 waterfall (JTDX-style).  A self-contained spectrogram of just
 * the FT8 audio passband (0..3000 Hz), driven by an FFT of the decoder's 12 kHz
 * ring (ft8_decoder_get_spectrum) — independent of the main RX panadapter, so it
 * gives fine per-Hz resolution without touching the WDSP analyzer.  Left-click
 * sets the FT8 TX offset directly on a clear frequency.
 */

#ifndef _FT8_WATERFALL_H
#define _FT8_WATERFALL_H

#include <gtk/gtk.h>

// Build the FT8 waterfall widget (a drawing area with its own refresh timer,
// torn down on "destroy").  Only one is expected to exist at a time.
extern GtkWidget *ft8_waterfall_create(void);

#endif
