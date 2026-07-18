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
and closed-receiver settings are remembered between sessions. Each receiver's VFO
row has a compact mute (speaker) button next to the AF-gain slider that silences
its audio without losing the volume setting; the mute state is remembered between
sessions.

**Colour skins.** A dark interface with five selectable colour schemes (Charcoal,
Solarized Dark, Solarized Light, Nord, Gruvbox), chosen in Configure → Misc →
Appearance and remembered per radio. Includes a redesigned S-meter and frequency
display. The waterfall display also has several selectable colour themes of its own.
The panadapter trace colour is now chosen from a named drop-down list (Gradient,
Skin Accent, Red, Orange, Yellow, Green, Blue, Violet, Magenta, Cyan) in the
receiver settings instead of a numeric spin box.

**Broadcast FM (WFM).** FM broadcast reception on SoapySDR devices (HackRF,
RTL-SDR) with a selectable bandwidth and de-emphasis (50/75 µs), stereo decoding,
and RDS. The RDS panel shows the station name, programme type, RadioText, the
currently playing track, clock time and alternative frequencies. The decoder
block in the bottom bar is titled **RDS** only while the active receiver is in
WFM mode (where it decodes and displays RDS); in every other mode it carries the
neutral default title **Decode** and stays blank.

**FT8 decoding.** Selecting the **DIGU** mode on the active receiver automatically
starts an FT8 decoder — no separate window or button. The receiver's demodulated
audio is tapped, decimated to 12 kHz, buffered into 15-second UTC time slots and
decoded in a background thread. Decoding runs on a sliding 15-second window
(re-run every ~2 s) rather than a single clock-locked slot, so it still works
when the system clock is slightly off and when driving it from a looped I/Q
recording that is not aligned to real UTC slots. Decoded traffic (signal
report, audio frequency and message text) appears in the bottom-bar decoder
block, retitled **FT8** with the slot time; up to seven decodes are shown with
an "(+N more)" summary when a window yields more, and the readout holds the last
decodes until the next batch arrives (it does not blank between transmissions).
Switching away from DIGU stops the decoder. Requires an accurate system clock (UTC), like WSJT-X.
The codec is the vendored [ft8_lib](https://github.com/kgoba/ft8_lib) by Kārlis
Goba (MIT). *(Transmit / QSO answering is planned as a later phase.)*

**I/Q file player for the fake device.** The synthetic `--faker` test device can
loop any 16-bit stereo I/Q WAV recording instead of the built-in noise+tones, so
features can be exercised without hardware. Pass the file directly:
`./machpsdr --faker ft8.wav` (or set `MACHPSDR_FAKE_IQ=...`); with no argument it
falls back to `iq.wav`. The recording's sample rate is resampled to the
receiver's rate and its carrier auto-centred to baseband, then looped. A
6th-order Butterworth low-pass at the recording's Nyquist band-limits the
resampled stream so the panadapter shows the file's own bandwidth instead of
the resampling images ("mirror" copies) that a wide receiver span would
otherwise reveal. Add `--revert-iq` to swap the recording's I and Q channels
(mirrors the spectrum) when its sideband is inverted — i.e. signals
appear/decode as their mirror.

**Ring-buffer depth is per device (latency vs. glitch-free wide reception).** Wide
reception needs a deep WDSP output I/O ring so DSP-thread jitter at high sample rates
(e.g. a wide span at 1536k/1920k — useful even for CW band-scanning, not only WFM)
cannot underrun into zero-fill clicks. But a deep ring pre-fills roughly `(ring-1)`
DSP output blocks, which adds a fixed audio latency to *every* mode on that receiver.
Earlier this fork raised the ring globally (2 → 16) and so paid that latency on all
hardware. The ring depth is now scaled at runtime to each receiver's span
(`rx_ring_depth()` → `SetDSPMult()`, applied before every channel open / rate change):
**≤384k → 2**, **768k → 4**, **1536k → 8**, **1920k → 16**. Narrow spans (which
includes every HPSDR rate) get the original snappy low-latency ring back, while only
the genuinely wide spans pay for the deeper ring they need to stay glitch-free. The
transmitter always uses depth 2.

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

**Disconnect recovery.** If the radio stops delivering data (a HackRF/SoapySDR
device unplugged, or a network HPSDR radio going away), a watchdog detects the
stall and pops a "Connection lost" dialog offering **Reconnect** or **Exit**,
instead of leaving the app frozen and forcing a full restart. Reconnect
re-initialises the hardware in place (re-opening the SoapySDR device or
re-issuing the Protocol 1/2 start sequence) while keeping your session; if the
device is still missing the dialog simply reappears so you can retry.

**Test device.** A built-in "Fake Noise SDR" lets you run the app with no hardware
connected (receive, transmit, spectrum and demodulation), and can play back a
recorded IQ file. Hidden by default; enable it by starting with `--faker`.

**macOS packaging.** Builds a self-contained `MacHPSDR.app` bundle (`make app`),
Cmd-Q quits the app, and the required WDSP library is built and bundled
automatically — no separate install needed. The fork was renamed to MacHPSDR;
existing settings are migrated automatically on first run. The app now ships with a
new MacHPSDR application icon (window, Dock and `.app` bundle).

**Faster startup.** The Protocol 1 and Protocol 2 network discoveries now run in
parallel instead of one after the other, and each waits one second instead of two
for a device to answer, so the device-selection list appears in about a second
rather than four on startup. If you only use a USB device (HackRF, RTL-SDR), start
with `--usb-only` to skip network discovery entirely for a near-instant list.

**Other changes.** The SoapySDR RX frequency and gain are now re-applied just after
the stream starts, so the saved gain (and the HackRF RF preamp) takes effect on
startup instead of staying weak until you nudged the slider, and a cold HackRF
(freshly powered / just after a reboot) tunes correctly from the first go instead
of receiving garbage until you re-tune. The RX sample rate is also re-asserted when
the receive stream starts, so a half-duplex HackRF no longer receives with artefacts
on a fresh launch (the transmit stream's rate had been left as the active hardware
clock, which previously only cleared up after toggling the device off/on).
Broadcast FM crackle at high sample rates fixed; `Space` toggles
transmit; waterfall shows the passband and centre cursor; assorted FM/squelch/NFM
fixes. Half-duplex receive no longer dies after a transmit over (the HackRF RX
stream was left in a runaway overflow that flooded the DSP with "fexchange0:
error=-2"); the RX stream is now rebuilt fresh on each return to receive. A
receiver that was left muted now really starts muted after a restart (the mute
state was saved and the button showed it, but the DSP gain was still set to full
volume on launch, so audio played anyway). Enabling the receive equalizer no
longer crashes on SoapySDR/HackRF: the wide WFM chain runs a 5120-sample DSP block,
but the RX equalizer's partitioned filter was still built with the historic 2048
coefficients, so it had zero filter partitions and dereferenced an empty FFT-plan
array the moment it was switched on — its length is now sized to the DSP block. On HPSDR hardware: PPM frequency correction, and the Att 10/Att 20 outputs
usable as custom switches (for example attenuator outputs on trx-duo or red pitaya 
can be used as xverter or filter switches) - lables can be changed in settings.

**Audio devices (macOS).** The output and microphone device lists now include a
**System Default** entry that follows whatever macOS is currently using, so you can
pick it once and never re-select a device when you connect Bluetooth headphones.
Devices that don't run at 48 kHz (e.g. Bluetooth headsets locked to 44.1 kHz, which
were previously hidden and silent) now appear and work: audio is resampled on the fly
between 48 kHz and the device rate, in both directions. The device lists are also
re-scanned each time you open the RX/TX audio page, so a headset connected after
launch shows up without restarting. When **System Default** is selected, changing
the macOS output device while audio is playing now takes effect live — the stream
re-opens onto the new default automatically, instead of staying stuck on the old
output. The switch is event-driven (a CoreAudio default-device listener), so it is
near-instant and costs nothing while idle, with a slow timer as a safety net. The
same live, instant follow applies to the **microphone** when its input is set to
System Default: switch the macOS input device and the open mic stream re-opens onto
the new default. The output also follows a device's *sample-rate* changes, not just
device swaps, so a Bluetooth headset flipping between its A2DP and hands-free (HFP)
profiles no longer turns RX audio into garbage.

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


