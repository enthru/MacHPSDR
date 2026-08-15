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

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>

#include "mode.h"
#include "filter.h"
#include "property.h"

// Bound for a persisted Var1/Var2 edge. Nothing read out of a props file is
// trusted: an older or hand-edited one is a supported input, and these numbers
// go straight to RXASetPassband(). Wider than any entry in the tables below
// (WFM's +/-90 kHz is the widest) and than Nyquist at the highest rate the DSP
// runs at, so it can never clip a setting an operator actually made.
//
// The ORDER is deliberately not checked. For CWL/CWU the pair is a half-width
// either side of the sidetone pitch (receiver.c: pitch-low .. pitch+high), so
// low == high is normal there and a `low < high` rule would be wrong.
#define FILTER_EDGE_LIMIT_HZ 192000

FILTER filterLSB[FILTERS]={
    {-5150,-150,"5.0k"},
    {-4550,-150,"4.4k"},
    {-3950,-150,"3.8k"},
    {-3450,-150,"3.3k"},
    {-3050,-150,"2.9k"},
    {-2850,-150,"2.7k"},
    {-2550,-150,"2.4k"},
    {-2250,-150,"2.1k"},
    {-1950,-150,"1.8k"},
    {-1150,-150,"1.0k"},
    {-2850,-150,"Var1"},
    {-2850,-150,"Var2"}
    };

FILTER filterDIGL[FILTERS]={
    {-5150,-150,"5.0k"},
    {-4550,-150,"4.4k"},
    {-3950,-150,"3.8k"},
    {-3450,-150,"3.3k"},
    {-3050,-150,"2.9k"},
    {-2850,-150,"2.7k"},
    {-2550,-150,"2.4k"},
    {-2250,-150,"2.1k"},
    {-1950,-150,"1.8k"},
    {-1150,-150,"1.0k"},
    {-2850,-150,"Var1"},
    {-2850,-150,"Var2"}
    };

FILTER filterUSB[FILTERS]={
    {150,5150,"5.0k"},
    {150,4550,"4.4k"},
    {150,3950,"3.8k"},
    {150,3450,"3.3k"},
    {150,3050,"2.9k"},
    {150,2850,"2.7k"},
    {150,2550,"2.4k"},
    {150,2250,"2.1k"},
    {150,1950,"1.8k"},
    {150,1150,"1.0k"},
    {150,2850,"Var1"},
    {150,2850,"Var2"}
    };

FILTER filterDIGU[FILTERS]={
    {150,5150,"5.0k"},
    {150,4550,"4.4k"},
    {150,3950,"3.8k"},
    {150,3450,"3.3k"},
    {150,3050,"2.9k"},
    {150,2850,"2.7k"},
    {150,2550,"2.4k"},
    {150,2250,"2.1k"},
    {150,1950,"1.8k"},
    {150,1150,"1.0k"},
    {150,2850,"Var1"},
    {150,2850,"Var2"}
    };

FILTER filterCWL[FILTERS]={
    {500,500,"1.0k"},
    {400,400,"800"},
    {375,375,"750"},
    {300,300,"600"},
    {250,250,"500"},
    {200,200,"400"},
    {100,100,"200"},
    {50,50,"100"},
    {25,25,"50"},
    {13,13,"25"},
    {250,250,"Var1"},
    {250,250,"Var2"}
    };

FILTER filterCWU[FILTERS]={
    {500,500,"1.0k"},
    {400,400,"800"},
    {375,375,"750"},
    {300,300,"600"},
    {250,250,"500"},
    {200,200,"400"},
    {100,100,"200"},
    {50,50,"100"},
    {25,25,"50"},
    {13,13,"25"},
    {250,250,"Var1"},
    {250,250,"Var2"}
    };

FILTER filterAM[FILTERS]={
    {-8000,8000,"16k"},
    {-6000,6000,"12k"},
    {-5000,5000,"10k"},
    {-4000,4000,"8k"},
    {-3300,3300,"6.6k"},
    {-2600,2600,"5.2k"},
    {-2000,2000,"4.0k"},
    {-1550,1550,"3.1k"},
    {-1450,1450,"2.9k"},
    {-1200,1200,"2.4k"},
    {-3300,3300,"Var1"},
    {-3300,3300,"Var2"}
    };

FILTER filterSAM[FILTERS]={
    {-8000,8000,"16k"},
    {-6000,6000,"12k"},
    {-5000,5000,"10k"},
    {-4000,4000,"8k"},
    {-3300,3300,"6.6k"},
    {-2600,2600,"5.2k"},
    {-2000,2000,"4.0k"},
    {-1550,1550,"3.1k"},
    {-1450,1450,"2.9k"},
    {-1200,1200,"2.4k"},
    {-3300,3300,"Var1"},
    {-3300,3300,"Var2"}
    };

// The first two labels used to read "8k" and "16k" for these same passbands --
// a straight typo, and a visible one: it put a second button called "8k" next
// to the real +/-4000 one, and called the +/-6000 filter wider than the
// +/-8000 above it. Every other mode's table labels these two "16k" and "12k".
// The passbands themselves are unchanged.
FILTER filterFMN[FILTERS]={
    {-8000,8000,"16k"},
    {-6000,6000,"12k"},
    {-5000,5000,"10k"},
    {-4000,4000,"8k"},
    {-3300,3300,"6.6k"},
    {-2600,2600,"5.2k"},
    {-2000,2000,"4.0k"},
    {-1550,1550,"3.1k"},
    {-1450,1450,"2.9k"},
    {-1200,1200,"2.4k"},
    {-3300,3300,"Var1"},
    {-3300,3300,"Var2"}
    };

FILTER filterDSB[FILTERS]={
    {-8000,8000,"16k"},
    {-6000,6000,"12k"},
    {-5000,5000,"10k"},
    {-4000,4000,"8k"},
    {-3300,3300,"6.6k"},
    {-2600,2600,"5.2k"},
    {-2000,2000,"4.0k"},
    {-1550,1550,"3.1k"},
    {-1450,1450,"2.9k"},
    {-1200,1200,"2.4k"},
    {-3300,3300,"Var1"},
    {-3300,3300,"Var2"}
    };

FILTER filterSPEC[FILTERS]={
    {-8000,8000,"16k"},
    {-6000,6000,"12k"},
    {-5000,5000,"10k"},
    {-4000,4000,"8k"},
    {-3300,3300,"6.6k"},
    {-2600,2600,"5.2k"},
    {-2000,2000,"4.0k"},
    {-1550,1550,"3.1k"},
    {-1450,1450,"2.9k"},
    {-1200,1200,"2.4k"},
    {-3300,3300,"Var1"},
    {-3300,3300,"Var2"}
    };

FILTER filterDRM[FILTERS]={
    {-8000,8000,"16k"},
    {-6000,6000,"12k"},
    {-5000,5000,"10k"},
    {-4000,4000,"8k"},
    {-3300,3300,"6.6k"},
    {-2600,2600,"5.2k"},
    {-2000,2000,"4.0k"},
    {-1550,1550,"3.1k"},
    {-1450,1450,"2.9k"},
    {-1200,1200,"2.4k"},
    {-3300,3300,"Var1"},
    {-3300,3300,"Var2"}
    };

FILTER filterWFM[FILTERS]={
    {-90000,90000,"180k"},
    {-80000,80000,"160k"},
    {-70000,70000,"140k"},
    {-60000,60000,"120k"},
    {-50000,50000,"100k"},
    {-90000,90000,"180k"},
    {-80000,80000,"160k"},
    {-70000,70000,"140k"},
    {-60000,60000,"120k"},
    {-50000,50000,"100k"},
    {-90000,90000,"Var1"},
    {-90000,90000,"Var2"}
    };

FILTER *filters[]={
    filterLSB
    ,filterUSB
    ,filterDSB
    ,filterCWL
    ,filterCWU
    ,filterFMN
    ,filterAM
    ,filterDIGU
    ,filterSPEC
    ,filterDIGL
    ,filterSAM
    ,filterDRM
    ,filterWFM

};

// Var1/Var2 are the only editable entries, and they are persisted for EVERY
// mode by walking the same filters[] table the rest of the app indexes -- not
// by a hand-written block per mode.
//
// The hand-written version covered ten of the thirteen: SPEC, DRM and WFM were
// saved by nothing and restored by nothing, so a Var filter set in one of them
// was silently discarded on exit. All three are reachable -- the UI edits Var
// for whatever mode is current, and CAT (`FL`/`ZZFL` and friends) writes
// filters[mode][FVar1] the same way. That is the "eleven ways a setting was
// silently lost" disease in the one file that pass did not touch, and the cure
// has to be structural: a per-mode block is a list that the next mode gets left
// off, a loop over MODES cannot be.
//
// The key is the mode's own lowercased name, which reproduces the existing
// spelling exactly ("filter.lsb.var1.low", ...), so props files written by
// earlier builds still load.
static void filter_var_key(char *buf, size_t n, int mode, int var, const char *edge) {
  char m[16];
  size_t i = 0;
  for (; mode_string[mode][i] != '\0' && i < sizeof(m) - 1; i++)
    m[i] = g_ascii_tolower(mode_string[mode][i]);
  m[i] = '\0';
  snprintf(buf, n, "filter.%s.var%d.%s", m, var == FVar1 ? 1 : 2, edge);
}

void filterSaveState(void) {
  char name[64];
  char value[32];

  for (int mode = 0; mode < MODES; mode++) {
    for (int var = FVar1; var <= FVar2; var++) {
      snprintf(value, sizeof(value), "%d", filters[mode][var].low);
      filter_var_key(name, sizeof(name), mode, var, "low");
      setProperty(name, value);
      snprintf(value, sizeof(value), "%d", filters[mode][var].high);
      filter_var_key(name, sizeof(name), mode, var, "high");
      setProperty(name, value);
    }
  }
}

static int filter_edge(const char *value, int fallback) {
  if (value == NULL) return fallback;
  long v = atol(value);
  if (v >  FILTER_EDGE_LIMIT_HZ) v =  FILTER_EDGE_LIMIT_HZ;
  if (v < -FILTER_EDGE_LIMIT_HZ) v = -FILTER_EDGE_LIMIT_HZ;
  return (int)v;
}

void filterRestoreState(void) {
  char name[64];

  for (int mode = 0; mode < MODES; mode++) {
    for (int var = FVar1; var <= FVar2; var++) {
      filter_var_key(name, sizeof(name), mode, var, "low");
      filters[mode][var].low  = filter_edge(getProperty(name), filters[mode][var].low);
      filter_var_key(name, sizeof(name), mode, var, "high");
      filters[mode][var].high = filter_edge(getProperty(name), filters[mode][var].high);
    }
  }
}
