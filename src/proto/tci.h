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
 * TCI (Transceiver Control Interface, Expert Electronics) server — Phase A:
 * control only (VFO / mode / PTT + handshake) over a real WebSocket transport,
 * so standard TCI loggers/clients (Log4OM, N1MM+, SkookumLogger, …) can drive
 * and follow the radio. Spectrum/audio streaming (TCI binary frames) are later
 * phases and NOT implemented here.
 *
 * Threading: a background accept thread + one thread per client do all socket
 * I/O and never touch GTK. Inbound commands are dispatched to the GTK main
 * thread via g_idle_add()/ext.c wrappers. Outbound tci_notify_*() are called
 * from the GTK/audio thread (frequency_changed/receiver_mode_changed/set_mox)
 * and best-effort broadcast to connected clients; they early-return when the
 * server is not running, so the hot path costs a single atomic read.
 */

#ifndef TCI_H
#define TCI_H

#include "receiver.h"
#include "radio.h"

// Default TCI listening port (Expert Electronics convention).
#define TCI_DEFAULT_PORT 40001

// Startup hook: remembers the RADIO and auto-starts if radio->tci_enable.
extern void tci_init(RADIO *radio);

// Start/stop the server. Idempotent; stop is non-blocking for the UI (it
// shuts down the listen + client sockets and joins the accept thread).
extern void tci_start(void);
extern void tci_stop(void);

// Human-readable one-liner for the Configure → Network status label.
extern const char *tci_status(void);

// Outbound state notifications (call from the GTK/audio thread). No-op unless
// the server is running and has at least one connected client.
extern void tci_notify_vfo(RECEIVER *rx);   // frequency_a / frequency_b changed
extern void tci_notify_mode(RECEIVER *rx);  // mode changed
extern void tci_notify_trx(gboolean mox);   // PTT / MOX changed

// Phase B: feed one block of off-air I/Q (interleaved doubles, `nsamples`
// complex pairs at `sample_rate`) to any client that sent iq_start. Called from
// the RX audio thread (receiver.c:full_rx_buffer). No-op with no IQ subscribers.
extern void tci_iq_feed(RECEIVER *rx, const double *iq, int nsamples, int sample_rate);

// Phase C — RX audio out: feed one block of demodulated audio (interleaved
// stereo doubles, `nstereo` frames at `sample_rate`) to any client that sent
// audio_start. Called from the RX audio thread (receiver.c:process_rx_buffer).
extern void tci_audio_feed(RECEIVER *rx, const double *audio, int nstereo, int sample_rate);

// Phase C — TX audio in: while a client streams TX audio and we transmit,
// tci_tx_active() is TRUE and tci_tx_next_sample() yields the next mono mic
// sample (0.0 on underrun). Consumed by transmitter.c:add_mic_sample(). Inert
// (active=FALSE, mic untouched) unless a client is driving TX audio right now.
extern gboolean tci_tx_active(void);
extern float    tci_tx_next_sample(void);

#endif
