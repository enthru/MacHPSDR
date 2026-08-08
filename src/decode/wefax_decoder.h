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
 * WEFAX / HF radiofax receive decoder.
 *
 * The shortwave analogue-fax cousin of the SSTV decoder: weather charts and
 * satellite images broadcast by coastal/meteorological stations (DWD, Northwood,
 * …) on HF, received in USB/DIGU.  It reuses the exact same tone convention as
 * SSTV (1500 Hz = black .. 2300 Hz = white) and the same Hilbert-transform FM
 * discriminator, but the format differs fundamentally: a *continuous* scan (no
 * fixed frame height — the image grows and scrolls), no VIS header, and line
 * synchronisation from the transmission's own start tone + phasing signal rather
 * than a per-line sync pulse.  Self-contained (no WDSP/FFT); runs inline on the
 * RX audio thread, hands the image to the UI through a mutex-protected buffer.
 */

#ifndef _WEFAX_DECODER_H
#define _WEFAX_DECODER_H

#include <gtk/gtk.h>

// Enable/disable the decoder (driven from the RX audio tap in receiver.c off the
// bottom-bar decode selector).  A no-op fast path when disabled.
void wefax_decoder_set_enabled(gboolean on);

// Feed demodulated audio: 48 kHz, interleaved-stereo doubles (left channel).
void wefax_decoder_add_audio(const gdouble *samples, int nframes);

// Lines per minute (60/90/120/240; 120 is the weather-fax standard) and Index Of
// Cooperation (576 standard / 288).  LPM sets the line period; IOC affects the
// start-tone rate and aspect.  Settable live from the panel.
void wefax_decoder_set_lpm(int lpm);
void wefax_decoder_set_ioc(int ioc);

// Automatic start-tone detection: when on, the 300 Hz (IOC576) / 675 Hz (IOC288)
// start signal auto-starts a fresh image, sets the IOC and seeds the AFC.  With
// it off only the manual Start button begins a page.
void wefax_decoder_set_autostart(gboolean on);

// Continuous auto-phasing: when on (default), the decoder finds the recurring
// vertical reference (the fax margin/border) and keeps it at the left margin, so
// the image self-aligns without the operator clicking.  Turn off for manual
// click-to-phase.
void wefax_decoder_set_autophase(gboolean on);

// Conditional-median despeckle (default on): removes isolated impulse-noise
// pixels while preserving real edges / thin lines.
void wefax_decoder_set_denoise(gboolean on);

// Negative image: swap white<->black.  Standard weather fax is black-on-white
// (off); turn on if the signal comes in inverted (e.g. wrong sideband).
void wefax_decoder_set_invert(gboolean on);

// Manual exposure trim on top of the fixed tone→grey mapping: `contrast` is a
// gain about mid-grey (1.0 = as decoded), `brightness` an offset in grey levels.
// A weak or hazy chart comes out flat grey, and the decoder cannot fix that for
// the operator — the levels are defined by the tone convention, not measured.
// Applied on output, so a change re-maps the whole page rather than seaming it.
void wefax_decoder_set_levels(double contrast, double brightness);

// Write the page to `dir` as a PNG when the next start tone wipes it (or the
// decoder is switched off).  Same reason as APT and SSTV: the wipe is automatic,
// the transmission is not repeatable, and an unattended receiver is the normal
// way to take a fax.  Explicit Clear does not save.
void wefax_decoder_set_autosave(gboolean on, const char *dir);

// Manual controls (GTK thread).  Start begins a fresh page now (as if a start
// tone was seen); reset clears the image.
void wefax_decoder_start(void);
void wefax_decoder_reset(void);

// Fine slant / line-period trim, in ppm added to the assumed clock.
void   wefax_decoder_adjust_slant(double dppm);
double wefax_decoder_get_slant(void);

// Manual phase alignment: shift the line start by a fraction of one line
// (frac in −1..+1).  The panel passes the clicked column fraction so the click
// lands at the left margin.
void wefax_decoder_nudge_phase(double frac_of_line);

// Measured audio-frequency offset (AFC), in Hz — how far the received tones sit
// from nominal (mistuning).  Corrected automatically in the decode.
double wefax_decoder_get_afc(void);

// Snapshot for the UI (polled on the GTK thread).
typedef struct {
  gboolean receiving;    // a page is being decoded (past the start/phasing)
  int      lpm, ioc;
  int      width, height;// image buffer geometry (pixels)
  int      line;         // rows decoded so far into the current page
  char     status[64];   // short human-readable status line
} wefax_status_t;

void wefax_decoder_get_status(wefax_status_t *st);

// Fresh GdkPixbuf copy of the current image (caller owns the ref), or NULL if
// nothing has been decoded yet.
GdkPixbuf *wefax_decoder_get_image(void);

#endif
