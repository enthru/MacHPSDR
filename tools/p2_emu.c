// A software HPSDR (Protocol-2) board, in one file.
//
//   make p2-emu && ./p2_emu                        # Angelia, answers discovery
//   ./p2_emu --board 5 --receivers 8               # Orion2, eight DDCs
//   ./p2_emu --tone 24000 --pace 8                 # for a SANITIZE=1 build
//
// WHY THIS EXISTS.  tools/metis_emu.c is the same idea for Protocol 1, and the
// run it made possible is what found the output-path self-deadlock nothing else
// in the tree could reach.  Protocol 2 had no such thing: process_iq_data(), the
// delete_rx_mutex discipline around radio->receiver[] in protocol2_thread(), the
// discovery / start / receive-specific / high-priority register path and the
// 24-bit I/Q unpacking had all been reviewed and compiled and NEVER RUN.  The
// faker cannot stand in -- PROTOCOL_FAKE takes a wholly different path into the
// DSP -- and neither can metis_emu, which is a different protocol on a different
// packet layout and different ports.
//
// It is a separate PROCESS speaking UDP, not a harness linked against the app,
// which is what makes it cheap: it needs nothing from the tree, and the app
// needs no build flag, no CLI switch and no test hook to talk to it.  Start it,
// then start machpsdr; the ordinary discovery finds it and it appears in the
// device dialog as a real board.
//
// USE WITH MACHPSDR_RX_CHURN.  The pairing this was written for:
//
//   ./p2_emu --pace 8 &
//   HOME=$(mktemp -d) MACHPSDR_RX_CHURN=10 ./machpsdr --open Angelia
//
// built with SANITIZE=1, which adds and closes receivers while their DDCs are
// streaming.  Closing a receiver does NOT stop its DDC (the app's 100 ms timer
// re-sends receive-specific afterwards, and packets already in flight arrive
// regardless), so this is exactly the race protocol2_thread's delete_rx_mutex
// exists for, driven through the real socket path.
//
// THE PORTS, and why there are so many.  Protocol 2 is not one socket like
// Metis: each register block and each stream has its own UDP port, and a real
// board both receives and sends on the same port number.  From protocol2.h:
//
//   1024  general registers in            (also command-response out)
//   1025  receive-specific registers in   (also high-priority status out)
//   1026  transmit-specific registers in  (also mic/line samples out)
//   1027  high-priority registers in      (also wideband ADC samples out)
//   1028  RX audio from host              (drained)
//   1029  TX I/Q from host                (drained)
//   1035+ DDC I/Q to host, one port per DDC
//
// The app sends everything from ONE socket bound to an ephemeral port, so the
// peer is learned from whichever packet arrives first; it dispatches inbound
// packets on their SOURCE port, which is why each stream here is sent from a
// socket bound to the port that names it.
//
// WHAT IT ACTUALLY MODELS.  Three things the app tells it, read out of the
// registers exactly as a board would: which DDCs are enabled and at what rate
// (receive-specific byte 7 and the 6-byte per-DDC block at 17+ddc*6), whether
// the radio is running (high-priority byte 4 bit 0), and whether the wideband
// ADC feed was asked for (general byte 23).  Everything else in those 1444-byte
// register blocks is parsed by nobody: the filters, the ALEX words, the PA
// enable, the attenuators and the OC bits are read and thrown away, because
// there is no radio here for them to configure.  It reports back a healthy
// board -- PTT down, no ADC overload, PLL locked, ~13.8 V supply -- so
// process_high_priority() runs on real bytes without driving the app's TX state
// from a test tool.
//
// THE TONE, and why its side of centre is the whole point.  Each DDC streams a
// steady complex tone plus a low noise floor.  The I/Q pair on the wire is
// (left, right) and the app hands them to add_iq_samples(rx, left, right),
// which stores left at iq_input_buffer[2n] and right at [2n+1] -- and WDSP
// reads that buffer as (Q, I), not (I, Q) (see analyzer.c Spectrum0 and the
// CLAUDE.md section of that name).  So the complex signal the panadapter shows
// is right + j*left, and a tone that must appear ABOVE the dial needs
// right = cos, left = sin.  That is what this sends.  Get the sign wrong
// anywhere in the unpacking and the tone lands on the wrong side of centre,
// which is visible; a tone at the centre would hide it.  DDC n is offset a
// further --tone-step (1 kHz default) so a DDC delivered to the wrong receiver
// is visible too, rather than merely plausible.
//
// Amplitude is --amp of 24-bit full scale (0.4 by default), i.e. the wire code
// is amp*8388607, and protocol2.c divides by the same 8388607, so the DSP sees
// exactly --amp.  It did not when this file was written: protocol2.c divided by
// 16777215.0 and the DSP saw amp/2.  Measuring 0.199999 for an --amp 0.4 tone
// through this emulator is what settled that (6 dB low against protocol1.c,
// against protocol2.c's own PureSignal path, and against pihpsdr) and got it
// fixed.  If an --amp tone ever reads back as something other than --amp, the
// unpacking has moved again.
//
// SYNCHRONISED DDCs, and why they change the packet shape.  A board can be
// asked to synchronise one DDC to another through the sync map at byte 1363 of
// the receive-specific block (one byte per base DDC, its bits naming the
// followers).  A synchronised pair does NOT then stream on two ports.  From
// openHPSDR Ethernet Protocol v3.5:
//
//   "Sets the DDC that DDC (n) is synchronised or multiplexed with... All DDC's
//    frequencies will be set to the frequency of the base DDC.  NOTE: For the
//    time being, due to FPGA size limitations and timing closure issues only
//    DDC0 and DDC1 may be synchronised, with synchronised data presented from
//    DDC0's output."
//
// So the base DDC's packets carry BOTH streams, sample-interleaved -- base,
// follower, base, follower -- and the frame's sample count covers both, i.e.
// each stream gets half the samples per packet and the packet rate doubles.
// This emulator reads the sync map and does exactly that, which is the point:
// the app's sync register is not merely written into the void, it is read back
// and answered in the shape the register asked for.  The follower's own port
// stays silent.
//
// COHERENT-PAIR MODE (--diversity), for the diversity mixer.  By default every
// DDC carries its OWN tone (offset a further --tone-step per DDC), which is the
// routing proof above and must stay the default.  Diversity is the opposite
// requirement: the mixer combines two streams off two ADCs of ONE radio that
// see the SAME signal, differing only by a complex gain, and it can only be
// exercised by a feed where that is true.  With --diversity every stream runs
// the same tone from a phase accumulator advanced by the same increment over
// the same sample index, so sample k of one and sample k of the other are
// coherent by construction; a stream the app assigned to a NON-ZERO ADC is then
// multiplied by --div-gain dB at --div-phase degrees, i.e. the relative gain
// A*e^{j*phi} an operator's two antennas would present.  Keying that off the
// ADC rather than the DDC number is deliberate: the ADC assignment is what the
// app puts in the register, so a build that mispairs the ADCs produces two
// IDENTICAL streams and the relative phase reads 0 no matter what was asked
// for -- a failure that would otherwise be invisible.  Noise stays INDEPENDENT
// per stream, because two ADCs really do have independent noise and a perfectly
// identical pair would be a weaker test than a nearly identical one.
//
//   ./p2_emu --diversity --div-phase 45 --div-gain 6
//
// What that buys: the relative phase measured at the mixer is a DIRECT readout
// of sample alignment.  At 10 kHz on a 192 kHz DDC one sample is 18.75 deg, so
// asking for --div-phase 0 and measuring 0 proves the two streams reach
// diversity_mix_full_buffers() aligned to the sample -- a whole-buffer or
// even one-sample slip would show up as a phase error, which no amount of
// looking at two spectra could tell you.
//
// NETWORK NOTE, deliberate and worth reading.  Discovery is a broadcast, and on
// macOS a broadcast cannot be sent from a loopback-bound socket (EADDRNOTAVAIL
// -- which is why the app's own lo0 pass already fails silently), so the request
// arrives over a real interface and this has to bind 0.0.0.0.  To make sure a
// fake radio can never be announced onto somebody else's machine, EVERY request
// is answered ONLY when it came from this host: 127.0.0.1 or one of this
// machine's own interface addresses.  Anything else is counted and dropped.
//
// WHAT IT IS NOT.  It speaks enough of Protocol 2 to be discovered, started,
// stopped and to stream DDC I/Q; it does not model the radio.  Nothing here
// transmits: the TX I/Q and RX audio the app sends are received and discarded
// (so its sends never fail), the mic stream it sends back is silence, and the
// PureSignal feedback DDCs are NOT emulated -- that path needs a board with a
// feedback ADC and stays unverified.  The wideband feed is honest in format and
// invented in pacing (the P2 rate registers are left at their defaults by the
// app, so there is no rate to obey); it exists so the wideband window has
// something to draw.  A green run says the app's protocol-2 path survives a live
// feed; it says nothing about a real board's behaviour.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// Ports, from src/proto/protocol2.h.  A board is one endpoint per port number:
// the "in" and "out" uses of 1024..1027 share a socket, exactly as real hardware
// shares them.
#define PORT_GENERAL   1024   // general registers in  / command response out
#define PORT_RXSPEC    1025   // receive-specific in   / high-priority status out
#define PORT_TXSPEC    1026   // transmit-specific in  / mic samples out
#define PORT_HIGHPRIO  1027   // high-priority in      / wideband samples out
#define PORT_AUDIO     1028   // RX audio from host    (drained)
#define PORT_TXIQ      1029   // TX I/Q from host      (drained)
#define PORT_IQ_BASE   1035   // DDC n I/Q out on PORT_IQ_BASE + n

// Board ids as protocol2_discovery.c reads them out of byte 11.
#define BOARD_ATLAS        0
#define BOARD_HERMES       1
#define BOARD_HERMES2      2
#define BOARD_ANGELIA      3
#define BOARD_ORION        4
#define BOARD_ORION2       5
#define BOARD_HERMES_LITE  6

#define MAX_DDC        8      // RX_IQ_TO_HOST_PORT_0..7, and MAX_RECEIVERS

// One DDC I/Q packet: 16-byte header then 238 (I,Q) pairs of 24 bits = 1444
// bytes, the size the app's own discovery loop already recognises as an I/Q
// packet.  process_iq_data() reads the sample count out of bytes 14..15 rather
// than assuming it, but a real board fills the packet, so this does too.
#define IQ_PKT_BYTES   1444
#define IQ_SAMPLES     238

// Wideband: 4-byte sequence then 512 16-bit samples.  process_wideband_data()
// walks a FIXED range (b = 4 while b < 1028), so this length is not a choice.
#define WB_PKT_BYTES   1028
#define WB_SAMPLES     512

// Mic/line to host: 4-byte sequence then MIC_SAMPLES (64) 16-bit samples.
#define MIC_SAMPLES    64
#define MIC_PKT_BYTES  (4 + MIC_SAMPLES * 2)

static volatile sig_atomic_t stop_now = 0;
static void on_signal(int sig) { (void)sig; stop_now = 1; }

// ---- options ---------------------------------------------------------------
static int    opt_board     = BOARD_ANGELIA;
static int    opt_version   = 39;
static int    opt_receivers = 4;      // advertised DDC count (byte 20)
static double opt_tone_hz   = 10000.0;
static double opt_tone_step = 1000.0; // DDC n's tone is tone + n*step
static double opt_amp       = 0.4;    // of 24-bit full scale
static double opt_noise     = 0.002;
static int    opt_mic       = 1;
static int    opt_diversity = 0;      // all DDCs coherent, see header comment
static double opt_div_gain  = 0.0;    // dB, DDC n>0 relative to DDC 0
static double opt_div_phase = 0.0;    // degrees, DDC n>0 relative to DDC 0
static int    opt_wb_rate   = 96000;  // wideband samples/s (see header comment)
static int    opt_quiet     = 0;
// --pace: stream at 1/N of real time.  DELIBERATELY UNFAITHFUL, and the only
// option here that is: a real board streams in real time and so does this by
// default.  It exists because a SANITIZE=1 build runs the DSP chain roughly an
// order of magnitude slower than wall clock, so a faithful 192 kHz feed buries
// it -- the app never finishes a buffer, the GTK timer never gets the CPU, and
// the window freezes on whatever frame it last drew.  That is the harness
// outrunning the app under instrumentation, not a fault in either.  Slowing the
// feed keeps every code path identical and merely starves the audio, which no
// race test cares about.  Leave it at 1 for an ordinary build.
static double opt_pace      = 1.0;
static unsigned char opt_mac[6] = { 0x00, 0x1C, 0xC0, 0xA2, 0x20, 0x02 };

// ---- state the app tells us, exactly as a real board learns it --------------
static int      running     = 0;             // high-priority byte 4 bit 0
static unsigned ddc_enable  = 0;             // receive-specific byte 7
static int      ddc_rate[MAX_DDC];           // Hz, from bytes 18..19 of each block
static long long ddc_freq[MAX_DDC];          // Hz, from the phase words (log only)
static int      wb_enable   = 0;             // general byte 23 bit 0
static int      ddc_adc[MAX_DDC];            // ADC, from byte 17 of each block
static unsigned ddc_sync[MAX_DDC];           // followers of DDC n, byte 1363+n

// Byte 1363 is the sync map for DDC0, one byte per DDC after it.
#define RXSPEC_SYNC_BASE 1363

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

static int bind_udp(int port) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { perror("socket"); return -1; }
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
  setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
  struct sockaddr_in me;
  memset(&me, 0, sizeof(me));
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  me.sin_port = htons((unsigned short)port);
  if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
    // The usual cause is a second copy of this already running, or metis_emu
    // (which owns 1024 too).  Say so rather than "bind failed".
    fprintf(stderr, "p2-emu: cannot bind UDP %d: %s\n"
                    "  (something else is already listening -- another p2_emu, or metis_emu?)\n",
            port, strerror(errno));
    close(s);
    return -1;
  }
  return s;
}

static void put_be32(unsigned char *p, uint32_t v) {
  p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
  p[2] = (v >>  8) & 0xFF; p[3] =  v        & 0xFF;
}

// 24-bit signed, big-endian, full scale 2^23 - 1.
static void put24(unsigned char *p, double v) {
  if (v >  1.0) v =  1.0;
  if (v < -1.0) v = -1.0;
  int32_t s = (int32_t)(v * 8388607.0);
  p[0] = (s >> 16) & 0xFF;
  p[1] = (s >>  8) & 0xFF;
  p[2] =  s        & 0xFF;
}

// A cheap uniform noise source: xorshift32, so the floor is reproducible and
// costs nothing next to the tone.
static uint32_t rng_state = 0x1BADF00D;
static double noise_sample(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return ((double)(int32_t)rng_state / 2147483648.0);
}

// ---- DDC I/Q ----------------------------------------------------------------
static double   ddc_phase[MAX_DDC];
static uint32_t ddc_seq[MAX_DDC];
static uint64_t ddc_stamp[MAX_DDC];

// The follower DDC of a synchronised pair, or -1.  Only one follower is
// modelled: the spec allows exactly the DDC0/DDC1 pair today, and a list here
// would be inventing hardware.
static int sync_follower(int ddc) {
  unsigned f = ddc_sync[ddc] & ~(1u << ddc);
  if (f == 0) return -1;
  for (int d = 0; d < MAX_DDC; d++) if (f & (1u << d)) return d;
  return -1;
}

// One packet on `ddc`'s port.  Carries one stream normally, or two
// sample-interleaved streams when a follower is synchronised to it.
static void build_iq_packet(unsigned char *pkt, int ddc) {
  memset(pkt, 0, IQ_PKT_BYTES);
  put_be32(pkt, ddc_seq[ddc]++);

  int follower = sync_follower(ddc);
  int stream[2] = { ddc, follower };
  int nstreams  = (follower >= 0) ? 2 : 1;
  // The sample count in the header covers BOTH streams, which is why the app
  // (and pihpsdr) step the unpacking loop by two.  Each stream therefore gets
  // half the samples per packet -- and the caller halves the packet interval,
  // so the per-stream rate is still ddc_rate.
  int nper  = IQ_SAMPLES / nstreams;
  int total = nper * nstreams;

  // Timestamp: the app parses it and uses it for nothing, so a sample counter
  // is as honest as anything else that monotonically increases.
  uint64_t ts = ddc_stamp[ddc];
  for (int k = 0; k < 8; k++) pkt[4 + k] = (unsigned char)((ts >> (56 - 8 * k)) & 0xFF);
  ddc_stamp[ddc] += nper;
  pkt[12] = 0; pkt[13] = 24;                    // bits per sample
  pkt[14] = (total >> 8) & 0xFF;                // samples in this frame
  pkt[15] =  total       & 0xFF;

  double rate = (double)(ddc_rate[ddc] > 0 ? ddc_rate[ddc] : 192000);
  double dphi[2], amp[2], poff[2];
  for (int s = 0; s < nstreams; s++) {
    int d = stream[s];
    // Coherent-pair mode: ONE signal on every stream, so the per-DDC tone step
    // is deliberately not applied -- two streams at different frequencies are
    // not what two coherent ADCs deliver and the mixer would have nothing to
    // combine.
    dphi[s] = 2.0 * M_PI *
              (opt_tone_hz + (opt_diversity ? 0.0 : opt_tone_step * d)) / rate;
    amp[s]  = opt_amp;
    poff[s] = 0.0;
    // The relative complex gain a stream on a non-zero ADC carries.  Applying
    // it as an amplitude and a phase OFFSET (rather than rotating a finished
    // I/Q pair) keeps the streams exactly coherent: they share one phase ramp
    // and differ by a constant, which is the definition of what the mixer
    // corrects.  Keyed on the ADC the app assigned, not on the DDC number --
    // see the header comment.
    if (opt_diversity && ddc_adc[d] != 0) {
      amp[s]  = opt_amp * pow(10.0, opt_div_gain / 20.0);
      if (amp[s] > 1.0) amp[s] = 1.0;          // put24 clips anyway; say so here
      poff[s] = opt_div_phase * M_PI / 180.0;
    }
  }

  int b = 16;
  for (int i = 0; i < nper; i++) {
    // Coherent mode runs every stream off the BASE DDC's single phase
    // accumulator.  Advancing one accumulator per DDC would be enough only if
    // both had always been generated together -- and they have not: before the
    // app's sync map arrives, the follower DDC streams on its own port at its
    // own pace, so the two accumulators are already some samples apart by the
    // time the pair is formed.  That difference is indistinguishable from a
    // relative phase and would be silently attributed to the app, which is the
    // measurement this whole mode exists to make.  One ramp means the ONLY
    // relative phase the app can see is --div-phase plus its own misalignment.
    if (opt_diversity) {
      ddc_phase[ddc] += dphi[0];
      if (ddc_phase[ddc] >  M_PI) ddc_phase[ddc] -= 2.0 * M_PI;
      if (ddc_phase[ddc] < -M_PI) ddc_phase[ddc] += 2.0 * M_PI;
    }
    for (int s = 0; s < nstreams; s++) {
      int d = stream[s];
      double ph;
      if (opt_diversity) {
        ph = ddc_phase[ddc];
      } else {
        ddc_phase[d] += dphi[s];
        if (ddc_phase[d] >  M_PI) ddc_phase[d] -= 2.0 * M_PI;
        if (ddc_phase[d] < -M_PI) ddc_phase[d] += 2.0 * M_PI;
        ph = ddc_phase[d];
      }
      // (left, right) on the wire; WDSP reads the app's buffer as (Q, I), so
      // right must be the cosine for the tone to sit ABOVE the dial.  See the
      // header comment -- this sign is the point of the tone.
      double left  = amp[s] * sin(ph + poff[s]) + opt_noise * noise_sample();
      double right = amp[s] * cos(ph + poff[s]) + opt_noise * noise_sample();
      put24(&pkt[b], left);  b += 3;
      put24(&pkt[b], right); b += 3;
    }
  }
}

// ---- main -------------------------------------------------------------------
static void usage(const char *me) {
  fprintf(stderr,
    "usage: %s [options]\n"
    "  --board N      0 Atlas, 1 Hermes, 2 Hermes2, 3 Angelia (default),\n"
    "                 4 Orion, 5 Orion2, 6 Hermes Lite\n"
    "  --version N    reported gateware version (default 39)\n"
    "  --receivers N  DDCs advertised, 1..8 (default 4)\n"
    "  --tone HZ      baseband tone offset, ABOVE the dial (default 10000)\n"
    "  --tone-step HZ extra offset per DDC (default 1000; ignored with --diversity)\n"
    "  --diversity    coherent-pair mode: every DDC carries the SAME signal, as\n"
    "                 two coherent ADCs would (opt-in; the default per-DDC tone\n"
    "                 offset is what proves DDC -> receiver routing)\n"
    "  --div-gain dB  relative gain of DDC n>0 in coherent mode (default 0)\n"
    "  --div-phase D  relative phase in degrees of DDC n>0 (default 0)\n"
    "  --amp A        tone amplitude, fraction of full scale (default 0.4)\n"
    "  --noise A      noise floor amplitude (default 0.002)\n"
    "  --no-mic       do not send the (silent) mic/line stream\n"
    "  --wb-rate N    wideband samples/s when the app asks for it (default 96000)\n"
    "  --pace N       stream at 1/N of real time (default 1 = real time);\n"
    "                 use e.g. 8 to feed a SANITIZE=1 build it can keep up with\n"
    "  --mac aa:bb:.. reported MAC address\n"
    "  --quiet        no per-second status line\n", me);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--board") && i + 1 < argc)           opt_board = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--version") && i + 1 < argc)    opt_version = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--receivers") && i + 1 < argc)  opt_receivers = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--tone") && i + 1 < argc)       opt_tone_hz = atof(argv[++i]);
    else if (!strcmp(argv[i], "--tone-step") && i + 1 < argc)  opt_tone_step = atof(argv[++i]);
    else if (!strcmp(argv[i], "--amp") && i + 1 < argc)        opt_amp = atof(argv[++i]);
    else if (!strcmp(argv[i], "--noise") && i + 1 < argc)      opt_noise = atof(argv[++i]);
    else if (!strcmp(argv[i], "--diversity"))                  opt_diversity = 1;
    else if (!strcmp(argv[i], "--div-gain") && i + 1 < argc)   opt_div_gain = atof(argv[++i]);
    else if (!strcmp(argv[i], "--div-phase") && i + 1 < argc)  opt_div_phase = atof(argv[++i]);
    else if (!strcmp(argv[i], "--no-mic"))                     opt_mic = 0;
    else if (!strcmp(argv[i], "--wb-rate") && i + 1 < argc)    opt_wb_rate = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--pace") && i + 1 < argc)       opt_pace = atof(argv[++i]);
    else if (!strcmp(argv[i], "--quiet"))                      opt_quiet = 1;
    else if (!strcmp(argv[i], "--mac") && i + 1 < argc) {
      unsigned m[6];
      if (sscanf(argv[++i], "%x:%x:%x:%x:%x:%x", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6)
        for (int k = 0; k < 6; k++) opt_mac[k] = (unsigned char)m[k];
    }
    else { usage(argv[0]); return 2; }
  }
  // MAX_RECEIVERS in the app is 8 and there are only eight I/Q ports; a larger
  // count would make it index data_addr[] past its end.
  if (opt_receivers < 1) opt_receivers = 1;
  if (opt_receivers > MAX_DDC) opt_receivers = MAX_DDC;
  if (opt_pace <= 0.0) opt_pace = 1.0;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  // Line-buffered even when stdout is a pipe: this is a status log from a
  // long-running server, and a log that only appears when the process is killed
  // is no log at all.
  setvbuf(stdout, NULL, _IOLBF, 0);

  int fd_cmd[6];
  static const int cmd_port[6] = { PORT_GENERAL, PORT_RXSPEC, PORT_TXSPEC,
                                   PORT_HIGHPRIO, PORT_AUDIO, PORT_TXIQ };
  for (int i = 0; i < 6; i++) {
    fd_cmd[i] = bind_udp(cmd_port[i]);
    if (fd_cmd[i] < 0) return 1;
  }
  int fd_iq[MAX_DDC];
  for (int i = 0; i < MAX_DDC; i++) {
    fd_iq[i] = bind_udp(PORT_IQ_BASE + i);
    if (fd_iq[i] < 0) return 1;
  }

  for (int i = 0; i < MAX_DDC; i++) ddc_rate[i] = 192000;

  printf("p2-emu: board %d, version %d, %d DDC(s), "
         "MAC %02X:%02X:%02X:%02X:%02X:%02X, tone %.0f Hz (+%.0f per DDC)\n",
         opt_board, opt_version, opt_receivers,
         opt_mac[0], opt_mac[1], opt_mac[2], opt_mac[3], opt_mac[4], opt_mac[5],
         opt_tone_hz, opt_tone_step);
  printf("p2-emu: listening on UDP %d-%d and %d-%d; "
         "answers only requests from this host\n",
         PORT_GENERAL, PORT_TXIQ, PORT_IQ_BASE, PORT_IQ_BASE + MAX_DDC - 1);
  if (opt_pace != 1.0)
    printf("p2-emu: PACED at 1/%.3g of real time -- not what a board does\n", opt_pace);

  struct sockaddr_in peer;
  socklen_t peerlen = 0;
  int have_peer = 0;
  memset(&peer, 0, sizeof(peer));

  long long next_iq_us[MAX_DDC];
  for (int i = 0; i < MAX_DDC; i++) next_iq_us[i] = 0;
  long long next_hp_us = 0, next_mic_us = 0, next_wb_us = 0;
  uint32_t hp_seq = 0, mic_seq = 0, wb_seq = 0;
  long long last_report_us = now_us();
  long long iq_pkts = 0, reg_pkts = 0, discoveries = 0, refused = 0, drained = 0;

  unsigned char iq_pkt[IQ_PKT_BYTES];
  unsigned char wb_pkt[WB_PKT_BYTES];
  unsigned char mic_pkt[MIC_PKT_BYTES];
  unsigned char hp_pkt[60];
  unsigned char rxbuf[2048];

  struct pollfd pfd[6];
  for (int i = 0; i < 6; i++) { pfd[i].fd = fd_cmd[i]; pfd[i].events = POLLIN; }

  while (!stop_now) {
    // 2 ms: this loop both answers registers and paces every stream, so it must
    // never park while a packet is due.
    poll(pfd, 6, 2);

    for (int i = 0; i < 6; i++) {
      if (!(pfd[i].revents & POLLIN)) continue;
      for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd_cmd[i], rxbuf, sizeof(rxbuf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) break;
        if (!is_local_address(from.sin_addr)) { refused++; continue; }

        // Discovery: 60 bytes, [0..3] zero and [4] == 2.  It arrives on the
        // general-register port, so it has to be told apart from a general
        // register block -- which is also 60 bytes with a zero sequence on its
        // first send.  [4] is the discriminator: general registers put nothing
        // there, discovery puts the command byte.
        if (cmd_port[i] == PORT_GENERAL && n >= 5 &&
            rxbuf[0] == 0 && rxbuf[1] == 0 && rxbuf[2] == 0 && rxbuf[3] == 0 &&
            rxbuf[4] == 0x02) {
          discoveries++;
          unsigned char rep[60];
          memset(rep, 0, sizeof(rep));
          rep[4]  = 2;                     // status 2 = idle (3 = in use)
          memcpy(&rep[5], opt_mac, 6);
          rep[11] = (unsigned char)opt_board;
          rep[13] = (unsigned char)opt_version;
          rep[20] = (unsigned char)opt_receivers;
          sendto(fd_cmd[i], rep, sizeof(rep), 0, (struct sockaddr *)&from, fromlen);
          continue;
        }

        // Every other packet comes from the app's one data socket, so any of
        // them identifies the peer to stream back to.
        peer = from; peerlen = fromlen; have_peer = 1;
        reg_pkts++;

        if (cmd_port[i] == PORT_GENERAL && n >= 24) {
          int wb = rxbuf[23] & 0x01;
          if (wb != wb_enable) {
            wb_enable = wb;
            printf("p2-emu: wideband ADC feed %s\n", wb ? "ENABLED" : "disabled");
            next_wb_us = now_us();
          }
        } else if (cmd_port[i] == PORT_RXSPEC && n >= 1444) {
          unsigned en = rxbuf[7];
          unsigned sync_changed = 0;
          for (int d = 0; d < MAX_DDC; d++) {
            int r = (((rxbuf[18 + d * 6] & 0xFF) << 8) | (rxbuf[19 + d * 6] & 0xFF)) * 1000;
            if (r > 0 && r != ddc_rate[d]) ddc_rate[d] = r;
            ddc_adc[d] = rxbuf[17 + d * 6] & 0xFF;
            // Sync map: byte 1363+d names the DDCs that FOLLOW DDC d.  Reading
            // it is what makes the app's register mean something here rather
            // than being written into the void.
            unsigned sy = rxbuf[RXSPEC_SYNC_BASE + d] & 0xFF;
            if (sy != ddc_sync[d]) { ddc_sync[d] = sy; sync_changed = 1; }
          }
          if (en != ddc_enable || sync_changed) {
            for (int d = 0; d < MAX_DDC; d++)
              if ((en & ~ddc_enable) & (1u << d)) next_iq_us[d] = now_us();
            ddc_enable = en;
            printf("p2-emu: DDC enable=0x%02X rates", ddc_enable);
            for (int d = 0; d < MAX_DDC; d++)
              if (ddc_enable & (1u << d)) printf(" [%d]=%d/adc%d", d, ddc_rate[d], ddc_adc[d]);
            for (int d = 0; d < MAX_DDC; d++) {
              int f = sync_follower(d);
              if (f >= 0)
                printf("  SYNC: DDC%d(adc%d) follows DDC%d(adc%d), interleaved on port %d",
                       f, ddc_adc[f], d, ddc_adc[d], PORT_IQ_BASE + d);
            }
            printf("\n");
          }
        } else if (cmd_port[i] == PORT_HIGHPRIO && n >= 1444) {
          int run = rxbuf[4] & 0x01;
          for (int d = 0; d < MAX_DDC; d++) {
            uint32_t ph = ((uint32_t)rxbuf[9 + d * 4] << 24) |
                          ((uint32_t)rxbuf[10 + d * 4] << 16) |
                          ((uint32_t)rxbuf[11 + d * 4] <<  8) |
                          ((uint32_t)rxbuf[12 + d * 4]);
            // The app sends a phase word, not a frequency: f = phase * fs / 2^32
            // with fs = 122.88 MHz (protocol2_high_priority()).
            ddc_freq[d] = (long long)((double)ph * 122880000.0 / 4294967296.0);
          }
          if (run != running) {
            running = run;
            if (running) {
              printf("p2-emu: START from %s:%d (DDC0 at %lld Hz)\n",
                     inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), ddc_freq[0]);
              long long t = now_us();
              for (int d = 0; d < MAX_DDC; d++) next_iq_us[d] = t;
              next_hp_us = next_mic_us = next_wb_us = t;
            } else {
              printf("p2-emu: STOP\n");
            }
          }
        } else if (cmd_port[i] == PORT_AUDIO || cmd_port[i] == PORT_TXIQ) {
          // Drained on purpose: the app must never see its RX-audio or TX-I/Q
          // sends fail, and there is nothing here to play or transmit them.
          drained++;
        }
      }
    }

    if (!running || !have_peer) continue;

    long long t = now_us();
    int dead_peer = 0;

    // DDC I/Q, one paced stream per enabled DDC.  Catch-up is capped: if this
    // process is descheduled (or run under a sanitiser) sending the whole
    // backlog at once would hand the app a burst it never sees from a board.
    for (int d = 0; d < MAX_DDC && !dead_peer; d++) {
      if (!(ddc_enable & (1u << d))) continue;
      // A DDC that FOLLOWS another does not stream on its own port -- its
      // samples ride the base DDC's packets.  Skipping it here is not an
      // optimisation: sending it as well would be a stream the app is not
      // reading, and the enable bit for it should not be set in the first place.
      int is_follower = 0;
      for (int m = 0; m < MAX_DDC; m++)
        if (m != d && (ddc_sync[m] & (1u << d))) is_follower = 1;
      if (is_follower) continue;
      // A base DDC with a follower packs two streams into one packet, so each
      // stream gets half the samples and the packet interval halves to keep the
      // per-stream rate at ddc_rate.
      int nstreams = (sync_follower(d) >= 0) ? 2 : 1;
      int budget = 32;
      while (t >= next_iq_us[d] && budget-- > 0) {
        long long interval_us = (long long)((double)(IQ_SAMPLES / nstreams) * 1e6 * opt_pace /
                                            (double)(ddc_rate[d] > 0 ? ddc_rate[d] : 192000));
        if (interval_us < 1) interval_us = 1;
        build_iq_packet(iq_pkt, d);
        // Destination is the app's single data socket; what tells it which DDC
        // this is, is the SOURCE port -- hence one bound socket per DDC.
        if (sendto(fd_iq[d], iq_pkt, sizeof(iq_pkt), 0,
                   (struct sockaddr *)&peer, peerlen) < 0) {
          if (errno == ECONNREFUSED) { dead_peer = 1; break; }
        }
        iq_pkts++;
        next_iq_us[d] += interval_us;
        if (t - next_iq_us[d] > 200000) next_iq_us[d] = t;
      }
    }

    // High-priority status back to the host, 20/s.  A healthy idle board: PTT,
    // dot and dash released, PLL locked, no ADC overload, ~13.8 V supply (the
    // app reads that through calibrate() in protocol2.c).
    if (!dead_peer && t >= next_hp_us) {
      memset(hp_pkt, 0, sizeof(hp_pkt));
      put_be32(hp_pkt, hp_seq++);
      hp_pkt[4] = 0x08;                 // bit 3 = PLL locked
      hp_pkt[49] = (1421 >> 8) & 0xFF;  // supply volts, ~13.8 V through calibrate()
      hp_pkt[50] =  1421       & 0xFF;
      if (sendto(fd_cmd[1], hp_pkt, sizeof(hp_pkt), 0,
                 (struct sockaddr *)&peer, peerlen) < 0 && errno == ECONNREFUSED)
        dead_peer = 1;
      next_hp_us = t + 50000;
    }

    // Mic/line samples, silence, at the 48 kHz / MIC_SAMPLES cadence a board
    // uses.  Not decoration: it is the only thing that runs process_mic_data(),
    // and through it add_mic_sample() and the whole TX DSP block assembly.
    if (opt_mic && !dead_peer) {
      int budget = 16;
      while (t >= next_mic_us && budget-- > 0) {
        memset(mic_pkt, 0, sizeof(mic_pkt));
        put_be32(mic_pkt, mic_seq++);
        if (sendto(fd_cmd[2], mic_pkt, sizeof(mic_pkt), 0,
                   (struct sockaddr *)&peer, peerlen) < 0 && errno == ECONNREFUSED) {
          dead_peer = 1; break;
        }
        long long iv = (long long)((double)MIC_SAMPLES * 1e6 * opt_pace / 48000.0);
        if (iv < 1) iv = 1;
        next_mic_us += iv;
        if (t - next_mic_us > 200000) next_mic_us = t;
      }
    }

    // Wideband ADC samples, only while the app has a wideband window open.
    if (wb_enable && !dead_peer) {
      int budget = 8;
      while (t >= next_wb_us && budget-- > 0) {
        put_be32(wb_pkt, wb_seq++);
        for (int s = 0; s < WB_SAMPLES; s++) {
          double v = 0.05 * noise_sample();
          int16_t q = (int16_t)(v * 32767.0);
          wb_pkt[4 + s * 2]     = (unsigned char)((q >> 8) & 0xFF);
          wb_pkt[4 + s * 2 + 1] = (unsigned char)( q       & 0xFF);
        }
        if (sendto(fd_cmd[3], wb_pkt, sizeof(wb_pkt), 0,
                   (struct sockaddr *)&peer, peerlen) < 0 && errno == ECONNREFUSED) {
          dead_peer = 1; break;
        }
        long long iv = (long long)((double)WB_SAMPLES * 1e6 * opt_pace /
                                   (double)(opt_wb_rate > 0 ? opt_wb_rate : 96000));
        if (iv < 1) iv = 1;
        next_wb_us += iv;
        if (t - next_wb_us > 200000) next_wb_us = t;
      }
    }

    if (dead_peer) {
      running = 0;
      have_peer = 0;
      printf("p2-emu: peer gone, stopped streaming\n");
    }

    if (!opt_quiet && t - last_report_us >= 1000000) {
      printf("p2-emu: %lld I/Q pkts/s on DDCs 0x%02X, %lld register pkts, "
             "%lld drained%s\n",
             iq_pkts, ddc_enable, reg_pkts, drained,
             refused ? " (some non-local requests refused)" : "");
      iq_pkts = 0; reg_pkts = 0; drained = 0;
      last_report_us = t;
    }
  }

  printf("p2-emu: exit (%lld discoveries answered, %lld refused as non-local)\n",
         discoveries, refused);
  for (int i = 0; i < 6; i++) close(fd_cmd[i]);
  for (int i = 0; i < MAX_DDC; i++) close(fd_iq[i]);
  return 0;
}
