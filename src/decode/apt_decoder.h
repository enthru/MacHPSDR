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
 * APT (Automatic Picture Transmission) receive decoder — the NOAA-15/18/19
 * weather-satellite image mode on 137 MHz.
 *
 * The image cousin of SSTV/WEFAX, but a different problem in two ways:
 *
 *  - It is FM on VHF, ~34 kHz wide (±17 kHz deviation).  Neither of our FM
 *    filters fits it: FMN tops out at 16 kHz (it would cut the signal in half)
 *    and WFM starts at 100 kHz (it would drag in 60 kHz of extra noise).  So
 *    this decoder does NOT tap the demodulated audio the way SSTV/WEFAX do —
 *    it takes the raw off-air I/Q and runs its own front-end (NCO downmix →
 *    decimating low-pass → FM discriminator), exactly like the HFDL decoder.
 *    The receiver's demod mode therefore has no effect on the decode; it only
 *    changes what the operator hears.
 *
 *  - The video is AM on a 2400 Hz subcarrier rather than direct FM-audio tone
 *    coding, so the detector is a 2400 Hz quadrature mixer + low-pass +
 *    magnitude.  That also makes the decode immune to mistuning and Doppler:
 *    a carrier offset lands as DC at the discriminator output, which the
 *    subcarrier mix pushes to −2400 Hz and the video low-pass throws away.
 *    (Doppler at 137 MHz is ±3 kHz — well inside the front-end's own filter.)
 *
 * Line format (2 lines/s, 4160 words/s, 2080 words per line):
 *   sync A 39 | space A 47 | image A 909 | telemetry A 45 |
 *   sync B 39 | space B 47 | image B 909 | telemetry B 45
 * Sync A is 7 cycles of a 1040 Hz square wave; correlating against it is what
 * locks the line start and, through a slow clock trim, removes the slant.
 *
 * Self-contained: no WDSP, no FFT, no external DSP library.  Runs inline on the
 * RX audio thread and hands the image to the UI through a mutex-protected
 * buffer, like the other image decoders.
 */

#ifndef _APT_DECODER_H
#define _APT_DECODER_H

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

// Enable/disable the decoder (driven from the raw-I/Q tap in receiver.c off the
// bottom-bar decode selector). A no-op fast path when disabled; enabling resets.
void apt_decoder_set_enabled(gboolean on);

// Feed raw off-air complex I/Q. `iq` is the receiver's interleaved buffer, which
// is (Q, I) per pair — the same order WDSP reads (see receiver.c) — `nframes`
// complex samples at `rate` Hz, taken around `centre_hz`. `cursor_hz` is where
// the operator is pointing (the CTUN/freetune cursor, else the centre): the
// decoder downmixes cursor−centre to DC, so the satellite can sit anywhere in
// the passband.
void apt_decoder_add_iq(const gdouble *iq, int nframes, double rate,
                        long long centre_hz, long long cursor_hz);

// Feed already-FM-demodulated APT audio (the widely-available recording format).
// `stride` is the sample stride: 2 for the app's interleaved-stereo buffers,
// 1 for a mono WAV. Any sample rate; everything downstream is rate-parametric.
void apt_decoder_add_audio(const gdouble *samples, int nframes, int stride, double rate);

// Clear the image and drop the line lock (GTK thread; applied on the audio thread).
void apt_decoder_reset(void);

// Which part of the 2080-word line to show: 0 = the whole line (both channels,
// sync and telemetry included), 1 = channel A only, 2 = channel B only.
void apt_decoder_set_channel(int ch);

// Fine slant / line-period trim in ppm, added to the automatic clock trim.
void   apt_decoder_adjust_slant(double dppm);
double apt_decoder_get_slant(void);

// Snapshot for the UI (polled on the GTK thread).
typedef struct {
  gboolean locked;     // sync A is being tracked (only then are lines drawn)
  int      lines;      // rows decoded into the current image
  double   quality;    // last sync correlation, −1..1
  double   clock_ppm;  // automatic clock/slant trim the servo has settled on
  long long tuned_hz;  // where the front-end is listening (absolute Hz) — a
                       // decoder pointed elsewhere than the operator believes
                       // looks exactly like a dead band
  char     status[64]; // short human-readable status line
} apt_status_t;

void apt_decoder_get_status(apt_status_t *st);

// Fresh GdkPixbuf copy of the current image, cropped to the selected channel
// (caller owns the ref), or NULL if nothing has been decoded yet.
GdkPixbuf *apt_decoder_get_image(void);

#endif
