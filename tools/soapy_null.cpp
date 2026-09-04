// A SoapySDR device that is not there: a null radio, in one file.
//
//   tools/build-soapy-null.sh                       # builds build/soapy-null/
//   export SOAPY_SDR_PLUGIN_PATH=build/soapy-null
//   SoapySDRUtil --find
//   HOME=$(mktemp -d) ./machpsdr --open machpsdrnull
//
// (no line here ends in a backslash on purpose: a `//` comment continued that
// way swallows the line after it, and gcc -Wall says so where clang does not.)
//
// WHY THIS EXISTS.  The SoapySDR path in this tree -- device open/close, stream
// setup and teardown, and above all the receive thread in soapy_protocol.c,
// which is handed its RECEIVER once and keeps it for its whole life and so asks
// receiver_is_live() under delete_rx_mutex instead of re-reading the slot -- had
// never been executed by anything.  It is the last of the three protocol paths
// where "reasoned and compiled" was the whole claim.  metis_emu.c answers that
// for protocol 1 by speaking UDP; SoapySDR has no wire to fake, it has a
// PLUGIN ABI, so the equivalent is a module that enumerates, opens and streams.
//
// USE WITH MACHPSDR_RX_CHURN.  The pairing this was written for:
//
//   export SOAPY_SDR_PLUGIN_PATH=build/soapy-null
//   export MACHPSDR_RX_CHURN=10 MACHPSDR_NULL_PACE=8
//   HOME=$(mktemp -d) ./machpsdr --open machpsdrnull          # SANITIZE=1 build
//
// which adds and closes receivers over a live SoapySDR stream.  The device
// advertises two RX channels for exactly that reason: create_receiver gives
// receiver 1 adc=1 on PROTOCOL_SOAPYSDR, so a second receiver is a second
// channel and a second stream, and add_receiver/receiver_close then drive the
// whole soapy_protocol_create_receiver -> start_receiver -> delete_receiver
// cycle under the sanitisers.  With MACHPSDR_NULL_PACE=8 the feed is slowed to
// 1/8 of real time, which is what an instrumented build needs (the same reason
// metis_emu has --pace).
//
// OPT-IN, AND ONLY OPT-IN.  This is not built by `make`, is not in `make check`,
// is not installed, and cannot be loaded by accident: SoapySDR only sees it when
// SOAPY_SDR_PLUGIN_PATH names the directory it was built into, which is
// gitignored and lives inside the repo.  Nothing about an ordinary build or an
// ordinary run changes.
//
// WHAT IT PUTS ON THE AIR.  A single complex exponential MACHPSDR_NULL_TONE Hz
// (default +40000) from whatever centre frequency the app tuned, plus a low
// uniform noise floor.  The sign is the part worth reading twice.  SoapySDR's
// CF32 is the ordinary (I, Q) convention, this program's I/Q buffers are (Q, I)
// -- see CLAUDE.md, "I/Q buffer order" -- and radio.c sets radio->iqswap=TRUE
// for every DEVICE_SOAPYSDR, which is precisely the swap that undoes it.  So the
// two cancel and a tone emitted here as I=cos, Q=sin at +f lands at centre+f,
// i.e. to the RIGHT of centre on the panadapter.  A positive default was chosen
// so that "the tone is on the wrong side" is a visible failure rather than a
// symmetric non-observation.  There is nothing else in the spectrum: no image,
// no DC spike, no LO leakage.
//
// WHAT IT IS NOT.  It models the SoapySDR API surface this app calls and
// nothing else.  Sample rate, bandwidth, frequency, antenna, gain and gain mode
// are stored and read back verbatim -- they never affect the samples, so the
// tone does not move when you tune, and turning the gain down does not make it
// quieter.  There is no hardware timing, no burst mode, no stream time stamps
// beyond a monotonic count, no sensors worth reading, no half-duplex constraint
// and no failure injection.  A green run says this app's SoapySDR path survives
// a live stream and a receiver teardown; it says nothing whatever about how any
// real driver behaves.
//
// KNOBS (environment, read at open; a device arg of the same lowercase name
// wins if one is passed):
//
//   MACHPSDR_NULL_TONE   Hz offset of the tone from centre      (40000)
//   MACHPSDR_NULL_PACE   stream at 1/N of real time             (1)
//   MACHPSDR_NULL_RX     RX channel count                       (2)
//   MACHPSDR_NULL_TX     TX channel count                       (1)
//   MACHPSDR_NULL_AMPL   tone amplitude, full scale = 1.0       (0.1)
//   MACHPSDR_NULL_NOISE  noise amplitude, full scale = 1.0      (0.002)

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static const char *DRIVER_KEY = "machpsdrnull";

// ---- knobs -----------------------------------------------------------------

// std::atof honours the process locale, and this module is dlopen'd INTO an app
// that has called setlocale(LC_ALL,"") -- so on a comma-decimal machine a knob
// written the way every other knob in this tree is written ("0.9999986979")
// parses as 0 and the fixture silently runs unconfigured.  That is worse than a
// wrong number: it is a test that reports PASS while exercising nothing, which
// is exactly what happened the first time the rate-substitution knob was used
// below 1.  Parse in the C locale, always, like the property store does.
static double parse_c(const char *s) {
  std::istringstream is(s);
  is.imbue(std::locale::classic());
  double v = 0.0;
  is >> v;
  return is.fail() ? 0.0 : v;
}

static double env_or(const SoapySDR::Kwargs &args, const char *key,
                     const char *env, double dflt) {
  const auto it = args.find(key);
  if (it != args.end() && !it->second.empty()) return parse_c(it->second.c_str());
  const char *e = std::getenv(env);
  if (e != nullptr && *e != '\0') return parse_c(e);
  return dflt;
}

// ---- the stream ------------------------------------------------------------
//
// One of these per setupStream().  It carries its own lock because the app can
// (and, when a receiver is closed, does) end up with more than one thread
// reading the same stream: soapy_protocol.c's receive thread never exits, so a
// churn cycle leaves the previous thread reading channel 1 while the new one
// reads the stream that replaced it.  A real driver would be entitled to
// misbehave there; this one must not, or every finding is its own.

struct NullStream {
  int direction = SOAPY_SDR_RX;
  std::string format = SOAPY_SDR_CF32;
  std::vector<size_t> channels;
  bool active = false;

  std::mutex mtx;
  std::chrono::steady_clock::time_point t0;
  long long produced = 0;          // samples handed out since activation
  double phase = 0.0;              // tone phase accumulator, radians
  uint32_t rng = 0x1234567u;
};

// ---- the device ------------------------------------------------------------

class MachpsdrNull : public SoapySDR::Device {
public:
  explicit MachpsdrNull(const SoapySDR::Kwargs &args) {
    rx_channels = (size_t)std::max(1.0, env_or(args, "rx", "MACHPSDR_NULL_RX", 2));
    tx_channels = (size_t)std::max(0.0, env_or(args, "tx", "MACHPSDR_NULL_TX", 1));
    tone_hz = env_or(args, "tone", "MACHPSDR_NULL_TONE", 40000.0);
    pace = std::max(1.0, env_or(args, "pace", "MACHPSDR_NULL_PACE", 1));
    ampl = env_or(args, "ampl", "MACHPSDR_NULL_AMPL", 0.1);
    noise = env_or(args, "noise", "MACHPSDR_NULL_NOISE", 0.002);
    // Reproduce the way a driver may answer a rate its hardware cannot deliver:
    // accept the call, return success, run somewhere else entirely, and say so
    // only through getSampleRate().  SoapyPlutoSDR does exactly this with x8 --
    // the AD9361 has no stream below ~2.08 MHz, so a request for 768 kHz comes
    // back running at 6.144 MHz.  An app that believes the request then reads
    // the stream eight times too fast.  Default 1 = no substitution.
    rate_mult = std::max(0.0, env_or(args, "rate_mult", "MACHPSDR_NULL_RATE_MULT", 1.0));
    if (rate_mult <= 0.0) rate_mult = 1.0;
    // Pluto-like shared clock, for live device-rate transition tests.
    shared_rate = env_or(args, "shared_rate", "MACHPSDR_NULL_SHARED_RATE", 0) != 0;
    fail_rx_setup_rate = env_or(args, "fail_rx_setup_rate", "MACHPSDR_NULL_FAIL_RX_SETUP_RATE", 0);

    rx_state.resize(rx_channels);
    tx_state.resize(std::max<size_t>(tx_channels, 1));

    SoapySDR::logf(SOAPY_SDR_INFO,
                   "machpsdrnull: rx=%d tx=%d tone=%g Hz pace=%g ampl=%g noise=%g",
                   (int)rx_channels, (int)tx_channels, tone_hz, pace, ampl, noise);
  }

  ~MachpsdrNull() override {
    SoapySDR::log(SOAPY_SDR_INFO, "machpsdrnull: closed");
  }

  // ---- identification ----

  std::string getDriverKey(void) const override { return DRIVER_KEY; }
  std::string getHardwareKey(void) const override { return "MachPSDR Null"; }

  SoapySDR::Kwargs getHardwareInfo(void) const override {
    SoapySDR::Kwargs info;
    info["fw_version"] = "0.0";
    info["origin"] = "tools/soapy_null.cpp";
    return info;
  }

  // ---- channels ----

  size_t getNumChannels(const int direction) const override {
    return direction == SOAPY_SDR_RX ? rx_channels : tx_channels;
  }

  bool getFullDuplex(const int, const size_t) const override { return true; }

  // ---- antennas ----

  std::vector<std::string> listAntennas(const int direction, const size_t) const override {
    if (direction == SOAPY_SDR_RX) return {"RX", "RX2"};
    return {"TX"};
  }
  void setAntenna(const int direction, const size_t channel, const std::string &name) override {
    chan(direction, channel).antenna = name;
  }
  std::string getAntenna(const int direction, const size_t channel) const override {
    return chan(direction, channel).antenna;
  }

  // ---- gain ----

  std::vector<std::string> listGains(const int direction, const size_t) const override {
    if (direction == SOAPY_SDR_RX) return {"LNA", "VGA"};
    return {"PA"};
  }
  bool hasGainMode(const int direction, const size_t) const override {
    return direction == SOAPY_SDR_RX;
  }
  void setGainMode(const int direction, const size_t channel, const bool automatic) override {
    chan(direction, channel).agc = automatic;
  }
  bool getGainMode(const int direction, const size_t channel) const override {
    return chan(direction, channel).agc;
  }
  void setGain(const int direction, const size_t channel, const double value) override {
    chan(direction, channel).gain = clampd(value, 0.0, 60.0);
  }
  void setGain(const int direction, const size_t channel, const std::string &name,
               const double value) override {
    chan(direction, channel).element_gain[name] = clampd(value, 0.0, 30.0);
  }
  double getGain(const int direction, const size_t channel) const override {
    return chan(direction, channel).gain;
  }
  double getGain(const int direction, const size_t channel,
                 const std::string &name) const override {
    const auto &m = chan(direction, channel).element_gain;
    const auto it = m.find(name);
    return it == m.end() ? 0.0 : it->second;
  }
  SoapySDR::Range getGainRange(const int, const size_t) const override {
    return SoapySDR::Range(0.0, 60.0, 1.0);
  }
  SoapySDR::Range getGainRange(const int, const size_t, const std::string &) const override {
    return SoapySDR::Range(0.0, 30.0, 1.0);
  }

  // ---- frequency ----

  std::vector<std::string> listFrequencies(const int, const size_t) const override {
    return {"RF"};
  }
  void setFrequency(const int direction, const size_t channel, const double frequency,
                    const SoapySDR::Kwargs &) override {
    chan(direction, channel).freq = frequency;
  }
  void setFrequency(const int direction, const size_t channel, const std::string &,
                    const double frequency, const SoapySDR::Kwargs &) override {
    chan(direction, channel).freq = frequency;
  }
  double getFrequency(const int direction, const size_t channel) const override {
    return chan(direction, channel).freq;
  }
  double getFrequency(const int direction, const size_t channel,
                      const std::string &) const override {
    return chan(direction, channel).freq;
  }
  SoapySDR::RangeList getFrequencyRange(const int, const size_t) const override {
    return {SoapySDR::Range(100e3, 6e9, 1.0)};
  }
  SoapySDR::RangeList getFrequencyRange(const int, const size_t,
                                        const std::string &) const override {
    return {SoapySDR::Range(100e3, 6e9, 1.0)};
  }

  // ---- sample rate ----

  void setSampleRate(const int direction, const size_t channel, const double rate) override {
    // No error, no clue -- see rate_mult in the constructor.  The stream really
    // does run at the substituted rate (the pacing and the tone below both use
    // it), so an app that trusts its request rather than getSampleRate() gets
    // the whole fault and not just a wrong number in a log line.
    chan(direction, channel).rate = clampd(rate * rate_mult, 48e3, 20e6);
    if (shared_rate) {
      const double actual = chan(direction, channel).rate;
      for (auto &state : rx_state) state.rate = actual;
      for (auto &state : tx_state) state.rate = actual;
    }
  }
  double getSampleRate(const int direction, const size_t channel) const override {
    return chan(direction, channel).rate;
  }
  std::vector<double> listSampleRates(const int, const size_t) const override {
    return {192000.0, 384000.0, 768000.0, 1536000.0, 1920000.0, 2400000.0, 8000000.0};
  }
  SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override {
    return {SoapySDR::Range(48e3, 20e6, 0.0)};
  }

  // ---- bandwidth ----

  // The analogue filter is a LADDER of fixed steps and setBandwidth rounds DOWN
  // to the nearest one -- the MAX2837's, which is what a HackRF really does
  // (hackrf_compute_baseband_filter_bw).  It is modelled here because rounding
  // down is the whole reason the app has to pick a step at or above the rate:
  // ask this device for 9600000 and the filter runs at 8 MHz, so the outer
  // 800 kHz of each side of the span is attenuated before anything downstream
  // can see it.  A driver that reported a continuous range (as this one used to)
  // hides that, and hides the fix with it.
  static const std::vector<double> &bw_ladder(void) {
    static const std::vector<double> l = {1.75e6, 2.5e6, 3.5e6, 5e6, 5.5e6, 6e6, 7e6,
                                          8e6, 9e6, 10e6, 12e6, 14e6, 15e6, 20e6, 24e6, 28e6};
    return l;
  }
  void setBandwidth(const int direction, const size_t channel, const double bw) override {
    double got = bw_ladder().front();
    for (double step : bw_ladder()) if (step <= bw) got = step;   // round DOWN, like the part
    chan(direction, channel).bandwidth = got;
  }
  double getBandwidth(const int direction, const size_t channel) const override {
    return chan(direction, channel).bandwidth;
  }
  std::vector<double> listBandwidths(const int, const size_t) const override {
    return bw_ladder();
  }
  SoapySDR::RangeList getBandwidthRange(const int, const size_t) const override {
    SoapySDR::RangeList rl;
    for (double step : bw_ladder()) rl.push_back(SoapySDR::Range(step, step));  // fixed steps
    return rl;
  }

  // ---- sensors ----

  std::vector<std::string> listSensors(void) const override { return {"clock_locked"}; }
  std::string readSensor(const std::string &key) const override {
    return key == "clock_locked" ? "true" : "";
  }

  // ---- DC offset ----

  bool hasDCOffsetMode(const int direction, const size_t) const override {
    return direction == SOAPY_SDR_RX;
  }
  void setDCOffsetMode(const int direction, const size_t channel, const bool automatic) override {
    chan(direction, channel).dc_auto = automatic;
  }
  bool getDCOffsetMode(const int direction, const size_t channel) const override {
    return chan(direction, channel).dc_auto;
  }

  // ---- streaming ----

  std::vector<std::string> getStreamFormats(const int, const size_t) const override {
    return {SOAPY_SDR_CF32, SOAPY_SDR_CS16};
  }

  std::string getNativeStreamFormat(const int, const size_t, double &fullScale) const override {
    fullScale = 32767.0;
    return SOAPY_SDR_CS16;
  }

  SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                const std::vector<size_t> &channels,
                                const SoapySDR::Kwargs &) override {
    if (format != SOAPY_SDR_CF32 && format != SOAPY_SDR_CS16) {
      throw std::runtime_error("machpsdrnull: unsupported stream format " + format);
    }
    if (direction == SOAPY_SDR_RX && fail_rx_setup_rate > 0.0 &&
        getSampleRate(direction, channels.empty() ? 0 : channels[0]) == fail_rx_setup_rate) {
      throw std::runtime_error("machpsdrnull: injected RX setup failure");
    }
    NullStream *s = new NullStream();
    s->direction = direction;
    s->format = format;
    s->channels = channels.empty() ? std::vector<size_t>{0} : channels;
    for (size_t c : s->channels) {
      if (c >= getNumChannels(direction)) {
        delete s;
        throw std::runtime_error("machpsdrnull: channel out of range");
      }
    }
    // A distinct noise seed per stream so two receivers are not bit-identical.
    s->rng = 0x1234567u + (uint32_t)(s->channels[0] * 2654435761u) +
             (uint32_t)(direction == SOAPY_SDR_RX ? 0 : 0x9e3779b9u);
    SoapySDR::logf(SOAPY_SDR_INFO, "machpsdrnull: setupStream %s ch=%d fmt=%s",
                   direction == SOAPY_SDR_RX ? "RX" : "TX", (int)s->channels[0],
                   format.c_str());
    return reinterpret_cast<SoapySDR::Stream *>(s);
  }

  void closeStream(SoapySDR::Stream *stream) override {
    NullStream *s = cast(stream);
    if (s == nullptr) return;
    SoapySDR::logf(SOAPY_SDR_INFO, "machpsdrnull: closeStream %s ch=%d",
                   s->direction == SOAPY_SDR_RX ? "RX" : "TX", (int)s->channels[0]);
    delete s;
  }

  size_t getStreamMTU(SoapySDR::Stream *) const override { return 2048; }

  int activateStream(SoapySDR::Stream *stream, const int, const long long,
                     const size_t) override {
    NullStream *s = cast(stream);
    if (s == nullptr) return SOAPY_SDR_STREAM_ERROR;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->active = true;
    s->t0 = std::chrono::steady_clock::now();
    s->produced = 0;
    s->phase = 0.0;
    return 0;
  }

  int deactivateStream(SoapySDR::Stream *stream, const int, const long long) override {
    NullStream *s = cast(stream);
    if (s == nullptr) return SOAPY_SDR_STREAM_ERROR;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->active = false;
    return 0;
  }

  int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems,
                 int &flags, long long &timeNs, const long timeoutUs) override {
    NullStream *s = cast(stream);
    if (s == nullptr) return SOAPY_SDR_STREAM_ERROR;

    const double rate = chan(SOAPY_SDR_RX, s->channels[0]).rate;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(timeoutUs < 0 ? 0 : timeoutUs);

    std::unique_lock<std::mutex> lk(s->mtx);
    if (!s->active) return SOAPY_SDR_TIMEOUT;

    long long avail = 0;
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed = std::chrono::duration<double>(now - s->t0).count();
      const long long due = (long long)(elapsed * rate / pace);
      avail = due - s->produced;
      // Never stream faster than (paced) real time.  If the app stalled -- which
      // a SANITIZE=1 build does routinely -- discard the backlog rather than
      // handing it over in a burst: a source that outruns the consumer is the
      // HackRF runaway soapy_protocol_rx_resume() was written for, and it would
      // arrive here as a mystery rather than as a test.
      if (avail > (long long)(rate / 2.0)) {
        s->produced = due;
        avail = 0;
        flags |= SOAPY_SDR_END_ABRUPT;
      }
      if (avail > 0) break;
      if (now >= deadline) return SOAPY_SDR_TIMEOUT;
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      lk.lock();
      if (!s->active) return SOAPY_SDR_TIMEOUT;
    }

    size_t n = (size_t)std::min<long long>(avail, (long long)numElems);
    n = std::min<size_t>(n, getStreamMTU(stream));

    const double dphi = 2.0 * M_PI * tone_hz / rate;
    const bool cf32 = (s->format == SOAPY_SDR_CF32);
    float *fbuf = cf32 ? (float *)buffs[0] : nullptr;
    int16_t *sbuf = cf32 ? nullptr : (int16_t *)buffs[0];

    for (size_t i = 0; i < n; i++) {
      // I = cos, Q = sin: a tone ABOVE the tuned centre in SoapySDR's ordinary
      // (I, Q) convention.  See the header for why that lands to the right of
      // centre on this app's panadapter and not to the left.
      const double ii = ampl * std::cos(s->phase) + noise * urand(s->rng);
      const double qq = ampl * std::sin(s->phase) + noise * urand(s->rng);
      s->phase += dphi;
      if (s->phase > 2.0 * M_PI) s->phase -= 2.0 * M_PI;
      if (cf32) {
        fbuf[2 * i] = (float)ii;
        fbuf[2 * i + 1] = (float)qq;
      } else {
        sbuf[2 * i] = (int16_t)clampd(ii * 32767.0, -32767.0, 32767.0);
        sbuf[2 * i + 1] = (int16_t)clampd(qq * 32767.0, -32767.0, 32767.0);
      }
    }
    // Every stream channel gets the same samples; the app only ever asks for one
    // channel per stream, so this is for completeness rather than for use.
    for (size_t c = 1; c < s->channels.size(); c++) {
      std::memcpy(buffs[c], buffs[0], n * (cf32 ? 2 * sizeof(float) : 2 * sizeof(int16_t)));
    }

    s->produced += (long long)n;
    timeNs = (long long)(1e9 * (double)s->produced / rate);
    flags |= SOAPY_SDR_HAS_TIME;
    return (int)n;
  }

  int writeStream(SoapySDR::Stream *stream, const void *const *, const size_t numElems,
                  int &, const long long, const long timeoutUs) override {
    NullStream *s = cast(stream);
    if (s == nullptr) return SOAPY_SDR_STREAM_ERROR;
    const double rate = chan(SOAPY_SDR_TX, s->channels[0]).rate;
    // Accept at (paced) real time and throw the samples away, so the app's TX
    // pump is back-pressured exactly as a real sink would do it.
    std::unique_lock<std::mutex> lk(s->mtx);
    if (!s->active) return SOAPY_SDR_TIMEOUT;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(timeoutUs < 0 ? 0 : timeoutUs);
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed = std::chrono::duration<double>(now - s->t0).count();
      const long long due = (long long)(elapsed * rate / pace);
      const long long room = due - s->produced;
      if (room > 0) {
        size_t n = (size_t)std::min<long long>(room, (long long)numElems);
        s->produced += (long long)n;
        return (int)n;
      }
      if (now >= deadline) return SOAPY_SDR_TIMEOUT;
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      lk.lock();
      if (!s->active) return SOAPY_SDR_TIMEOUT;
    }
  }

private:
  struct ChannelState {
    double freq = 100e6;
    double rate = 768000.0;
    double bandwidth = 800000.0;
    double gain = 20.0;
    bool agc = false;
    bool dc_auto = false;
    std::string antenna = "RX";
    std::map<std::string, double> element_gain;
  };

  static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  // Uniform in [-1, 1).  xorshift32: the noise floor only has to be noise.
  static double urand(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return ((double)s / 2147483648.0) - 1.0;
  }

  static NullStream *cast(SoapySDR::Stream *s) { return reinterpret_cast<NullStream *>(s); }

  ChannelState &chan(const int direction, const size_t channel) {
    auto &v = (direction == SOAPY_SDR_RX) ? rx_state : tx_state;
    return v[std::min(channel, v.size() - 1)];
  }
  const ChannelState &chan(const int direction, const size_t channel) const {
    const auto &v = (direction == SOAPY_SDR_RX) ? rx_state : tx_state;
    return v[std::min(channel, v.size() - 1)];
  }

  size_t rx_channels = 2;
  size_t tx_channels = 1;
  double tone_hz = 40000.0;
  double pace = 1.0;
  double ampl = 0.1;
  double noise = 0.002;
  double rate_mult = 1.0;
  bool shared_rate = false;
  double fail_rx_setup_rate = 0.0;

  // Settings are per channel and mutated from the GTK thread while the receive
  // thread reads the rate; benign for a test source, and deliberately not
  // locked -- a lock here would hide nothing and cost a lock per sample block.
  mutable std::vector<ChannelState> rx_state;
  mutable std::vector<ChannelState> tx_state;
};

// ---- registration ----------------------------------------------------------

static SoapySDR::KwargsList findMachpsdrNull(const SoapySDR::Kwargs &args) {
  SoapySDR::KwargsList results;
  const auto d = args.find("driver");
  if (d != args.end() && d->second != DRIVER_KEY) return results;

  SoapySDR::Kwargs dev;
  dev["driver"] = DRIVER_KEY;
  dev["label"] = "MachPSDR Null (synthetic)";
  dev["serial"] = "null0";
  results.push_back(dev);
  return results;
}

static SoapySDR::Device *makeMachpsdrNull(const SoapySDR::Kwargs &args) {
  return new MachpsdrNull(args);
}

static SoapySDR::Registry registerMachpsdrNull(DRIVER_KEY, &findMachpsdrNull,
                                               &makeMachpsdrNull, SOAPY_SDR_ABI_VERSION);
