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

#ifndef _AGC_H
#define _AGC_H

#define AGC_OFF 0
#define AGC_LONG 1
#define AGC_SLOW 2
#define AGC_MEDIUM 3
#define AGC_FAST 4

#define AGC_LAST AGC_FAST

#include "mode.h"

// Should WDSP's AGC block be IN the chain for this (mode, agc speed) pair?
//
// SetRXAMode() owns that run flag and clears it for FM and WFM (wdsp/RXA.c);
// nothing else in WDSP ever writes it, so until set_agc() started pushing this
// the AGC menu and the AGC-G slider did nothing at all on FM. AGC_OFF stays
// genuinely off there rather than becoming run=1/mode=0, which is wcpagc's
// fixed-gain path -- create_wcpagc opens it at fixed_gain=1000, i.e. +60 dB.
// Every other mode keeps the 1 SetRXAMode() already gives it.
//
// An inline in the header rather than a function in receiver.c because
// tools/agc_offline.c has to exercise THIS rule and cannot link receiver.o
// (which drags in the whole application).
static inline int agc_run_for(int mode, int agc) {
  return ((mode != FMN && mode != WFM) || agc != AGC_OFF) ? 1 : 0;
}

#endif
