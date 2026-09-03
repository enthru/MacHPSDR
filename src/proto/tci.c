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

#include "net_compat.h"   // must precede gtk.h: winsock2 before windows.h
#include <gtk/gtk.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "mode.h"
#include "ext.h"
#include "vfo.h"
#include "subrx.h"
#include "protocol2.h"
#ifdef SOAPYSDR
  #include "soapy_protocol.h"
#endif
#include "log.h"
#include "tci.h"
// cw_msg field split/join: pure, split out so tools/tci_offline.c can assert
// the concatenation this fork chose for an ambiguous part of the spec.
#include "tci_cw.h"
// RFC 6455 framing + the Upgrade handshake: sockets and glib only, no radio.
// Split out for the same reason, and it carries TCI_SEND_FLAGS.
#include "tci_ws.h"
#ifdef SSTV
  #include "cw_encoder.h"   // cw_tx_send_text / cw_tx_abort (CW keyer, SSTV-flag)
#endif

#define TCI_MAX_CLIENTS   8
#define TCI_MODLIST       "am,sam,dsb,lsb,usb,cw,nfm,digl,digu,wfm,drm,spec"

// TCI binary data-stream constants (TCI Protocol.pdf v2). The 64-byte header is
// eight little-endian uint32 fields followed by 32 bytes of padding, then the
// interleaved float32 sample payload.
#define TCI_SAMPLE_FLOAT32   3    // TciSampleType::FLOAT32
#define TCI_STREAM_IQ        0    // TciStreamType::IQ_STREAM
#define TCI_STREAM_RX_AUDIO  1    // TciStreamType::RX_AUDIO_STREAM
#define TCI_STREAM_TX_AUDIO  2    // TciStreamType::TX_AUDIO_STREAM
#define TCI_STREAM_TX_CHRONO 3    // TciStreamType::TX_CHRONO
#define TCI_HDR_BYTES        64
// TCI 1.9 defaults for the TX-audio handshake: the block size a chrono asks for
// at 48 kHz (AUDIO_STREAM_SAMPLES, range 100..2048) and how much audio the
// client wants queued ahead (TX_STREAM_AUDIO_BUFFERING, ms).
#define TCI_TX_SAMPLES_DEF   2048
#define TCI_TX_SAMPLES_MIN   100
#define TCI_TX_SAMPLES_MAX   2048
#define TCI_TX_BUFFER_MS_DEF 50
#define TCI_TX_BUFFER_MS_MAX 1000
#define TCI_AUDIO_RATE       48000 // RX/TX audio streamed at the native AF rate
#define TCI_TX_RING          48000 // TX-audio jitter ring: 1 s of mono float
#define TCI_TX_ACTIVE_US     250000 // TX-audio idle timeout (µs) -> release mic

// TCI carries audio levels in dB (ExpertSDR3's volume sliders are dB, with a
// floor that means silence); this application stores a linear 0..1 gain per
// receiver. -60 dB is the floor in both directions.
#define TCI_VOL_MIN_DB       (-60)

// One connected client. `send_mtx` serialises the byte stream to `fd` so control
// replies (client thread), state notifications (GTK thread) and IQ frames (audio
// thread) can never interleave partial WebSocket frames on the same socket. The
// GMutex lives in static storage, so it needs no g_mutex_init.
typedef struct {
  int    fd;        // -1 = free slot
  GMutex send_mtx;  // serialise all sends to fd
  gint   iq_mask;    // atomic: bitmask of rx indices subscribed to IQ (iq_start:<rx>)
  gint   audio_mask; // atomic: bitmask of rx indices subscribed to RX audio (audio_start:<rx>)
  gint   tx_chrono;  // atomic: this client keyed with source "tci" -> it is owed TX_CHRONO ticks
} TCI_CLIENT;

static RADIO   *g_radio = NULL;
static volatile gint server_running = 0;      // atomic: accept loop alive
static GThread *server_thread = NULL;
static int      listen_socket = -1;
static int      listening_port = TCI_DEFAULT_PORT;

static GMutex     clients_mutex;              // guards the clients[] table
static TCI_CLIENT clients[TCI_MAX_CLIENTS];
static gint       iq_sub_count = 0;           // atomic: # active (client,rx) IQ subscriptions
static gint       audio_sub_count = 0;        // atomic: # active (client,rx) audio subscriptions

// TX audio ingest: a mono float ring filled by client threads (from inbound TCI
// TX-audio binary frames) and drained by the TX thread via tci_tx_next_sample().
static GMutex   tx_ring_mutex;
static float    tx_ring[TCI_TX_RING];
static int      tx_ring_head = 0;             // next write
static int      tx_ring_tail = 0;             // next read
static gint64   tx_audio_last_us = 0;         // monotonic time of last TX-audio frame
static gint     tx_frames_in = 0;             // atomic: TX-audio frames accepted, ever

// TX_CHRONO state. A client that keys with `trx:<n>,true,tci;` is telling us it
// will supply the modulator audio -- and per TCI 1.9 it then waits for us to ask
// for it, tick by tick. `tx_chrono_clients` is how many clients are in that
// state; the two knobs are what the client may configure with
// AUDIO_STREAM_SAMPLES / TX_STREAM_AUDIO_BUFFERING.
static gint     tx_chrono_clients = 0;        // atomic: armed clients
static gint     tx_chrono_samples = TCI_TX_SAMPLES_DEF;   // atomic: Stream.length we ask for
static gint     tx_chrono_buffer_ms = TCI_TX_BUFFER_MS_DEF; // atomic
static gint     tx_chrono_at_arm = 0;         // tx_frames_in when the over started

// Defined with the rest of the TX-audio machinery at the foot of this file; used
// by the command handler and the client teardown above it.
static void tci_set_tx_chrono(TCI_CLIENT *c, gboolean on);
static void tci_tx_chrono_disarm_all(void);

static char     status_line[96] = "stopped";

// Requested RX/TX audio stream rate (Hz). 48000 = native fast path (no resample).
// A client changes it with audio_samplerate:<rate>; the stream is shared, so the
// last writer wins. RX-out is resampled 48k->this; TX-in is resampled this->48k.
static volatile gint audio_stream_rate = TCI_AUDIO_RATE;

// Union over clients of the rx indices subscribed to RX audio. Maintained by
// client_set_audio(); read as one atomic by tci_audio_subscribed().
static volatile gint audio_rx_mask = 0;

// Store a little-endian uint32 (TCI is LE; target hosts are LE, but stay explicit).
static inline void st32le(guint8 *p, guint32 v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

// ---------------------------------------------------------------------------
// per-client serialised send
// ---------------------------------------------------------------------------

// Blocking text send from a client's own thread (delivery guaranteed).
static void client_send_text(TCI_CLIENT *c, const char *s) {
  g_mutex_lock(&c->send_mtx);
  if (c->fd >= 0) tci_ws_send_blocking(c->fd, 0x1, s, strlen(s));
  g_mutex_unlock(&c->send_mtx);
}

// Best-effort non-blocking send of pre-framed on-wire bytes (a complete WS
// frame) from a foreign thread (GTK notify / audio IQ). Never blocks: if the
// send lock is contended it skips this client; on EAGAIN it drops the frame
// cleanly; a partial write would desync the stream, so the client is shut down.
// Returns TRUE when the whole frame went out. A dropped frame is a spectrum
// line nobody misses on the IQ stream and a HOLE in a decoder's audio on the
// audio one, so the caller counts the audio case (tci_audio_drop_account) --
// see there for why it is reported rather than merely tolerated.
static gboolean client_send_framed_try(TCI_CLIENT *c, const guint8 *frame, size_t len) {
  if (!g_mutex_trylock(&c->send_mtx)) return FALSE;   // busy -> drop for this client
  gboolean sent = FALSE;
  int fd = c->fd;
  if (fd >= 0) {
    ssize_t w = net_send_nowait(fd, frame, len, TCI_SEND_FLAGS);
    if (w > 0 && (size_t)w != len) {
      // Partial write mid-frame: the WebSocket stream is now unrecoverable for
      // this client — drop it (its own thread will clean up on the recv error).
      shutdown(fd, SHUT_RDWR);
    } else if (w > 0) {
      sent = TRUE;
    }
    // w<=0 (EAGAIN/would-block or error): nothing sent, frame dropped cleanly.
  }
  g_mutex_unlock(&c->send_mtx);
  return sent;
}

// Broadcast a text line (ends with ';') to every client, best-effort. Builds the
// frame once and reuses it. Caller must NOT hold clients_mutex.
static void tci_broadcast_text(const char *line) {
  size_t len = strlen(line);
  guint8 frame[160 + 10];
  if (len > sizeof(frame) - 10) return;         // control lines are short
  size_t hlen = tci_ws_write_header(frame, 0x1, len);
  memcpy(frame + hlen, line, len);
  size_t flen = hlen + len;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++)
    if (clients[i].fd >= 0) client_send_framed_try(&clients[i], frame, flen);
  g_mutex_unlock(&clients_mutex);
}

// ---------------------------------------------------------------------------
// TCI protocol — mode mapping
// ---------------------------------------------------------------------------

static const char *mode_to_tci(int m) {
  switch (m) {
    case LSB:  return "lsb";
    case USB:  return "usb";
    case DSB:  return "dsb";
    case CWL:  return "cw";
    case CWU:  return "cw";
    case FMN:  return "nfm";
    case AM:   return "am";
    case DIGU: return "digu";
    case SPEC: return "spec";
    case DIGL: return "digl";
    case SAM:  return "sam";
    case DRM:  return "drm";
    case WFM:  return "wfm";
    default:   return "usb";
  }
}

static int tci_to_mode(const char *s) {
  if (!g_ascii_strcasecmp(s, "lsb"))  return LSB;
  if (!g_ascii_strcasecmp(s, "usb"))  return USB;
  if (!g_ascii_strcasecmp(s, "dsb"))  return DSB;
  if (!g_ascii_strcasecmp(s, "cw"))   return CWU;
  if (!g_ascii_strcasecmp(s, "cwu"))  return CWU;
  if (!g_ascii_strcasecmp(s, "cwl"))  return CWL;
  if (!g_ascii_strcasecmp(s, "nfm"))  return FMN;
  if (!g_ascii_strcasecmp(s, "fm"))   return FMN;
  if (!g_ascii_strcasecmp(s, "am"))   return AM;
  if (!g_ascii_strcasecmp(s, "digu")) return DIGU;
  if (!g_ascii_strcasecmp(s, "digl")) return DIGL;
  if (!g_ascii_strcasecmp(s, "spec")) return SPEC;
  if (!g_ascii_strcasecmp(s, "sam"))  return SAM;
  if (!g_ascii_strcasecmp(s, "drm"))  return DRM;
  if (!g_ascii_strcasecmp(s, "wfm"))  return WFM;
  return -1;
}

// ---------------------------------------------------------------------------
// multi-RX index helpers — a TCI "trx" is a *visible* receiver (show_rx). Hidden
// receivers (diversity / PureSignal feedback) are not exposed. Indices are the
// receiver's position among the visible ones (0-based), stable in array order.
// ---------------------------------------------------------------------------

static int tci_trx_count(void) {
  if (g_radio == NULL) return 1;
  int n = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++)
    if (g_radio->receiver[i] != NULL && g_radio->receiver[i]->show_rx) n++;
  return n > 0 ? n : 1;
}

// TCI index of a receiver, or -1 if it is hidden / not found.
static int tci_rx_index(RECEIVER *rx) {
  if (g_radio == NULL || rx == NULL) return -1;
  int idx = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    RECEIVER *r = g_radio->receiver[i];
    if (r == NULL || !r->show_rx) continue;
    if (r == rx) return idx;
    idx++;
  }
  return -1;
}

// TCI index of the receiver the (single) transmitter is attached to. This app
// has one transmitter, but it is not necessarily bound to receiver 0, and TCI
// addresses PTT per trx — so "which trx transmits" is a real question and the
// answer used to be hard-coded to 0. Falls back to 0 when the transmitter's
// receiver is hidden or absent, which keeps a plain single-RX client working.
static int tci_tx_index(void) {
  if (g_radio == NULL || g_radio->transmitter == NULL) return 0;
  int idx = tci_rx_index(g_radio->transmitter->rx);
  return idx >= 0 ? idx : 0;
}

// The visible receiver at TCI index `idx`, or NULL.
static RECEIVER *tci_rx_at(int idx) {
  if (g_radio == NULL || idx < 0) return NULL;
  int n = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    RECEIVER *r = g_radio->receiver[i];
    if (r == NULL || !r->show_rx) continue;
    if (n == idx) return r;
    n++;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// inbound command dispatch to the GTK main thread
// ---------------------------------------------------------------------------

static void dispatch_set_frequency(RECEIVER *rx, long long hz) {
  if (rx == NULL) return;
  RX_FREQUENCY *f = g_new0(RX_FREQUENCY, 1);
  f->rx = rx;
  f->frequency = hz;
  g_idle_add(ext_set_frequency_a, f);
}

static void dispatch_set_frequency_b(RECEIVER *rx, long long hz) {
  if (rx == NULL) return;
  RX_FREQUENCY *f = g_new0(RX_FREQUENCY, 1);
  f->rx = rx;
  f->frequency = hz;
  g_idle_add(ext_set_frequency_b, f);
}

static void dispatch_set_mode(RECEIVER *rx, int mode) {
  if (rx == NULL) return;
  MODE *m = g_new0(MODE, 1);
  m->rx = rx;
  m->mode_a = mode;   // ext_set_mode assigns rx->mode_a = m->mode_a on the GTK thread
  g_idle_add(ext_set_mode, m);
}

static void dispatch_set_mox(gboolean state) {
  if (g_radio == NULL || !g_radio->can_transmit) return;
  MOX_STATE *m = g_new0(MOX_STATE, 1);
  m->radio = g_radio;
  m->state = state;
  g_idle_add(ext_set_mox, m);
}

// TCI booleans are "true"/"false" (also accept "1"/"0").
static gboolean tci_argbool(const char *s) {
  return (!g_ascii_strcasecmp(s, "true") || !strcmp(s, "1"));
}

// TCI dB level -> this app's linear 0..1 audio gain, and back. The floor is
// TCI_VOL_MIN_DB, which maps to a true zero (mute), not to 0.001.
static gdouble tci_db_to_gain(gdouble db) {
  if (db <= (gdouble)TCI_VOL_MIN_DB) return 0.0;
  if (db >= 0.0) return 1.0;
  return pow(10.0, db / 20.0);
}

// Reported as a whole dB: the reply is built with %d so it can never pick up a
// comma decimal separator from the operator's locale (which would make the line
// unparseable to the client — the same trap hfdl_msg.c hits with coordinates).
static int tci_gain_to_db(gdouble gain) {
  if (gain <= 0.0) return TCI_VOL_MIN_DB;
  if (gain >= 1.0) return 0;
  long db = lround(20.0 * log10(gain));
  if (db < TCI_VOL_MIN_DB) db = TCI_VOL_MIN_DB;
  if (db > 0) db = 0;
  return (int)db;
}

// Parse a numeric argument locale-independently: the client always writes '.'
// but atof()/strtod() follow LC_NUMERIC, so "0.5" reads as 0 under a
// comma-decimal locale.
static gdouble tci_argdouble(const char *s) {
  return (s != NULL) ? g_ascii_strtod(s, NULL) : 0.0;
}

// Deferred RIT/XIT/split/IF state changes: mutated on the GTK main thread (like
// ext.c's wrappers) since they touch RECEIVER/TRANSMITTER + WDSP + the VFO.
typedef enum {
  TCI_OP_RIT_ENABLE, TCI_OP_RIT_OFFSET,
  TCI_OP_XIT_ENABLE, TCI_OP_XIT_OFFSET,
  TCI_OP_SPLIT,      TCI_OP_IF,
  TCI_OP_MUTE,       TCI_OP_VOLUME,       // per receiver (rx_mute / rx_volume)
  TCI_OP_LOCK,       TCI_OP_SUBRX,        // lock / rx_channel_enable sub-rx
  // Radio-wide: these act on RADIO/TRANSMITTER, not on one receiver.
  TCI_OP_MUTE_ALL,   TCI_OP_VOLUME_ALL,
  TCI_OP_TUNE,       TCI_OP_DRIVE,        TCI_OP_TUNE_DRIVE
} TCI_OP;

typedef struct { TCI_OP op; int rx_index; gboolean b; gint64 v; gdouble d; } TCI_STATE_CMD;

// TRUE for the operations that do NOT belong to one receiver. They must run
// outside rx->mutex: set_tune() -> rxtx() walks every receiver's WDSP channel,
// and the global mute/volume take each receiver's own lock in turn.
static gboolean tci_op_is_global(TCI_OP op) {
  return op == TCI_OP_MUTE_ALL || op == TCI_OP_VOLUME_ALL ||
         op == TCI_OP_TUNE     || op == TCI_OP_DRIVE || op == TCI_OP_TUNE_DRIVE;
}

// Radio-wide part of the applier (GTK main thread, no rx->mutex held).
static void tci_apply_global(RADIO *r, TCI_STATE_CMD *c) {
  switch (c->op) {
    case TCI_OP_MUTE_ALL:
    case TCI_OP_VOLUME_ALL:
      // There is no master AF stage in this radio — the only audio gain is per
      // receiver — so a global TCI mute/volume is applied to every visible
      // receiver, one rx->mutex at a time (never two held at once).
      for (int i = 0; i < MAX_RECEIVERS; i++) {
        RECEIVER *t = r->receiver[i];
        if (t == NULL || !t->show_rx) continue;
        g_mutex_lock(&t->mutex);
        if (c->op == TCI_OP_MUTE_ALL) t->mute = c->b;
        else                          t->volume = c->d;
        receiver_set_volume(t);
        update_vfo(t);
        g_mutex_unlock(&t->mutex);
      }
      break;
    case TCI_OP_TUNE:
      // set_tune() drops MOX, drives the WDSP tone generator, switches every
      // channel's RX/TX state and re-syncs the TUNE button, so it is exactly
      // the GTK-thread call the UI's own toggle makes.
      if (r->can_transmit && r->transmitter != NULL) set_tune(r, c->b);
      break;
    case TCI_OP_DRIVE:
    case TCI_OP_TUNE_DRIVE:
      if (r->transmitter == NULL) break;
      {
        gdouble v = c->d;
        if (v <   0.0) v =   0.0;
        if (v > 100.0) v = 100.0;
        if (c->op == TCI_OP_TUNE_DRIVE) {
          // Tune power is read live off the transmitter when tune is keyed
          // (transmitter_dialog.c's slider does no more than this).
          r->transmitter->tune_percent = v;
        } else {
          r->transmitter->drive = v;
          // The same three steps drive_level.c and actions.c take: P2 pushes the
          // level in its high-priority frame, SoapySDR maps drive onto the
          // hardware TX gain, and the on-screen slider is redrawn.
          if (r->discovered != NULL && r->discovered->protocol == PROTOCOL_2)
            protocol2_high_priority();
#ifdef SOAPYSDR
          if (r->discovered != NULL && r->discovered->protocol == PROTOCOL_SOAPYSDR)
            soapy_protocol_set_tx_drive(v);
#endif
          if (r->drive_level != NULL) gtk_widget_queue_draw(r->drive_level);
        }
      }
      break;
    default:
      break;
  }
}

static gboolean tci_apply_state_idle(gpointer data) {
  TCI_STATE_CMD *c = (TCI_STATE_CMD *)data;
  RADIO *r = g_radio;
  if (r != NULL && tci_op_is_global(c->op)) {
    tci_apply_global(r, c);
    g_free(c);
    return G_SOURCE_REMOVE;
  }
  // Resolve the target receiver on the GTK thread (race-free — delete_receiver
  // also runs here); fall back to the active RX if the index is gone.
  RECEIVER *rx = (r != NULL) ? tci_rx_at(c->rx_index) : NULL;
  if (rx == NULL && r != NULL) rx = r->active_receiver;
  if (r != NULL && rx != NULL) {
    g_mutex_lock(&rx->mutex);
    switch (c->op) {
      case TCI_OP_RIT_ENABLE:
        rx->rit_enabled = c->b;
        frequency_changed(rx);
        break;
      case TCI_OP_RIT_OFFSET:
        rx->rit = c->v;
        frequency_changed(rx);
        break;
      case TCI_OP_XIT_ENABLE:
        if (r->transmitter != NULL) r->transmitter->xit_enabled = c->b;
        break;
      case TCI_OP_XIT_OFFSET:
        if (r->transmitter != NULL) r->transmitter->xit = c->v;
        break;
      case TCI_OP_SPLIT:
        rx->split = c->b ? SPLIT_ON : SPLIT_OFF;
        if (r->transmitter != NULL)
          transmitter_set_mode(r->transmitter, c->b ? rx->mode_b : rx->mode_a);
        break;
      case TCI_OP_IF:
        // Place the demod point c->v Hz off the panorama centre (CTUN offset).
        // A non-zero offset with plain tuning implies CTUN, so enable it.
        if (c->v != 0 && !rx->ctun && !rx->freetune) rx->ctun = TRUE;
        rx->ctun_frequency = rx->frequency_a + c->v;
        frequency_changed(rx);
        break;
      case TCI_OP_MUTE:
        rx->mute = c->b;
        receiver_set_volume(rx);      // mute is the panel gain going to 0
        break;
      case TCI_OP_VOLUME:
        rx->volume = c->d;
        receiver_set_volume(rx);
        break;
      case TCI_OP_LOCK:
        rx->locked = c->b;            // update_vfo below re-syncs the LOCK button
        break;
      case TCI_OP_SUBRX:
        // rx_channel_enable:<rx>,1 — this radio's second demod channel per
        // receiver (the SUBRX button). Creating/destroying it under rx->mutex is
        // stricter than the UI's own path: the audio thread holds that same lock
        // across fexchange0() and the sub-channel's processing, so the swap
        // cannot land mid-block. Order matches vfo.c's subrx_b_cb.
        if (c->b && !rx->subrx_enable) {
          create_subrx(rx);
          rx->subrx_enable = TRUE;
        } else if (!c->b && rx->subrx_enable) {
          rx->subrx_enable = FALSE;
          destroy_subrx(rx);
          rx->subrx = NULL;
        }
        break;
      default:                        // radio-wide ops never reach here
        break;
    }
    update_vfo(rx);
    g_mutex_unlock(&rx->mutex);
  }
  g_free(c);
  return G_SOURCE_REMOVE;
}

static void tci_dispatch_state(int rx_index, TCI_OP op, gboolean b, gint64 v, gdouble d) {
  if (g_radio == NULL) return;
  TCI_STATE_CMD *c = g_new0(TCI_STATE_CMD, 1);
  c->op = op; c->rx_index = rx_index; c->b = b; c->v = v; c->d = d;
  g_idle_add(tci_apply_state_idle, c);
}

// CW keyer integration (only when the CW encoder is compiled in — SSTV flag).
// cw_tx_send_text() keys MOX and starts a GLib timeout, so it must run on the
// GTK main thread; dispatch a strdup'd copy there.
#ifdef SSTV
static gboolean tci_cw_send_idle(gpointer data) {
  char *text = (char *)data;
  cw_tx_send_text(text);
  g_free(text);
  return G_SOURCE_REMOVE;
}
static gboolean tci_cw_stop_idle(gpointer data) {
  (void)data;
  cw_tx_abort();
  return G_SOURCE_REMOVE;
}
#endif

// Queue a CW message for transmission (no-op if the CW encoder isn't built).
static void tci_dispatch_cw_send(const char *text) {
#ifdef SSTV
  g_idle_add(tci_cw_send_idle, g_strdup(text));
#else
  (void)text;
#endif
}
static void tci_dispatch_cw_stop(void) {
#ifdef SSTV
  g_idle_add(tci_cw_stop_idle, NULL);
#endif
}

// Set/clear this client's IQ subscription for one rx index (or all rx when
// rx_index < 0), keeping the global gate count = total set bits across clients.
// The index arrives off the wire, so it is bounded here rather than at the call
// sites: `1 << rx_index` past the width of the mask is undefined, and a receiver
// that cannot exist has nothing to subscribe to or unsubscribe from.
static void client_set_iq(TCI_CLIENT *c, int rx_index, gboolean on) {
  if (rx_index >= MAX_RECEIVERS) return;
  gint old = g_atomic_int_get(&c->iq_mask);
  gint bits = (rx_index < 0) ? ~0 : (1 << rx_index);
  gint nw = on ? (old | bits) : (old & ~bits);
  if (nw == old) return;
  g_atomic_int_set(&c->iq_mask, nw);
  g_atomic_int_add(&iq_sub_count, __builtin_popcount((guint)(nw ^ old)) * (on ? 1 : -1));
}

// Same for the RX audio subscription, bounded for the same reason.
// Re-push the WDSP panel gain for a receiver whose audio-subscription state just
// changed. Subscribing forces that channel to unity (receiver_panel_gain), and
// nothing else re-pushes the gain, so without this the change would not take
// until the operator next touched the volume. GTK thread, hence the idle.
static gboolean tci_repush_gain_idle(gpointer data) {
  RECEIVER *rx = (RECEIVER *)data;
  if (!receiver_is_live(rx) || rx->channel < 0) return G_SOURCE_REMOVE;
  // Panel gain only. Subscribing does NOT touch NR/ANF/SNB or the squelch: they
  // are the operator's settings on the operator's receiver, and a client is not
  // entitled to change what comes out of their speaker.
  receiver_set_volume(rx);
  return G_SOURCE_REMOVE;
}

static void client_set_audio(TCI_CLIENT *c, int rx_index, gboolean on) {
  if (rx_index >= MAX_RECEIVERS) return;
  gint old = g_atomic_int_get(&c->audio_mask);
  gint bits = (rx_index < 0) ? ~0 : (1 << rx_index);
  gint nw = on ? (old | bits) : (old & ~bits);
  if (nw == old) return;
  g_atomic_int_set(&c->audio_mask, nw);
  g_atomic_int_add(&audio_sub_count, __builtin_popcount((guint)(nw ^ old)) * (on ? 1 : -1));

  // Recompute the union of every client's subscriptions, WITHOUT taking
  // clients_mutex: clients_remove() calls this function while holding it, and
  // GMutex is not recursive -- taking it again deadlocks that thread with the
  // lock held, which then stops the accept thread, every broadcast and the GTK
  // thread behind it. (Measured the hard way: one client disconnecting froze
  // the whole application, so the next connection could not be answered.)
  //
  // No lock is needed. Every slot's audio_mask is an atomic, and a departing
  // client's mask is cleared here before its fd is closed, so a slot that is
  // being torn down contributes 0 whether or not we see the fd change.
  gint all = 0;
  for (int i = 0; i < TCI_MAX_CLIENTS; i++)
    all |= g_atomic_int_get(&clients[i].audio_mask);
  gint was = g_atomic_int_get(&audio_rx_mask);
  g_atomic_int_set(&audio_rx_mask, all);

  for (int idx = 0; idx < MAX_RECEIVERS; idx++) {
    if (((was ^ all) & (1 << idx)) == 0) continue;
    RECEIVER *rx = tci_rx_at(idx);
    if (rx != NULL) g_idle_add(tci_repush_gain_idle, rx);
  }
}

// TRUE when some client is streaming this receiver's audio. The stream is not a
// speaker, so this makes the receiver hand it a full-level signal: see
// receiver_panel_gain().
gboolean tci_audio_subscribed(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return FALSE;
  int idx = tci_rx_index(rx);
  if (idx < 0 || idx >= MAX_RECEIVERS) return FALSE;
  return (g_atomic_int_get(&audio_rx_mask) & (1 << idx)) != 0;
}

// --- arbitrary-ratio audio resampler (windowed-sinc) ------------------------
// Used ONLY when a client asks for an RX/TX audio rate other than 48 kHz; the
// 48 kHz path never touches this (zero cost, zero risk to the tested fast path).
// Each channel keeps its FIR history + fractional read pointer across blocks so
// there is no click at block boundaries. Quality is fine for monitoring audio.
#define TCI_RS_HALF 16                       // sinc half-width -> 32 taps
#define TCI_RS_HIST (2 * TCI_RS_HALF)
// Largest block tci_resamp_run() may be handed in one call. Its scratch window
// is a STACK allocation of (TCI_RS_HIST + n_in) floats, so n_in must never come
// straight off the wire: a client's 256 KB TX-audio frame is 65520 samples,
// which is a quarter-megabyte array on a thread whose whole stack is 512 KB on
// macOS. The RX side is always the DSP block (output_samples <= buffer_size,
// 2048 at the widest), and the TX ingest chunks itself down to this.
#define TCI_RS_MAX_IN 4096

typedef struct {
  int    rin, rout;
  double step;                               // rin/rout (input samples per output)
  double rp;                                 // read pointer into [hist | new]
  float  hist[TCI_RS_HIST];
} TCI_RESAMP;

static void tci_resamp_reset(TCI_RESAMP *s, int rin, int rout) {
  s->rin = rin; s->rout = rout;
  s->step = (double)rin / (double)rout;
  s->rp = (double)TCI_RS_HIST;               // history is zero-padded at start
  memset(s->hist, 0, sizeof(s->hist));
}

static inline double tci_sinc(double x) {
  if (x < 1e-9 && x > -1e-9) return 1.0;
  double p = G_PI * x;
  return sin(p) / p;
}

// Resample one channel (rin->rout). in[n_in] -> out[], returns sample count.
// out_cap must be >= ceil(n_in*rout/rin)+2. n_in must be <= TCI_RS_MAX_IN — see
// the note there: the scratch window below lives on the caller's stack.
static int tci_resamp_run(TCI_RESAMP *s, const float *in, int n_in, float *out, int out_cap) {
  if (n_in <= 0 || n_in > TCI_RS_MAX_IN) return 0;
  int W = TCI_RS_HIST + n_in;
  float *w = g_newa(float, W);
  memcpy(w, s->hist, TCI_RS_HIST * sizeof(float));
  memcpy(w + TCI_RS_HIST, in, (size_t)n_in * sizeof(float));

  double fc   = 0.5 * ((s->rout < s->rin) ? (double)s->rout / (double)s->rin : 1.0);
  double gain = 2.0 * fc;
  double limit = (double)(W - 1 - TCI_RS_HALF);   // need HALF future samples

  int n_out = 0;
  double rp = s->rp;
  while (rp <= limit && n_out < out_cap) {
    int i0 = (int)floor(rp);
    double acc = 0.0;
    for (int k = i0 - TCI_RS_HALF + 1; k <= i0 + TCI_RS_HALF; k++) {
      double x   = rp - k;
      double wnd = 0.5 + 0.5 * cos(G_PI * x / (double)TCI_RS_HALF);   // Hann
      double h   = gain * tci_sinc(2.0 * fc * x) * wnd;
      float  smp = (k >= 0 && k < W) ? w[k] : 0.0f;
      acc += smp * h;
    }
    out[n_out++] = (float)acc;
    rp += s->step;
  }

  memcpy(s->hist, w + (W - TCI_RS_HIST), TCI_RS_HIST * sizeof(float));
  s->rp = rp - n_in;                         // reindex after dropping n_in front samples
  if (s->rp < 0.0) s->rp = 0.0;
  return n_out;
}

// Per-rx stereo RX-audio resamplers (touched only by that rx's audio thread) and
// one mono TX-audio resampler (guarded by tx_ring_mutex).
static TCI_RESAMP rs_rx[MAX_RECEIVERS][2];
static TCI_RESAMP rs_tx;

// Append n mono samples to the TX ring. Caller holds tx_ring_mutex.
// Single-consumer (audio thread) reads head/tail lock-free, so publish the slot
// write BEFORE advancing head (g_atomic_int_set is a full barrier). Producers
// are serialised by tx_ring_mutex, so head increments don't race.
static void tx_ring_push_locked(const float *p, int n) {
  int head = tx_ring_head;
  for (int i = 0; i < n; i++) {
    int next = (head + 1) % TCI_TX_RING;
    if (next == g_atomic_int_get(&tx_ring_tail)) break;   // ring full: drop the rest
    tx_ring[head] = p[i];
    g_atomic_int_set(&tx_ring_head, next);
    head = next;
  }
}

// Ingest one inbound TCI binary frame from a client. Only TX-audio (type 2,
// float32) is consumed: its left channel is pushed into the mono TX ring for
// add_mic_sample() to drain. Everything else is ignored.
static void tci_ingest_binary(const guint8 *buf, size_t len) {
  if (len < TCI_HDR_BYTES + 4) return;
  guint32 srate   = ((guint32)buf[ 4]) | ((guint32)buf[ 5]<<8) | ((guint32)buf[ 6]<<16) | ((guint32)buf[ 7]<<24);
  guint32 fmt     = ((guint32)buf[ 8]) | ((guint32)buf[ 9]<<8) | ((guint32)buf[10]<<16) | ((guint32)buf[11]<<24);
  guint32 dtype   = ((guint32)buf[24]) | ((guint32)buf[25]<<8) | ((guint32)buf[26]<<16) | ((guint32)buf[27]<<24);
  guint32 channels= ((guint32)buf[28]) | ((guint32)buf[29]<<8) | ((guint32)buf[30]<<16) | ((guint32)buf[31]<<24);
  // Announce what arrives BEFORE any of the refusals below, and rate-limited
  // because it is otherwise one line per audio frame.  The whole point is to
  // separate "the client sends nothing" from "the client sends something we
  // throw away", which are indistinguishable from the operator's chair -- no
  // RF, an empty TX panadapter, and nothing anywhere saying why.  The first
  // report of a run always prints, so a single stray frame is never missed.
  // The timestamps are plain statics shared by every client thread; the worst a
  // race costs is a duplicated log line.
  static gint64 last_seen_log = 0;
  static gint64 last_fmt_gripe = 0;
  static gint64 last_accept_log = 0;
  const gint64 now_us = g_get_monotonic_time();
  if (now_us - last_seen_log >= 5000000) {
    last_seen_log = now_us;
    log_info("tci: binary frame in: stream type %u, format %u, %u Hz, %u channel(s), %zu bytes"
             " (TX audio is type %d, format %d)\n",
             (unsigned)dtype, (unsigned)fmt, (unsigned)srate, (unsigned)channels,
             len - TCI_HDR_BYTES, TCI_STREAM_TX_AUDIO, TCI_SAMPLE_FLOAT32);
  }
  if (dtype != TCI_STREAM_TX_AUDIO) return;
  // TCI 1.9's SampleType enum stops at FLOAT32 = 3, but at least one widely
  // copied client library writes 4 in this field for float32 payloads.  Since 4
  // means nothing else in the enum, take it as float32 rather than throw the
  // audio away over a spelling: what arrives is measured in bytes below, not
  // trusted from the header.
  if (fmt == 4) fmt = TCI_SAMPLE_FLOAT32;
  if (fmt != TCI_SAMPLE_FLOAT32) {                    // only float32 supported
    if (now_us - last_fmt_gripe >= 5000000) {
      last_fmt_gripe = now_us;
      log_error("tci: TX audio dropped -- sample format %u, this build accepts float32 (%d) only\n",
                (unsigned)fmt, TCI_SAMPLE_FLOAT32);
    }
    return;
  }
  if (channels < 1) channels = 1;
  if (now_us - last_accept_log >= 5000000) {
    last_accept_log = now_us;
    log_info("tci: TX audio in: %u Hz, %u channel(s), %zu bytes\n",
             (unsigned)srate, (unsigned)channels, len - TCI_HDR_BYTES);
  }

  const float *s = (const float *)(buf + TCI_HDR_BYTES);
  size_t nfloats = (len - TCI_HDR_BYTES) / sizeof(float);
  size_t stride  = channels;                          // take the left channel
  size_t nmono   = nfloats / stride;
  if (nmono == 0) return;

  // Extract the left (mono) channel, then resample srate -> 48 kHz if needed.
  // rs_tx + the ring are both guarded by tx_ring_mutex (client threads may race).
  float *mono = g_new(float, nmono);
  for (size_t i = 0, j = 0; j < nmono; i += stride, j++) mono[j] = s[i];

  // Clamp the client-supplied sample rate: an unvalidated tiny srate makes the
  // resampler's output capacity (nmono*48000/srate) overflow int and abort the
  // whole app in g_new(). srate==0 falls back to the native 48 kHz fast path.
  int want_rate = (srate > 0) ? (int)srate : TCI_AUDIO_RATE;
  if(want_rate < 8000)   want_rate = 8000;
  if(want_rate > 192000) want_rate = 192000;

  g_mutex_lock(&tx_ring_mutex);
  if (want_rate == TCI_AUDIO_RATE) {
    tx_ring_push_locked(mono, (int)nmono);
  } else {
    if (rs_tx.rin != want_rate || rs_tx.rout != TCI_AUDIO_RATE)
      tci_resamp_reset(&rs_tx, want_rate, TCI_AUDIO_RATE);
    // In TCI_RS_MAX_IN-sample chunks: nmono comes off the wire and the
    // resampler's scratch window is on this thread's stack. The filter history
    // carries across calls, so chunking is inaudible — it is the same stream.
    int cap = (int)((gint64)TCI_RS_MAX_IN * TCI_AUDIO_RATE / want_rate) + 4;
    float *res = g_new(float, cap);
    for (size_t off = 0; off < nmono; off += TCI_RS_MAX_IN) {
      size_t left = nmono - off;
      int n  = (int)(left < TCI_RS_MAX_IN ? left : (size_t)TCI_RS_MAX_IN);
      int nr = tci_resamp_run(&rs_tx, mono + off, n, res, cap);
      tx_ring_push_locked(res, nr);
    }
    g_free(res);
  }
  g_mutex_unlock(&tx_ring_mutex);

  g_free(mono);
  tx_audio_last_us = g_get_monotonic_time();
  g_atomic_int_inc(&tx_frames_in);
}

// Answer a command whose value does not exist on this radio right now (no
// transmitter, no receiver) the way the unwired commands are answered: echo it
// back. A request/response client must never be left waiting, and an echo says
// "heard, nothing to report" without inventing a number.
static void tci_ack_echo(TCI_CLIENT *c, const char *token) {
  char r[160];
  g_snprintf(r, sizeof(r), "%s;", token);
  client_send_text(c, r);
}

// Handle one ';'-stripped command token from client `c`.
static void tci_handle_command(TCI_CLIENT *c, const char *token) {
  // Every command a client sends, at DEBUG.  Nothing logged what came IN, so a
  // client that goes quiet -- MSHV connecting, asking its questions and then
  // never streaming TX audio -- left nothing to read: you could see what the app
  // answered only by inferring it from the source.  The guard is in the macro,
  // so this costs one int compare per command.
  log_debug("tci: <- %s;\n", token);
  char name[32];
  const char *colon = strchr(token, ':');
  size_t nlen = colon ? (size_t)(colon - token) : strlen(token);
  if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
  for (size_t i = 0; i < nlen; i++) name[i] = g_ascii_tolower(token[i]);
  name[nlen] = '\0';

  char *args[4] = {NULL,NULL,NULL,NULL};
  int nargs = 0;
  char argbuf[128];
  if (colon) {
    g_strlcpy(argbuf, colon + 1, sizeof(argbuf));
    char *save = NULL;
    for (char *t = strtok_r(argbuf, ",", &save); t && nargs < 4; t = strtok_r(NULL, ",", &save))
      args[nargs++] = g_strstrip(t);
  }

  // Most commands carry a 0-based rx (trx) index in their first field. Resolve
  // the addressed receiver. Commands whose first field is NOT an rx index
  // (cw_macros_speed = wpm) simply ignore these.
  //   req_index : the index EXACTLY as the client wrote it. It must NOT be
  //               clamped into range: every "is this the addressed rx?" test
  //               below compares against it, so folding an impossible index
  //               onto 0 makes `trx:99,true` look like the transmitting trx and
  //               key the radio — measured, the bug this used to have. Nothing
  //               indexes an array with it: tci_rx_at() answers NULL for any
  //               out-of-range value and the subscription helpers reject one.
  //   addr      : the exact addressed receiver, or NULL if it does not exist.
  //   rx_index/trx : for control commands, which fall back to the active RX
  //               (labelled index 0) when the addressed rx is absent.
  int req_index = (nargs >= 1 && args[0] != NULL) ? atoi(args[0]) : 0;
  RECEIVER *addr = tci_rx_at(req_index);
  int rx_index = (addr != NULL) ? req_index : 0;
  RECEIVER *trx = (addr != NULL) ? addr
                                 : ((g_radio != NULL) ? g_radio->active_receiver : NULL);

  if (!strcmp(name, "vfo") || !strcmp(name, "dds")) {
    if (nargs >= 3) {
      int ch = atoi(args[1]);
      long long hz = g_ascii_strtoll(args[2], NULL, 10);
      if (ch == 0)      dispatch_set_frequency(trx, hz);
      else if (ch == 1) dispatch_set_frequency_b(trx, hz);
    } else if (trx != NULL) {
      int ch = (nargs >= 2) ? atoi(args[1]) : 0;
      // VFO A is reported as the frequency actually being demodulated, which is
      // the cursor under ctun/freetune and not the span centre -- the same value
      // the set above moves. See receiver_tuned_frequency().
      long long f = (ch == 1) ? (long long)trx->frequency_b
                              : receiver_tuned_frequency(trx);
      char r[64];
      g_snprintf(r, sizeof(r), "vfo:%d,%d,%lld;", rx_index, ch, f);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "modulation") || !strcmp(name, "trx_mode")) {
    if (nargs >= 2) {
      int m = tci_to_mode(args[1]);
      if (m >= 0) dispatch_set_mode(trx, m);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "modulation:%d,%s;", rx_index, mode_to_tci(trx->mode_a));
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "trx")) {
    // There is one transmitter and it belongs to one trx, so PTT addressed to
    // any other trx is a no-op — the rule iq_start/audio_start already follow
    // for an absent rx. The alternative is that `trx:1,true` transmits on rx 0
    // without saying so.
    //
    // Matched on the RAW req_index, not the shared rx_index: that one falls
    // back to 0 for a receiver that does not exist, which would make every
    // bogus index look like the transmitting one and key the radio anyway.
    // (Caught by exactly this case in the faker probe.) A single-RX client is
    // unaffected: it addresses trx 0, which is the transmitting one.
    gboolean is_tx = (addr != NULL) && (req_index == tci_tx_index());
    if (nargs >= 2) {
      gboolean on = (!g_ascii_strcasecmp(args[1], "true") || !strcmp(args[1], "1"));
      if (is_tx) {
        // Third field is the signal source (TCI 2.0's updated TRX command).
        // "tci" means the CLIENT supplies the modulator audio -- and that is
        // what arms the TX_CHRONO ticks it then waits for.  Anything else, and
        // an absent field (= the radio's own microphone), leaves the mic path
        // alone: keying over the network must not take the microphone away.
        gboolean src_tci = (nargs >= 3) && args[2] != NULL &&
                           !g_ascii_strcasecmp(args[2], "tci");
        tci_set_tx_chrono(c, on && src_tci);
        dispatch_set_mox(on);
      }
    } else if (g_radio != NULL) {
      char r[32];
      g_snprintf(r, sizeof(r), "trx:%d,%s;", req_index,
                 (is_tx && g_radio->mox) ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "iq_start")) {
    if (addr != NULL) {
      client_set_iq(c, req_index, TRUE);   // subscribe only to an existing rx
      char r[32];
      g_snprintf(r, sizeof(r), "iq_start:%d;", req_index);
      client_send_text(c, r);              // the echo IS the answer — see audio_start
    }
    log_info("tci: iq_start rx=%d%s (fd=%d, subs=%d)\n", req_index,
             addr ? "" : " (no such rx)", c->fd, g_atomic_int_get(&iq_sub_count));
  } else if (!strcmp(name, "iq_stop")) {
    // No argument means "every stream"; an argument names one rx, and a negative
    // one names none — it must not be read as the internal all-rx sentinel.
    client_set_iq(c, (nargs >= 1) ? (req_index >= 0 ? req_index : MAX_RECEIVERS) : -1, FALSE);
    if (nargs >= 1 && req_index >= 0) {
      char r[32];
      g_snprintf(r, sizeof(r), "iq_stop:%d;", req_index);
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);              // the "every stream" form has no index to name
    }
    log_info("tci: iq_stop rx=%d (fd=%d, subs=%d)\n", req_index, c->fd, g_atomic_int_get(&iq_sub_count));
  } else if (!strcmp(name, "iq_samplerate") || !strcmp(name, "iq_sample_rate")) {
    // We stream at the receiver's native DDC rate; report it (a requested rate
    // is acknowledged but not honoured — the radio's rate is fixed here).
    int rate = (trx != NULL) ? trx->sample_rate : 48000;
    char r[48];
    g_snprintf(r, sizeof(r), "iq_samplerate:%d;", rate);
    client_send_text(c, r);
  } else if (!strcmp(name, "audio_start")) {
    // A stream command is CONFIRMED by the server echoing it back, and a client
    // that gates on the confirmation has no other way to learn the subscription
    // took: subscribing in silence is indistinguishable from ignoring it.
    // Streaming the audio is not the answer either — the client is still in its
    // start-up sequence and not yet reading the stream.  JTDX sends
    // `audio_start:<rx>;`, waits 500 ms for `audio_start:<rx>;` to come back and
    // otherwise reports "TCI Audio could not be switched on" and drops the
    // socket, which is what the operator's log showed on a loop: connect,
    // audio_start, disconnect.  (JTDX's TCITransceiver.cpp sets stream_audio_
    // only in its Cmd_AudioStart case, and compares the echoed index against its
    // own rig number as a STRING — so the reply must name the index the client
    // wrote, which is req_index and never the fallback rx_index.)
    //
    // An absent rx is left UNANSWERED on purpose: acking it would promise a
    // stream that is never going to arrive, and the client's own timeout is
    // then the truth.  Same rule as the subscription itself — never a silent
    // fallback to rx 0.
    if (addr != NULL) {
      client_set_audio(c, req_index, TRUE);
      char r[32];
      g_snprintf(r, sizeof(r), "audio_start:%d;", req_index);
      client_send_text(c, r);
    }
    log_info("tci: audio_start rx=%d%s (fd=%d, subs=%d)\n", req_index,
             addr ? "" : " (no such rx)", c->fd, g_atomic_int_get(&audio_sub_count));
  } else if (!strcmp(name, "audio_stop")) {
    client_set_audio(c, (nargs >= 1) ? (req_index >= 0 ? req_index : MAX_RECEIVERS) : -1, FALSE);
    if (nargs >= 1 && req_index >= 0) {
      char r[32];
      g_snprintf(r, sizeof(r), "audio_stop:%d;", req_index);
      client_send_text(c, r);              // a stop is confirmed the same way
    } else {
      tci_ack_echo(c, token);              // the "every stream" form has no index to name
    }
    log_info("tci: audio_stop rx=%d (fd=%d, subs=%d)\n", req_index, c->fd, g_atomic_int_get(&audio_sub_count));
  } else if (!strcmp(name, "audio_samplerate") || !strcmp(name, "audio_sample_rate")) {
    // RX/TX audio stream rate. A requested rate is honoured via a resampler
    // (the native AF path is 48 kHz); 48000 is the zero-cost fast path.
    if (nargs >= 1 && args[0] != NULL && atoi(args[0]) > 0) {
      int req = atoi(args[0]);
      if (req < 8000)  req = 8000;
      if (req > 192000) req = 192000;
      g_atomic_int_set(&audio_stream_rate, req);
    }
    char r[48];
    g_snprintf(r, sizeof(r), "audio_samplerate:%d;", g_atomic_int_get(&audio_stream_rate));
    client_send_text(c, r);
  } else if (!strcmp(name, "rit_enable")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_RIT_ENABLE, tci_argbool(args[1]), 0, 0.0);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "rit_enable:%d,%s;", rx_index, trx->rit_enabled ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "rit_offset")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_RIT_OFFSET, FALSE, g_ascii_strtoll(args[1], NULL, 10), 0.0);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "rit_offset:%d,%lld;", rx_index, (long long)trx->rit);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "xit_enable")) {
    // XIT lives on the SINGLE transmitter, so there is only one value however
    // many trx a client sees. A set therefore applies whichever trx addressed
    // it, but the state is dispatched against tci_tx_index() so update_vfo()
    // lands on the VFO row that actually shows the XIT button, and every reply
    // names that same index — telling a multi-RX client "xit_enable:0" while
    // the transmitter sits on receiver 1 is a lie it cannot detect.
    if (nargs >= 2) {
      tci_dispatch_state(tci_tx_index(), TCI_OP_XIT_ENABLE, tci_argbool(args[1]), 0, 0.0);
    } else {
      gboolean en = (g_radio != NULL && g_radio->transmitter != NULL)
                    ? g_radio->transmitter->xit_enabled : FALSE;
      char r[48];
      g_snprintf(r, sizeof(r), "xit_enable:%d,%s;", tci_tx_index(), en ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "xit_offset")) {
    if (nargs >= 2) {
      tci_dispatch_state(tci_tx_index(), TCI_OP_XIT_OFFSET, FALSE,
                         g_ascii_strtoll(args[1], NULL, 10), 0.0);
    } else {
      long long v = (g_radio != NULL && g_radio->transmitter != NULL)
                    ? (long long)g_radio->transmitter->xit : 0;
      char r[48];
      g_snprintf(r, sizeof(r), "xit_offset:%d,%lld;", tci_tx_index(), v);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "split_enable")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_SPLIT, tci_argbool(args[1]), 0, 0.0);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "split_enable:%d,%s;", rx_index, (trx->split != SPLIT_OFF) ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "if")) {
    // if:<rx>,<sub_rx>,<offset_hz> — demod offset within the panorama (CTUN).
    // Only the main channel (sub_rx 0) is applied; a sub-rx request is acked.
    if (nargs >= 3) {
      int chan = atoi(args[1]);
      gint64 off = g_ascii_strtoll(args[2], NULL, 10);
      gint64 lim = (trx != NULL && trx->sample_rate > 0) ? trx->sample_rate / 2 : 24000;
      if (off >  lim) off =  lim;
      if (off < -lim) off = -lim;
      if (chan == 0) {
        tci_dispatch_state(rx_index, TCI_OP_IF, FALSE, off, 0.0);
      } else {
        char r[64];
        g_snprintf(r, sizeof(r), "if:%d,%d,%lld;", rx_index, chan, (long long)off);
        client_send_text(c, r);
      }
    } else if (trx != NULL) {
      int chan = (nargs >= 2) ? atoi(args[1]) : 0;
      long long off = (chan == 0 && (trx->ctun || trx->freetune))
                      ? (long long)(trx->ctun_frequency - trx->frequency_a) : 0;
      char r[64];
      g_snprintf(r, sizeof(r), "if:%d,%d,%lld;", rx_index, chan, off);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "cw_msg")) {
    // cw_msg:<rx>,<before>,<after>,<text> — transmit through the CW encoder.
    // Fields can be empty and <text> may itself contain commas, so parse
    // positionally from the raw token rather than the comma-collapsed args[].
    //
    // <before>/<after> are the callsign-substitution markers: a TCI client puts
    // the text that surrounds the worked callsign in them while <text> carries
    // the fixed part of the macro. The protocol document does not say whether
    // they are sent or only used for on-screen highlighting, so this takes the
    // reading that agrees with the %C macro this app already has — what goes on
    // the air is  <before> <text> <after>, with %C expanding to
    // radio->station_call in ALL THREE (cw_encoder.c:cw_expand_macros runs over
    // the joined string). Non-empty parts are joined with ONE space, a CW word
    // gap, unless the client already supplied whitespace at that boundary: the
    // fields are words, not glue. ASSUMPTION, stated so it can be corrected
    // against a real client rather than rediscovered.
    //
    // Keying is refused for any trx other than the transmitting one, exactly as
    // `trx` does above: this starts a transmission, and one transmitter means a
    // request aimed elsewhere has no honest target.
    gboolean is_tx = (addr != NULL) && (req_index == tci_tx_index());
    if (is_tx) {
      char *msg = tci_cw_msg_text(token);
      if (msg != NULL) {
        tci_dispatch_cw_send(msg);
        g_free(msg);
      }
    } else {
      log_info("tci: cw_msg for rx=%d ignored (the transmitter is on rx=%d)\n",
               req_index, tci_tx_index());
    }
  } else if (!strcmp(name, "cw_macros_stop") || !strcmp(name, "cw_stop")) {
    tci_dispatch_cw_stop();
  } else if (!strcmp(name, "cw_macros_speed")) {
    if (nargs >= 1 && args[0] != NULL) {
      int wpm = atoi(args[0]);
      if (wpm >= 5 && wpm <= 60 && g_radio != NULL) g_radio->cw_keyer_speed = wpm;
    } else if (g_radio != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "cw_macros_speed:%d;", g_radio->cw_keyer_speed);
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "mute")) {
    // Global (device) mute. This radio has no master AF stage — the only audio
    // gain is the per-receiver WDSP panel gain — so a global mute is applied to
    // every visible receiver and reported from the active one (after a global
    // set they all agree). Per-receiver addressing is rx_mute. Muting is
    // lossless here: rx->mute is a separate flag from rx->volume, so unmuting
    // restores each receiver's own level.
    if (nargs >= 1 && args[0] != NULL) {
      tci_dispatch_state(0, TCI_OP_MUTE_ALL, tci_argbool(args[0]), 0, 0.0);
    } else {
      RECEIVER *a = (g_radio != NULL) ? g_radio->active_receiver : NULL;
      char r[32];
      g_snprintf(r, sizeof(r), "mute:%s;", (a != NULL && a->mute) ? "true" : "false");
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "volume")) {
    // Global (device) volume in dB, same all-receivers rule as `mute`. Unlike
    // mute this IS lossy — it overwrites each receiver's own level, because a
    // master gain the setting could live in does not exist here.
    if (nargs >= 1 && args[0] != NULL) {
      tci_dispatch_state(0, TCI_OP_VOLUME_ALL, FALSE, 0, tci_db_to_gain(tci_argdouble(args[0])));
    } else {
      RECEIVER *a = (g_radio != NULL) ? g_radio->active_receiver : NULL;
      char r[32];
      g_snprintf(r, sizeof(r), "volume:%d;", tci_gain_to_db(a != NULL ? a->volume : 0.0));
      client_send_text(c, r);
    }
  } else if (!strcmp(name, "rx_mute")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_MUTE, tci_argbool(args[1]), 0, 0.0);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "rx_mute:%d,%s;", rx_index, trx->mute ? "true" : "false");
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);
    }
  } else if (!strcmp(name, "rx_volume")) {
    // rx_volume:<rx>,<sub_rx>,<dB>. A SET must carry all three fields: the
    // two-field form is a get for that sub-rx, because reading `rx_volume:0,0;`
    // as a set would take the sub-rx number for a level and command 0 dB — full
    // volume — on what the client meant as a question.
    //
    // Only the main channel has a level of its own: the sub-receiver shares the
    // receiver's audio panel (subrx_mix is a balance, not a gain), so a sub_rx
    // request is answered with the receiver's value instead of being silently
    // retargeted.
    int chan = (nargs >= 2 && args[1] != NULL) ? atoi(args[1]) : 0;
    if (nargs >= 3 && chan == 0) {
      tci_dispatch_state(rx_index, TCI_OP_VOLUME, FALSE, 0, tci_db_to_gain(tci_argdouble(args[2])));
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "rx_volume:%d,%d,%d;", rx_index, chan, tci_gain_to_db(trx->volume));
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);
    }
  } else if (!strcmp(name, "drive")) {
    if (nargs >= 1 && args[0] != NULL) {
      tci_dispatch_state(0, TCI_OP_DRIVE, FALSE, 0, tci_argdouble(args[0]));
    } else if (g_radio != NULL && g_radio->transmitter != NULL) {
      char r[32];
      g_snprintf(r, sizeof(r), "drive:%d;", (int)lround(g_radio->transmitter->drive));
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);   // receive-only radio: no drive to report
    }
  } else if (!strcmp(name, "tune_drive")) {
    if (nargs >= 1 && args[0] != NULL) {
      tci_dispatch_state(0, TCI_OP_TUNE_DRIVE, FALSE, 0, tci_argdouble(args[0]));
    } else if (g_radio != NULL && g_radio->transmitter != NULL) {
      char r[32];
      g_snprintf(r, sizeof(r), "tune_drive:%d;", (int)lround(g_radio->transmitter->tune_percent));
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);   // receive-only radio: no tune drive to report
    }
  } else if (!strcmp(name, "tune")) {
    // Same rule as `trx`: tune keys the one transmitter, so a request aimed at
    // any other trx is a no-op rather than a transmission nobody asked for.
    gboolean is_tx = (addr != NULL) && (req_index == tci_tx_index());
    if (nargs >= 2) {
      if (is_tx) tci_dispatch_state(0, TCI_OP_TUNE, tci_argbool(args[1]), 0, 0.0);
    } else if (g_radio != NULL) {
      char r[32];
      g_snprintf(r, sizeof(r), "tune:%d,%s;", req_index,
                 (is_tx && g_radio->tune) ? "true" : "false");
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);
    }
  } else if (!strcmp(name, "lock")) {
    if (nargs >= 2) {
      tci_dispatch_state(rx_index, TCI_OP_LOCK, tci_argbool(args[1]), 0, 0.0);
    } else if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "lock:%d,%s;", rx_index, trx->locked ? "true" : "false");
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);
    }
  } else if (!strcmp(name, "rx_enable")) {
    // A receiver here is created and destroyed from the UI (Add Receiver, gated
    // on panels_idle()); there is no enable flag to set, and building or tearing
    // down a receiver behind the operator's back is not something a network
    // client gets to do. So the SET is refused — but the reply carries the REAL
    // state either way, which is the honest answer to both a get and a refused
    // set: a client that sets then reads is told what actually happened instead
    // of getting its own request echoed back at it.
    char r[48];
    g_snprintf(r, sizeof(r), "rx_enable:%d,%s;", req_index,
               (addr != NULL) ? "true" : "false");
    client_send_text(c, r);
  } else if (!strcmp(name, "rx_channel_enable")) {
    // rx_channel_enable:<rx>,<sub_rx>[,<bool>]. Channel 0 is the receiver's main
    // demod and is always on; channel 1 is this app's sub-receiver (the SUBRX
    // button), which is a genuine second WDSP demod channel on the same off-air
    // I/Q. Channels above that do not exist and answer false.
    int chan = (nargs >= 2 && args[1] != NULL) ? atoi(args[1]) : 0;
    if (nargs >= 3 && chan == 1 && addr != NULL) {
      tci_dispatch_state(rx_index, TCI_OP_SUBRX, tci_argbool(args[2]), 0, 0.0);
    } else if (trx != NULL) {
      gboolean on = (chan == 0) ? TRUE : (chan == 1 ? trx->subrx_enable : FALSE);
      char r[64];
      g_snprintf(r, sizeof(r), "rx_channel_enable:%d,%d,%s;", rx_index, chan,
                 on ? "true" : "false");
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);
    }
  } else if (!strcmp(name, "rx_smeter")) {
    // Read-only. rx->meter_db is the calibrated S-meter reading in dBm, written
    // by the display timer on the GTK thread and read here unlocked — a benign
    // scalar race, the same one every getter in this file takes.
    int chan = (nargs >= 2 && args[1] != NULL) ? atoi(args[1]) : 0;
    if (trx != NULL) {
      char r[64];
      g_snprintf(r, sizeof(r), "rx_smeter:%d,%d,%d;", rx_index, chan,
                 (int)lround(trx->meter_db));
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);
    }
  } else if (!strcmp(name, "sql_enable")) {
    // Read-only, deliberately. squelch_enable is DERIVED here — set_squelch()
    // recomputes it as (squelch > 0.0) — so it is not a flag that can be set:
    // "off" would have to throw the operator's threshold away and "on" would
    // have to invent one. The level itself (sql_level) is not wired for the
    // matching reason: TCI carries dB, while this radio's squelch is one 0..1
    // bar whose meaning is mode-dependent (FMSQ noise squelch vs an AMSQ whose
    // dB endpoints are operator-calibrated), so there is no honest conversion.
    if (trx != NULL) {
      char r[48];
      g_snprintf(r, sizeof(r), "sql_enable:%d,%s;", rx_index,
                 trx->squelch_enable ? "true" : "false");
      client_send_text(c, r);
    } else {
      tci_ack_echo(c, token);
    }
  } else if (!strcmp(name, "trx_count")) {
    char r[32];
    g_snprintf(r, sizeof(r), "trx_count:%d;", tci_trx_count());
    client_send_text(c, r);
  } else if (!strcmp(name, "audio_stream_sample_type")) {
    // Fact, not a stub: tci_stream_broadcast() writes float32 and
    // tci_ingest_binary() accepts nothing else.  A client that configures the
    // stream before sending TX audio asks this first.
    client_send_text(c, "audio_stream_sample_type:float32;");
  } else if (!strcmp(name, "audio_stream_channels")) {
    // Also a fact: RX audio goes out as interleaved stereo.
    client_send_text(c, "audio_stream_channels:2;");
  } else if (!strcmp(name, "audio_stream_samples")) {
    // How many samples a TX_CHRONO tick asks for (TCI 1.9: 100..2048, default
    // 2048 at 48 kHz).  Honoured rather than merely acknowledged -- it sets the
    // block size the client is going to answer with, so agreeing to a number
    // and then asking for another is how a modulator ends up stuttering.
    if (nargs >= 1) {
      int n = atoi(args[0]);
      if (n < TCI_TX_SAMPLES_MIN) n = TCI_TX_SAMPLES_MIN;
      if (n > TCI_TX_SAMPLES_MAX) n = TCI_TX_SAMPLES_MAX;
      g_atomic_int_set(&tx_chrono_samples, n);
    }
    char r[48];
    g_snprintf(r, sizeof(r), "audio_stream_samples:%d;", g_atomic_int_get(&tx_chrono_samples));
    client_send_text(c, r);
  } else if (!strcmp(name, "tx_stream_audio_buffering")) {
    // How much TX audio the client wants queued ahead, in ms (default 50).
    // Used to size the burst of ticks at the start of a transmission.
    if (nargs >= 1) {
      int ms = atoi(args[0]);
      if (ms < 0) ms = 0;
      if (ms > TCI_TX_BUFFER_MS_MAX) ms = TCI_TX_BUFFER_MS_MAX;
      g_atomic_int_set(&tx_chrono_buffer_ms, ms);
    }
    char r[56];
    g_snprintf(r, sizeof(r), "tx_stream_audio_buffering:%d;", g_atomic_int_get(&tx_chrono_buffer_ms));
    client_send_text(c, r);
  } else if (!strcmp(name, "channels_count")) {
    client_send_text(c, "channels_count:2;");         // main + sub per receiver
  } else if (!strcmp(name, "device")) {
    client_send_text(c, "device:MacHPSDR;");
  } else if (!strcmp(name, "protocol")) {
    client_send_text(c, "protocol:ExpertSDR3,1.9;");
  } else if (!strcmp(name, "receive_only")) {
    client_send_text(c, (g_radio != NULL && g_radio->can_transmit)
                        ? "receive_only:false;" : "receive_only:true;");
  } else if (!strcmp(name, "vfo_limits")) {
    // The ceiling the tuning guard uses, not a device's own top end: a client
    // told 6 GHz while the app accepts 10.49 GHz through a QO-100 converter is
    // being lied to about the one thing this command exists to answer. The
    // per-device narrowing is not reportable here — this is one number for a
    // whole application that may have several receivers on it.
    char r[48];
    g_snprintf(r, sizeof(r), "vfo_limits:0,%lld;", (long long)RECEIVER_FREQ_CEILING_HZ);
    client_send_text(c, r);
  } else if (!strcmp(name, "if_limits")) {
    int half = (trx != NULL && trx->sample_rate > 0) ? trx->sample_rate / 2 : 24000;
    char r[48];
    g_snprintf(r, sizeof(r), "if_limits:%d,%d;", -half, half);
    client_send_text(c, r);
  } else if (!strcmp(name, "modulations_list")) {
    client_send_text(c, "modulations_list:" TCI_MODLIST ";");
  } else {
    // Say which ones landed here: an unimplemented command a client is WAITING
    // on looks exactly like one it does not care about, and the echo below makes
    // both of them look answered.
    log_debug("tci: '%s' has no implementation here -- echoed as an ack\n", name);
    // Everything left over — start, stop, spot, spot_delete, sql_level,
    // rx_balance, rx_filter_band, cw_macros_delay, … — is echoed as an ack so a
    // request/response client cannot hang waiting on it.
    //
    // These are NOT stubs waiting to be filled in; each one is a command with no
    // honest counterpart in this application, and inventing state to answer with
    // would be worse than saying nothing:
    //   start/stop   — TCI's device start/stop is ExpertSDR's "power"; the radio
    //                  here is opened and closed by the startup/exit path, and a
    //                  socket client does not get to tear the DSP chain down.
    //                  The handshake announces `start;` because the radio IS on
    //                  by then; this echo answers a client that asks anyway.
    //   spot/…       — dxcluster.c's store is fed by the cluster thread with
    //                  age-out and dup-merge; it has no injection API and its
    //                  entries mean "someone spotted this", not "a client drew
    //                  this".
    //   sql_level    — see sql_enable above: dB vs a mode-dependent 0..1 bar.
    //   rx_balance   — no per-receiver pan; audio_channels is L/R routing and
    //                  subrx_mix is the main/sub crossfeed, neither is a balance.
    //   cw_macros_delay, rx_filter_band, … — no field behind them at all.
    tci_ack_echo(c, token);
  }
}

static void tci_process_text(TCI_CLIENT *c, char *text) {
  char *save = NULL;
  for (char *t = strtok_r(text, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
    char *cmd = g_strstrip(t);
    if (*cmd) tci_handle_command(c, cmd);
  }
}

static void tci_send_handshake(TCI_CLIENT *c) {
  RECEIVER *rx = (g_radio != NULL) ? g_radio->active_receiver : NULL;
  client_send_text(c, "protocol:ExpertSDR3,1.9;");
  client_send_text(c, "device:MacHPSDR;");
  client_send_text(c, g_radio && g_radio->can_transmit ? "receive_only:false;" : "receive_only:true;");
  {
    char r[32];
    g_snprintf(r, sizeof(r), "trx_count:%d;", tci_trx_count());
    client_send_text(c, r);
  }
  client_send_text(c, "channels_count:2;");
  {
    char r[48];
    g_snprintf(r, sizeof(r), "vfo_limits:0,%lld;", (long long)RECEIVER_FREQ_CEILING_HZ);
    client_send_text(c, r);
  }
  if (rx != NULL && rx->sample_rate > 0) {
    char r[48];
    g_snprintf(r, sizeof(r), "if_limits:%d,%d;", -rx->sample_rate / 2, rx->sample_rate / 2);
    client_send_text(c, r);
  } else {
    client_send_text(c, "if_limits:-24000,24000;");
  }
  client_send_text(c, "modulations_list:" TCI_MODLIST ";");
  {
    char r[48];
    g_snprintf(r, sizeof(r), "audio_samplerate:%d;", g_atomic_int_get(&audio_stream_rate));
    client_send_text(c, r);
  }
  if (rx != NULL) {
    char r[48];
    g_snprintf(r, sizeof(r), "iq_samplerate:%d;", rx->sample_rate);
    client_send_text(c, r);
  }
  // Per-rx initial state so a connecting (multi-RX) logger starts fully in sync.
  int ntrx = tci_trx_count();
  for (int idx = 0; idx < ntrx; idx++) {
    RECEIVER *t = tci_rx_at(idx);
    if (t == NULL) continue;
    char r[64];
    g_snprintf(r, sizeof(r), "vfo:%d,0,%lld;", idx, receiver_tuned_frequency(t));
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "vfo:%d,1,%lld;", idx, (long long)t->frequency_b);
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "modulation:%d,%s;", idx, mode_to_tci(t->mode_a));
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "rit_enable:%d,%s;", idx, t->rit_enabled ? "true" : "false");
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "rit_offset:%d,%lld;", idx, (long long)t->rit);
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "split_enable:%d,%s;", idx, (t->split != SPLIT_OFF) ? "true" : "false");
    client_send_text(c, r);
    // "Sent to the client when connected" (TCI 1.9, TX_ENABLE), and it was not.
    // A client that gates its transmit button on this never keys at all, and
    // says nothing about why -- one line is cheaper than finding that out from
    // a remote operator.  Per trx: only the one the transmitter is on may key.
    g_snprintf(r, sizeof(r), "tx_enable:%d,%s;", idx,
               (idx == tci_tx_index() && g_radio != NULL && g_radio->can_transmit)
               ? "true" : "false");
    client_send_text(c, r);
  }
  if (g_radio != NULL && g_radio->transmitter != NULL) {
    // Transmitter state is announced against the trx the transmitter is on, not
    // a hard-coded 0 — an unsolicited "xit_enable:0" tells a multi-RX client the
    // wrong thing when the transmitter is not on receiver 0.
    int txi = tci_tx_index();
    char r[48];
    g_snprintf(r, sizeof(r), "xit_enable:%d,%s;", txi, g_radio->transmitter->xit_enabled ? "true" : "false");
    client_send_text(c, r);
    g_snprintf(r, sizeof(r), "xit_offset:%d,%lld;", txi, (long long)g_radio->transmitter->xit);
    client_send_text(c, r);
    // Initial PTT state, which the handshake never sent at all.
    g_snprintf(r, sizeof(r), "trx:%d,%s;", txi, g_radio->mox ? "true" : "false");
    client_send_text(c, r);
  }
  // TCI's device power, and a fact rather than a courtesy here: the server is
  // started at the end of create_radio, with the hardware already streaming, so
  // a client that got as far as this handshake is talking to a radio that is
  // on.  Announcing it matters because a client may refuse to go any further
  // without it — JTDX throws "TCI SDR is not switched on" unless the operator
  // has ticked its own "switch the SDR on" box, which only makes it send
  // `start;` and take the echo below for the same answer.
  client_send_text(c, "start;");
  client_send_text(c, "ready;");
}

// ---------------------------------------------------------------------------
// client & accept threads
// ---------------------------------------------------------------------------

// Closes the socket too, and does BOTH under send_mtx.  The descriptor must not
// be released while a foreign thread is inside client_send_framed_try(), which
// reads c->fd under that lock and then sends: the kernel hands the same number
// to the next accept(), so an IQ frame from the audio thread could land in a
// brand-new client's socket before its handshake had even finished.  Lock order
// is the documented clients_mutex -> send_mtx.
static void clients_remove(TCI_CLIENT *c) {
  g_mutex_lock(&clients_mutex);
  client_set_iq(c, -1, FALSE);      // clear all rx bits, keep iq_sub_count balanced
  client_set_audio(c, -1, FALSE);   // clear all rx bits, keep audio_sub_count balanced
  tci_set_tx_chrono(c, FALSE);      // a client that vanishes owes us no TX audio
  g_mutex_lock(&c->send_mtx);
  int fd = c->fd;
  c->fd = -1;
  if (fd >= 0) closesocket(fd);
  g_mutex_unlock(&c->send_mtx);
  g_mutex_unlock(&clients_mutex);
}

static gpointer tci_client_thread(gpointer data) {
  TCI_CLIENT *c = (TCI_CLIENT *)data;
  int fd = c->fd;

#ifdef __APPLE__
  { int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
  { int on = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on)); }
  // A generous send buffer keeps a full IQ frame atomic for MSG_DONTWAIT sends.
  { int sz = 1 << 20; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)); }

  if (!tci_ws_handshake(fd)) {
    log_info("tci: websocket handshake failed for fd=%d\n", fd);
    clients_remove(c);   // closes fd
    return NULL;
  }
  log_info("tci: client connected (fd=%d)\n", fd);
  tci_send_handshake(c);

  while (g_atomic_int_get(&server_running)) {
    char *payload = NULL;
    size_t plen = 0;
    int op = tci_ws_recv(fd, &payload, &plen);
    if (op < 0) break;
    if (op == 0x8) { g_free(payload); break; }          // close
    if (op == 0x9) {                                     // ping -> pong
      g_mutex_lock(&c->send_mtx);
      if (c->fd >= 0) tci_ws_send_blocking(fd, 0xA, payload, plen);
      g_mutex_unlock(&c->send_mtx);
      g_free(payload);
      continue;
    }
    if (op == 0x1 && payload) tci_process_text(c, payload);          // text command
    else if (op == 0x2 && payload) tci_ingest_binary((guint8 *)payload, plen);  // TX audio
    g_free(payload);
  }

  log_info("tci: client disconnected (fd=%d)\n", fd);
  clients_remove(c);   // closes fd
  return NULL;
}

static gpointer tci_server_thread(gpointer data) {
  (void)data;
  int on = 1;

  listen_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_socket < 0) {
    log_error("tci: socket() failed: %s\n", strerror(errno));
    g_atomic_int_set(&server_running, 0);
    return NULL;
  }
  setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
  setsockopt(listen_socket, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(listening_port);
  if (bind(listen_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    log_error("tci: bind(:%d) failed: %s\n", listening_port, strerror(errno));
    closesocket(listen_socket);
    listen_socket = -1;
    g_atomic_int_set(&server_running, 0);
    g_snprintf(status_line, sizeof(status_line), "bind :%d failed", listening_port);
    return NULL;
  }
  if (listen(listen_socket, 4) < 0) {
    log_error("tci: listen() failed: %s\n", strerror(errno));
    closesocket(listen_socket);
    listen_socket = -1;
    g_atomic_int_set(&server_running, 0);
    return NULL;
  }
  g_snprintf(status_line, sizeof(status_line), "listening on :%d", listening_port);
  log_info("tci: %s\n", status_line);

  while (g_atomic_int_get(&server_running)) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int fd = accept(listen_socket, (struct sockaddr *)&caddr, &clen);
    if (fd < 0) {
      if (!g_atomic_int_get(&server_running)) break;
      if (errno == EINTR) continue;
      break;
    }
    TCI_CLIENT *slot = NULL;
    g_mutex_lock(&clients_mutex);
    for (int i = 0; i < TCI_MAX_CLIENTS; i++)
      if (clients[i].fd < 0) {
        clients[i].fd = fd;
        g_atomic_int_set(&clients[i].iq_mask, 0);
        g_atomic_int_set(&clients[i].audio_mask, 0);
        slot = &clients[i];
        break;
      }
    g_mutex_unlock(&clients_mutex);
    if (slot == NULL) {
      log_info("tci: too many clients, rejecting fd=%d\n", fd);
      closesocket(fd);
      continue;
    }
    GThread *t = g_thread_new("tci-client", tci_client_thread, slot);
    if (t) g_thread_unref(t);
  }

  if (listen_socket >= 0) { closesocket(listen_socket); listen_socket = -1; }
  return NULL;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void tci_init(RADIO *radio) {
  g_radio = radio;
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) clients[i].fd = -1;
  // Dev/verify hook: MACHPSDR_TCI[=port] force-enables the server (optionally on
  // a chosen port) so the faker can exercise it headlessly.
  const char *env = getenv("MACHPSDR_TCI");
  if (env != NULL) {
    radio->tci_enable = TRUE;
    int p = atoi(env);
    if (p > 0) radio->tci_port = p;
  }
  if (radio != NULL && radio->tci_enable) tci_start();
}

void tci_start(void) {
  if (g_atomic_int_get(&server_running)) return;
  if (g_radio == NULL) return;
  listening_port = (g_radio->tci_port > 0) ? g_radio->tci_port : TCI_DEFAULT_PORT;
  g_atomic_int_set(&server_running, 1);
  g_snprintf(status_line, sizeof(status_line), "starting on :%d", listening_port);
  server_thread = g_thread_new("tci-server", tci_server_thread, NULL);
}

void tci_stop(void) {
  if (!g_atomic_int_get(&server_running) && server_thread == NULL) return;

  g_atomic_int_set(&server_running, 0);

  if (listen_socket >= 0) {
    shutdown(listen_socket, SHUT_RDWR);
    closesocket(listen_socket);
    listen_socket = -1;
  }
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd >= 0) {
      shutdown(clients[i].fd, SHUT_RDWR);   // unblock the client recv; it self-cleans
    }
  }
  g_mutex_unlock(&clients_mutex);

  if (server_thread != NULL) {
    g_thread_join(server_thread);
    server_thread = NULL;
  }
  g_snprintf(status_line, sizeof(status_line), "stopped");
}

const char *tci_status(void) {
  if (!g_atomic_int_get(&server_running)) return status_line;
  static char buf[128];
  int n = 0;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) if (clients[i].fd >= 0) n++;
  g_mutex_unlock(&clients_mutex);
  int iq = g_atomic_int_get(&iq_sub_count);
  int au = g_atomic_int_get(&audio_sub_count);
  g_snprintf(buf, sizeof(buf), "%s (%d client%s%s%s)", status_line, n, n == 1 ? "" : "s",
             iq > 0 ? ", IQ" : "", au > 0 ? ", audio" : "");
  return buf;
}

// --- outbound notifications (GTK/audio thread) ------------------------------

void tci_notify_vfo(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;                         // hidden receiver: not a TCI trx
  char line[64];
  g_snprintf(line, sizeof(line), "vfo:%d,0,%lld;", idx, receiver_tuned_frequency(rx));
  tci_broadcast_text(line);
  g_snprintf(line, sizeof(line), "vfo:%d,1,%lld;", idx, (long long)rx->frequency_b);
  tci_broadcast_text(line);
}

void tci_notify_mode(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;
  char line[48];
  g_snprintf(line, sizeof(line), "modulation:%d,%s;", idx, mode_to_tci(rx->mode_a));
  tci_broadcast_text(line);
}

void tci_notify_trx(gboolean mox) {
  if (!g_atomic_int_get(&server_running)) return;
  // A transmission that ends -- however it ended, including the operator's own
  // MOX button -- ends the client's role as the audio source with it.  Left
  // armed, the next over started from the microphone would still be asking a
  // network client for audio and would take its samples over the mic's.
  if (!mox) tci_tx_chrono_disarm_all();
  char line[32];
  g_snprintf(line, sizeof(line), "trx:%d,%s;", tci_tx_index(), mox ? "true" : "false");
  tci_broadcast_text(line);
}

// RIT / split (RECEIVER) + XIT (TRANSMITTER) changed locally — push the full
// small state set so a following logger stays synced. Called from actions.c.
//
// RIT and split are per receiver and are announced against the receiver that
// changed. XIT is not: there is ONE transmitter, so there is one XIT, and the
// index in the message has to name the trx that transmits — `idx` here is
// merely whichever receiver's VFO row the operator happened to act on (the XIT
// button appears on every row and writes the same transmitter field). Sending
// xit_enable:0 while the transmitter sits on receiver 1 is a lie a client
// cannot detect, and it is the multi-RX gap this pair of lines used to have.
void tci_notify_state(RECEIVER *rx) {
  if (!g_atomic_int_get(&server_running) || rx == NULL) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;
  char line[64];
  g_snprintf(line, sizeof(line), "rit_enable:%d,%s;", idx, rx->rit_enabled ? "true" : "false");
  tci_broadcast_text(line);
  g_snprintf(line, sizeof(line), "rit_offset:%d,%lld;", idx, (long long)rx->rit);
  tci_broadcast_text(line);
  g_snprintf(line, sizeof(line), "split_enable:%d,%s;", idx, (rx->split != SPLIT_OFF) ? "true" : "false");
  tci_broadcast_text(line);
  if (g_radio != NULL && g_radio->transmitter != NULL) {
    int txi = tci_tx_index();
    g_snprintf(line, sizeof(line), "xit_enable:%d,%s;", txi, g_radio->transmitter->xit_enabled ? "true" : "false");
    tci_broadcast_text(line);
    g_snprintf(line, sizeof(line), "xit_offset:%d,%lld;", txi, (long long)g_radio->transmitter->xit);
    tci_broadcast_text(line);
  }
}

// --- outbound binary streams (audio thread taps in receiver.c) --------------

// Clipping is a fact about the SIGNAL, not about the stream, so say it out loud
// rather than only bounding it: the same overload is clipping the operator's
// speaker and any recording they make, and the cure is AGC-G, not anything
// here.  One line per 5 s window, and only while something is actually being
// clipped.  Counters are plain statics touched from the RX audio threads (one
// per receiver): a torn count costs a wrong percentage in a diagnostic, which
// is not worth an atomic on the audio path.
static void tci_audio_drop_account(guint32 dropped, guint32 tried) {
  static gint64 window_us = 0;
  static guint64 n_drop = 0, n_try = 0;
  gint64 now = g_get_monotonic_time();
  n_drop += dropped;
  n_try  += tried;
  if (window_us == 0) { window_us = now; return; }
  if (now - window_us < 5000000) return;
  if (n_drop > 0 && n_try > 0)
    log_info("tci: dropped %llu of %llu RX audio frames in the last %.0f s (%.1f%%) -- the send "
             "is non-blocking, so a client that reads slowly gets HOLES, which a decoder sees as "
             "lost data rather than as quiet\n",
             (unsigned long long)n_drop, (unsigned long long)n_try,
             (double)(now - window_us) / 1e6, 100.0 * (double)n_drop / (double)n_try);
  window_us = now;
  n_drop = 0;
  n_try  = 0;
}

static void tci_audio_clip_account(guint32 clipped, guint32 total, double peak) {
  static gint64 window_us = 0;
  static guint64 n_clip = 0, n_tot = 0;
  static double  w_peak = 0.0;
  gint64 now = g_get_monotonic_time();
  n_clip += clipped;
  n_tot  += total;
  if (peak > w_peak) w_peak = peak;
  if (window_us == 0) { window_us = now; return; }
  if (now - window_us < 5000000) return;
  // Name the cut, not just the fault: the peak is measured before the clamp, so
  // 20*log10(peak) is exactly how much AF GAIN (or AGC-G) has to come down, and
  // an operator should not have to find that by bisection while a band is open.
  if (n_clip > 0 && n_tot > 0)
    log_info("tci: RX audio over full scale -- %.1f%% of samples clipped in the last %.0f s, "
             "peak %.2f (turn the level down by %.0f dB). The stream is float32 normalised to "
             "1.0 and clients convert it to 16-bit, where this is a square wave; the same "
             "overload clips the speaker and the recorder\n",
             100.0 * (double)n_clip / (double)n_tot, (double)(now - window_us) / 1e6,
             w_peak, 20.0 * log10(w_peak > 1.0 ? w_peak : 1.0));
  window_us = now;
  n_clip = 0;
  n_tot = 0;
  w_peak = 0.0;
}

// Build the complete on-wire WS frame once (WS binary header + 64-byte TCI
// header + interleaved float32 down-cast from `interleaved`) and broadcast to
// every client subscribed to this rx's stream (`audio` picks audio_mask vs
// iq_mask; only clients with rx_index's bit set receive it). `data` may be a
// float source (`fsrc`) or double source (`interleaved`) — exactly one is used.
static void tci_stream_broadcast_gain(int data_type, int rx_index, int sample_rate, int channels,
                                      const double *interleaved, const float *fsrc,
                                      guint32 nfloats, gboolean audio, double gain) {
  if (rx_index < 0 || rx_index >= TCI_MAX_CLIENTS * 4) return;   // bit index sanity
  guint  rxbit = 1u << rx_index;

  // Cheap pre-scan: if no connected client is subscribed to THIS rx's stream,
  // skip building the frame entirely (the malloc + double->float conversion of a
  // whole IQ/audio block). The global sub-count gate already handled "nobody
  // subscribed to anything"; this covers the multi-RX case where clients follow
  // other receivers. A client subscribing between this check and the send below
  // just misses one block — harmless.
  gboolean any = FALSE;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd < 0) continue;
    gint mask = audio ? g_atomic_int_get(&clients[i].audio_mask)
                      : g_atomic_int_get(&clients[i].iq_mask);
    if ((guint)mask & rxbit) { any = TRUE; break; }
  }
  g_mutex_unlock(&clients_mutex);
  if (!any) return;

  size_t  payload = TCI_HDR_BYTES + (size_t)nfloats * sizeof(float);
  guint8  wshdr[10];
  size_t  wshlen = tci_ws_write_header(wshdr, 0x2 /*binary*/, payload);
  size_t  flen = wshlen + payload;
  guint8 *frame = g_malloc(flen);

  memcpy(frame, wshdr, wshlen);
  guint8 *th = frame + wshlen;                 // 64-byte TCI header
  memset(th, 0, TCI_HDR_BYTES);
  st32le(th +  0, (guint32)rx_index);           // rx index
  st32le(th +  4, (guint32)sample_rate);        // sample_rate
  st32le(th +  8, TCI_SAMPLE_FLOAT32);          // data_format
  st32le(th + 12, 0);                           // codec
  st32le(th + 16, 0);                           // crc
  st32le(th + 20, nfloats);                     // length (float count)
  st32le(th + 24, (guint32)data_type);          // data_type
  st32le(th + 28, (guint32)channels);           // channels

  // TCI float32 is normalised to +/-1.0 full scale, and a client turns it
  // straight back into 16-bit PCM.  JTDX's does it with no saturation --
  // `dest[i] = int16_t(0x7FFF * source[i*2])` -- so a sample of 1.5 does not
  // arrive loud, it arrives with the SIGN FLIPPED.  This receiver's demod
  // buffer is not bounded: every other consumer clamps on its way out (the
  // sound card and the protocol path in process_rx_buffer(), the recorder's
  // clamp16()) and this tap alone handed it over raw.  Measured off the live
  // radio on QO-100 at S8: peak 4.93, RMS 1.07, and 38.75 % of every block past
  // full scale -- so a third of the samples reached the client wrapped, which
  // turns strong carriers into broadband noise.  JTDX drew an empty band and
  // decoded nothing while this app's own FT8 panel, fed from the same buffer,
  // drew four solid traces.
  //
  // IQ is left alone: it is bounded by the ADC scaling and a clamp there would
  // silently disguise a scaling bug rather than fix a client.
  float *fp = (float *)(th + TCI_HDR_BYTES);
  if (audio) {
    guint32 clipped = 0;
    double  peak = 0.0;
    if (fsrc != NULL) {
      for (guint32 i = 0; i < nfloats; i++) {
        float v = fsrc[i];                       // already scaled by the caller
        if (fabs(v) > peak) peak = fabs(v);
        if (v >  1.0f) { v =  1.0f; clipped++; }
        else if (v < -1.0f) { v = -1.0f; clipped++; }
        fp[i] = v;
      }
    } else {
      for (guint32 i = 0; i < nfloats; i++) {
        double v = interleaved[i] * gain;
        double a = fabs(v);
        if (a > peak) peak = a;
        if (v >  1.0) { v =  1.0; clipped++; }
        else if (v < -1.0) { v = -1.0; clipped++; }
        fp[i] = (float)v;
      }
    }
    tci_audio_clip_account(clipped, nfloats, peak);
  } else if (fsrc != NULL) {
    memcpy(fp, fsrc, (size_t)nfloats * sizeof(float));
  } else {
    for (guint32 i = 0; i < nfloats; i++) fp[i] = (float)interleaved[i];
  }


  guint32 tried = 0, dropped = 0;
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd < 0) continue;
    gint mask = audio ? g_atomic_int_get(&clients[i].audio_mask)
                      : g_atomic_int_get(&clients[i].iq_mask);
    if (!((guint)mask & rxbit)) continue;
    tried++;
    if (!client_send_framed_try(&clients[i], frame, flen)) dropped++;
  }
  g_mutex_unlock(&clients_mutex);
  if (audio) tci_audio_drop_account(dropped, tried);

  g_free(frame);
}

// Phase B: off-air I/Q (interleaved doubles, `nsamples` complex pairs). Gated so
// it costs one atomic read when nobody subscribed. Streams per-rx (multi-RX): the
// frame's rx index = the receiver's TCI index and only clients subscribed to
// that index receive it.
void tci_iq_feed(RECEIVER *rx, const double *iq, int nsamples, int sample_rate) {
  if (!g_atomic_int_get(&server_running)) return;
  if (g_atomic_int_get(&iq_sub_count) <= 0) return;
  if (rx == NULL || iq == NULL || nsamples <= 0) return;
  int idx = tci_rx_index(rx);
  if (idx < 0) return;
  // The receiver's buffer is (Q, I) — that is how WDSP reads it (analyzer.c
  // Spectrum0) and therefore what the panadapter shows. TCI carries (I, Q), so
  // send it in that order: passing the buffer through verbatim hands the client
  // the conjugate, i.e. a spectrum mirrored against this radio's own display,
  // which puts every signal a skimmer finds on the wrong side of the centre.
  guint32 nf = (guint32)nsamples * 2u;
  float *swapped = g_new(float, nf);
  for (int i = 0; i < nsamples; i++) {
    swapped[2*i]     = (float)iq[2*i + 1];   // I
    swapped[2*i + 1] = (float)iq[2*i];       // Q
  }
  tci_stream_broadcast_gain(TCI_STREAM_IQ, idx, sample_rate, 2, NULL, swapped, nf, FALSE, 1.0);
  g_free(swapped);
}

// Phase C: demodulated RX audio (interleaved stereo doubles, `nstereo` frames at
// `sample_rate`, natively 48 kHz). Per-rx (multi-RX). If a client asked for a
// non-48k stream rate the block is resampled per channel first.
// --- output limiter ---------------------------------------------------------
//
// The stream must be usable without the operator setting anything up for it,
// which means the level cannot be theirs to get wrong: the receiver's audio is
// whatever the demodulator and AGC-G produce, and on this radio that has been
// measured at a peak of 4.93 with AGC on and pinned against the ceiling with
// AGC off.  Clamping bounds it but leaves a square wave; asking the operator to
// trim AF GAIN makes a network stream depend on a speaker control.  So the feed
// carries its own gain: peak-following, attenuating fast enough that nothing
// reaches the clamp and recovering slowly enough that a decoder's slot sees an
// essentially steady gain (10 s release; measured at 1.1 dB across the rest of
// a 15 s FT8 slot once settled).
//
// Measured against a recorded slot with the overload put back (x6, i.e. what
// this radio produced with AGC-G where it was): clamping alone loses a decode
// (4 of the 5 the healthy recording gives), the limiter returns all 5, holds
// the peak at exactly the 0.500 target and clips nothing.
//
// It is NOT an AGC and must not become one -- no per-slot levelling, no
// compression -- because a decoder reads the waveform.  It only stops the
// signal leaving the format's range, and it can lift a quiet receiver by at
// most TCI_LIM_MAX_GAIN so that switching the radio's own AGC off does not
// leave a client with nothing.
#define TCI_LIM_TARGET    0.50     // peak we aim the stream at (-6 dBFS)
#define TCI_LIM_MAX_GAIN  10.0     // never lift by more than 20 dB
#define TCI_LIM_REL_S     10.0     // rise this slowly (seconds)

static double lim_gain[MAX_RECEIVERS];     // audio thread of that receiver only
static gboolean lim_init = FALSE;

// Returns the gain to END this block on; the caller ramps from the previous one
// so a change never lands as a step.
static double tci_limiter_step(int idx, const double *audio, int nstereo, double *from) {
  if (!lim_init) {
    for (int i = 0; i < MAX_RECEIVERS; i++) lim_gain[i] = 1.0;
    lim_init = TRUE;
  }
  double peak = 0.0;
  for (int i = 0; i < nstereo * 2; i++) {
    double a = fabs(audio[i]);
    if (a > peak) peak = a;
  }
  double g = lim_gain[idx];
  double want = (peak > 1e-9) ? TCI_LIM_TARGET / peak : TCI_LIM_MAX_GAIN;
  if (want > TCI_LIM_MAX_GAIN) want = TCI_LIM_MAX_GAIN;

  // The attack has to be taken WHOLE and on this block, because the peak that
  // demands it is inside this block: ramping into a reduction lets the front of
  // the block through at the old gain, and it is exactly the loud one. Measured
  // on a recorded slot with the overload put back (x6): ramping the attack over
  // 100 ms still let the stream reach a peak of 3.17 and clip, while taking it
  // in one step holds the peak at 0.500 with nothing clipped. A step DOWN in
  // gain is what every limiter does and is not audible as a click; the release
  // is the half that must stay slow, and it is ramped across the block.
  double g1;
  if (want < g) {
    g1 = want;
    *from = want;                      // flat across the block, no ramp-in
  } else {
    double dur = (double)nstereo / 48000.0;
    double a = 1.0 - exp(-dur / TCI_LIM_REL_S);
    g1 = g + (want - g) * a;
    *from = g;
  }
  if (g1 < 1e-6) g1 = 1e-6;
  lim_gain[idx] = g1;
  return g1;
}

void tci_audio_feed(RECEIVER *rx, const double *audio, int nstereo, int sample_rate) {
  if (!g_atomic_int_get(&server_running)) return;
  if (g_atomic_int_get(&audio_sub_count) <= 0) return;
  if (rx == NULL || audio == NULL || nstereo <= 0) return;
  int idx = tci_rx_index(rx);
  if (idx < 0 || idx >= MAX_RECEIVERS) return;

  // The receiver hands this tap a full-level signal (the panel gain is forced to
  // unity while a client is subscribed, exactly as for a decoder), so AF GAIN
  // and Mute do not reach the stream at all -- they are the speaker's. What
  // bounds it is the limiter above.
  double g0 = 1.0;
  double g1 = tci_limiter_step(idx, audio, nstereo, &g0);
  float *lin = g_new(float, (size_t)nstereo * 2);
  for (int i = 0; i < nstereo; i++) {
    double g = g0 + (g1 - g0) * ((double)i / (double)nstereo);
    lin[2*i]     = (float)(audio[2*i]     * g);
    lin[2*i + 1] = (float)(audio[2*i + 1] * g);
  }

  int target = g_atomic_int_get(&audio_stream_rate);
  if (target <= 0 || target == sample_rate) {
    tci_stream_broadcast_gain(TCI_STREAM_RX_AUDIO, idx, sample_rate, 2, NULL, lin,
                              (guint32)nstereo * 2u, TRUE, 1.0);
    g_free(lin);
    return;
  }

  // Resample this rx's two channels sample_rate -> target (state per rx).
  TCI_RESAMP *L = &rs_rx[idx][0], *R = &rs_rx[idx][1];
  if (L->rin != sample_rate || L->rout != target) tci_resamp_reset(L, sample_rate, target);
  if (R->rin != sample_rate || R->rout != target) tci_resamp_reset(R, sample_rate, target);

  int cap = (int)((gint64)nstereo * target / sample_rate) + 4;
  float *inL = g_new(float, nstereo), *inR = g_new(float, nstereo);
  for (int i = 0; i < nstereo; i++) { inL[i] = lin[2*i]; inR[i] = lin[2*i+1]; }
  g_free(lin);
  float *outL = g_new(float, cap), *outR = g_new(float, cap);
  int nL = tci_resamp_run(L, inL, nstereo, outL, cap);
  int nR = tci_resamp_run(R, inR, nstereo, outR, cap);
  int no = (nL < nR) ? nL : nR;             // stay interleave-aligned
  if (no > 0) {
    float *inter = g_new(float, (size_t)no * 2);
    for (int i = 0; i < no; i++) { inter[2*i] = outL[i]; inter[2*i+1] = outR[i]; }
    tci_stream_broadcast_gain(TCI_STREAM_RX_AUDIO, idx, target, 2, NULL, inter,
                              (guint32)no * 2u, TRUE, 1.0);   // gain already applied above
    g_free(inter);
  }
  g_free(inL); g_free(inR); g_free(outL); g_free(outR);
}

// --- TX_CHRONO: the half of TCI TX audio that has to come from the server ----
//
// TCI Protocol 1.9, section 3.4, on sending an audio stream to a transmitter:
// "ExpertSDR3 sends a TX_CHRONO timestamp that notifies the client to send an
// audio signal marked as TX_AUDIO_STREAM with the specified number of samples
// in the Stream.length.  Timestamps are sent without waiting for a response
// from the client."  Nothing here ever sent one, so a client that keys with
// `trx:0,true,tci;` -- which is exactly what MSHV does -- keyed the radio and
// then waited for a tick that was never coming.  Neither side was at fault in a
// way either could see: the app reported a healthy transmitter (measured on a
// PlutoSDR: 28209152 samples delivered to the DAC over 12.6 s, nothing refused)
// and every one of those samples was silence, because the modulator was waiting
// on a handshake with one half missing.
//
// The tick is clocked by the TX chain itself (add_mic_sample -> the buffer
// boundary), not by a timer: that is the same 48 kHz the audio is consumed at,
// on whatever thread the protocol clocks TX with, so the request rate cannot
// drift from the consumption rate.
static void tci_send_chrono(guint32 nsamples) {
  guint8  wshdr[10];
  size_t  wshlen = tci_ws_write_header(wshdr, 0x2 /*binary*/, TCI_HDR_BYTES);
  size_t  flen = wshlen + TCI_HDR_BYTES;
  guint8  frame[10 + TCI_HDR_BYTES];

  memcpy(frame, wshdr, wshlen);
  guint8 *th = frame + wshlen;
  memset(th, 0, TCI_HDR_BYTES);
  st32le(th +  0, (guint32)tci_tx_index());              // receiver (the transmitting trx)
  st32le(th +  4, (guint32)g_atomic_int_get(&audio_stream_rate));
  st32le(th +  8, TCI_SAMPLE_FLOAT32);                   // format
  st32le(th + 12, 0);                                    // codec
  st32le(th + 16, 0);                                    // crc
  st32le(th + 20, nsamples);                             // length: what we are asking for
  st32le(th + 24, TCI_STREAM_TX_CHRONO);                 // type
  st32le(th + 28, 2);                                    // channels
  // A chrono carries no payload -- it is a timestamp, and the client answers it
  // with a TX_AUDIO_STREAM frame of its own.

  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd < 0) continue;
    if (!g_atomic_int_get(&clients[i].tx_chrono)) continue;
    client_send_framed_try(&clients[i], frame, flen);
  }
  g_mutex_unlock(&clients_mutex);
}

// Arm/disarm one client as the TX-audio source. Called from that client's own
// thread (the trx command) and from tci_notify_trx() when the transmission ends
// -- including one the operator ended with the MOX button, which a client that
// keyed over the network never hears about otherwise.
static void tci_set_tx_chrono(TCI_CLIENT *c, gboolean on) {
  if (c == NULL) return;
  if (g_atomic_int_get(&c->tx_chrono) == (on ? 1 : 0)) return;
  g_atomic_int_set(&c->tx_chrono, on ? 1 : 0);
  if (on) {
    g_atomic_int_inc(&tx_chrono_clients);
    g_atomic_int_set(&tx_chrono_at_arm, g_atomic_int_get(&tx_frames_in));
    log_info("tci: client (fd=%d) keyed with source=tci -- asking it for TX audio, "
             "%d sample(s) per tick at %d Hz, %d ms of buffering\n",
             c->fd, g_atomic_int_get(&tx_chrono_samples),
             g_atomic_int_get(&audio_stream_rate), g_atomic_int_get(&tx_chrono_buffer_ms));
  } else {
    g_atomic_int_dec_and_test(&tx_chrono_clients);
    // The one line that names the fault when a client keys and stays silent.
    // Without it the operator sees a transmitter that runs perfectly and puts
    // nothing on the air, and there is nothing in any log to read.
    if (g_atomic_int_get(&tx_frames_in) == g_atomic_int_get(&tx_chrono_at_arm)) {
      log_error("tci: the client keyed with source=tci and then sent no TX audio at all "
                "-- it was asked for it (TX_CHRONO), so it either does not answer chrono "
                "ticks or it is streaming to something else\n");
    }
  }
}

static void tci_tx_chrono_disarm_all(void) {
  g_mutex_lock(&clients_mutex);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (clients[i].fd < 0) continue;
    tci_set_tx_chrono(&clients[i], FALSE);
  }
  g_mutex_unlock(&clients_mutex);
}

// One TX buffer has been clocked into the transmitter (add_mic_sample). Turn
// that into chrono ticks at the same rate. `nsamples` is in the 48 kHz mic
// domain; a chrono asks for tx_chrono_samples samples of the STREAM rate, which
// is not the same clock when a client asked for 8/12/24 kHz.
//
// The accumulator is a plain static: the TX chain is clocked by exactly one
// thread at a time (the mic thread or the protocol's TX pump), and the arm path
// resets it from a client thread only when no transmission is running.
void tci_tx_chrono_tick(int nsamples) {
  static int      acc = 0;
  static gboolean primed = FALSE;

  if (!g_atomic_int_get(&server_running)) return;
  if (g_atomic_int_get(&tx_chrono_clients) <= 0) { acc = 0; primed = FALSE; return; }
  if (g_radio == NULL || !g_radio->mox) { acc = 0; primed = FALSE; return; }
  if (nsamples <= 0) return;

  const int want = g_atomic_int_get(&tx_chrono_samples);
  const int rate = g_atomic_int_get(&audio_stream_rate);
  if (want <= 0 || rate <= 0) return;
  // 48 kHz mic samples covered by one tick's worth of stream samples.
  int per = (int)(((gint64)want * TCI_AUDIO_RATE) / rate);
  if (per < 1) per = 1;

  if (!primed) {
    // Ask for the client's requested buffering depth up front, or the ring
    // starts empty and the first tenth of a second of every over is silence.
    int ms = g_atomic_int_get(&tx_chrono_buffer_ms);
    int ticks = (int)(((gint64)ms * TCI_AUDIO_RATE) / (1000LL * per)) + 1;
    if (ticks > 16) ticks = 16;          // a client that ignores us must not be flooded
    for (int i = 0; i < ticks; i++) tci_send_chrono((guint32)want);
    primed = TRUE;
  }

  acc += nsamples;
  while (acc >= per) {
    acc -= per;
    tci_send_chrono((guint32)want);
  }
}

// --- TX audio ingest -> mic substitution (transmitter.c:add_mic_sample) ------

// TRUE while a client is actively streaming TX audio and we are transmitting.
// Self-clears TCI_TX_ACTIVE_US after the last frame (or when MOX drops), so the
// real mic path is untouched unless a TCI client is driving TX right now.
gboolean tci_tx_active(void) {
  if (!g_atomic_int_get(&server_running)) return FALSE;
  if (g_radio == NULL || !g_radio->mox) return FALSE;
  return (g_get_monotonic_time() - tx_audio_last_us) < TCI_TX_ACTIVE_US;
}

// Pop one mono TX sample (0.0 on underrun). Called per-sample on the TX thread.
float tci_tx_next_sample(void) {
  // Lock-free single-consumer read off the audio (hot) path: producers publish
  // head atomically after the slot write, so reading head atomically then the
  // slot is safe without taking tx_ring_mutex per sample.
  float s = 0.0f;
  int tail = tx_ring_tail;                             // only this thread writes tail
  if (tail != g_atomic_int_get(&tx_ring_head)) {
    s = tx_ring[tail];
    g_atomic_int_set(&tx_ring_tail, (tail + 1) % TCI_TX_RING);
  }
  return s;
}
