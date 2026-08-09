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
 * VHF ACARS receive decoder — see acars_decoder.h for the design.
 */

#include <glib.h>
#include <string.h>
#include <math.h>

#include "acars_decoder.h"
#include "acars_demod.h"
#include "hfdl_msg.h"
#include "log.h"

// Known channels.  ARINC in North America, SITA in Europe, plus the worldwide
// primary; the 136.7–136.85 block is ARINC's newer North-American allocation.
// 136.9 upwards is left out on purpose — that is VDL Mode 2, a different link.
static const ACARS_CHANNEL_INFO channels[] = {
  { 129125, "N. America"  },
  { 130025, "N. America"  },
  { 130425, "N. America"  },
  { 130450, "N. America"  },
  { 130825, "N. America"  },
  { 131125, "N. America"  },
  { 131450, "Japan"       },
  { 131475, "Asia/Pac"    },
  { 131525, "Europe"      },
  { 131550, "Worldwide"   },
  { 131725, "Europe"      },
  { 131850, "Europe"      },
  { 136700, "N. America"  },
  { 136750, "N. America"  },
  { 136800, "N. America"  },
  { 136850, "N. America"  },
};

int acars_channel_count(void) { return (int)G_N_ELEMENTS(channels); }

const ACARS_CHANNEL_INFO *acars_channel_at(int i) {
  if (i < 0 || i >= (int)G_N_ELEMENTS(channels)) return NULL;
  return &channels[i];
}

// --- state (audio-thread-owned unless noted) --------------------------------

typedef struct {
  double       offset_hz;    // channel centre relative to the receiver centre
  guint32      khz;          // 0 = "just the dial"
  gboolean     is_tuned;     // follows the cursor rather than being a scan slot
  acars_demod *demod;
} ACARS_CHAN;

static volatile gint enabled     = 0;
static volatile gint rebuild_req = 0;

static ACARS_CHAN chans[ACARS_MAX_CHANNELS];
static int        nchans = 0;
static double     built_rate   = 0.0;
static long long  built_center = 0;
static long long  built_cursor = 0;
static gboolean   scan_band    = FALSE;

// A cursor move retunes the NCO in place; only a jump to another channel
// rebuilds.  Half the 25 kHz channel spacing is the natural line: anything
// closer is the same channel being nudged.
#define RETUNE_HZ 12500.0

static GMutex   lock;                    // guards everything below
static GString *pending = NULL;          // panel drain
static GString *history = NULL;          // non-draining peek for the readout
#define ACARS_HISTORY_MAX 4096
#define ACARS_PENDING_MAX 65536
static int      status_rate  = 0;
static double   status_level = -160.0;
static glong    status_msgs  = 0;        // blocks that passed the CRC
static glong    status_bad   = 0;        // blocks that did not
static gboolean status_fed   = FALSE;
static gboolean reset_pending = FALSE;

static gboolean echo = FALSE;
static gboolean env_read = FALSE;

// Aircraft table (audio thread writes, panel reads under `lock`).
#define ACARS_MAX_AC 128
static ACARS_AC_INFO ac_tab[ACARS_MAX_AC];
static int           ac_cnt = 0;

static void ensure_init(void) {
  if (pending == NULL) pending = g_string_new(NULL);
  if (history == NULL) history = g_string_new(NULL);
  if (!env_read) {
    echo = (g_getenv("MACHPSDR_ACARS_ECHO") != NULL);
    env_read = TRUE;
  }
}

static void chans_free(void) {
  for (int i = 0; i < nchans; i++)
    if (chans[i].demod) acars_demod_destroy(chans[i].demod);
  memset(chans, 0, sizeof(chans));
  nchans = 0;
  built_rate = 0.0;
  built_center = built_cursor = 0;
}

static void chan_add(double offset_hz, guint32 khz, double rate, gboolean tuned) {
  if (nchans >= ACARS_MAX_CHANNELS) return;
  acars_demod *d = acars_demod_create(rate, offset_hz);
  if (d == NULL) return;
  chans[nchans].offset_hz = offset_hz;
  chans[nchans].khz       = khz;
  chans[nchans].is_tuned  = tuned;
  chans[nchans].demod     = d;
  nchans++;
}

// Audio thread. Build the channel set for this centre / cursor / rate.
static void chans_build(double rate, long long center_hz, long long cursor_hz) {
  chans_free();
  if (cursor_hz <= 0) cursor_hz = center_hz;

  double cursor_off = (center_hz > 0 && cursor_hz > 0)
                      ? (double)(cursor_hz - center_hz) : 0.0;
  // Outside the passband there is nothing to decode; fall back to the centre
  // rather than pointing a front-end at empty spectrum.
  double usable = rate * 0.5 * 0.9;
  if (fabs(cursor_off) > usable) { cursor_off = 0.0; cursor_hz = center_hz; }
  chan_add(cursor_off, cursor_hz > 0 ? (guint32)(cursor_hz / 1000) : 0, rate, TRUE);

  built_rate   = rate;
  built_center = center_hz;
  built_cursor = cursor_hz;
  log_info("acars: tuned channel %lld Hz (%+.0f Hz from centre %lld), rate %.0f\n",
           cursor_hz, cursor_off, center_hz, rate);

  if (!scan_band || center_hz <= 0) return;

  // Every known channel inside the usable part of the passband. 80 % of it: the
  // outer edges are where the receiver's own filtering rolls off.
  double half = rate * 0.5 * 0.8;
  for (int i = 0; i < acars_channel_count() && nchans < ACARS_MAX_CHANNELS; i++) {
    double off = (double)((long long)channels[i].khz * 1000LL - center_hz);
    if (off < -half || off > half) continue;
    if (fabs(off - cursor_off) < 1000.0) continue;    // that is the tuned channel
    chan_add(off, channels[i].khz, rate, FALSE);
  }
  if (nchans > 1)
    log_info("acars: decoding %d channel(s) around %lld Hz\n", nchans, center_hz);
}

void acars_decoder_set_scan(gboolean on) {
  if (scan_band == (on != FALSE)) return;
  scan_band = (on != FALSE);
  g_atomic_int_set(&rebuild_req, 1);   // the audio thread owns the channels
}

gboolean acars_decoder_get_scan(void) { return scan_band; }

int acars_decoder_channel_count(void) { return nchans; }

void acars_decoder_set_enabled(gboolean on) {
  gint was = g_atomic_int_get(&enabled);
  if (on && !was) {
    g_mutex_lock(&lock);
    ensure_init();
    g_string_truncate(pending, 0);
    g_string_truncate(history, 0);
    status_rate  = 0;
    status_level = -160.0;
    status_fed   = FALSE;
    reset_pending = FALSE;
    gboolean do_echo = echo;
    g_mutex_unlock(&lock);
    chans_free();          // rebuilt lazily on the first feed, at the live rate
    log_info("acars: decoder enabled (VHF, 2400 bps MSK/AM)\n");
    if (do_echo) {
      g_printerr("[ACARS] enabled\n");
      if (g_getenv("MACHPSDR_ACARS_SELFTEST"))
        g_printerr("[ACARS] selftest: %s\n", acars_decoder_selftest() ? "PASS" : "FAIL");
    }
  } else if (!on && was) {
    chans_free();
    log_info("acars: decoder disabled\n");
  }
  g_atomic_int_set(&enabled, on ? 1 : 0);
}

// --- aircraft table ----------------------------------------------------------
//
// Keyed by registration, which is the only identity every downlink carries (the
// flight number is in the text and an uplink has neither).  Runs on the audio
// thread as a block is parsed, so it takes the lock the panel reads under.
static void ac_note(const char *reg, const char *flight, const char *label,
                    guint32 khz) {
  if (reg == NULL || reg[0] == '\0') return;
  g_mutex_lock(&lock);
  ACARS_AC_INFO *e = NULL;
  for (int i = 0; i < ac_cnt; i++)
    if (!strcmp(ac_tab[i].reg, reg)) { e = &ac_tab[i]; break; }
  if (e == NULL) {
    if (ac_cnt < ACARS_MAX_AC) e = &ac_tab[ac_cnt++];
    else {
      // Full: drop the least recently heard rather than the oldest seen.
      e = &ac_tab[0];
      for (int i = 1; i < ac_cnt; i++)
        if (ac_tab[i].last_heard_us < e->last_heard_us) e = &ac_tab[i];
      memset(e, 0, sizeof(*e));
    }
    memset(e, 0, sizeof(*e));
    g_strlcpy(e->reg, reg, sizeof(e->reg));
  }
  if (flight && flight[0]) g_strlcpy(e->flight, flight, sizeof(e->flight));
  if (label && label[0])   g_strlcpy(e->label, label, sizeof(e->label));
  e->khz = khz;
  e->last_heard_us = g_get_monotonic_time();
  e->msgs++;
  g_mutex_unlock(&lock);
}

int acars_decoder_ac_list(ACARS_AC_INFO *out, int max) {
  if (out == NULL || max <= 0) return 0;
  g_mutex_lock(&lock);
  int n = ac_cnt < max ? ac_cnt : max;
  // Most recently heard first — the panel shows a live picture, not a log.
  ACARS_AC_INFO tmp[ACARS_MAX_AC];
  memcpy(tmp, ac_tab, sizeof(ACARS_AC_INFO) * (size_t)ac_cnt);
  int cnt = ac_cnt;
  g_mutex_unlock(&lock);
  for (int i = 0; i < cnt - 1; i++)
    for (int j = i + 1; j < cnt; j++)
      if (tmp[j].last_heard_us > tmp[i].last_heard_us) {
        ACARS_AC_INFO t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
      }
  memcpy(out, tmp, sizeof(ACARS_AC_INFO) * (size_t)n);
  return n;
}

// The registration and flight the block carries, pulled straight out of the
// bytes rather than out of the rendered text: the layout is fixed (mode, 7-byte
// address, ack, 2-byte label, block id, STX, then on a downlink a 4-byte
// message number and a 6-byte flight id) and re-parsing prose would break the
// moment the renderer's wording changed.
static void block_identity(const guint8 *b, int len, char *reg, char *flight,
                           char *label) {
  reg[0] = flight[0] = label[0] = '\0';
  if (len < 14) return;
  const guint8 *p = b + 1;                      // past SOH
  char r[8];
  for (int i = 0; i < 7; i++) r[i] = (char)(p[1 + i] & 0x7f);
  r[7] = '\0';
  g_strchomp(r);
  const char *rr = (r[0] == '.') ? r + 1 : r;   // the address is sent dot-prefixed
  g_strlcpy(reg, rr, 8);
  label[0] = (char)(p[9] & 0x7f);
  label[1] = (char)(p[10] & 0x7f);
  label[2] = '\0';
  char block_id = (char)(p[11] & 0x7f);
  gboolean downlink = (block_id >= '0' && block_id <= '9');
  if (downlink && len >= 1 + 12 + 1 + 10 && (p[12] & 0x7f) == 0x02) {
    char f[7];
    for (int i = 0; i < 6; i++) f[i] = (char)(p[17 + i] & 0x7f);
    f[6] = '\0';
    g_strchomp(f);
    g_strlcpy(flight, f, 8);
  }
}

// --- block sink --------------------------------------------------------------

typedef struct {
  guint32  khz;         // channel this block came in on (0 = the dial)
  gboolean label_chan;  // prefix the line with the channel (scanning several)
  GString *text;        // rendered decode, allocated lazily
  int      good, bad;
} block_sink;

static void on_block(void *ctx, const acars_block_t *blk) {
  block_sink *s = ctx;
  if (s->text == NULL) s->text = g_string_new(NULL);

  // Header line: when it was heard, where, how strong, and whether it checked
  // out.  A decoder that prints only the message loses the two things that tell
  // an operator whether the channel is worth listening to.
  GDateTime *now = g_date_time_new_now_utc();
  char *ts = g_date_time_format(now, "%H:%M:%S");
  g_date_time_unref(now);
  g_string_append_printf(s->text, "%s", ts ? ts : "--:--:--");
  g_free(ts);
  if (s->label_chan && s->khz)
    g_string_append_printf(s->text, "  [%u.%03u MHz]", s->khz / 1000, s->khz % 1000);
  g_string_append_printf(s->text, "  %.0f dB", blk->level_db);
  if (blk->corrected)   g_string_append(s->text, "  (1 bit corrected)");
  if (!blk->crc_ok)     g_string_append(s->text, "  CRC FAIL");
  g_string_append_c(s->text, '\n');

  if (!blk->crc_ok) {
    // A block that fails the CRC after repair is not shown as a message: unlike
    // an HFDL frame (where a hex dump is still worth seeing) the payload here is
    // text, and printing corrupt text as if it were a message is how a decoder
    // earns distrust.
    s->bad++;
    return;
  }
  s->good++;

  char reg[8], flight[8], label[3];
  block_identity(blk->bytes, blk->len, reg, flight, label);
  ac_note(reg, flight, label, s->khz);

  // Everything above the block is HFDL's application layer, unchanged: message
  // header, multi-block reassembly, and the ATS applications inside the text.
  if (!hfdl_msg_acars_block(blk->bytes, blk->len, s->text, 0))
    g_string_append(s->text, "  (unparsable block)\n");
}

// --- feed --------------------------------------------------------------------

// Apply a reset the GTK thread asked for.  This has to happen BEFORE the buffer
// is processed, not after it: a block decoded out of this very buffer would
// otherwise be counted and then wiped by a reset that was requested before it
// ever arrived (which is exactly what emptied the aircraft table in the
// self-test).
static void apply_reset(void) {
  g_mutex_lock(&lock);
  ensure_init();
  if (reset_pending) {
    g_string_truncate(pending, 0);
    g_string_truncate(history, 0);
    status_msgs = status_bad = 0;
    ac_cnt = 0;
    reset_pending = FALSE;
    hfdl_msg_reset();
  }
  g_mutex_unlock(&lock);
}

static void publish(block_sink *s, int sample_rate, double level) {
  g_mutex_lock(&lock);
  ensure_init();
  status_rate  = sample_rate;
  status_level = level;
  status_fed   = TRUE;
  status_msgs += s->good;
  status_bad  += s->bad;
  if (s->text != NULL && s->text->len > 0) {
    g_string_append(pending, s->text->str);
    if (pending->len > ACARS_PENDING_MAX)
      g_string_erase(pending, 0, pending->len - ACARS_PENDING_MAX);
    g_string_append(history, s->text->str);
    if (history->len > ACARS_HISTORY_MAX)
      g_string_erase(history, 0, history->len - ACARS_HISTORY_MAX);
  }
  gboolean do_echo = echo;
  g_mutex_unlock(&lock);

  if (do_echo && s->text != NULL && s->text->len > 0)
    g_printerr("%s", s->text->str);
  if (s->text != NULL) g_string_free(s->text, TRUE);
  s->text = NULL;
}

void acars_decoder_add_iq(const double *iq, int nframes, int sample_rate,
                          long long centre_hz, long long cursor_hz) {
  if (!g_atomic_int_get(&enabled) || iq == NULL || nframes <= 0 || sample_rate <= 0)
    return;
  if (cursor_hz <= 0) cursor_hz = centre_hz;
  apply_reset();

  double moved = fabs((double)(cursor_hz - built_cursor));
  if (nchans == 0 || built_rate != (double)sample_rate ||
      built_center != centre_hz || moved > RETUNE_HZ ||
      g_atomic_int_get(&rebuild_req)) {
    g_atomic_int_set(&rebuild_req, 0);
    chans_build((double)sample_rate, centre_hz, cursor_hz);
  } else if (moved > 0.0) {
    // Small move: follow it without dropping filter state, so a nudge of the
    // dial never costs the block being received.
    double off = (double)(cursor_hz - centre_hz);
    for (int c = 0; c < nchans; c++) {
      if (!chans[c].is_tuned) continue;
      chans[c].offset_hz = off;
      chans[c].khz       = (guint32)(cursor_hz / 1000);
      acars_demod_set_offset(chans[c].demod, off);
    }
    built_cursor = cursor_hz;
  }

  block_sink s = { 0, FALSE, NULL, 0, 0 };
  double level = -160.0;
  for (int c = 0; c < nchans; c++) {
    s.khz        = chans[c].khz;
    s.label_chan = (nchans > 1);
    acars_demod_process_iq(chans[c].demod, iq, nframes, on_block, &s);
    if (c == 0) level = acars_demod_level_db(chans[c].demod);
  }
  publish(&s, sample_rate, level);
}

// Offline entry point: one channel of already-demodulated AM audio.
static acars_demod *audio_demod = NULL;
static double       audio_rate  = 0.0;

void acars_decoder_add_audio(const double *samples, int nframes, int stride,
                             double rate) {
  if (!g_atomic_int_get(&enabled) || samples == NULL || nframes <= 0) return;
  apply_reset();
  if (audio_demod == NULL || audio_rate != rate) {
    if (audio_demod) acars_demod_destroy(audio_demod);
    audio_demod = acars_demod_create_audio(rate);
    audio_rate  = rate;
    if (audio_demod == NULL) return;
  }
  block_sink s = { 0, FALSE, NULL, 0, 0 };
  acars_demod_process_audio(audio_demod, samples, nframes, stride, on_block, &s);
  publish(&s, (int)rate, acars_demod_level_db(audio_demod));
}

// --- published state ---------------------------------------------------------

int acars_decoder_get_messages(char *buf, int buflen) {
  if (buf == NULL || buflen <= 0) return 0;
  g_mutex_lock(&lock);
  ensure_init();
  int n = (int)MIN((gsize)(buflen - 1), pending->len);
  if (n > 0) {
    memcpy(buf, pending->str, (size_t)n);
    g_string_erase(pending, 0, n);
  }
  buf[n] = '\0';
  g_mutex_unlock(&lock);
  return n;
}

int acars_decoder_get_recent(char *buf, int buflen) {
  if (buf == NULL || buflen <= 0) return 0;
  g_mutex_lock(&lock);
  ensure_init();
  int take = (int)MIN((gsize)(buflen - 1), history->len);
  memcpy(buf, history->str + history->len - take, (size_t)take);
  buf[take] = '\0';
  g_mutex_unlock(&lock);
  return take;
}

void acars_decoder_get_status(gboolean *listening, int *sample_rate, glong *msgs) {
  g_mutex_lock(&lock);
  ensure_init();
  if (listening)   *listening   = g_atomic_int_get(&enabled) && status_fed;
  if (sample_rate) *sample_rate = status_rate;
  if (msgs)        *msgs        = status_msgs;
  g_mutex_unlock(&lock);
}

double acars_decoder_get_level_db(void) {
  g_mutex_lock(&lock);
  double v = status_level;
  g_mutex_unlock(&lock);
  return v;
}

glong acars_decoder_get_messages_count(void) {
  g_mutex_lock(&lock);
  glong v = status_msgs;
  g_mutex_unlock(&lock);
  return v;
}

glong acars_decoder_get_bad_count(void) {
  g_mutex_lock(&lock);
  glong v = status_bad;
  g_mutex_unlock(&lock);
  return v;
}

void acars_decoder_get_tuned(long long *cursor_hz, double *offset_hz) {
  // Audio-thread-owned scalars; a torn read here only misprints a readout.
  if (cursor_hz) *cursor_hz = built_cursor;
  if (offset_hz) *offset_hz = (nchans > 0) ? chans[0].offset_hz : 0.0;
}

void acars_decoder_reset(void) {
  g_mutex_lock(&lock);
  ensure_init();
  g_string_truncate(pending, 0);
  reset_pending = TRUE;    // counters/tables cleared on the next audio-thread feed
  status_fed = FALSE;
  g_mutex_unlock(&lock);
}

// --- self-test ----------------------------------------------------------------

gboolean acars_decoder_selftest(void) {
  gboolean ok = acars_demod_selftest();

  // Full stack: a real downlink block, modulated onto a carrier 30 kHz off
  // centre at 192 kHz, read back as rendered message text.
  static const char *body = "M01AAF447/OPS NORMAL FL350";
  guint8 payload[128];
  int n = 0;
  memcpy(payload, "2.F-GTAE\x15" "H1" "4", 12); n = 12;
  payload[n++] = 0x02;                                  // STX
  memcpy(payload + n, body, strlen(body)); n += (int)strlen(body);
  payload[n++] = 0x03;                                  // ETX

  guint8 frame[192];
  int flen = acars_demod_build_frame(frame, payload, n);
  double rate = 192000.0;
  int max = (int)(rate * 1.0);
  double *env = g_new0(double, (size_t)max);
  int ns = acars_demod_modulate(env, max, frame, flen, rate);
  double *iq = g_new0(double, (size_t)2 * ns);
  double ph = 0.0, dph = 2.0 * M_PI * 30000.0 / rate;
  for (int i = 0; i < ns; i++) {
    double a = 0.5 + 0.4 * env[i];
    iq[2 * i]     = a * sin(ph);      // (Q, I) — the receiver's buffer order
    iq[2 * i + 1] = a * cos(ph);
    ph += dph; if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
  }

  // Run it through the real entry point, with the cursor 30 kHz above centre.
  gboolean was = g_atomic_int_get(&enabled);
  acars_decoder_set_enabled(TRUE);
  acars_decoder_reset();
  acars_decoder_add_iq(iq, ns, (int)rate, 131550000LL, 131580000LL);
  char buf[8192];
  acars_decoder_get_messages(buf, sizeof(buf));
  if (strstr(buf, "F-GTAE") == NULL || strstr(buf, "OPS NORMAL FL350") == NULL ||
      strstr(buf, "CRC FAIL") != NULL) {
    g_printerr("[ACARS selftest] full stack: message not recovered\n%s\n", buf);
    ok = FALSE;
  }
  ACARS_AC_INFO ac[4];
  int nac = acars_decoder_ac_list(ac, 4);
  if (nac != 1 || strcmp(ac[0].reg, "F-GTAE") != 0) {
    g_printerr("[ACARS selftest] aircraft table: %d entr(ies), first '%s'\n",
               nac, nac > 0 ? ac[0].reg : "");
    ok = FALSE;
  }
  acars_decoder_reset();
  if (!was) acars_decoder_set_enabled(FALSE);

  g_free(iq);
  g_free(env);
  return ok;
}
