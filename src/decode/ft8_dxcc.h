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
 * DXCC entity resolver, backed by AD1C's cty.dat country file.
 *
 * Maps a callsign to a DXCC entity so the FT8 panel can flag a station whose
 * entity we have not worked before ("new one").  cty.dat (Jim Reisert AD1C,
 * https://www.country-files.com/) is bundled with the app and freely
 * redistributable for amateur-radio use.
 */

#ifndef _FT8_DXCC_H
#define _FT8_DXCC_H

#include <glib.h>

// Load cty.dat (searches the bundle/cwd/config dir).  Safe to call once at
// start-up; a missing file just leaves the resolver empty (lookups return -1).
extern void ft8_dxcc_init(void);

// TRUE once cty.dat has been loaded with at least one entity.
extern gboolean ft8_dxcc_loaded(void);

// Number of DXCC entities currently loaded (0 if cty.dat was not found).
extern int ft8_dxcc_count(void);

// Discard the current table and re-read cty.dat from disk; returns the new
// entity count.  Used by the "Reload" button on the FT8 configuration page.
extern int ft8_dxcc_reload(void);

// The path cty.dat was last loaded from (for display), or NULL if not found.
extern const char *ft8_dxcc_path(void);

// Resolve a callsign to a DXCC entity index (>=0), or -1 if unknown / not
// loaded.  Exact-call overrides win; otherwise the longest matching prefix.
extern int ft8_dxcc_entity(const char *call);

// Human-readable country name for an entity index, or NULL if out of range.
extern const char *ft8_dxcc_name(int ent);

#endif
