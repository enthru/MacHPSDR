# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LinHPSDR is a GTK3-based SDR (Software Defined Radio) control application for HPSDR hardware, written in C. This is a personal fork with a primary focus on macOS compatibility and several feature additions (freetune mode, waterfall themes, ppm correction, att10/att20 filter control, etc.).

## Build Commands

```bash
# Build the binary
make

# Install to /usr/local/bin
sudo make install        # Linux
make install             # macOS

# Clean build artifacts
make clean

# Build self-contained macOS .app bundle (requires dylibbundler)
make app
open LinHPSDR.app
```

No automated test suite exists — testing is manual with physical hardware.

## Dependencies

### macOS (via Homebrew)
```bash
brew install fftw gtk+3 gnome-icon-theme libsoundio libffi soapysdr dylibbundler
```

### Linux (via apt)
```bash
sudo apt-get install libfftw3-dev libpulse-dev libsoundio-dev libasound2-dev libgtk-3-dev libsoapysdr-dev
```

### WDSP (required on all platforms)
```bash
git clone https://github.com/g0orx/wdsp.git && cd wdsp && make && sudo make install
```

## Platform Differences

- **macOS**: Uses `portaudio.c` for audio, `mac_midi.c` for MIDI (CoreMIDI framework), no CWDAEMON support
- **Linux**: Uses `audio.c` with ALSA/PulseAudio/SoundIO, `alsa_midi.c` for MIDI, CWDAEMON enabled by default

Platform is detected via `uname -s` in the Makefile (`Darwin` vs `Linux`).

## Build Feature Flags (Makefile)

| Flag | Default | Notes |
|------|---------|-------|
| `SOAPYSDR` | enabled | RTL-SDR/LimeSDR/HackRF support (RX; HackRF TX implemented, half-duplex, untested on hardware) |
| `MIDI` | enabled | MIDI controller support |
| `CWDAEMON` | enabled (Linux only) | CW keying via unixcw |
| `PURESIGNAL` | disabled | Adaptive distortion correction (Protocol 1 only) |
| `OPENGL` | disabled | OpenGL rendering |

To toggle, comment/uncomment the `*_INCLUDE=*` lines near the top of the Makefile.

## Architecture

### Application State
The global `RADIO *radio` struct (`radio.h`) is the central application state. It holds references to up to 8 `RECEIVER` structs, one `TRANSMITTER` struct, and hardware/protocol configuration.

### Key Modules

- **`main.c`** — Entry point, GTK init, device discovery, main window creation
- **`radio.c/h`** — Radio lifecycle management, hardware model definitions (ANAN, Hermes, Hermes-Lite, Orion, Atlas, etc.)
- **`receiver.c/h`** — Per-receiver state: VFO A/B, AGC, NR/NB/ANF, filters, modes, panadapter/waterfall
- **`transmitter.c/h`** — TX path: filtering, CTCSS, EER, PureSignal, metering
- **`protocol1.c`, `protocol2.c`, `soapy_protocol.c`** — Hardware communication for Classic HPSDR, enhanced protocol, and SoapySDR devices respectively
- **`discovery.c`, `protocol1_discovery.c`, `protocol2_discovery.c`, `soapy_discovery.c`** — Network device discovery
- **`vfo.c`** — Frequency display and VFO control widget
- **`waterfall.c`, `rx_panadapter.c`, `tx_panadapter.c`** — Spectrum display rendering
- **`audio.c` / `portaudio.c`** — Audio I/O (platform-specific)
- **`midi2.c`, `midi3.c`** — MIDI learn and control mapping (platform-agnostic); `mac_midi.c` / `alsa_midi.c` for platform I/O
- **`actions.c`** — Global action dispatch (used for keybindings and MIDI mappings)
- **`property.c`** — Persistence: save/load radio configuration to/from properties files (`~/.local/share/linhpsdr/<device-mac>.props`; MIDI mappings in `midi.props`)
- **`waterfall_theme.c`** — Waterfall color theme definitions
- **`hl2.c`** — Hermes-Lite 2 specific protocol extensions
- **`wideband.c`, `wideband_panadapter.c`, `wideband_waterfall.c`** — Wideband spectrum display (separate from per-receiver panadapter)
- **`subrx.c`** — Sub-receiver support
- **`ext.c`** — Thread-safe UI dispatch helpers (`ext_*` functions wrap `g_idle_add()` calls)

### UI Dialogs
All dialogs follow the `*_dialog.c/h` naming convention: `radio_dialog`, `receiver_dialog`, `transmitter_dialog`, `midi_dialog`, `configure_dialog`, `bookmark_dialog`, `diversity_dialog`, `pa_dialog`, `eer_dialog`, `puresignal_dialog`, etc.

### Threading
Background tasks (protocol I/O, wisdom file creation) use `GThread`. UI updates must be dispatched to the GTK main thread via `g_idle_add()` or the `ext_*` wrappers in `ext.c` — never call GTK functions directly from protocol/audio threads.
