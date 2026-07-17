/* Copyright (C)
* 2022 - m5evt
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
#ifndef _PSIGNAL_H
#define _PSIGNAL_H

#include <gtk/gtk.h>

typedef struct _psignal {
  gint ints;
  gint spi;
  gint stbl;
  gint map;
  gint pin;
  gdouble ptol;
  gdouble mox_delay;
  gdouble loop_delay;
  gdouble amp_delay;

  gdouble peak_value;

  gint info_timer_id;

  gint attenuation;

  gint state;
  gint auto_on;
  gint old_cor_cnt;
} PSIGNAL;

// Runtime PureSignal debug logging. Off by default; when zero, none of the
// PS_DEBUG() diagnostics (including the per-TX-sample feedback trace) fire.
// Enabled by running with the LINHPSDR_PS_DEBUG environment variable set
// (any value); create_puresignal() reads it. Can also be set directly.
extern int ps_debug;
#define PS_DEBUG(...) do { if (ps_debug) g_print(__VA_ARGS__); } while (0)

extern void ps_change_tx_attenuation(PSIGNAL *ps, int att_diff);
extern int ps_get_tx_attenuation(PSIGNAL *ps);
extern PSIGNAL *create_puresignal(void);
#endif
