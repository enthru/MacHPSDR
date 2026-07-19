# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MacHPSDR is a GTK3-based SDR (Software Defined Radio) control application for HPSDR hardware, written in C. It is a personal fork of [LinHPSDR](https://github.com/g0orx/linhpsdr) by John Melton (G0ORX/N6LYT), with a primary focus on macOS compatibility and several feature additions (freetune mode, waterfall themes, ppm correction, att10/att20 filter control, etc.). Licensed under the GNU GPL (v2 or later); see `LICENSE` and `NOTICE`.

The binary, install paths and config directory use the lowercase name `machpsdr`; the display name (window title, About dialog, `.app` bundle) is `MacHPSDR`.

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
open MacHPSDR.app
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
| `FT8` | enabled | FT8 RX decoder + TX/auto-QSO (decoder auto-enabled in DIGU; big QSO panel is opt-in via a bottom-bar toggle); vendored `ft8_lib/` |

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
- **`property.c`** — Persistence: save/load radio configuration to/from properties files (`~/.local/share/machpsdr/<device-mac>.props`; MIDI mappings in `midi.props`)
- **`waterfall_theme.c`** — Waterfall color theme definitions
- **`hl2.c`** — Hermes-Lite 2 specific protocol extensions
- **`wideband.c`, `wideband_panadapter.c`, `wideband_waterfall.c`** — Wideband spectrum display (separate from per-receiver panadapter)
- **`ft8_decoder.c/h`** — FT8 receive decoder. Auto-enabled when the active receiver's mode is DIGU (tap in `receiver.c:process_rx_buffer`); decimates the 48 kHz demod audio to 12 kHz, buffers 15-second UTC slots, decodes in a background `GThread`. Displayed in the bottom-bar decoder block (see `rds_update_cb` in `radio.c`). Uses the vendored `ft8_lib/` (Karlis Goba, MIT); gated by the `FT8` Makefile flag. Each `FT8_DECODE` also carries structured `call_to`/`call_de`/`extra` fields (via `ftx_message_decode_std`) for the QSO engine.
- **`ft8_encoder.c/h`** — FT8 **TX engine** (Phase 2). `ft8_tx_prepare()` packs a message (`ftx_message_encode` + `ft8_encode`) and synthesizes the GFSK waveform at the 48 kHz mic rate (ported `gfsk_pulse`/`synth_gfsk`, BT=2.0, 79 symbols ≈ 12.64 s). A ~100 ms slot scheduler (`ft8_tx_arm`, GTK thread) keys MOX (`set_mox`) at the top of the chosen even/odd UTC slot and drops it when the waveform ends. `add_mic_sample()` in `transmitter.c` substitutes `ft8_tx_next_sample()` for the mic input in DIGU while `ft8_tx_active()`, so the normal DIGU (USB) TX chain modulates the tone to dial+offset (universal across protocol1/2/soapy).
- **`ft8_qso.c/h`** — FT8 **auto-QSO state machine** (Phase 2). Polls the decoder every ~500 ms (GTK thread), drives the standard WSJT-X exchange (CQ → grid → report → R-report → RR73 → 73), keys TX on the opposite slot, and appends completed QSOs to `~/.local/share/machpsdr/ft8_log.adi` (ADIF). Also: `Enable Tx`/`Auto Seq` gates, manual `Tx1..Tx6` (`ft8_qso_select_tx`) and free text (`ft8_qso_send_free`), worked-before lookup from the ADIF log (`ft8_qso_worked`), and a 6-minute Tx watchdog. Station call/grid + TX offset/slot live on `RADIO` (`station_call`, `station_grid`, `ft8_tx_offset`, `ft8_tx_even`), persisted in `radio_save_state`.
- **`ft8_panel.c/h`** — Embedded **FT8 QSO panel** (Phase 2). A GtkWidget slotted into the RX stack *in place of the second receiver*. It is **opt-in**, not automatic: in DIGU the bottom-bar decode block shows a compact decode readout by default and offers a **"Show FT8 Panel"** toggle button (in that block, wired via `ft8_expand_cb`); the panel appears only while `radio->ft8_panel_open` is set. `radio_ft8_panel_sync()` (in `radio.c`, hooked from `receiver_mode_changed`) creates/tears down the panel from `ft8_panel_open && mode==DIGU`, disables "Add Receiver" while it owns the slot, and force-closes it on leaving DIGU. While the panel is open the bottom-bar block switches to a **QSO status** readout (DX call, next Tx message, exchange state, offset/slot) rather than duplicating the list. Panel layout: config row on top, then a horizontal split with the Tx1–Tx6 message column on the **left** and the rolling band-activity list on the **right** (CQ rows green, to-me rows bold, "CQ only" filter via a `GtkTreeModelFilter`; double-click a row to work that station). **Enable Tx** / **Auto Seq** toggles, free-text entry, DX + status readout along the bottom. TX offset is also settable by **Shift+click** on the RX panadapter (`receiver_button_press_event_cb`), which draws a green "TX" marker (`rx_panadapter.c`).
- **`subrx.c`** — Sub-receiver support
- **`ext.c`** — Thread-safe UI dispatch helpers (`ext_*` functions wrap `g_idle_add()` calls)

### UI Dialogs
All dialogs follow the `*_dialog.c/h` naming convention: `radio_dialog`, `receiver_dialog`, `transmitter_dialog`, `midi_dialog`, `configure_dialog`, `bookmark_dialog`, `diversity_dialog`, `pa_dialog`, `eer_dialog`, `puresignal_dialog`, etc.

### Threading
Background tasks (protocol I/O, wisdom file creation) use `GThread`. UI updates must be dispatched to the GTK main thread via `g_idle_add()` or the `ext_*` wrappers in `ext.c` — never call GTK functions directly from protocol/audio threads.
