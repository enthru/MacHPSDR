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

#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <gtk/gtk.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"

#include "ft8_decoder.h"
#include "ft8_encoder.h"
#include "ft8_qso.h"
#include "ft8_udp.h"

// QSO sequence states.  "ANS" = we answered a CQ; "CQ" = we called CQ.
typedef enum {
  ST_IDLE,
  ST_CALLING_CQ,       // sending "CQ MYCALL GRID", waiting for an answer
  ST_CQ_SENT_REPORT,   // answered -> sent "DX MYCALL -rpt", waiting for R-rpt
  ST_CQ_SENT_RR73,     // sent "DX MYCALL RR73", waiting for 73 (or done)
  ST_ANS_SENT_GRID,    // sent "DX MYCALL GRID", waiting for a report
  ST_ANS_SENT_RREPORT, // sent "DX MYCALL R-rpt", waiting for RR73
  ST_ANS_SENT_73,      // sent "DX MYCALL 73" (final, one shot)
  ST_FREE              // repeating a manual free-text message
} qso_state_t;

#define WATCHDOG_CYCLES 5     // give up after this many of our TX with no progress

static qso_state_t state = ST_IDLE;
static char   my_call[16] = "";
static char   my_grid[8]  = "";
static char   dx_call[16] = "";
static char   dx_grid[8]  = "";
static int    sent_report = 0;   // report we send the DX (from received SNR)
static int    recv_report = 0;   // report the DX sends us
static gboolean tx_even = TRUE;
static gboolean tx_enabled = FALSE; // master TX gate (WSJT-X "Enable Tx")
static gboolean auto_seq = TRUE;    // auto-advance the sequence on RX ("Auto Seq")

static char   pending_msg[32]  = "";  // message to (re)transmit each of our slots
static char   prepared_msg[32] = "";  // last message actually synthesized
static gboolean have_pending = FALSE;
static gboolean need_arm = FALSE;      // (re)arm the scheduler on the next poll

static char   last_utc[8] = "";       // decoder slot label last processed
static gboolean prev_active = FALSE;   // ft8_tx_active() on the previous poll
static int    cycles = 0;             // our TX completions since last progress
static gboolean logged = FALSE;        // this QSO already written to the log
static char   status[96] = "Idle";
static guint  poll_id = 0;

#define TX_WATCHDOG_SEC (6*60)         // auto-disable Tx after this long w/o progress
static time_t tx_enabled_since = 0;    // when Tx was last enabled or progressed
static gboolean prev_txen = FALSE;      // tx_enabled on the previous poll

// Set of callsigns already logged (worked-before), loaded from the ADIF log and
// kept updated as QSOs complete.  Keys are uppercased g_strdup'd callsigns.
static GHashTable *worked = NULL;

static void worked_add(const char *call) {
  if (!worked || !call || !call[0]) return;
  char up[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up[j++] = g_ascii_toupper(call[i]);
  up[j] = '\0';
  if (!g_hash_table_contains(worked, up)) g_hash_table_add(worked, g_strdup(up));
}

// Extract every "<CALL:len>value" from one ADIF line into the worked set.
static void worked_parse_line(const char *line) {
  const char *p = line;
  while ((p = strstr(p, "<CALL:")) != NULL) {
    p += 6;
    int len = atoi(p);
    const char *gt = strchr(p, '>');
    if (gt == NULL || len <= 0) break;
    const char *val = gt + 1;
    char call[16];
    int n = len < 15 ? len : 15;
    strncpy(call, val, n);
    call[n] = '\0';
    worked_add(call);
    p = val + len;
  }
}

static void worked_load(void) {
  worked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char path[512];
  snprintf(path, sizeof(path), "%s/.local/share/machpsdr/ft8_log.adi", g_get_home_dir());
  FILE *f = fopen(path, "r");
  if (f == NULL) return;
  char line[1024];
  while (fgets(line, sizeof(line), f) != NULL) worked_parse_line(line);
  fclose(f);
}

gboolean ft8_qso_worked(const char *call) {
  if (!worked || !call || !call[0]) return FALSE;
  char up[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up[j++] = g_ascii_toupper(call[i]);
  up[j] = '\0';
  return g_hash_table_contains(worked, up);
}

// ---- small text helpers ----------------------------------------------------
static gboolean is_grid(const char *s) {
  int n = (int)strlen(s);
  if (n != 4 && n != 6) return FALSE;
  if (s[0] < 'A' || s[0] > 'R' || s[1] < 'A' || s[1] > 'R') return FALSE;
  if (s[2] < '0' || s[2] > '9' || s[3] < '0' || s[3] > '9') return FALSE;
  return TRUE;
}
static gboolean is_report(const char *s) {           // "-12" / "+05"
  return (s[0] == '-' || s[0] == '+') && g_ascii_isdigit(s[1]);
}
static gboolean has_roger(const char *s) {           // "R-12" / "R+05"
  return s[0] == 'R' && (s[1] == '-' || s[1] == '+');
}
static gboolean is_rr73(const char *s) {
  return strcmp(s, "RR73") == 0 || strcmp(s, "RRR") == 0;
}
static gboolean is_73(const char *s) {
  return strcmp(s, "73") == 0;
}
static int parse_report(const char *s) {             // "-12"/"R-09" -> int
  if (s[0] == 'R') s++;
  return atoi(s);
}
static int clamp_report(float snr) {
  int r = (int)lroundf(snr);
  if (r < -24) r = -24;
  if (r > 20) r = 20;
  return r;
}
static gboolean same_dx(const char *call) {
  return dx_call[0] && strcasecmp(call, dx_call) == 0;
}

// Slot parity from a decoder "hhmmss" label.  There are 5760 (even) 15 s slots
// per UTC day, so parity of seconds-of-day/15 == parity of epoch/15.
static gboolean slot_even_from_utc(const char *utc) {
  int hh = (utc[0]-'0')*10 + (utc[1]-'0');
  int mm = (utc[2]-'0')*10 + (utc[3]-'0');
  int ss = (utc[4]-'0')*10 + (utc[5]-'0');
  long slot = (hh*3600L + mm*60L + ss) / 15;
  return (slot % 2) == 0;
}

// ---- outgoing message construction -----------------------------------------
static void set_pending(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(pending_msg, sizeof(pending_msg), fmt, ap);
  va_end(ap);
  have_pending = TRUE;
  need_arm = TRUE;   // a new outgoing message -> queue it for our next slot
}

static void msg_cq(void) {
  if (my_grid[0]) set_pending("CQ %s %s", my_call, my_grid);
  else            set_pending("CQ %s", my_call);
}
static void msg_grid(void)    { set_pending("%s %s %s", dx_call, my_call, my_grid); }
static void msg_report(void)  { set_pending("%s %s %+03d", dx_call, my_call, sent_report); }
static void msg_rreport(void) { set_pending("%s %s R%+03d", dx_call, my_call, sent_report); }
static void msg_rr73(void)    { set_pending("%s %s RR73", dx_call, my_call); }
static void msg_73(void)      { set_pending("%s %s 73", dx_call, my_call); }

static void progress(void) { cycles = 0; tx_enabled_since = time(NULL); }

// ---- ADIF logging ----------------------------------------------------------
static void adif_field(char **p, const char *name, const char *val) {
  *p += sprintf(*p, "<%s:%d>%s ", name, (int)strlen(val), val);
}

static void log_qso(void) {
  if (logged) return;
  logged = TRUE;

  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char date[9], tm_on[7], rs[8], rr[8], freq[16];
  snprintf(date, sizeof(date), "%04d%02d%02d", tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday);
  snprintf(tm_on, sizeof(tm_on), "%02d%02d%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  snprintf(rs, sizeof(rs), "%+03d", sent_report);
  snprintf(rr, sizeof(rr), "%+03d", recv_report);
  double mhz = 0.0;
  if (radio && radio->active_receiver)
    mhz = (double)(radio->active_receiver->frequency_a + radio->ft8_tx_offset) / 1.0e6;
  snprintf(freq, sizeof(freq), "%.6f", mhz);

  char rec[512], *p = rec;
  adif_field(&p, "CALL", dx_call);
  if (dx_grid[0]) adif_field(&p, "GRIDSQUARE", dx_grid);
  adif_field(&p, "MODE", "FT8");
  adif_field(&p, "RST_SENT", rs);
  adif_field(&p, "RST_RCVD", rr);
  adif_field(&p, "QSO_DATE", date);
  adif_field(&p, "TIME_ON", tm_on);
  if (mhz > 0.0) adif_field(&p, "FREQ", freq);
  p += sprintf(p, "<EOR>\n");

  char path[512];
  snprintf(path, sizeof(path), "%s/.local/share/machpsdr/ft8_log.adi", g_get_home_dir());
  FILE *f = fopen(path, "a");
  if (f) { fputs(rec, f); fclose(f); }
  ft8_udp_log(rec);   // also push to a network logger (JTDX-style), if enabled
  worked_add(dx_call);
  fprintf(stderr, "ft8-qso: logged %s\n", dx_call);
}

// ---- state control ---------------------------------------------------------
static void go_idle(const char *why) {
  state = ST_IDLE;
  have_pending = FALSE;
  need_arm = FALSE;
  tx_enabled = FALSE;
  pending_msg[0] = prepared_msg[0] = '\0';
  cycles = 0;
  ft8_tx_disarm();
  snprintf(status, sizeof(status), "%s", why ? why : "Idle");
}

static void set_dx(const char *call, const char *grid) {
  snprintf(dx_call, sizeof(dx_call), "%s", call);
  snprintf(dx_grid, sizeof(dx_grid), "%s", is_grid(grid) ? grid : "");
}

// Process one decoded message against the current QSO state.
static void handle_decode(const FT8_DECODE *d) {
  if (!auto_seq) return;   // manual operation: the user drives Tx selection
  if (d->call_de[0] == '\0' || my_call[0] == '\0') return;
  gboolean to_me = strcasecmp(d->call_to, my_call) == 0;
  if (!to_me) return;

  switch (state) {
    case ST_CALLING_CQ:
      // Someone answered: "MYCALL DXCALL GRID".
      set_dx(d->call_de, d->extra);
      sent_report = clamp_report(d->snr);
      msg_report();
      state = ST_CQ_SENT_REPORT;
      snprintf(status, sizeof(status), "Working %s: sent %+03d", dx_call, sent_report);
      progress();
      break;

    case ST_CQ_SENT_REPORT:
      if (!same_dx(d->call_de)) break;
      if (has_roger(d->extra) || is_report(d->extra) || is_rr73(d->extra)) {
        if (has_roger(d->extra) || is_report(d->extra)) recv_report = parse_report(d->extra);
        msg_rr73();
        log_qso();
        state = ST_CQ_SENT_RR73;
        snprintf(status, sizeof(status), "Working %s: RR73 (rcvd %+03d)", dx_call, recv_report);
        progress();
      }
      break;

    case ST_CQ_SENT_RR73:
      if (same_dx(d->call_de) && (is_73(d->extra) || is_rr73(d->extra)))
        go_idle("QSO complete");
      break;

    case ST_ANS_SENT_GRID:
      if (!same_dx(d->call_de)) break;
      if (is_rr73(d->extra) || is_73(d->extra)) {          // fast finish
        recv_report = 0;
        msg_73();
        log_qso();
        state = ST_ANS_SENT_73;
        snprintf(status, sizeof(status), "Working %s: 73", dx_call);
        progress();
      } else if (is_report(d->extra) && !has_roger(d->extra)) {
        recv_report = parse_report(d->extra);
        sent_report = clamp_report(d->snr);
        msg_rreport();
        state = ST_ANS_SENT_RREPORT;
        snprintf(status, sizeof(status), "Working %s: R%+03d (rcvd %+03d)",
                 dx_call, sent_report, recv_report);
        progress();
      }
      break;

    case ST_ANS_SENT_RREPORT:
      if (!same_dx(d->call_de)) break;
      if (is_rr73(d->extra) || is_73(d->extra) || has_roger(d->extra)) {
        msg_73();
        log_qso();
        state = ST_ANS_SENT_73;
        snprintf(status, sizeof(status), "Working %s: 73", dx_call);
        progress();
      }
      break;

    default:
      break;
  }
}

// ---- poll timer (GTK main thread) ------------------------------------------
static gboolean qso_poll(gpointer data) {
  if (state == ST_IDLE) { prev_active = FALSE; prev_txen = FALSE; return G_SOURCE_CONTINUE; }

  // 0) Tx watchdog: auto-disable Tx after a long stretch with no progress so a
  // forgotten CQ or unanswered call can't key the rig indefinitely.
  if (tx_enabled && !prev_txen) tx_enabled_since = time(NULL);
  prev_txen = tx_enabled;
  if (tx_enabled && (time(NULL) - tx_enabled_since) > TX_WATCHDOG_SEC) {
    ft8_qso_set_tx_enabled(FALSE);
    snprintf(status, sizeof(status), "Tx watchdog — disabled");
  }

  // 1) Process a freshly completed slot's decodes exactly once.
  FT8_DECODE d[64];
  char utc[8] = "";
  int n = ft8_decoder_get_decodes(d, 64, utc);
  if (n > 0 && utc[0] && strcmp(utc, last_utc) != 0) {
    snprintf(last_utc, sizeof(last_utc), "%s", utc);
    for (int i = 0; i < n && state != ST_IDLE; i++) handle_decode(&d[i]);
  }

  // 2) React to the end of one of our transmissions.
  gboolean act = ft8_tx_active();
  if (prev_active && !act) {
    cycles++;
    if (state == ST_ANS_SENT_73) {
      go_idle("QSO complete");                 // final 73 sent once
    } else if (auto_seq && state != ST_CALLING_CQ && state != ST_FREE &&
               cycles >= WATCHDOG_CYCLES) {
      go_idle("No reply — stopped");
    } else if (have_pending) {
      need_arm = TRUE;                         // repeat the current message next cycle
    }
  }
  prev_active = act;

  // 3) (Re)arm the pending message once per cycle for our next matching slot.
  // Arming exactly once (not every poll) keeps the scheduler's slot anchor
  // stable, so we never accidentally skip our own slot.  Gated by Enable Tx.
  if (need_arm && have_pending && tx_enabled && !act && state != ST_IDLE) {
    if (strcmp(pending_msg, prepared_msg) != 0) {
      if (ft8_tx_prepare(pending_msg, (float)radio->ft8_tx_offset))
        snprintf(prepared_msg, sizeof(prepared_msg), "%s", pending_msg);
    }
    // Arm only when a valid waveform for the current message is ready; a
    // non-standard call may be unencodable for this step (grid/report), in
    // which case we surface it rather than transmitting a stale waveform.
    if (strcmp(pending_msg, prepared_msg) == 0) {
      ft8_tx_arm(tx_even);
      need_arm = FALSE;
    } else {
      snprintf(status, sizeof(status), "Can't encode: %s", pending_msg);
    }
  }
  return G_SOURCE_CONTINUE;
}

// ===========================================================================
// Public API
// ===========================================================================
void ft8_qso_init(void) {
  worked_load();
  if (poll_id == 0) poll_id = g_timeout_add(500, qso_poll, NULL);
}

void ft8_qso_start_cq(void) {
  if (!radio || radio->station_call[0] == '\0') {
    snprintf(status, sizeof(status), "Set your callsign first");
    return;
  }
  snprintf(my_call, sizeof(my_call), "%s", radio->station_call);
  snprintf(my_grid, sizeof(my_grid), "%s", radio->station_grid);
  dx_call[0] = dx_grid[0] = '\0';
  sent_report = recv_report = 0;
  tx_even = radio->ft8_tx_even;
  logged = FALSE;
  prepared_msg[0] = '\0';
  last_utc[0] = '\0';
  cycles = 0;
  state = ST_CALLING_CQ;
  tx_enabled = TRUE;
  msg_cq();
  snprintf(status, sizeof(status), "Calling CQ");
}

void ft8_qso_answer(const FT8_DECODE *d) {
  if (!radio || radio->station_call[0] == '\0') {
    snprintf(status, sizeof(status), "Set your callsign first");
    return;
  }
  if (d == NULL || d->call_de[0] == '\0') return;
  snprintf(my_call, sizeof(my_call), "%s", radio->station_call);
  snprintf(my_grid, sizeof(my_grid), "%s", radio->station_grid);
  if (my_grid[0] == '\0') {
    snprintf(status, sizeof(status), "Set your grid to answer");
    return;
  }
  set_dx(d->call_de, d->extra);
  sent_report = recv_report = 0;
  // Transmit in the slot opposite the one we heard the DX in.
  tx_even = !slot_even_from_utc(d->utc);
  logged = FALSE;
  prepared_msg[0] = '\0';
  last_utc[0] = '\0';
  cycles = 0;
  state = ST_ANS_SENT_GRID;
  tx_enabled = TRUE;
  msg_grid();
  snprintf(status, sizeof(status), "Answering %s", dx_call);
}

void ft8_qso_halt(void) {
  go_idle("Idle");
}

gboolean ft8_qso_active(void) {
  return state != ST_IDLE;
}

const char *ft8_qso_status(void) {
  return status;
}

const char *ft8_qso_next_tx(void) {
  return have_pending ? pending_msg : "";
}

const char *ft8_qso_dx_call(void) {
  return dx_call;
}

void ft8_qso_set_tx_enabled(gboolean en) {
  tx_enabled = en;
  if (!en) {
    ft8_tx_disarm();
    if (state != ST_IDLE) snprintf(status, sizeof(status), "Tx disabled");
  } else if (have_pending) {
    need_arm = TRUE;
  }
}
gboolean ft8_qso_tx_enabled(void) { return tx_enabled; }

void ft8_qso_set_auto(gboolean en) { auto_seq = en; }
gboolean ft8_qso_auto(void) { return auto_seq; }

// Build the six standard messages (Tx1..Tx6) into out[0..5] from the current
// call/grid/DX/report context.  Returns the 1-based index of the message that
// equals the currently queued one, or 0 if none.
int ft8_qso_messages(char out[6][32]) {
  const char *mc = (radio && radio->station_call[0]) ? radio->station_call : my_call;
  const char *mg = (radio && radio->station_grid[0]) ? radio->station_grid : my_grid;
  int sr = sent_report ? sent_report : -15;   // placeholder until measured
  if (dx_call[0]) {
    snprintf(out[0], 32, "%s %s %s",  dx_call, mc, mg);
    snprintf(out[1], 32, "%s %s %+03d",  dx_call, mc, sr);
    snprintf(out[2], 32, "%s %s R%+03d", dx_call, mc, sr);
    snprintf(out[3], 32, "%s %s RR73", dx_call, mc);
    snprintf(out[4], 32, "%s %s 73",   dx_call, mc);
  } else {
    for (int i = 0; i < 5; i++) out[i][0] = '\0';
  }
  if (mg[0]) snprintf(out[5], 32, "CQ %s %s", mc, mg);
  else       snprintf(out[5], 32, "CQ %s", mc);

  for (int i = 0; i < 6; i++)
    if (have_pending && strcmp(out[i], pending_msg) == 0) return i + 1;
  return 0;
}

// Manually queue Tx message idx (1..6), setting the matching sequence state so
// Auto Seq (if on) continues from there.  Tx6 (CQ) needs a callsign; Tx1..Tx5
// need a selected DX station.
void ft8_qso_select_tx(int idx) {
  if (!radio || radio->station_call[0] == '\0') {
    snprintf(status, sizeof(status), "Set your callsign first");
    return;
  }
  snprintf(my_call, sizeof(my_call), "%s", radio->station_call);
  snprintf(my_grid, sizeof(my_grid), "%s", radio->station_grid);

  if (idx == 6) {                              // CQ: same as Call CQ but keep dx
    tx_even = radio->ft8_tx_even;
    logged = FALSE; prepared_msg[0] = '\0'; cycles = 0;
    dx_call[0] = dx_grid[0] = '\0';
    state = ST_CALLING_CQ;
    msg_cq();
    snprintf(status, sizeof(status), "Calling CQ");
    return;
  }
  if (dx_call[0] == '\0') {
    snprintf(status, sizeof(status), "Pick a station first");
    return;
  }
  if (sent_report == 0) sent_report = -15;
  logged = FALSE; prepared_msg[0] = '\0'; cycles = 0;
  switch (idx) {
    case 1: state = ST_ANS_SENT_GRID;    msg_grid();    break;
    case 2: state = ST_CQ_SENT_REPORT;   msg_report();  break;
    case 3: state = ST_ANS_SENT_RREPORT; msg_rreport(); break;
    case 4: state = ST_CQ_SENT_RR73;     msg_rr73();    break;
    case 5: state = ST_ANS_SENT_73;      msg_73();      break;
    default: return;
  }
  snprintf(status, sizeof(status), "Tx%d -> %s", idx, dx_call);
}

// Queue an arbitrary free-text message (up to 13 chars, uppercased) and start
// repeating it each of our slots until halted.  Enables Tx.
void ft8_qso_send_free(const char *text) {
  if (text == NULL || text[0] == '\0') return;
  char msg[16];
  int j = 0;
  for (int i = 0; text[i] && j < 13; i++) msg[j++] = (char)g_ascii_toupper(text[i]);
  msg[j] = '\0';
  logged = FALSE; prepared_msg[0] = '\0'; cycles = 0;
  state = ST_FREE;
  tx_enabled = TRUE;
  set_pending("%s", msg);
  snprintf(status, sizeof(status), "Free: %s", msg);
}
