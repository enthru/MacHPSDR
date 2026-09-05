#!/bin/bash
#
# build-soapy-plutosdr.sh — build the PlutoSDR SoapySDR driver, and the two
# Analog Devices libraries under it, into a prefix `make app` can bundle:
#
#   tools/build-soapy-plutosdr.sh                 # -> build/soapy-plutosdr
#   tools/build-soapy-plutosdr.sh /tmp/somewhere  # -> that prefix instead
#
# WHY THIS EXISTS AT ALL.  HackRF and RTL-SDR arrive as `brew install
# soapyhackrf soapyrtlsdr`; Homebrew core has no PlutoSDR driver and no libiio
# either, and the tap that used to carry them is unmaintained.  So the one SDR
# this fork has done the most work for — every rate-substitution and coprime-
# resampler rule in CLAUDE.md came off a Pluto — is also the one that cannot be
# installed.  Building it here is what puts it in the .app beside the other two.
#
# It is NOT part of `make`.  Nothing links against libiio; this produces a
# dlopen'd module for another project's ABI, and an ordinary build of the
# application must not depend on a network fetch of three repositories.
# `make app` copies whatever it finds in the prefix below, so a tree that has
# never run this script simply ships two drivers instead of three.
#
# VERSIONS ARE PINNED, and the pin is the interesting part.  libiio's 1.x line
# is a different API; SoapyPlutoSDR 0.2.2 is written against 0.x, so v0.26 (the
# last of that line) is the version to build, not the newest tag.  Being pinned
# also means a CI cache can be keyed on this file: change a version here and the
# cache misses, which is the behaviour you want and not a coincidence.
IIO_TAG=v0.26
AD9361_TAG=v0.3
PLUTO_TAG=soapy-plutosdr-0.2.2
#
# libad9361-iio is optional to SoapyPlutoSDR's build and NOT optional to its
# usefulness here: it is what programmes the AD9361's FIR, and the FIR is how a
# Pluto reaches sample rates below the part's ~2.08 MHz floor.  Without it the
# driver quietly substitutes a rate eight times the one asked for — the exact
# fault CLAUDE.md describes as "looks like a bad LNB" — so a bundle built
# without this library ships the bug pre-installed.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PREFIX="${1:-$ROOT/build/soapy-plutosdr}"
WORK="$PREFIX/src"

command -v cmake >/dev/null 2>&1 || {
  echo "error: cmake not found (brew install cmake / apt-get install cmake)" >&2; exit 1; }
command -v git >/dev/null 2>&1 || {
  echo "error: git not found" >&2; exit 1; }

# SoapySDR itself is a build dependency of the driver, and unlike the null
# module this one cannot fall back to a bare -l: CMake has to find the config
# package.  Ask pkg-config first, brew second — the same order, and for the same
# reason, as tools/build-soapy-null.sh.
SOAPY_PREFIX=""
if pkg-config --exists SoapySDR 2>/dev/null; then
  SOAPY_PREFIX="$(pkg-config --variable=prefix SoapySDR)"
elif command -v brew >/dev/null 2>&1 && brew --prefix soapysdr >/dev/null 2>&1; then
  SOAPY_PREFIX="$(brew --prefix soapysdr)"
else
  echo "error: SoapySDR not found.  brew install soapysdr, or" >&2
  echo "       apt-get install libsoapysdr-dev" >&2
  exit 1
fi

EXTRA_PREFIX="$SOAPY_PREFIX"
if command -v brew >/dev/null 2>&1; then
  EXTRA_PREFIX="$EXTRA_PREFIX;$(brew --prefix)"
fi

mkdir -p "$WORK"

clone() {  # clone <url> <tag> <dir>
  local url="$1" tag="$2" dir="$3"
  if [ -d "$WORK/$dir/.git" ]; then
    echo "==> $dir already cloned"
    return
  fi
  echo "==> cloning $dir @ $tag"
  git clone --quiet --depth 1 --branch "$tag" "$url" "$WORK/$dir"
}

build() {  # build <dir> <extra cmake args...>
  local dir="$1"; shift
  echo "==> building $dir"
  # CMAKE_INSTALL_NAME_DIR: without it a dylib built here carries its build
  # directory as its install name, and dylibbundler then cannot rewrite what it
  # cannot resolve.  With it the reference is an absolute path in this prefix,
  # which resolves, gets copied into the bundle and gets relinked.
  #
  # CMAKE_INSTALL_RPATH is the ELF half of exactly the same problem, and it is
  # needed for the same reason and by the same kind of tool.  A Linux .so has no
  # install name; CMake strips the build rpath at install time, so the PlutoSDR
  # module ships a bare DT_NEEDED of libiio.so.0 and nothing that says where
  # that lives -- and libiio lives in THIS PREFIX, not on the system path.
  # linuxdeploy then stops with "Could not find dependency: libiio.so.0".  The
  # absolute path baked in here does not survive into the bundle: linuxdeploy
  # rewrites it to $ORIGIN when it copies the library in, exactly as
  # dylibbundler rewrites the install name.
  cmake -S "$WORK/$dir" -B "$WORK/$dir/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" \
        -DCMAKE_INSTALL_RPATH="$PREFIX/lib" \
        -DCMAKE_PREFIX_PATH="$PREFIX;$EXTRA_PREFIX" \
        "$@" >/dev/null
  # TARGET=<name> builds that target instead of everything, which is how a
  # project whose *tests* do not compile still yields its library.  The install
  # step is unaffected: it installs what the install rules name, and no upstream
  # here installs its tests.
  if [ -n "${TARGET:-}" ]; then
    cmake --build "$WORK/$dir/build" --target "$TARGET" \
          --parallel "$(getconf _NPROCESSORS_ONLN)" >/dev/null
  else
    cmake --build "$WORK/$dir/build" --parallel "$(getconf _NPROCESSORS_ONLN)" >/dev/null
  fi
  cmake --install "$WORK/$dir/build" >/dev/null
}

clone https://github.com/analogdevicesinc/libiio.git        "$IIO_TAG"    libiio
clone https://github.com/analogdevicesinc/libad9361-iio.git "$AD9361_TAG" libad9361-iio
clone https://github.com/pothosware/SoapyPlutoSDR.git       "$PLUTO_TAG"  SoapyPlutoSDR

# THE ONE UPSTREAM PATCH, and it is not cosmetic.
#
# SoapyPlutoSDR reads the `bufflen` stream argument in rx_streamer and NOT in
# tx_streamer, whose ctor hardcodes `buf_size = 4096`.  On a 2.304 MS/s Pluto
# that is 1.78 ms of DAC data, pushed one non-cyclic iio_buffer at a time; over
# a network (or USB) backend, where every push is a request/response round trip,
# the DAC runs dry between pushes and the emitted signal is chopped at the push
# rate.  Measured on the operator's networked Pluto: a carrier streamed from the
# host carried a comb of sidebands out to +/-92 kHz at -21 dBc, while the SAME
# hardware told to generate the SAME tone with its own FPGA DDS -- not one
# sample from the host -- was clean to -54 dBc.  That pair is the whole
# argument: the transmitter is fine and the delivery is not.
#
# The patch is the RX branch's own code moved to the TX ctor, so a caller can
# ask for a buffer that covers the link's latency (soapy_tx_stream_args in
# soapy_protocol.c does).  Idempotent, because clone() skips a tree that is
# already there.
patch_tx_bufflen() {
  local f="$WORK/SoapyPlutoSDR/PlutoSDR_Streaming.cpp"
  [ -f "$f" ] || { echo "error: $f missing; cannot patch" >&2; exit 1; }
  if grep -q 'MACHPSDR tx bufflen' "$f"; then
    echo "==> SoapyPlutoSDR already patched (tx bufflen)"
    return
  fi
  grep -q '^	buf_size = 4096;$' "$f" || {
    echo "error: SoapyPlutoSDR's tx_streamer no longer says 'buf_size = 4096'." >&2
    echo "       The pin moved or upstream changed; re-read the patch before bumping PLUTO_TAG." >&2
    exit 1; }
  python3 - "$f" <<'PATCH'
import sys
p=sys.argv[1]
s=open(p).read()
old="\tbuf_size = 4096;\n"
new=("\tbuf_size = 4096;\n"
     "\t/* MACHPSDR tx bufflen: upstream honours this argument on RX only, and a\n"
     "\t   4096-sample TX buffer is 1.78 ms at 2.304 MS/s -- less than one\n"
     "\t   request/response round trip on a networked Pluto, so the DAC starves\n"
     "\t   between pushes and the emission is chopped at the push rate. */\n"
     "\tif (args.count(\"bufflen\") != 0) {\n"
     "\t\ttry {\n"
     "\t\t\tsize_t bufferLength = std::stoi(args.at(\"bufflen\"));\n"
     "\t\t\tif (bufferLength > 0)\n"
     "\t\t\t\tbuf_size = bufferLength;\n"
     "\t\t}\n"
     "\t\tcatch (const std::invalid_argument &){}\n"
     "\t}\n")
assert s.count(old)==1, "expected exactly one 'buf_size = 4096;'"
open(p,'w').write(s.replace(old,new,1))
PATCH
  # And make the TX MTU tell the truth about it.  getStreamMTU's TX branch is a
  # hardcoded `return 4096;`, so the buffer the driver really built is not
  # observable from outside -- which would leave "did the argument take effect?"
  # unanswerable from a log line, exactly the trap CLAUDE.md's rate-readback
  # rules exist to avoid.  A getter on tx_streamer and the real number here.
  python3 - "$WORK/SoapyPlutoSDR/SoapyPlutoSDR.hpp" "$f" <<'PATCH2'
import sys
hpp,cpp=sys.argv[1],sys.argv[2]
h=open(hpp).read()
oldh="\t\tint send(const void * const *buffs,const size_t numElems,int &flags,const long long timeNs,const long timeoutUs );\n\t\tint flush();\n"
newh=oldh+"\t\t/* MACHPSDR: the buffer really built, so the caller can see whether\n\t\t   its bufflen was honoured. */\n\t\tsize_t get_buf_size() const { return buf_size; }\n"
assert h.count(oldh)==1, "tx_streamer public section not as expected"
open(hpp,'w').write(h.replace(oldh,newh,1))
c=open(cpp).read()
oldc="\tif (IsValidTxStreamHandle(handle)) {\n\t\treturn 4096;\n\t}\n"
newc="\tif (IsValidTxStreamHandle(handle)) {\n\t\treturn this->tx_stream->get_buf_size();   /* MACHPSDR: was a hardcoded 4096 */\n\t}\n"
assert c.count(oldc)==1, "getStreamMTU TX branch not as expected"
open(cpp,'w').write(c.replace(oldc,newc,1))
PATCH2
  echo "==> patched SoapyPlutoSDR: tx_streamer honours bufflen, TX MTU reports it"
}
patch_tx_bufflen

# SoapyPlutoSDR 0.2.2 turns the TX LO on while setupStream() is called, but
# deactivateStream() only flushes the host buffer.  The LO is not powered down
# until closeStream(), which this application quite reasonably does only when
# the radio is closed.  On an AD9361 that leaves LO leakage at the last transmit
# frequency for the entire receive period after the first over.
#
# Make stream activation own the LO state: setup leaves it off, activate turns
# it on, and deactivate turns it off after flushing.  This keeps the stream and
# its (potentially large, network-latency-sized) IIO buffer allocated between
# overs, while actually unkeying the RF hardware.
patch_tx_lo_lifecycle() {
  local f="$WORK/SoapyPlutoSDR/PlutoSDR_Streaming.cpp"
  [ -f "$f" ] || { echo "error: $f missing; cannot patch" >&2; exit 1; }
  if grep -q 'MACHPSDR TX LO lifecycle' "$f"; then
    echo "==> SoapyPlutoSDR already patched (TX LO lifecycle)"
    return
  fi
  python3 - "$f" <<'PATCH'
import sys
p=sys.argv[1]
s=open(p).read()

old_setup='''\t\tiio_channel_attr_write_bool(
\t\t\tiio_device_find_channel(dev, "altvoltage1", true), "powerdown", false); // Turn ON TX LO
'''
new_setup='''\t\t/* MACHPSDR TX LO lifecycle: setup allocates the stream but must not
\t\t   put an unkeyed transmitter on the air. */
\t\tiio_channel_attr_write_bool(
\t\t\tiio_device_find_channel(dev, "altvoltage1", true), "powerdown", true); // TX LO stays OFF until activate
'''
assert s.count(old_setup)==1, "TX setup powerdown block not as expected"
s=s.replace(old_setup,new_setup,1)

old_activate='''    std::lock_guard<pluto_spin_mutex> lock(rx_device_mutex);

    if (IsValidRxStreamHandle(handle)) {
        return this->rx_stream->start(flags, timeNs, numElems);
    }

    return 0;
'''
new_activate='''    // Scope the RX lock: TX has its own device lock.
    {
        std::lock_guard<pluto_spin_mutex> lock(rx_device_mutex);

        if (IsValidRxStreamHandle(handle)) {
            return this->rx_stream->start(flags, timeNs, numElems);
        }
    }

    {
        std::lock_guard<pluto_spin_mutex> lock(tx_device_mutex);

        if (IsValidTxStreamHandle(handle)) {
            /* MACHPSDR TX LO lifecycle: key the RF hardware with the stream. */
            return iio_channel_attr_write_bool(
                iio_device_find_channel(dev, "altvoltage1", true), "powerdown", false);
        }
    }

    return 0;
'''
assert s.count(old_activate)==1, "activateStream block not as expected"
s=s.replace(old_activate,new_activate,1)

old_deactivate='''        if (IsValidTxStreamHandle(handle)) {
            this->tx_stream->flush();
            return 0;
        }
'''
new_deactivate='''        if (IsValidTxStreamHandle(handle)) {
            this->tx_stream->flush();
            /* MACHPSDR TX LO lifecycle: deactivate must really unkey Pluto;
               otherwise AD9361 LO leakage remains at the last TX frequency. */
            return iio_channel_attr_write_bool(
                iio_device_find_channel(dev, "altvoltage1", true), "powerdown", true);
        }
'''
assert s.count(old_deactivate)==1, "TX deactivateStream block not as expected"
s=s.replace(old_deactivate,new_deactivate,1)

open(p,'w').write(s)
PATCH
  echo "==> patched SoapyPlutoSDR: TX LO follows stream activation"
}
patch_tx_lo_lifecycle

# A Pluto retains the AD9361 TX LO state across client connections.  Waiting
# until setupStream() to power it down is therefore too late: opening a network
# context and constructing the rest of the radio can take seconds, during which
# a LO left on by a crashed/older client remains on air.  Put TX in a safe state
# in the driver constructor, at the first instant the new context has exposed
# the PHY, and again in the destructor before releasing that context.  The app
# also holds TX at minimum gain, so this is the hard on/off half of the guard.
patch_tx_safe_open_close() {
  local f="$WORK/SoapyPlutoSDR/PlutoSDR_Settings.cpp"
  [ -f "$f" ] || { echo "error: $f missing; cannot patch" >&2; exit 1; }
  if grep -q 'MACHPSDR safe TX on open' "$f"; then
    echo "==> SoapyPlutoSDR already patched (safe TX open/close)"
    return
  fi
  python3 - "$f" <<'PATCH'
import sys
p=sys.argv[1]
s=open(p).read()

old_ctor='''\tif (dev == nullptr || rx_dev == nullptr || tx_dev == nullptr) {
\t\tSoapySDR_logf(SOAPY_SDR_ERROR, "no device found in this context.");
\t\tthrow std::runtime_error("no device found in this context");
\t}

\tthis->setAntenna(SOAPY_SDR_RX, 0, "A_BALANCED");
'''
new_ctor='''\tif (dev == nullptr || rx_dev == nullptr || tx_dev == nullptr) {
\t\tSoapySDR_logf(SOAPY_SDR_ERROR, "no device found in this context.");
\t\tthrow std::runtime_error("no device found in this context");
\t}

\t/* MACHPSDR safe TX on open: a Pluto retains this state between IIO
\t   clients.  Power it down at the first instant this context can address
\t   the PHY, before any slower radio/stream configuration begins. */
\tiio_channel_attr_write_bool(
\t\tiio_device_find_channel(dev, "altvoltage1", true), "powerdown", true);

\tthis->setAntenna(SOAPY_SDR_RX, 0, "A_BALANCED");
'''
assert s.count(old_ctor)==1, "constructor device block not as expected"
s=s.replace(old_ctor,new_ctor,1)

old_dtor='''SoapyPlutoSDR::~SoapyPlutoSDR(void){

\tlong long samplerate=0;
'''
new_dtor='''SoapyPlutoSDR::~SoapyPlutoSDR(void){

\t/* MACHPSDR safe TX on close: do not leave RF enabled if no TX stream was
\t   ever opened, or if teardown bypassed closeStream(). */
\tif (dev != nullptr)
\t\tiio_channel_attr_write_bool(
\t\t\tiio_device_find_channel(dev, "altvoltage1", true), "powerdown", true);

\tlong long samplerate=0;
'''
assert s.count(old_dtor)==1, "destructor block not as expected"
s=s.replace(old_dtor,new_dtor,1)

open(p,'w').write(s)
PATCH
  echo "==> patched SoapyPlutoSDR: TX is safe from context open through close"
}
patch_tx_safe_open_close

# RX and TX share the AD9361 baseband clock/FIR. The startup log measured
# 3.84 seconds in EACH setSampleRate(768000): libad9361 disables, uploads and
# enables the same FIR twice. Cache only a successful configuration in this
# device instance; never infer FIR state from a rounded rate on a new context.
patch_shared_bb_rate() {
  python3 - "$WORK/SoapyPlutoSDR/SoapyPlutoSDR.hpp" "$WORK/SoapyPlutoSDR/PlutoSDR_Settings.cpp" <<'PATCH'
import sys
from pathlib import Path
h, c = map(Path, sys.argv[1:])
hs, cs = h.read_text(), c.read_text()
if 'MACHPSDR shared BB rate' in cs:
    assert 'configured_bb_rate' in hs, "incomplete shared BB rate patch"
    print('==> SoapyPlutoSDR already patched (shared BB rate)')
    sys.exit(0)
member = '\t\tbool decimation, interpolation;'
start = 'void SoapyPlutoSDR::setSampleRate( const int direction, const size_t channel, const double rate )\n{'
old = '\tif(ad9361_set_bb_rate(dev,(unsigned long)samplerate))\n\t\tSoapySDR_logf(SOAPY_SDR_ERROR, "Unable to set BB rate.");\t'
new = """	/* MACHPSDR shared BB rate: directional FPGA setup above is still required,
	   including interpolation/decimation flags and RX buffer sizing. Only the
	   expensive shared clock/FIR configuration can be reused. */
	if (configured_bb_rate != (unsigned long)samplerate) {
		configured_bb_rate = 0;
		if (ad9361_set_bb_rate(dev, (unsigned long)samplerate))
			throw std::runtime_error("Unable to set BB rate.");
		configured_bb_rate = (unsigned long)samplerate;
	}
""".rstrip()
assert hs.count(member) == 1 and cs.count(start) == 1 and cs.count(old) == 1, 'shared BB rate patch: unexpected upstream source'
hs = hs.replace(member, member + '\n\t\tstd::mutex bb_rate_mutex;\n\t\tunsigned long configured_bb_rate = 0;', 1)
cs = cs.replace(start, start + '\n\tstd::lock_guard<std::mutex> rate_lock(bb_rate_mutex);', 1).replace(old, new, 1)
h.write_text(hs)
c.write_text(cs)
print('==> patched SoapyPlutoSDR: reuse successful shared clock/FIR setup')
PATCH
}
patch_shared_bb_rate

# libad9361 sets the PHY clock itself, after arranging the FIR. Writing it
# first can trigger an extra hardware reconfiguration (and can fail at low
# rates while the old FIR is still installed). Keep FPGA direction setup.
patch_single_phy_rate_write() {
  python3 - "$WORK/SoapyPlutoSDR/PlutoSDR_Settings.cpp" <<'PATCH'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
if 'MACHPSDR single PHY rate write v2' in s:
    print('==> SoapyPlutoSDR already patched (single PHY rate write)')
    sys.exit(0)
# Install the shared clock before selecting the FPGA rate: its available
# rates depend on that clock. Keep calibration under the shared rate mutex.
start = s.index('#ifdef HAS_AD9361_IIO\n\t/* MACHPSDR shared BB rate:')
end = s.index('#endif', start) + len('#endif')
calibration = s[start:end]
s = s[:start] + s[end:]
anchor = '\tlong long samplerate =(long long) rate;'
assert s.count(anchor) == 1
s = s.replace(anchor, anchor + '\n\tauto configure_bb_rate = [&]() {\n' + calibration + '\n\t};', 1)
for output in ('false', 'true'):
    old = '\t\tiio_channel_attr_write_longlong(iio_device_find_channel(dev, "voltage0", ' + output + '),"sampling_frequency", samplerate);'
    assert s.count(old) == 1, 'unexpected PHY sample rate write'
    # Also upgrade the first version if already applied to a local build.
    previous = ('#ifndef HAS_AD9361_IIO\n' + old + '\n#endif\n'
                '\t\t/* MACHPSDR single PHY rate write: libad9361 owns the PHY clock\n'
                '\t\t   when present; the FPGA direction below still needs setup. */')
    new = ('#ifndef HAS_AD9361_IIO\n' + old + '\n#else\n'
           '\t\tconfigure_bb_rate();\n#endif\n'
           '\t\t/* MACHPSDR single PHY rate write v2: configure the PHY once,\n'
           '\t\t   then select the FPGA rate using the resulting clock. */')
    s = s.replace(previous if previous in s else old, new, 1)
p.write_text(s)
print('==> patched SoapyPlutoSDR: avoid redundant PHY clock writes')
PATCH
}
patch_single_phy_rate_write

# THE ONE libiio PATCH, and it is worth more than every other line in this file
# put together: it is ten seconds of every start-up on a NETWORKED Pluto.
#
# libiio's DNS-SD scan resolves each _iio._tcp responder to an address, opens a
# context AT THAT ADDRESS to read the hardware out of it -- and then publishes
# the scan result as `ip:<mDNS hostname>`, throwing the address away.  Every
# later open therefore goes back through the name.  On macOS, getaddrinfo() of a
# `.local` name with AF_UNSPEC -- which is what network.c asks for -- waits the
# full mDNSResponder negative-answer timeout for an AAAA the Pluto never sends:
#
#     getaddrinfo("plutosky.local.", AF_UNSPEC)            5.003 s
#     getaddrinfo("plutosky.local.", AF_INET)              0.002 s
#     iio_create_context_from_uri("ip:plutosky.local.")    5.013 s
#     iio_create_context_from_uri("ip:192.168.100.5")      0.011 s
#
# and the application pays it three times over: inside the driver's
# find_PlutoSDR (which opens every scanned context to check it really is an
# AD9361), again in soapy_discovery.c's get_info(), and again when the operator
# opens the radio.  Measured on this tree before the patch: enumerate 5.36 s and
# EVERY SoapySDRDevice_make() a further 5.01 s -- including one passed a numeric
# uri, because SoapySDR's Device::make() merges the ENUMERATED args over the
# caller's.  That last part is why this cannot be fixed in the application:
# nothing it passes can win against the uri the scan published.  The mDNS browse
# itself is not the cost and never was (0.144 s here).
#
# So the scan publishes what it connected to.  libiio's own uri parser already
# takes a bare IPv4/IPv6 address and the bracketed `[v6]:port` form -- they are
# the shapes network_create_context() was handed a few lines above -- so this
# reuses that string verbatim rather than deriving it a second time.  Nothing is
# lost to the operator: the hostname is still what the browse matched on, and an
# address is the more useful of the two in a device list.
patch_iio_scan_publishes_address() {
  local f="$WORK/libiio/dns_sd.c"
  [ -f "$f" ] || { echo "error: $f missing; cannot patch" >&2; exit 1; }
  if grep -q 'MACHPSDR scan uri' "$f"; then
    echo "==> libiio already patched (scan publishes the address)"
    return
  fi
  python3 - "$f" <<'PATCH'
import sys
p=sys.argv[1]
s=open(p).read()
old = (
 '\tif (port == IIOD_PORT)\n'
 '\t\tiio_snprintf(uri, sizeof(uri), "ip:%s", hostname);\n'
 '\telse\n'
 '\t\tiio_snprintf(uri, sizeof(uri), "ip:%s:%d", hostname, port);\n')
new = (
 '\t/* MACHPSDR scan uri: publish the ADDRESS this scan just opened a context\n'
 '\t   at, not the mDNS hostname.  Every consumer re-opens the published uri,\n'
 '\t   and resolving a `.local` name with AF_UNSPEC costs a 5 s wait for an\n'
 '\t   AAAA record on macOS, against 11 ms for the address.  `uri` still holds\n'
 '\t   the exact string network_create_context() accepted above (bare v4/v6, or\n'
 '\t   the bracketed [v6]:port form), so it is reused rather than rebuilt. */\n'
 '\t{\n'
 '\t\tchar connected[sizeof(uri)];\n'
 '\n'
 '\t\tiio_strlcpy(connected, uri, sizeof(connected));\n'
 '\t\tiio_snprintf(uri, sizeof(uri), "ip:%s", connected);\n'
 '\t}\n'
 '\t(void) hostname;\n')
assert s.count(old)==1, "dns_sd.c uri block not as expected"
open(p,'w').write(s.replace(old,new,1))
PATCH
  echo "==> patched libiio: the DNS-SD scan publishes ip:<address>, not ip:<hostname>"
}
patch_iio_scan_publishes_address

# OSX_FRAMEWORK=OFF is load-bearing: libiio's macOS build defaults to producing
# an iio.framework, which no consumer here looks for and dylibbundler does not
# handle.  The rest is trimming — this build exists to be dlopen'd by one
# application, so the daemon, the command-line tools, the tests, the bindings
# and the man pages are all things that would only be copied into a .app to sit
# there unused.
# INSTALL_UDEV_RULE=OFF is not trimming, it is the difference between building
# and not: on Linux libiio's install rule writes 90-libiio.rules to an ABSOLUTE
# /lib/udev/rules.d (UDEV_RULES_INSTALL_DIR, line 659 of its CMakeLists at this
# tag), ignoring CMAKE_INSTALL_PREFIX entirely -- so the install step fails with
# "Permission denied" as any non-root build must.  macOS has no udev and never
# reached it.
#
# What that rule does, and therefore what a bundle without it does not do: it
# grants a non-root user access to a Pluto attached over USB.  That is a
# property of the HOST, not of a relocatable bundle -- an AppImage cannot
# install into /lib and should not want to.  An operator using the Pluto over
# its USB-Ethernet address (ip:192.168.2.1, the usual way) needs nothing; one
# using a usb: context needs their distribution's libiio package, or the rule
# by hand.
build libiio \
  -DINSTALL_UDEV_RULE=OFF \
  -DOSX_FRAMEWORK=OFF \
  -DWITH_TESTS=OFF \
  -DWITH_DOC=OFF \
  -DWITH_IIOD=OFF \
  -DWITH_MAN=OFF \
  -DWITH_EXAMPLES=OFF \
  -DWITH_SERIAL_BACKEND=OFF \
  -DENABLE_PACKAGING=OFF \
  -DPYTHON_BINDINGS=OFF

# CMAKE_POLICY_VERSION_MINIMUM: libad9361-iio v0.3 declares
# cmake_minimum_required(VERSION 2.8), which CMake 4 refuses outright.  The flag
# is CMake's own escape hatch for exactly that, and it is preferable to the
# alternative of moving to v0.4.0 -- that release is written against libiio 1.x
# (`#include <iio/iio.h>`) and does not compile against the 0.x we need here.
# On a CMake old enough not to know the variable it is an unused-variable
# warning, so it costs nothing to pass unconditionally.
# TARGET=ad9361, because this project's TESTS do not build against the libiio
# we just installed and never will: they are written `#ifdef __APPLE__ ->
# #include <iio/iio.h>`, i.e. against the macOS FRAMEWORK layout, and the
# framework is precisely what OSX_FRAMEWORK=OFF above declined to build.  The
# library itself includes <iio.h> and compiles fine.  Building the one target
# is the narrow fix; the alternatives are patching upstream sources or shipping
# a framework nothing else here understands.
#
# OSX_PACKAGE defaults to ON and would have CMake assemble a .pkg installer --
# a macOS system installer, built as a side effect of a driver build, in a tree
# whose whole point is a self-contained .app.
TARGET=ad9361 build libad9361-iio \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOSX_PACKAGE=OFF \
  -DENABLE_PACKAGING=OFF

# libad9361 sets FRAMEWORK TRUE on its target unconditionally -- there is no
# option to turn it off -- so it installs lib/ad9361.framework and no plain
# library or header.  SoapyPlutoSDR's FindLibAD9361 then finds the framework as
# a "library" and no include directory at all, decides AD9361 support is
# unavailable, and builds without it: a driver that looks fine and cannot
# programme the FIR.  Turning the framework back into a dylib and a header
# afterwards costs three commands and touches no upstream source.  The framework
# is then removed rather than left beside them, because find_library() prefers a
# framework on macOS and would go straight back to the one that does not work.
AD9361_FW="$PREFIX/lib/ad9361.framework"
if [ -d "$AD9361_FW" ]; then
  echo "==> unwrapping ad9361.framework into lib/libad9361.dylib"
  mkdir -p "$PREFIX/include"
  cp "$AD9361_FW"/Headers/*.h "$PREFIX/include/"
  cp "$AD9361_FW"/Versions/Current/ad9361 "$PREFIX/lib/libad9361.dylib"
  install_name_tool -id "$PREFIX/lib/libad9361.dylib" "$PREFIX/lib/libad9361.dylib"
  rm -rf "$AD9361_FW"
fi

build SoapyPlutoSDR \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# The module is the whole point, and CMake will happily have installed headers
# and a static library while failing to produce it (a SoapySDR it could not find
# is the usual way).  The extension is .so on macOS too — SoapySDR builds its
# modules as CMake MODULE targets and scans for that suffix.
MODULE="$(find "$PREFIX/lib/SoapySDR" -name 'libPlutoSDRSupport.so' 2>/dev/null | head -1)"
[ -n "$MODULE" ] || {
  echo "error: built, but no libPlutoSDRSupport.so under $PREFIX/lib/SoapySDR" >&2
  exit 1
}

# Not decoration: the FIR is how a Pluto reaches a rate below ~2.08 MHz, and a
# driver built without libad9361 answers such a request by running eight times
# faster and saying nothing.  A silent downgrade is exactly the failure this
# script exists to prevent, so it is an error here rather than a surprise later.
# It is asked of whichever tool this platform has: `otool -L` on macOS, the
# dynamic section on Linux.  Written with otool alone it was a check that ran on
# ONE platform and silently passed on the other -- and the Linux AppImage is
# exactly where a Pluto operator would meet the substituted rate.
deps_of() {
  if command -v otool >/dev/null 2>&1; then
    otool -L "$1" 2>/dev/null
  elif command -v objdump >/dev/null 2>&1; then
    objdump -p "$1" 2>/dev/null | grep NEEDED
  elif command -v readelf >/dev/null 2>&1; then
    readelf -d "$1" 2>/dev/null | grep NEEDED
  else
    echo "__no_tool__"
  fi
}
DEPS="$(deps_of "$MODULE")"
if [ "$DEPS" = "__no_tool__" ]; then
  echo "warning: no otool/objdump/readelf here -- cannot check for libad9361" >&2
elif ! echo "$DEPS" | grep -q "libad9361"; then
  echo "error: the driver was built WITHOUT libad9361 -- it cannot programme" >&2
  echo "       the AD9361 FIR, so every rate below ~2.08 MHz will be" >&2
  echo "       substituted by the hardware, silently." >&2
  exit 1
fi

echo "==> built $MODULE"
echo "    SOAPY_SDR_PLUGIN_PATH=$(dirname "$MODULE") SoapySDRUtil --find"
