# LinHPSDR

### Changelist for this fork

I'm using this software only on Mac OS, so some of this issues can be appeared only on Mac OS and fixes can affect normal working on linux systems :)

1) Added 'space' hotkey to enable MOX in receiver window
2) Replaced spaces in VFO freqency with zeroes (a bit ugly, but working) to fix wrong digit scrolling.
3) Changed default FPS for new receivers to 25.
4) Fix speech processing settings in TX configuration tab (was in wrong position - under equalizer).
5) Trigger waterfall drag event only when we moving mouse more that for a one pixel to prevent unwanted drag event caused by mouse jitter.
6) As I'm using TRX-DUO (Red Pitaya clone)  I'm using att10 and att20 outputs to control filters. So I've just added checkboxes in the configuration window to control those outputs - enable them or disable.
7) Added ppm correction setting.
8) Auto disabling squelch when non FM mode choosen
9) Disable squelch in NFM mode if setting for squelch of too low (there is no button for squelch so using just squelch bar to disable it)
10) Fixed deviation for NFM in receiver window
11) Fix switch tx mode to receiving when changing transmiter
12) Ignore SOAPY protocol for PA calibration settings (display all bands)
13) Added freetune mode
14) Added app bundling for Mac os to the Makefile
15) Add waterfall themes
16) Changed default paremeters for NR filter
17) Single-window redesign. All receivers now live stacked inside one resizable
    main window instead of separate floating windows. Adjustable GtkPaned dividers
    between stacked receivers, a per-receiver close button, and the window size +
    divider positions are persisted across restarts.
18) Fake "Noise" test device (a built-in virtual SDR). Lets you run the app — RX
    and TX, panadapter/waterfall, and the demodulators — with no hardware attached,
    which is handy for testing and development; it can also play back an IQ .wav.
19) VFO widget reflows to the window width and the window is freely resizable; moved
    common controls to a bottom toolbar.
20) Flat dark theme — full CSS rewrite of the main window, meters and controls.
21) Bottom bar with a console/log area, plus a working TX path on the fake device.
22) Configure dialog modernization — flat-dark theme, unified spacing, and vertical
    sidebar (GtkStackSidebar) navigation between the settings pages.
23) Custom Att10/Att20 labels, info-button sizing and TX-monitor font tweaks.
24) Cmd-Q quits the app (macOS).
25) Waterfall now draws the passband band and a centre-frequency cursor.
26) Closed receivers keep their settings (they're just marked inactive) and restore
    them when re-opened, instead of resetting to defaults.
27) Broadcast FM (WFM) support for SoapySDR devices (HackRF/RTL-SDR), with a
    selectable RX span (192k…1920k). Fixed the wide-rate audio crackle that came
    from WDSP's async I/O ring (block-boundary glitch + output-ring underruns), not
    from the resampler.
    Known tradeoff — latency vs. high sample rates: at high rates the DSP thread has
    much less time per block, so the WDSP output ring is given extra headroom
    (DSP_MULT, currently 16) to avoid dropouts. That headroom is a fixed global
    buffer, so the app is currently NOT very responsive (~40 ms of added audio
    latency) even in narrow modes / low rates that don't need it. It can be improved
    later (shrink the WFM output resampler filter — redundant since WFM is already
    band-limited to 15 kHz before it — and/or make the ring headroom per-channel so
    only wide WFM spans pay for it).
    RDS decoding is planned next.
28) WFM stereo decoding. A 19 kHz pilot is isolated (biquad band-pass), tracked by
    a 2nd-order PLL, and doubled to a coherent 38 kHz reference that demodulates the
    23–53 kHz L−R subcarrier; the L/R matrix is folded into the samples ahead of the
    shared 15 kHz audio filter. A pilot-lock detector blends smoothly to mono when no
    stereo pilot is present, so mono stations stay clean.
29) The synthetic "Fake Noise SDR" test device is now hidden by default; launch the
    binary with `--faker` to make it available (used for UI/DSP testing without
    hardware).

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

### Prerequisites for installing the Debian Package

```
  sudo apt-get install libfftw3-3
  sudo apt-get install libpulse
  sudo apt-get install libsoundio
  sudo apt-get install libasound2
  sudo apt-get install libsoapysdr
```


### linhpsdr requires WDSP to be built and installed

The patched WDSP is vendored in this repo under `wdsp/` (do not clone it
separately). After cloning linhpsdr, build and install it from that directory:

```
  cd wdsp
  make
  sudo make install
```
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

### To download, compile and install linHPSDR from here

```
  git clone https://github.com/m5evt/linhpsdr.git
  cd linhpsdr
  make
  sudo make install
```

# LinHPSDR MacOS Support
  
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

### linhpsdr requires WDSP to be built and installed

The patched WDSP is vendored in this repo under `wdsp/` (do not clone it
separately). After cloning linhpsdr, build and install it from that directory:

```
  cd wdsp
  make install
```

### To download, compile and install linHPSDR

```
  git clone https://github.com/m5evt/linhpsdr.git
  cd linhpsdr
  make install
```

The build installs linHPSDR into `/usr/local/bin`. To run it, type `linhpsdr` on the command line.


