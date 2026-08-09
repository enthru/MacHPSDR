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
 * VHF ACARS (ARINC 618) physical layer: raw off-air I/Q (or already
 * AM-demodulated audio) -> one validated ACARS block.
 *
 * ACARS on VHF is 2400 bps MSK amplitude-modulated onto the carrier: the
 * envelope is a tone alternating between 1200 Hz (a half cycle per bit) and
 * 2400 Hz (a full cycle per bit).  Bytes are 7 data bits plus an ODD parity
 * bit, sent LSB first, and a block is
 *
 *     pre-key ... SYN SYN SOH <mode><address 7><ack><label 2><block id>
 *                 [STX <text>] ETX|ETB <CRC lo> <CRC hi> DEL
 *
 * with the CRC being reflected CRC-16-CCITT (init 0) over everything from the
 * mode byte through the CRC itself, so a good block leaves a zero residue.
 *
 * The demodulator and the framing state machine follow acarsdec (Thierry
 * Leconte, LGPL-2) — the reference implementation for this link.  That matters
 * for the same reason the HFDL port did: a decoder built only against its own
 * modulator can agree with itself about a wrong wire format (that is exactly
 * how HFDL's missing REVERSE_BYTE survived a full round-trip test suite).  The
 * MSK demod here is acarsdec's algorithm rewritten to be rate-parameterised and
 * per-instance rather than file-static, so several channels can run at once at
 * whatever rate the receiver happens to be at.
 *
 * NOT ported: acarsdec's recursive multi-error corrector (up to 3 bad bytes
 * fixed via a precomputed CRC-syndrome table).  We attempt a single bad bit in
 * the one byte that failed parity, and a single bad bit in the CRC pair; deeper
 * repair trades false decodes for reach and needs on-air data to justify.
 *
 * Threading: one instance is owned by one thread (the RX audio thread in the
 * app, the main thread in the offline harness).  No locking here; the caller's
 * block callback runs on the feeding thread.
 */

#ifndef _ACARS_DEMOD_H
#define _ACARS_DEMOD_H

#include <glib.h>
#include <stdint.h>

// SOH + up to 240 text bytes + 2 CRC + DEL, with room to spare.
#define ACARS_MAX_BLOCK 260

typedef struct {
  uint8_t  bytes[ACARS_MAX_BLOCK];  // SOH .. DEL, parity bits still in place
  int      len;                     // bytes used
  gboolean crc_ok;
  int      parity_errs;             // bytes still failing parity after repair
  gboolean corrected;               // a bit was flipped to make it check out
  double   level_db;                // mean symbol level over this block
} acars_block_t;

typedef struct acars_demod acars_demod;

// A completed block (already CRC/parity-checked; a caller may still reject it).
typedef void (*acars_block_cb)(void *ctx, const acars_block_t *blk);

// I/Q front end: downmix by offset_hz, filter and decimate to the working rate,
// AM-detect, then demodulate.  offset_hz is the channel's position relative to
// the receiver centre.
acars_demod *acars_demod_create(double in_rate, double offset_hz);

// Already-demodulated AM audio (an envelope, DC removed or not) at audio_rate.
// Used by the offline harness — this is the form ACARS recordings circulate in,
// and the form acarsdec's own test vector is in.
acars_demod *acars_demod_create_audio(double audio_rate);

void   acars_demod_destroy(acars_demod *d);

// Move the channel without losing filter/PLL state (the operator nudging the
// cursor must not cost the block being received).
void   acars_demod_set_offset(acars_demod *d, double offset_hz);
double acars_demod_offset(const acars_demod *d);

// Feed raw receiver I/Q.  NOTE the buffer is (Q, I) per pair — the order WDSP
// reads it in; building the complex sample the other way conjugates it and
// mirrors the frequency axis against the panadapter.
void acars_demod_process_iq(acars_demod *d, const double *iq, int nframes,
                            acars_block_cb cb, void *ctx);

// Feed AM-demodulated audio (stride 1 for mono, 2 for one channel of a stereo
// buffer).
void acars_demod_process_audio(acars_demod *d, const double *samples, int nframes,
                               int stride, acars_block_cb cb, void *ctx);

// Mean envelope level in dB (relative to full scale) over the recent input.
double acars_demod_level_db(const acars_demod *d);

// Working (post-decimation) rate the MSK demod is running at.
double acars_demod_work_rate(const acars_demod *d);

// Headless self-test: modulates known blocks and reads them back, at several
// sample rates / channel offsets, with noise, with inverted polarity and with
// the I/Q conjugated.  Returns TRUE if all cases pass.
gboolean acars_demod_selftest(void);

// Build the on-air byte stream (pre-key, SYN SYN SOH, parity, CRC, DEL) for a
// block whose payload runs from the mode byte to ETX/ETB.  Exposed because the
// self-test and the offline harness both need a modulator, and a second
// hand-rolled copy of the framing rules is a second chance to get them wrong.
// Returns the number of bytes written to out (which must hold len + 8).
int acars_demod_build_frame(uint8_t *out, const uint8_t *payload, int len);

// Modulate a byte stream into MSK envelope samples at `rate` (amplitude ±1,
// zero mean), returning the number of samples written.  Shared by the self-test
// and the harness for the same reason as above.
int acars_demod_modulate(double *out, int max, const uint8_t *frame, int len,
                         double rate);

#endif
