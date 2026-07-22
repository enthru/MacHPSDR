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
 * SSTV receive decoder (Scottie + Martin families).
 *
 * A self-contained analogue-SSTV decoder: it FM-demodulates the 48 kHz
 * demodulated SSB/DIGU audio (1500 Hz = black .. 2300 Hz = white, 1200 Hz =
 * sync) with a Hilbert-transform discriminator, auto-detects the transmission
 * mode from its VIS header, and reconstructs the image line-by-line with
 * per-line sync locking (which absorbs clock offset / slant).  Supported modes:
 * Martin M1/M2 and Scottie S1/S2/DX — the GBR-sequential family that dominates
 * amateur SSTV.  No external DSP dependency (no WDSP/FFT); the discriminator and
 * line decoder run inline on the RX audio thread, the image is handed to the UI
 * through a mutex-protected RGB buffer.
 */

#ifndef _SSTV_DECODER_H
#define _SSTV_DECODER_H

#include <gtk/gtk.h>

// Enable/disable the decoder.  Driven from the RX audio tap (receiver.c) off the
// bottom-bar decode selector; a no-op fast path when disabled.
void sstv_decoder_set_enabled(gboolean on);

// Feed demodulated audio: 48 kHz, interleaved-stereo doubles (left channel used,
// samples[i*2]).  No-op unless enabled.  Runs on the RX audio thread.
void sstv_decoder_add_audio(const gdouble *samples, int nframes);

// Force a mode by VIS code (44/40 Martin M1/M2, 60/56/76 Scottie S1/S2/DX), or
// 0 for automatic VIS detection.  The image still anchors on the transmitted VIS
// leader; only the decoded mode is overridden.
void sstv_decoder_set_mode(int vis);

// Snapshot of decoder state for the UI (polled on the GTK thread).
typedef struct {
  gboolean receiving;    // a frame is currently being reconstructed
  int      vis;          // current/last mode VIS code (0 = none yet)
  char     mode_name[24];// "Martin M1", "Scottie DX", …  ("" if none)
  int      width, height;
  int      line;         // most recent completed line index
  int      progress;     // 0..100
  char     status[64];   // short human-readable status line
} sstv_status_t;

void sstv_decoder_get_status(sstv_status_t *st);

// Return a fresh GdkPixbuf copy of the current image (caller owns the ref,
// g_object_unref when done), or NULL if no image has been started yet.
GdkPixbuf *sstv_decoder_get_image(void);

// Clear the image and return to hunting for the next VIS header.
void sstv_decoder_reset(void);

// Fine slant / clock correction, in ppm added to the assumed pixel clock.
void   sstv_decoder_adjust_slant(double dppm);
double sstv_decoder_get_slant(void);

#endif
