/* Copyright (C)
* 2024
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*/

#include "waterfall_theme.h"
#include <math.h>

typedef struct {
    unsigned char r, g, b;
} RGB;

static RGB legacy_theme[256];
static RGB mono_theme[256];
static RGB hot_theme[256];
static RGB cold_theme[256];
static RGB spectral_theme[256];
static RGB viridis_theme[256];
static RGB plasma_theme[256];
static RGB inferno_theme[256];
static RGB turbo_theme[256];
static RGB ocean_theme[256];
static RGB fire_theme[256];
static RGB ice_theme[256];
static RGB gqrx_theme[256];

static void interpolate_color(int level, int steps, int r1, int g1, int b1, int r2, int g2, int b2, RGB *out) {
    float t = (float)level / (float)steps;
    out->r = (unsigned char)(r1 + t * (r2 - r1));
    out->g = (unsigned char)(g1 + t * (g2 - g1));
    out->b = (unsigned char)(b1 + t * (b2 - b1));
}

void init_waterfall_themes(void) {
    int i;

    for(i=0; i<256; i++) {
        float percent = i / 255.0f;

        if(percent < (2.0f/9.0f)) {
            float local_percent = percent / (2.0f/9.0f);
            legacy_theme[i].r = 0;
            legacy_theme[i].g = 0;
            legacy_theme[i].b = (unsigned char)(local_percent * 255);
        } else if(percent < (3.0f/9.0f)) {
            float local_percent = (percent - 2.0f/9.0f) / (1.0f/9.0f);
            legacy_theme[i].r = 0;
            legacy_theme[i].g = (unsigned char)(local_percent * 255);
            legacy_theme[i].b = 255;
        } else if(percent < (4.0f/9.0f)) {
            float local_percent = (percent - 3.0f/9.0f) / (1.0f/9.0f);
            legacy_theme[i].r = 0;
            legacy_theme[i].g = 255;
            legacy_theme[i].b = (unsigned char)((1.0f - local_percent) * 255);
        } else if(percent < (5.0f/9.0f)) {
            float local_percent = (percent - 4.0f/9.0f) / (1.0f/9.0f);
            legacy_theme[i].r = (unsigned char)(local_percent * 255);
            legacy_theme[i].g = 255;
            legacy_theme[i].b = 0;
        } else if(percent < (7.0f/9.0f)) {
            float local_percent = (percent - 5.0f/9.0f) / (2.0f/9.0f);
            legacy_theme[i].r = 255;
            legacy_theme[i].g = (unsigned char)((1.0f - local_percent) * 255);
            legacy_theme[i].b = 0;
        } else if(percent < (8.0f/9.0f)) {
            float local_percent = (percent - 7.0f/9.0f) / (1.0f/9.0f);
            legacy_theme[i].r = 255;
            legacy_theme[i].g = 0;
            legacy_theme[i].b = (unsigned char)(local_percent * 255);
        } else {
            float local_percent = (percent - 8.0f/9.0f) / (1.0f/9.0f);
            legacy_theme[i].r = (unsigned char)((0.75f + 0.25f * (1.0f - local_percent)) * 255.0f);
            legacy_theme[i].g = (unsigned char)(local_percent * 255.0f * 0.5f);
            legacy_theme[i].b = 255;
        }
    }

    // Mono
    for(i=0; i<256; i++) {
        mono_theme[i].r = i;
        mono_theme[i].g = i;
        mono_theme[i].b = i;
    }

    // Hot
    for(i=0; i<256; i++) {
        if(i < 85) {
            hot_theme[i].r = i * 3;
            hot_theme[i].g = 0;
            hot_theme[i].b = 0;
        } else if(i < 170) {
            hot_theme[i].r = 255;
            hot_theme[i].g = (i - 85) * 3;
            hot_theme[i].b = 0;
        } else {
            hot_theme[i].r = 255;
            hot_theme[i].g = 255;
            hot_theme[i].b = (i - 170) * 3;
        }
    }

    // Cold
    for(i=0; i<256; i++) {
        if(i < 128) {
            cold_theme[i].r = 0;
            cold_theme[i].g = i * 2;
            cold_theme[i].b = 255;
        } else {
            cold_theme[i].r = (i - 128) * 2;
            cold_theme[i].g = 255;
            cold_theme[i].b = 255;
        }
    }

    // Spectral
    for(i=0; i<256; i++) {
        float h = i / 255.0f * 300.0f;
        float s = 1.0f;
        float v = 1.0f;

        int hi = (int)(h / 60.0f) % 6;
        float f = h / 60.0f - hi;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);

        switch(hi) {
            case 0:
                spectral_theme[i].r = (unsigned char)(v * 255);
                spectral_theme[i].g = (unsigned char)(t * 255);
                spectral_theme[i].b = (unsigned char)(p * 255);
                break;
            case 1:
                spectral_theme[i].r = (unsigned char)(q * 255);
                spectral_theme[i].g = (unsigned char)(v * 255);
                spectral_theme[i].b = (unsigned char)(p * 255);
                break;
            case 2:
                spectral_theme[i].r = (unsigned char)(p * 255);
                spectral_theme[i].g = (unsigned char)(v * 255);
                spectral_theme[i].b = (unsigned char)(t * 255);
                break;
            case 3:
                spectral_theme[i].r = (unsigned char)(p * 255);
                spectral_theme[i].g = (unsigned char)(q * 255);
                spectral_theme[i].b = (unsigned char)(v * 255);
                break;
            case 4:
                spectral_theme[i].r = (unsigned char)(t * 255);
                spectral_theme[i].g = (unsigned char)(p * 255);
                spectral_theme[i].b = (unsigned char)(v * 255);
                break;
            case 5:
                spectral_theme[i].r = (unsigned char)(v * 255);
                spectral_theme[i].g = (unsigned char)(p * 255);
                spectral_theme[i].b = (unsigned char)(q * 255);
                break;
        }
    }

    // Viridis
    for(i=0; i<256; i++) {
        float t = i / 255.0f;
        viridis_theme[i].r = (unsigned char)(255 * (0.267 + 0.005 * t + 0.322 * t*t));
        viridis_theme[i].g = (unsigned char)(255 * (0.005 + 0.978 * t - 0.327 * t*t));
        viridis_theme[i].b = (unsigned char)(255 * (0.330 + 1.073 * t - 1.420 * t*t + 0.674 * t*t*t));
    }

    // Plasma
    for(i=0; i<256; i++) {
        float t = i / 255.0f;
        plasma_theme[i].r = (unsigned char)(255 * (0.050 + 2.035 * t - 1.915 * t*t + 0.830 * t*t*t));
        plasma_theme[i].g = (unsigned char)(255 * (0.020 + 0.982 * t - 1.040 * t*t + 0.040 * t*t*t));
        plasma_theme[i].b = (unsigned char)(255 * (0.530 - 0.093 * t + 1.185 * t*t - 0.622 * t*t*t));
    }

    // Inferno
    for(i=0; i<256; i++) {
        float t = i / 255.0f;
        inferno_theme[i].r = (unsigned char)(255 * (0.001 + 1.422 * t - 0.200 * t*t));
        inferno_theme[i].g = (unsigned char)(255 * (0.000 + 1.571 * t*t - 0.571 * t*t*t));
        inferno_theme[i].b = (unsigned char)(255 * (0.145 + 1.480 * t - 2.553 * t*t + 1.928 * t*t*t));
    }

    // Turbo
    for(i=0; i<256; i++) {
        if(i < 64) {
            interpolate_color(i, 64, 48, 18, 59, 50, 136, 189, &turbo_theme[i]);
        } else if(i < 128) {
            interpolate_color(i-64, 64, 50, 136, 189, 136, 194, 67, &turbo_theme[i]);
        } else if(i < 192) {
            interpolate_color(i-128, 64, 136, 194, 67, 254, 254, 51, &turbo_theme[i]);
        } else {
            interpolate_color(i-192, 64, 254, 254, 51, 240, 39, 32, &turbo_theme[i]);
        }
    }

    // Ocean
    for(i=0; i<256; i++) {
        if(i < 85) {
            ocean_theme[i].r = 0;
            ocean_theme[i].g = i * 2;
            ocean_theme[i].b = 128 + i;
        } else if(i < 170) {
            ocean_theme[i].r = 0;
            ocean_theme[i].g = 170 + (i-85);
            ocean_theme[i].b = 255 - (i-85);
        } else {
            ocean_theme[i].r = (i-170) * 2;
            ocean_theme[i].g = 255;
            ocean_theme[i].b = 170 - (i-170);
        }
    }

    // Fire
    for(i=0; i<256; i++) {
        if(i < 64) {
            fire_theme[i].r = i * 4;
            fire_theme[i].g = 0;
            fire_theme[i].b = 0;
        } else if(i < 128) {
            fire_theme[i].r = 255;
            fire_theme[i].g = (i-64) * 4;
            fire_theme[i].b = 0;
        } else if(i < 192) {
            fire_theme[i].r = 255;
            fire_theme[i].g = 255;
            fire_theme[i].b = (i-128) * 2;
        } else {
            fire_theme[i].r = 255;
            fire_theme[i].g = 255;
            fire_theme[i].b = 128 + (i-192) * 2;
        }
    }

    // Ice
    for(i=0; i<256; i++) {
        if(i < 128) {
            ice_theme[i].r = 0;
            ice_theme[i].g = i;
            ice_theme[i].b = 128 + i;
        } else {
            ice_theme[i].r = (i-128) * 2;
            ice_theme[i].g = 128 + (i-128);
            ice_theme[i].b = 255;
        }
    }

    // GQRX
    for(i=0; i<256; i++) {
        if(i < 43) {
            gqrx_theme[i].r = 0;
            gqrx_theme[i].g = 0;
            gqrx_theme[i].b = i * 6;
        } else if(i < 87) {
            gqrx_theme[i].r = 0;
            gqrx_theme[i].g = (i-43) * 5;
            gqrx_theme[i].b = 255;
        } else if(i < 120) {
            gqrx_theme[i].r = 0;
            gqrx_theme[i].g = 220 + (i-87);
            gqrx_theme[i].b = 255 - (i-87) * 3;
        } else if(i < 154) {
            gqrx_theme[i].r = (i-120) * 7;
            gqrx_theme[i].g = 255;
            gqrx_theme[i].b = 156 - (i-120) * 4;
        } else if(i < 217) {
            gqrx_theme[i].r = 238 + (i-154) / 4;
            gqrx_theme[i].g = 255 - (i-154);
            gqrx_theme[i].b = 20;
        } else {
            gqrx_theme[i].r = 252 + (i-217) / 13;
            gqrx_theme[i].g = 192 - (i-217) * 2;
            gqrx_theme[i].b = 20 + (i-217) * 6;
        }
    }
}

void get_waterfall_color(int theme, int level, unsigned char *r, unsigned char *g, unsigned char *b) {
    if(level < 0) level = 0;
    if(level > 255) level = 255;

    switch(theme) {
        case 0: // Legacy
            *r = legacy_theme[level].r;
            *g = legacy_theme[level].g;
            *b = legacy_theme[level].b;
            break;
        case 1: // Mono
            *r = mono_theme[level].r;
            *g = mono_theme[level].g;
            *b = mono_theme[level].b;
            break;
        case 2: // Hot
            *r = hot_theme[level].r;
            *g = hot_theme[level].g;
            *b = hot_theme[level].b;
            break;
        case 3: // Cold
            *r = cold_theme[level].r;
            *g = cold_theme[level].g;
            *b = cold_theme[level].b;
            break;
        case 4: // Spectral
            *r = spectral_theme[level].r;
            *g = spectral_theme[level].g;
            *b = spectral_theme[level].b;
            break;
        case 5: // Viridis
            *r = viridis_theme[level].r;
            *g = viridis_theme[level].g;
            *b = viridis_theme[level].b;
            break;
        case 6: // Plasma
            *r = plasma_theme[level].r;
            *g = plasma_theme[level].g;
            *b = plasma_theme[level].b;
            break;
        case 7: // Inferno
            *r = inferno_theme[level].r;
            *g = inferno_theme[level].g;
            *b = inferno_theme[level].b;
            break;
        case 8: // Turbo
            *r = turbo_theme[level].r;
            *g = turbo_theme[level].g;
            *b = turbo_theme[level].b;
            break;
        case 9: // Ocean
            *r = ocean_theme[level].r;
            *g = ocean_theme[level].g;
            *b = ocean_theme[level].b;
            break;
        case 10: // Fire
            *r = fire_theme[level].r;
            *g = fire_theme[level].g;
            *b = fire_theme[level].b;
            break;
        case 11: // Ice
            *r = ice_theme[level].r;
            *g = ice_theme[level].g;
            *b = ice_theme[level].b;
            break;
        case 12: // GQRX
            *r = gqrx_theme[level].r;
            *g = gqrx_theme[level].g;
            *b = gqrx_theme[level].b;
            break;
        default:
            *r = legacy_theme[level].r;
            *g = legacy_theme[level].g;
            *b = legacy_theme[level].b;
            break;
    }
}

const char* get_theme_name(int theme) {
    switch(theme) {
        case 0: return "Legacy";
        case 1: return "Mono";
        case 2: return "Hot";
        case 3: return "Cold";
        case 4: return "Spectral";
        case 5: return "Viridis";
        case 6: return "Plasma";
        case 7: return "Inferno";
        case 8: return "Turbo";
        case 9: return "Ocean";
        case 10: return "Fire";
        case 11: return "Ice";
        case 12: return "GQRX";
        default: return "Unknown";
    }
}

int get_theme_count(void) {
    return 13;
}
