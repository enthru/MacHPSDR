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
 * DX cluster telnet client (P4.2). Connects to a DX cluster host in its own
 * GThread, does the login handshake, parses "DX de" spot lines into a
 * mutex-guarded ring, ages out stale spots, and auto-reconnects on drop.
 *
 * Threading: the socket thread never touches GTK. It only mutates the
 * mutex-guarded spot store; a redraw is requested via g_idle_add() so the
 * actual widget touch happens on the GTK main thread.
 */

#include "net_compat.h"   // must precede gtk.h: winsock2 before windows.h
#include <gtk/gtk.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "ft8_dxcc.h"
#include "log.h"
#include "dxcluster.h"

#define DXCLUSTER_SPOT_MAX_AGE_SEC 900     // 15 minutes
#define DXCLUSTER_RETRY_SEC        5
#define DXCLUSTER_DUP_TOLERANCE_HZ 500
#define DXCLUSTER_CONNECT_TIMEOUT_SEC 10   // cap a single connect() attempt

static RADIO   *g_radio = NULL;

static GMutex   spot_mutex;
static DX_SPOT  spots[DXCLUSTER_MAX_SPOTS];
static int      nspots = 0;

static GMutex   status_mutex;
static char     status_buf[128] = "disconnected";

static GMutex   sock_mutex;
static int      cluster_sock = -1;

static gint     thread_running = 0;   // accessed via g_atomic_int_*
static GThread *cluster_thread = NULL;

// Connection config is snapshotted from RADIO in dxcluster_start() (called only
// while the thread is stopped, so there's no reader) and thereafter read only by
// the socket thread. This avoids a data race with the Configure dialog rewriting
// g_radio->cluster_host/login live; edits take effect on the next start (i.e. the
// next enable-toggle or restart), which is the expected UX.
static char     cfg_host[64];
static int      cfg_port;
static char     cfg_login[16];

// ---------------------------------------------------------------------------
// status helpers
// ---------------------------------------------------------------------------

static void set_status(const char *s) {
  g_mutex_lock(&status_mutex);
  g_strlcpy(status_buf, s, sizeof(status_buf));
  g_mutex_unlock(&status_mutex);
}

const char *dxcluster_status(void) {
  static char out[128];
  g_mutex_lock(&status_mutex);
  g_strlcpy(out, status_buf, sizeof(out));
  g_mutex_unlock(&status_mutex);
  return out;
}

gboolean dxcluster_running(void) {
  return g_atomic_int_get(&thread_running) != 0;
}

// ---------------------------------------------------------------------------
// spot store (mutex-guarded)
// ---------------------------------------------------------------------------

// Caller must already hold spot_mutex.
static void age_out_locked(time_t now) {
  int w = 0;
  for (int i = 0; i < nspots; i++) {
    if (spots[i].utc >= now - DXCLUSTER_SPOT_MAX_AGE_SEC) {
      if (w != i) spots[w] = spots[i];
      w++;
    }
  }
  nspots = w;
}

void dxcluster_lock(void) {
  g_mutex_lock(&spot_mutex);
  age_out_locked(time(NULL));
}

void dxcluster_unlock(void) {
  g_mutex_unlock(&spot_mutex);
}

int dxcluster_count(void) {
  return nspots;
}

const DX_SPOT *dxcluster_spot(int i) {
  if (i < 0 || i >= nspots) return NULL;
  return &spots[i];
}

int dxcluster_nearest(long long freq, long long tol_hz, long long *out_freq) {
  int found = 0;
  long long best_diff = tol_hz + 1;
  long long best_freq = 0;

  g_mutex_lock(&spot_mutex);
  age_out_locked(time(NULL));
  for (int i = 0; i < nspots; i++) {
    long long diff = llabs(spots[i].freq - freq);
    if (diff <= tol_hz && diff < best_diff) {
      best_diff = diff;
      best_freq = spots[i].freq;
      found = 1;
    }
  }
  g_mutex_unlock(&spot_mutex);

  if (found && out_freq != NULL) *out_freq = best_freq;
  return found;
}

// Request a redraw of every receiver's panadapter on the GTK main thread.
static gboolean dxcluster_redraw_idle(gpointer data) {
  (void)data;
  if (g_radio != NULL) {
    for (int i = 0; i < g_radio->receivers; i++) {
      RECEIVER *rx = g_radio->receiver[i];
      if (rx != NULL && rx->panadapter != NULL) {
        gtk_widget_queue_draw(rx->panadapter);
      }
    }
  }
  return G_SOURCE_REMOVE;
}

// Insert or refresh a spot. Newest-first; same call+freq (within
// DXCLUSTER_DUP_TOLERANCE_HZ) updates in place rather than duplicating.
static void dxcluster_add_spot(const char *call, long long freq, const char *spotter,
                                const char *comment, int entity) {
  time_t now = time(NULL);

  g_mutex_lock(&spot_mutex);
  age_out_locked(now);

  int found = -1;
  for (int i = 0; i < nspots; i++) {
    if (llabs(spots[i].freq - freq) <= DXCLUSTER_DUP_TOLERANCE_HZ &&
        g_ascii_strcasecmp(spots[i].call, call) == 0) {
      found = i;
      break;
    }
  }

  if (found >= 0) {
    spots[found].freq = freq;
    spots[found].utc = now;
    spots[found].entity = entity;
    g_strlcpy(spots[found].spotter, spotter, sizeof(spots[found].spotter));
    g_strlcpy(spots[found].comment, comment, sizeof(spots[found].comment));
  } else {
    if (nspots >= DXCLUSTER_MAX_SPOTS) nspots = DXCLUSTER_MAX_SPOTS - 1;   // drop oldest
    for (int i = nspots; i > 0; i--) spots[i] = spots[i - 1];
    g_strlcpy(spots[0].call, call, sizeof(spots[0].call));
    spots[0].freq = freq;
    g_strlcpy(spots[0].spotter, spotter, sizeof(spots[0].spotter));
    g_strlcpy(spots[0].comment, comment, sizeof(spots[0].comment));
    spots[0].utc = now;
    spots[0].entity = entity;
    nspots++;
  }
  g_mutex_unlock(&spot_mutex);

  // No explicit redraw: the RX update timer repaints the panadapter (and this
  // overlay) every frame while receiving, so the new spot shows on the next
  // frame. Queuing a redraw per spot would pile up idle callbacks on a busy
  // cluster for no visual gain.
}

// ---------------------------------------------------------------------------
// spot-line parsing
// ---------------------------------------------------------------------------

// Parse one already-dechunked line of cluster text. Ignores anything that
// doesn't look like a "DX de <spotter>:  <freq kHz>  <call>  <comment> <time>Z"
// spot announcement.
//
//   DX de W3LPL-#:   14025.0  JA1XYZ       CW  up 2         1830Z
static void parse_spot_line(const char *line) {
  const char *p = line;
  while (*p == ' ') p++;
  if (strncmp(p, "DX de ", 6) != 0) return;
  p += 6;

  const char *colon = strchr(p, ':');
  if (colon == NULL) return;

  char spotter[32];
  size_t slen = (size_t)(colon - p);
  if (slen >= sizeof(spotter)) slen = sizeof(spotter) - 1;
  memcpy(spotter, p, slen);
  spotter[slen] = '\0';
  char *dash = strchr(spotter, '-');       // strip trailing node suffix (-#, -3, ...)
  if (dash != NULL) *dash = '\0';
  if (spotter[0] == '\0') return;

  p = colon + 1;
  while (*p == ' ') p++;

  char freqtok[32];
  int fi = 0;
  while (*p != '\0' && !isspace((unsigned char)*p) && fi < (int)sizeof(freqtok) - 1) freqtok[fi++] = *p++;
  freqtok[fi] = '\0';
  if (fi == 0) return;

  // The frequency is a decimal number off a network socket, so it is bounded
  // before it is converted, not after. strtod() happily returns inf for
  // "1e400" and NaN for "nan" — both slip past a plain `<= 0.0` test (every
  // comparison against NaN is false), and llround() of either is undefined.
  // The upper bound is a decade above any amateur band a cluster carries.
  char *endptr = NULL;
  double khz = strtod(freqtok, &endptr);
  if (endptr == freqtok) return;
  if (!(khz > 0.0) || !(khz < 1.0e8)) return;         // rejects NaN, inf, absurd
  long long freq_hz = (long long)llround(khz * 1000.0);

  while (*p == ' ') p++;

  char calltok[24];
  int ci = 0;
  while (*p != '\0' && !isspace((unsigned char)*p) && ci < (int)sizeof(calltok) - 1) calltok[ci++] = *p++;
  calltok[ci] = '\0';
  if (ci == 0) return;

  while (*p == ' ') p++;

  char comment[64];
  g_strlcpy(comment, p, sizeof(comment));
  size_t clen = strlen(comment);
  while (clen > 0 && (comment[clen - 1] == '\r' || comment[clen - 1] == '\n' || comment[clen - 1] == ' ')) {
    comment[--clen] = '\0';
  }

  // Strip a trailing "NNNNZ" UTC timestamp token, if present, off the comment.
  {
    char *last_space = strrchr(comment, ' ');
    const char *last_tok = last_space != NULL ? last_space + 1 : comment;
    size_t tlen = strlen(last_tok);
    if (tlen == 5 && (last_tok[4] == 'Z' || last_tok[4] == 'z')) {
      gboolean digits = TRUE;
      for (int k = 0; k < 4; k++) {
        if (!isdigit((unsigned char)last_tok[k])) { digits = FALSE; break; }
      }
      if (digits) {
        if (last_space != NULL) *last_space = '\0';
        else comment[0] = '\0';
      }
    }
    clen = strlen(comment);
    while (clen > 0 && comment[clen - 1] == ' ') comment[--clen] = '\0';
  }

  int entity = ft8_dxcc_entity(calltok);
  dxcluster_add_spot(calltok, freq_hz, spotter, comment, entity);
  // Permanent, level-gated: silent unless MACHPSDR_LOG=debug (or --debug/-v).
  log_debug_area(LOG_PROTOCOL, "dxcluster: spot %s %.1f kHz spotter=%s entity=%d (nspots=%d)\n",
            calltok, khz, spotter, entity, nspots);
}

// ---------------------------------------------------------------------------
// socket / connection handling (client thread only)
// ---------------------------------------------------------------------------

static int dxcluster_connect(const char *host, int port) {
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
    log_error("dxcluster: cannot resolve %s:%s\n", host, portstr);
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) continue;

    // Non-blocking connect with a bounded, stop-aware poll: a plain blocking
    // connect() to an unreachable host stalls for the full OS TCP timeout (~75 s)
    // and can't be interrupted (its fd isn't registered yet), which would freeze
    // dxcluster_stop() -> g_thread_join() and with it the GTK main thread. Here
    // each connect attempt is capped at DXCLUSTER_CONNECT_TIMEOUT_SEC and aborts
    // early the moment thread_running clears.
    net_set_nonblocking(fd, 1);

    int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
    if (rc == 0) {
      net_set_nonblocking(fd, 0);   // restore blocking for the recv loop
      break;
    }
    if (rc < 0 && net_connect_in_progress()) {
      gboolean ok = FALSE;
      for (int waited = 0; waited < DXCLUSTER_CONNECT_TIMEOUT_SEC * 10; waited++) {
        if (!g_atomic_int_get(&thread_running)) break;
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, 100);   // 100 ms slices so a stop aborts promptly
        if (pr > 0) {
          int soerr = 0; socklen_t slen = sizeof(soerr);
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
          ok = (soerr == 0);
          break;
        }
        // pr == 0: timed out this slice, keep waiting; pr < 0: poll error -> fail
        if (pr < 0) break;
      }
      if (ok) {
        net_set_nonblocking(fd, 0);   // restore blocking for the recv loop
        break;
      }
    }
    closesocket(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

// Case-insensitive substring search (portable strcasestr replacement).
static gboolean str_ci_contains(const char *hay, const char *needle) {
  size_t hn = strlen(hay), nn = strlen(needle);
  if (nn == 0 || nn > hn) return FALSE;
  for (size_t i = 0; i + nn <= hn; i++) {
    size_t j = 0;
    for (; j < nn; j++) {
      if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
    }
    if (j == nn) return TRUE;
  }
  return FALSE;
}

static void dxcluster_send_login(int fd) {
  const char *call = cfg_login;
  if (call[0] == '\0') return;   // nothing configured; skip auto-login

  char msg[32];
  snprintf(msg, sizeof(msg), "%s\r\n", call);
  size_t len = strlen(msg);
  size_t written = 0;
  while (written < len) {
    ssize_t n = send(fd, msg + written, len - written, 0);
    if (n <= 0) break;
    written += (size_t)n;
  }
  log_info("dxcluster: sent login '%s'\n", call);
}

// Sleeps up to DXCLUSTER_RETRY_SEC in small increments so dxcluster_stop()
// doesn't have to wait out the whole retry delay. Returns FALSE if a stop was
// requested during the sleep.
static gboolean dxcluster_sleep_retry(void) {
  for (int i = 0; i < DXCLUSTER_RETRY_SEC * 10; i++) {
    if (!g_atomic_int_get(&thread_running)) return FALSE;
    g_usleep(100000);   // 100 ms
  }
  return g_atomic_int_get(&thread_running) != 0;
}

static gpointer dxcluster_thread_func(gpointer data) {
  (void)data;

  while (g_atomic_int_get(&thread_running)) {
    set_status("connecting");
    log_info("dxcluster: connecting to %s:%d\n", cfg_host, cfg_port);

    int fd = dxcluster_connect(cfg_host, cfg_port);
    if (fd < 0) {
      set_status("error: connect failed");
      if (!dxcluster_sleep_retry()) break;
      continue;
    }

    g_mutex_lock(&sock_mutex);
    cluster_sock = fd;
    g_mutex_unlock(&sock_mutex);
    set_status("connected");
    log_info("dxcluster: connected\n");

    gboolean sent_login = FALSE;
    char carry[512];
    size_t carry_len = 0;
    char buf[2048];

    while (g_atomic_int_get(&thread_running)) {
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n <= 0) break;
      buf[n] = '\0';

      // Only answer an actual login prompt. Match prompt-shaped phrases, not a
      // bare "call" — a MOTD/greeting containing that word would otherwise make
      // us fire the callsign before the real prompt, so the real prompt (already
      // sent_login) never gets answered and no spots ever arrive.
      if (!sent_login &&
          (str_ci_contains(buf, "login:") || str_ci_contains(buf, "callsign") ||
           str_ci_contains(buf, "your call") || str_ci_contains(buf, "enter your call"))) {
        if (cfg_login[0] != '\0') {
          dxcluster_send_login(fd);
        } else {
          // The cluster is waiting at its login prompt but neither the cluster
          // Login call nor the station call is configured, so we have nothing to
          // send and no spots will ever arrive. Surface it in the status readout
          // instead of silently hanging "connected".
          set_status("connected - set a Login call");
          log_error("dxcluster: login prompt seen but no callsign configured; "
                    "set 'Login call' (Configure -> Network) or the station call (FT8 page)\n");
        }
        sent_login = TRUE;
      }

      for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
          carry[carry_len] = '\0';
          if (carry_len > 0 && carry[carry_len - 1] == '\r') carry[--carry_len] = '\0';
          parse_spot_line(carry);
          carry_len = 0;
        } else if (carry_len < sizeof(carry) - 1) {
          carry[carry_len++] = c;
        }
        // else: pathologically long line, silently drop the overflow (bounded)
      }
    }

    g_mutex_lock(&sock_mutex);
    if (cluster_sock >= 0) { closesocket(cluster_sock); cluster_sock = -1; }
    g_mutex_unlock(&sock_mutex);
    set_status("disconnected");
    log_info("dxcluster: disconnected\n");

    if (!g_atomic_int_get(&thread_running)) break;
    if (!dxcluster_sleep_retry()) break;
  }

  return NULL;
}

// ---------------------------------------------------------------------------
// public lifecycle API
// ---------------------------------------------------------------------------

static void inject_test_spots(void) {
  if (g_radio == NULL || g_radio->receivers <= 0 || g_radio->receiver[0] == NULL) return;

  long long base = g_radio->receiver[0]->frequency_a;
  static const struct { const char *call; long long offset; } test_spots[] = {
    { "JA1ABC",  -30000 },
    { "DL9XYZ",   -5000 },
    { "W1AW",      8000 },
    { "VK3TEST",  40000 },
  };
  time_t now = time(NULL);

  g_mutex_lock(&spot_mutex);
  for (size_t i = 0; i < G_N_ELEMENTS(test_spots) && nspots < DXCLUSTER_MAX_SPOTS; i++) {
    DX_SPOT *s = &spots[nspots++];
    g_strlcpy(s->call, test_spots[i].call, sizeof(s->call));
    s->freq = base + test_spots[i].offset;
    g_strlcpy(s->spotter, "TEST", sizeof(s->spotter));
    g_strlcpy(s->comment, "synthetic test spot", sizeof(s->comment));
    s->utc = now;
    s->entity = ft8_dxcc_entity(test_spots[i].call);
  }
  g_mutex_unlock(&spot_mutex);

  log_info("dxcluster: MACHPSDR_CLUSTER_TESTSPOTS set, injected %d synthetic spots\n",
           (int)G_N_ELEMENTS(test_spots));
  g_idle_add(dxcluster_redraw_idle, NULL);
}

void dxcluster_init(RADIO *r) {
  g_radio = r;
  if (getenv("MACHPSDR_CLUSTER_TESTSPOTS") != NULL) {
    // Dev/verify hook: the overlay draw and click-to-tune are gated on
    // cluster_enable && cluster_spots_show, so force both on for the injected
    // spots to be visible/clickable. (Also arms the live client via radio.c's
    // enable check — harmless for a local test.)
    r->cluster_enable = TRUE;
    r->cluster_spots_show = TRUE;
    inject_test_spots();
  }
}

void dxcluster_start(void) {
  if (g_atomic_int_get(&thread_running)) return;   // already running
  if (g_radio == NULL) return;

  // Snapshot the connection config now, while the thread is stopped (no reader),
  // so the socket thread reads only this private copy and never races the
  // Configure dialog rewriting the live RADIO fields. Login falls back to the
  // station callsign when the operator left the cluster login blank.
  g_strlcpy(cfg_host, g_radio->cluster_host, sizeof(cfg_host));
  cfg_port = g_radio->cluster_port;
  if (g_radio->cluster_login[0] != '\0')
    g_strlcpy(cfg_login, g_radio->cluster_login, sizeof(cfg_login));
  else
    g_strlcpy(cfg_login, g_radio->station_call, sizeof(cfg_login));

  g_atomic_int_set(&thread_running, 1);
  cluster_thread = g_thread_new("dxcluster", dxcluster_thread_func, NULL);
}

void dxcluster_stop(void) {
  if (!g_atomic_int_get(&thread_running) && cluster_thread == NULL) return;

  g_atomic_int_set(&thread_running, 0);

  g_mutex_lock(&sock_mutex);
  if (cluster_sock >= 0) {
    shutdown(cluster_sock, SHUT_RDWR);   // unblock a pending recv()
    closesocket(cluster_sock);
    cluster_sock = -1;
  }
  g_mutex_unlock(&sock_mutex);

  if (cluster_thread != NULL) {
    g_thread_join(cluster_thread);
    cluster_thread = NULL;
  }
  set_status("disconnected");
}
