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

#ifndef _PROPERTY_H
#define _PROPERTY_H

#define PROPERTY_VERSION 2.0

// The property store is a name->value string map. Get/set are O(1) (backed by
// a GHashTable in property.c); the old singly-linked list made building the
// store O(n^2) at save/restore time.
extern void initProperties(void);
// Park the store and work on a fresh empty one — for any file that is NOT the
// radio's own props (bookmarks, a MIDI file). Without it, loading or saving a
// second file wipes the live settings out from under the running radio.
extern void pushPropertyStore(void);
extern void popPropertyStore(void);
extern void retainProperties(char* prefix);
extern void releaseRetainedProperties(void);
extern void loadProperties(char* filename);
extern char* getProperty(char* name);
extern void setProperty(char* name,char* value);
/* Floats go through these, never sprintf("%f")/atof: the props file is written
   in the C locale and read tolerant of a comma decimal point, so it survives a
   locale change and an LC_NUMERIC=C run. */
extern void setPropertyDouble(char* name,double value);
extern double propToDouble(const char* value);

extern void saveProperties(char* filename);

#endif
