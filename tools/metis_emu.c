// A software HPSDR (Metis/Protocol-1) board, in one file.
//
//   make metis-emu && ./metis_emu                 # Hermes, answers discovery
//   ./metis_emu --board 6 --tone 15000            # Hermes-Lite 2, 15 kHz tone
//   ./metis_emu --quiet                           # no per-second status line
//
// WHY THIS EXISTS.  "No hardware" is not the same as "no protocol".  Protocol 1
// is UDP with a frame layout this repository already contains a parser for, so
// the receive thread, the output thread, the sequence checking and everything
// downstream of add_iq_samples() can be exercised without a radio -- and that
// is the ONLY way to run the delete_rx_mutex discipline in protocol1.c, because
// the faker is PROTOCOL_FAKE and takes a completely different path into the DSP.
//
// It is a separate PROCESS speaking UDP, not a harness linked against the app,
// which is what makes it cheap: it needs nothing from the tree, and the app
// needs no build flag, no CLI switch and no test hook to talk to it.  Start it,
// then start machpsdr; the ordinary discovery finds it and it appears in the
// device dialog as a real board.
//
// USE WITH MACHPSDR_RX_CHURN.  The pairing this was written for:
//
//   ./metis_emu &
//   HOME=$(mktemp -d) MACHPSDR_RX_CHURN=20 ./machpsdr        # SANITIZE=1 build
//
// which adds and closes receivers under a live protocol-1 feed -- the race
// delete_receiver's locking exists for, driven through the real socket path.
//
// NETWORK NOTE, deliberate and worth reading.  Discovery is a broadcast, and on
// macOS a broadcast cannot be sent from a loopback-bound socket (EADDRNOTAVAIL
// -- which is why the app's own lo0 pass already fails silently), so the request
// arrives over a real interface and this has to bind 0.0.0.0.  To make sure a
// fake radio can never be announced onto somebody else's machine, a discovery
// request is answered ONLY when it came from this host: 127.0.0.1 or one of
// this machine's own interface addresses.  A request from anywhere else is
// counted and dropped.
//
// WHAT IT IS NOT.  It speaks enough of the protocol to be discovered, started,
// stopped and to stream EP6; it does not model the radio.  It ignores every
// command in the EP2 stream except the two it needs to frame correctly (sample
// rate and receiver count, which the app announces in C1/C4 of the C0=0 command
// exactly as a real board would read them), never sends EP4 wideband, and its
// control bytes are constant zeros -- so PTT, ADC overflow, power readings and
// the HL2 I2C dialogue all stay quiet.  A green run says the app's protocol-1
// path survives a live feed; it says nothing about a real board's behaviour.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DISCOVERY_PORT 1024      // the board listens here for everything
#define SYNC           0x7F

// Board ids as protocol1_discovery.c reads them out of byte 10.
#define BOARD_METIS        0
#define BOARD_HERMES       1
#define BOARD_HERMES_LITE  6
#define BOARD_ANGELIA      4
#define BOARD_ORION        5

static volatile sig_atomic_t stop_now = 0;
static void on_signal(int sig) { (void)sig; stop_now = 1; }

// ---- options ---------------------------------------------------------------
static int    opt_board   = BOARD_HERMES;
static int    opt_version = 33;
static double opt_tone_hz = 10000.0;
static int    opt_quiet   = 0;
// --pace: stream at 1/N of real time.  DELIBERATELY UNFAITHFUL, and the only
// option here that is: a real board streams in real time and so does this by
// default.  It exists because a SANITIZE=1 build runs the DSP chain roughly an
// order of magnitude slower than wall clock, so a faithful 192 kHz feed buries
// it -- the app never finishes a buffer, the GTK timer never gets the CPU, and
// the window freezes on whatever frame it last drew.  That is the harness
// outrunning the app under instrumentation, not a fault in either.  Slowing the
// feed keeps every code path identical and merely starves the audio, which no
// race test cares about.  Leave it at 1 for an ordinary build.
static double opt_pace    = 1.0;
static unsigned char opt_mac[6] = { 0x00, 0x1C, 0xC0, 0xA2, 0x13, 0x37 };

// ---- state the app tells us, exactly as a real board learns it --------------
// Both live in the C0=0 command of the EP2 stream the app sends us: C1's low
// two bits are the sample rate and C4 bits 5..3 are (receiver count - 1).  We
// need them only to frame EP6 and to pace it; getting either wrong does not
// break the app's parser (its state machine is driven by its OWN receiver
// count), it just feeds it the wrong number of samples per second.
static int rx_count   = 1;
static int sample_rate = 48000;

// ---- helpers ---------------------------------------------------------------
static long long now_us(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// TRUE if `a` is 127.0.0.1 or an address of one of this machine's interfaces.
// This is the whole safety property: without it, running the emulator would put
// a fake HPSDR board on the operator's LAN, answering anybody's discovery.
static int is_local_address(struct in_addr a) {
  if (a.s_addr == htonl(INADDR_LOOPBACK)) return 1;
  struct ifaddrs *addrs = NULL;
  if (getifaddrs(&addrs) != 0) return 0;
  int found = 0;
  for (struct ifaddrs *ifa = addrs; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) continue;
    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    if (sa->sin_addr.s_addr == a.s_addr) { found = 1; break; }
  }
  freeifaddrs(addrs);
  return found;
}

// ---- EP6 frame construction -------------------------------------------------
// A 512-byte USB frame: 3 sync bytes, 5 control bytes, then repeating
// (3 bytes I + 3 bytes Q) per receiver followed by a 2-byte mic sample, for as
// many whole sample groups as fit.  Written from process_ozy_byte()'s state
// machine, which is the definition of the format as far as this app is
// concerned.
#define USB_FRAME 512

static int samples_per_frame(void) {
  int group = rx_count * 6 + 2;
  return (USB_FRAME - 8) / group;
}

static void put24(unsigned char *p, double v) {
  // 24-bit signed, the scale process_ozy_byte divides back out (2^23 - 1).
  if (v >  1.0) v =  1.0;
  if (v < -1.0) v = -1.0;
  int32_t s = (int32_t)(v * 8388607.0);
  p[0] = (s >> 16) & 0xFF;
  p[1] = (s >>  8) & 0xFF;
  p[2] =  s        & 0xFF;
}

// A steady complex tone at opt_tone_hz, phase carried across frames so the
// panadapter shows one clean line rather than a smear of block-edge splatter.
//
// THE SIGN, and why it is not the obvious one.  The I/Q pair on the wire is
// (left, right): process_ozy_byte() reads left first and hands the pair to
// add_iq_samples(rx, left, right), which stores left at iq_input_buffer[2n] and
// right at [2n+1] -- and WDSP reads that buffer as (Q, I), NOT (I, Q) (see
// wdsp/analyzer.c Spectrum0 and the CLAUDE.md section of that name).  So the
// complex signal the panadapter, the demodulator and every decoder see is
// right + j*left, and a tone that must appear ABOVE the dial needs
//
//     right = cos(phase),  left = sin(phase)
//
// i.e. the cosine goes in the SECOND field of the pair.  This was written the
// other way round for as long as the tool existed, so --tone 10000 put its line
// 10 kHz BELOW the dial; measured through the TCI IQ stream (which carries true
// (I, Q), tci_iq_feed() un-swaps it) as -9999.9 Hz before and +9999.9 Hz after.
// Get this backwards anywhere and the tone lands on the wrong side of centre,
// which is at least visible -- a tone at the centre would hide it.
static double tone_phase = 0.0;

static void build_usb_frame(unsigned char *f) {
  memset(f, 0, USB_FRAME);
  f[0] = f[1] = f[2] = SYNC;
  // Control bytes stay zero: C0=0 selects control set 0, and with bits 0..2
  // clear that means PTT, dot and dash all released.  See
  // process_control_bytes() -- anything else here would drive the app's TX
  // state from a test tool, which is not what this is for.
  int n = samples_per_frame();
  int off = 8;
  double dphi = 2.0 * M_PI * opt_tone_hz / (double)sample_rate;
  for (int s = 0; s < n; s++) {
    tone_phase += dphi;
    if (tone_phase > 2.0 * M_PI) tone_phase -= 2.0 * M_PI;
    // Named for where they go on the wire, not I/Q: see the sign note above.
    double left  = 0.4 * sin(tone_phase);
    double right = 0.4 * cos(tone_phase);
    for (int r = 0; r < rx_count; r++) {
      put24(&f[off], left);  off += 3;
      put24(&f[off], right); off += 3;
    }
    off += 2;                       // mic sample: silence
  }
}

// ---- main -------------------------------------------------------------------
static void usage(const char *me) {
  fprintf(stderr,
    "usage: %s [--board N] [--version N] [--tone HZ] [--mac aa:bb:...] [--quiet]\n"
    "  --board    0 Metis, 1 Hermes (default), 4 Angelia, 5 Orion, 6 Hermes Lite\n"
    "  --tone     baseband tone offset in Hz, ABOVE the dial (default 10000);\n"
    "             negative puts it below\n"
    "  --pace     stream at 1/N of real time (default 1 = real time);\n"
    "             use e.g. 8 to feed a SANITIZE=1 build it can keep up with\n", me);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--board") && i + 1 < argc)        opt_board = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--version") && i + 1 < argc) opt_version = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--tone") && i + 1 < argc)    opt_tone_hz = atof(argv[++i]);
    else if (!strcmp(argv[i], "--quiet"))                   opt_quiet = 1;
    else if (!strcmp(argv[i], "--pace") && i + 1 < argc)    opt_pace = atof(argv[++i]);
    else if (!strcmp(argv[i], "--mac") && i + 1 < argc) {
      unsigned m[6];
      if (sscanf(argv[++i], "%x:%x:%x:%x:%x:%x", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6)
        for (int k = 0; k < 6; k++) opt_mac[k] = (unsigned char)m[k];
    }
    else { usage(argv[0]); return 2; }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) { perror("socket"); return 1; }
  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in me = { 0 };
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  me.sin_port = htons(DISCOVERY_PORT);
  if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
    // The usual cause is a second copy of this already running, or a real
    // radio's software on the same machine. Say so rather than "bind failed".
    fprintf(stderr, "metis-emu: cannot bind UDP %d: %s\n"
                    "  (something else is already listening -- another metis_emu?)\n",
            DISCOVERY_PORT, strerror(errno));
    return 1;
  }

  // Non-blocking: this loop both answers commands and paces the EP6 stream, so
  // it must never park in recvfrom while a stream is due.
  struct timeval tv = { 0, 2000 };            // 2 ms
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  printf("metis-emu: board %d, version %d, MAC %02X:%02X:%02X:%02X:%02X:%02X, "
         "tone %.0f Hz\n", opt_board, opt_version,
         opt_mac[0], opt_mac[1], opt_mac[2], opt_mac[3], opt_mac[4], opt_mac[5],
         opt_tone_hz);
  printf("metis-emu: listening on UDP %d; answers only requests from this host\n",
         DISCOVERY_PORT);
  if (opt_pace != 1.0)
    printf("metis-emu: PACED at 1/%.3g of real time -- not what a board does\n", opt_pace);

  int streaming = 0;
  struct sockaddr_in peer = { 0 };
  socklen_t peerlen = 0;
  uint32_t ep6_seq = 0;
  long long next_send_us = 0;
  long long last_report_us = now_us();
  long long pkts = 0, ep2_frames = 0, discoveries = 0, refused = 0;

  unsigned char pkt[1032];
  unsigned char rx[2048];

  while (!stop_now) {
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fromlen);

    if (n >= 3 && rx[0] == 0xEF && rx[1] == 0xFE) {
      switch (rx[2]) {
        case 0x02: {                          // discovery request
          if (!is_local_address(from.sin_addr)) {
            refused++;
            break;
          }
          discoveries++;
          unsigned char rep[64];
          memset(rep, 0, sizeof(rep));
          rep[0] = 0xEF; rep[1] = 0xFE;
          rep[2] = 0x02;                      // status 2 = idle (3 = in use)
          memcpy(&rep[3], opt_mac, 6);
          rep[9]  = (unsigned char)opt_version;
          rep[10] = (unsigned char)opt_board;
          // Hermes-Lite 2 alone reports its receiver count and gateware patch
          // level in the reply; harmless to fill in for any board.
          rep[0x13] = 4;                      // supported receivers (HL2 path)
          rep[0x15] = 0;                      // gateware patch number
          sendto(sock, rep, sizeof(rep), 0, (struct sockaddr *)&from, fromlen);
          break;
        }
        case 0x04:                            // start / stop
          if (!is_local_address(from.sin_addr)) { refused++; break; }
          if (rx[3] != 0) {
            peer = from; peerlen = fromlen;
            streaming = 1;
            ep6_seq = 0;
            next_send_us = now_us();
            printf("metis-emu: START from %s:%d (cmd 0x%02X)\n",
                   inet_ntoa(from.sin_addr), ntohs(from.sin_port), rx[3]);
          } else {
            streaming = 0;
            printf("metis-emu: STOP\n");
          }
          break;
        case 0x01:                            // EP2: the app's command stream
          ep2_frames++;
          // Learn the framing from the command set the app is already sending,
          // the way a board does. Both USB frames in the packet carry a command;
          // only C0==0 has the fields we want.
          for (int off = 8; off + 8 <= (int)n && off <= 520; off += 512) {
            if (rx[off] != SYNC || rx[off+1] != SYNC || rx[off+2] != SYNC) continue;
            unsigned char c0 = rx[off+3], c1 = rx[off+4], c4 = rx[off+7];
            if ((c0 & 0xFE) != 0x00) continue;         // C0 = 0 (bit 0 is MOX)
            int want_rx = ((c4 >> 3) & 0x07) + 1;
            int want_sr;
            switch (c1 & 0x03) {
              case 0:  want_sr = 48000;  break;
              case 1:  want_sr = 96000;  break;
              case 2:  want_sr = 192000; break;
              default: want_sr = 384000; break;
            }
            if (want_rx != rx_count || want_sr != sample_rate) {
              rx_count = want_rx; sample_rate = want_sr;
              printf("metis-emu: app asked for %d receiver(s) at %d Hz "
                     "(%d samples per USB frame)\n",
                     rx_count, sample_rate, samples_per_frame());
            }
          }
          break;
        default:
          break;
      }
    }

    if (!streaming) continue;

    // Pace EP6.  One UDP packet carries two USB frames, so it is worth
    // 2*samples_per_frame() samples of the app's timeline.  Catch-up is capped:
    // if this process is descheduled (or run under a sanitiser), sending the
    // whole backlog at once would hand the app a burst it never sees from a
    // real board.
    long long t = now_us();
    int budget = 32;
    while (streaming && t >= next_send_us && budget-- > 0) {
      double per_pkt = 2.0 * (double)samples_per_frame();
      long long interval_us = (long long)(per_pkt * 1e6 * opt_pace / (double)sample_rate);
      if (interval_us < 1) interval_us = 1;

      pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 6;   // EP6
      pkt[4] = (ep6_seq >> 24) & 0xFF;
      pkt[5] = (ep6_seq >> 16) & 0xFF;
      pkt[6] = (ep6_seq >>  8) & 0xFF;
      pkt[7] =  ep6_seq        & 0xFF;
      ep6_seq++;
      build_usb_frame(&pkt[8]);
      build_usb_frame(&pkt[520]);
      if (sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&peer, peerlen) < 0) {
        if (errno == ECONNREFUSED) {          // the app went away
          streaming = 0;
          printf("metis-emu: peer gone, stopped streaming\n");
          break;
        }
      }
      pkts++;
      next_send_us += interval_us;
      // Too far behind to catch up honestly: resynchronise rather than sprint.
      if (t - next_send_us > 200000) next_send_us = t;
    }

    if (!opt_quiet && t - last_report_us >= 1000000) {
      printf("metis-emu: %lld EP6 pkts/s, %lld EP2 in, %d rx @ %d Hz%s\n",
             pkts, ep2_frames, rx_count, sample_rate,
             refused ? " (some non-local requests refused)" : "");
      pkts = 0; ep2_frames = 0;
      last_report_us = t;
    }
  }

  printf("metis-emu: exit (%lld discoveries answered, %lld refused as non-local)\n",
         discoveries, refused);
  close(sock);
  return 0;
}
