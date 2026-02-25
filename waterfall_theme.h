/* Copyright (C)
* 2024
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*/

#ifndef WATERFALL_THEME_H
#define WATERFALL_THEME_H

void init_waterfall_themes(void);
void get_waterfall_color(int theme, int level, unsigned char *r, unsigned char *g, unsigned char *b);
const char* get_theme_name(int theme);
int get_theme_count(void);

#define WATERFALL_THEME_LEGACY    0
#define WATERFALL_THEME_MONO      1
#define WATERFALL_THEME_HOT       2
#define WATERFALL_THEME_COLD      3
#define WATERFALL_THEME_SPECTRAL  4
#define WATERFALL_THEME_VIRIDIS   5
#define WATERFALL_THEME_PLASMA    6
#define WATERFALL_THEME_INFERNO   7
#define WATERFALL_THEME_TURBO     8
#define WATERFALL_THEME_OCEAN     9
#define WATERFALL_THEME_FIRE      10
#define WATERFALL_THEME_ICE       11
#define WATERFALL_THEME_GQRX      12

#endif
