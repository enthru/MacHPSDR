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
 * Shared "write this decoded picture to disk, off the decoding thread" helper
 * for the image decoders (APT, SSTV, WEFAX).
 *
 * All three wipe their picture by themselves the moment the next transmission
 * starts — a retune or a lost pass for APT, a VIS header for SSTV, a start tone
 * for WEFAX — and none of those transmissions can be asked for again.  So each
 * one saves the outgoing picture first, and they all hit the same two problems:
 * the wipe happens on the RX audio thread, and a megabyte-scale PNG deflate has
 * no business running there (or on the UI thread).  Hence: the caller hands over
 * a plain pixel buffer it already owns and a worker encodes it.
 */

#ifndef _IMAGE_SAVE_H
#define _IMAGE_SAVE_H

#include <glib.h>

// Encode `pix` (w × h, `nchan` = 1 grey or 3 RGB) as <dir>/<prefix>_<UTC>.png on
// a throwaway thread.  TAKES OWNERSHIP of `pix` (g_free'd when done), so the
// caller can snapshot under its image lock and return immediately.  `dir` is
// created if missing; NULL/empty falls back to ~/.local/share/machpsdr/<prefix>.
// Safe to call from any thread; does nothing if the buffer is empty.
void image_save_async(guint8 *pix, int w, int h, int nchan,
                      const char *dir, const char *prefix);

// Resolve the folder the way image_save_async() does, for a panel that wants to
// show it or start a file chooser there.
void image_save_folder(char *out, gsize len, const char *dir, const char *prefix);

#endif
