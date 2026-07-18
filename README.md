# MacHPSDR

**MacHPSDR** is a fork of [LinHPSDR](https://github.com/g0orx/linhpsdr) by
John Melton (G0ORX/N6LYT), focused on macOS and with a number of feature
additions.

### License

MacHPSDR, like the LinHPSDR it derives from, is free software released under
the **GNU General Public License** (GPLv2 or, at your option, any later
version — see the `LICENSE` file). The bundled WDSP DSP library (in `wdsp/`)
is licensed under the **GNU GPL v2** (see `wdsp/COPYING`).

### What this fork adds

MacHPSDR is a LinHPSDR fork maintained mainly on macOS. The main additions over
upstream:

**Single-window interface.** All receivers are stacked in one resizable main
window instead of separate floating windows, with drag-to-resize dividers, a
per-receiver close button, and a bottom toolbar and log area. Window size, layout
and closed-receiver settings are remembered between sessions.

**Colour skins.** A dark interface with five selectable colour schemes (Charcoal,
Solarized Dark, Solarized Light, Nord, Gruvbox), chosen in Configure → Misc →
Appearance and remembered per radio. Includes a redesigned S-meter and frequency
display. The waterfall display also has several selectable colour themes of its own.

**Broadcast FM (WFM).** FM broadcast reception on SoapySDR devices (HackRF,
RTL-SDR) with a selectable bandwidth and de-emphasis (50/75 µs), stereo decoding,
and RDS. The RDS panel shows the station name, programme type, RadioText, the
currently playing track, clock time and alternative frequencies.

**Transmit on HackRF / SoapySDR.** Half-duplex transmit over SoapySDR. Voice modes
require a microphone input to be selected; the Drive slider controls output power.
CW and PureSignal are not available on this path. The TX IQ rate is rounded to a
multiple of the DSP rate (96 kHz) so WDSP's internal buffers stay consistent —
without this HackRF crashed on key-up (e.g. its 2 MHz rate → 1.92 MHz). Keys up on
real hardware without crashing; on-air signal quality still needs more testing.

**PureSignal.** Adaptive predistortion (Protocol 1 only) is available again, turned
on in Configure → Pure Signal. Still an unfinished prototype, calibrated mainly for
the Hermes-Lite 2.

**Freetune.** A tuning mode where the cursor moves within the visible span; exiting
keeps the frequency you were on, and the radio retunes automatically when the cursor
reaches a span edge.

**Test device.** A built-in "Fake Noise SDR" lets you run the app with no hardware
connected (receive, transmit, spectrum and demodulation), and can play back a
recorded IQ file. Hidden by default; enable it by starting with `--faker`.

**macOS packaging.** Builds a self-contained `MacHPSDR.app` bundle (`make app`),
Cmd-Q quits the app, and the required WDSP library is built and bundled
automatically — no separate install needed. The fork was renamed to MacHPSDR;
existing settings are migrated automatically on first run.

**Other changes.** SoapySDR RX gain is now re-applied just after the stream starts,
so the saved gain (and the HackRF RF preamp) takes effect on startup instead of
staying weak until you nudged the slider. Broadcast FM crackle at high sample rates fixed; `Space` toggles
transmit; waterfall shows the passband and centre cursor; assorted FM/squelch/NFM
fixes. On HPSDR hardware: PPM frequency correction, and the Att 10/Att 20 outputs
usable as custom switches (for example attenuator outputs on trx-duo or red pitaya 
can be used as xverter or filter switches) - lables can be changed in settings.

Note: some of these additions rely on a patched WDSP (this fork adds a WFM
demodulator and a couple of tweaks). The patched WDSP sources are **vendored in
this repository under `wdsp/`** (originally from g0orx/wdsp) — do NOT clone WDSP
separately; build and install it from that directory. See the WDSP build steps
below.

### Development environment

Development and testing has been run on Ubuntu and Arch Linux. If run on early versions there may be a problem with GTK not supporting the gtk_menu_popup_at_pointer function vfo.c. For information on MacOS support see [MacOS.md](./MacOS.md).

### Prerequisites for building

```
  sudo apt-get install libfftw3-dev
  sudo apt-get install libpulse-dev
  sudo apt-get install libsoundio-dev
  sudo apt-get install libasound2-dev
  sudo apt-get install libgtk-3-dev
  sudo apt-get install libsoapysdr-dev
```

### WDSP (vendored — built automatically)

The patched WDSP this fork needs is vendored in `wdsp/` (do not clone it
separately). It is built automatically as part of `make`, and MacHPSDR links
against that in-tree copy, so no separate WDSP build or system-wide install is
required.
### CW support

Hermes and HL2 CWX/cwdaemon support added. If you do not wish to use this, please ignore. This features requires the following to be installed (tested on Ubuntu 19.10, Kubuntu 18.04 LTS):

```
  sudo apt install libtool
  git clone https://git.code.sf.net/p/unixcw/code unixcw-code 
  cd unixcw-code
  git fetch --tags
  git checkout tags/v3.6.0
  autoreconf -i
  ./configure
  make
  sudo make install
  sudo ldconfig
```
If CWX/cwdaemon is wanted/required. You must enable it in the Makefile. Uncomment the following lines:
```
#CWDAEMON_INCLUDE=CWDAEMON

#ifeq ($(CWDAEMON_INCLUDE),CWDAEMON)
#CWDAEMON_OPTIONS=-D CWDAEMON
#CWDAEMON_LIBS=-lcw
#CWDAEMON_SOURCES= \
#cwdaemon.c
#CWDAEMON_HEADERS= \
#cwdaemon.h
#CWDAEMON_OBJS= \
#cwdaemon.o
#endif
```

### To download and compile MacHPSDR from here

```
  git clone https://github.com/enthru/MacHPSDR.git machpsdr
  cd machpsdr
  make
```

There is no `make install` target — the `machpsdr` binary runs in place; start
it with `./machpsdr` from the build directory.

# MacHPSDR MacOS Support
  
### Development environment

Development and testing has been run on MacOS Sierra 10.12.6 and MacOS high Sierra 10.13.6. Prerequisites are installed using [Homebrew](https://brew.sh/).

### Prerequisites for building

```
  brew install fftw
  brew install gtk+3
  brew install gnome-icon-theme
  brew install libsoundio
  brew install libffi
  brew install soapysdr
```

### WDSP (vendored — built automatically)

The patched WDSP this fork needs is vendored in `wdsp/` (do not clone it
separately). `make` and `make app` build it automatically and link/bundle that
in-tree copy, so no separate WDSP build or system-wide install is required.

### To download and compile MacHPSDR

```
  git clone https://github.com/enthru/MacHPSDR.git machpsdr
  cd machpsdr
  make          # build ./machpsdr (runs in place)
  make app      # optional: build the self-contained MacHPSDR.app bundle
```

`make` produces the `machpsdr` binary in the build directory; run it with
`./machpsdr`. There is no `make install` target. `make app` bundles everything
(GTK, the in-tree WDSP, resources) into `MacHPSDR.app`, which you can then
`open MacHPSDR.app` or drag to /Applications.


