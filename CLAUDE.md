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

No automated test suite exists — testing is manual with physical hardware. For
hardware-free smoke testing there is a synthetic **fake device**: run
`./machpsdr --faker <iq.wav>` (e.g. `--faker ft8.wav`) to loop an I/Q recording
through the full RX/decoder chain. With `--faker` the device-selection dialog is
skipped and the fake radio starts straight up, so it can be launched headless
(e.g. under lldb). See `fake_protocol.c` and the flag parsing in `main.c`.

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
- **`ft8_decoder.c/h`** — FT8 receive decoder. Auto-enabled when the active receiver's mode is DIGU (tap in `receiver.c:process_rx_buffer`); decimates the 48 kHz demod audio to 12 kHz, buffers 15-second UTC slots, decodes in a background `GThread`. Displayed in the bottom-bar decoder block (see `rds_update_cb` in `radio.c`). Uses the vendored `ft8_lib/` (Karlis Goba, MIT); gated by the `FT8` Makefile flag. **Decodes independently of the listening volume/mute:** in DIGU the WDSP audio-panel gain is forced to unity (`rx_panel_gain()` in `receiver.c`) so the decoder tap (`rx->audio_output_buffer`) always gets a full-level signal, and the volume/mute is applied to the audible output in software in `process_rx_buffer()` — the operator does not listen to FT8. Each `FT8_DECODE` also carries structured `call_to`/`call_de`/`extra` fields (via `ftx_message_decode_std`) for the QSO engine.
- **`ft8_encoder.c/h`** — FT8 **TX engine** (Phase 2). `ft8_tx_prepare()` packs a message (`ftx_message_encode` + `ft8_encode`) and synthesizes the GFSK waveform at the 48 kHz mic rate (ported `gfsk_pulse`/`synth_gfsk`, BT=2.0, 79 symbols ≈ 12.64 s). A ~100 ms slot scheduler (`ft8_tx_arm`, GTK thread) keys MOX (`set_mox`) at the top of the chosen even/odd UTC slot and drops it when the waveform ends. `add_mic_sample()` in `transmitter.c` substitutes `ft8_tx_next_sample()` for the mic input in DIGU while `ft8_tx_active()`, so the normal DIGU (USB) TX chain modulates the tone to dial+offset (universal across protocol1/2/soapy).
- **`ft8_qso.c/h`** — FT8 **auto-QSO state machine** (Phase 2). Polls the decoder every ~500 ms (GTK thread), drives the standard WSJT-X exchange (CQ → grid → report → R-report → RR73 → 73), keys TX on the opposite slot, and appends completed QSOs to `~/.local/share/machpsdr/ft8_log.adi` (ADIF). Also: `Enable Tx`/`Auto Seq` gates, manual `Tx1..Tx6` (`ft8_qso_select_tx`) and free text (`ft8_qso_send_free`), worked-before lookup from the ADIF log (`ft8_qso_worked`), and a 6-minute Tx watchdog. Station call/grid + TX offset/slot live on `RADIO` (`station_call`, `station_grid`, `ft8_tx_offset`, `ft8_tx_even`), persisted in `radio_save_state`. Each completed QSO is also pushed to a network logger via `ft8_udp_log()` (see `ft8_udp.c`).
- **`ft8_udp.c/h`** — **Network QSO logging** (JTDX/WSJT-X compatible). `ft8_udp_log()` sends the completed QSO's ADIF record as a WSJT-X UDP "ADIF" message (magic `0xadbccbda`, schema 2, message type 12; big-endian, uint32-length + UTF-8 strings) to a configurable host:port, so QSOs appear in a logger (Log4OM, N1MM+, JTAlert, GridTracker) over the network. Gated by `radio->ft8_log_udp`; host/port in `radio->ft8_log_udp_host`/`ft8_log_udp_port` (default `127.0.0.1:2237`), all persisted in `radio_save_state`.
- **`ft8_pskreporter.c/h`** — **PSK Reporter spot reporting.** `ft8_pskreporter_report()` is called from the decoder worker (`ft8_decoder.c:decode_slot`) once per 15 s slot and sends every decoded station as a reception report to `report.pskreporter.info:4739` (UDP), the same network WSJT-X/JTDX feed — so the operator's spots appear on the PSK Reporter map. Wire format is PSK Reporter's IPFIX protocol (private enterprise number 30351): a message header, a receiver-information options template (`0x50E2`: receiverCallsign/Locator/decodingSoftware/antennaInformation) and a sender/reception-record template (`0x50E3`: senderCallsign/frequency(5B)/sNR(1B)/mode/senderLocator/informationSource(1B)/dateTimeSeconds), both re-sent each packet, followed by the receiver data record and one sender record per spot. RF frequency = the active RX dial (`frequency_a`) + the decode's audio offset; `senderLocator` is filled only when the decode's `extra` is a Maidenhead grid; free-text/telemetry decodes (no `call_de`) and self-spots are skipped. **Gated by `radio->ft8_pskr` AND a non-empty `station_call` + `station_grid`** (per the user requirement — nothing is sent until both identity fields are set). Enabled via a checkbox in Configure → FT8 (`ft8_dialog.c`); `ft8_pskr` persisted in `radio_save_state`. Packet layout byte-verified with an IPFIX round-trip parser. Compiled only with the `FT8` flag.
- **`ft8_waterfall.c/h`** — **Dedicated FT8 band waterfall** (JTDX-style). Self-contained spectrogram of just the FT8 audio passband (0–3000 Hz): a windowed 4096-pt real FFT (`kiss_fftr`) of the decoder's 12 kHz audio ring via `ft8_decoder_get_spectrum()` (in `ft8_decoder.c`) → ~2.93 Hz/bin, fine enough to separate FT8 tones. Renders a scrolling `GdkPixbuf` coloured with `get_waterfall_color()` (the active RX's `waterfall_color_theme`), with a 500 Hz frequency grid and a green TX-offset marker. **Independent of the main WDSP analyzer** (no `SetAnalyzer`/`dMAX_PIXELS` limit), so it gives real per-Hz zoom the `pixels=width×zoom` main panadapter can't. **Left-click sets the FT8 TX offset** directly on a clear frequency. Its own ~14 fps `g_timeout`, torn down on `destroy`. **Placement:** to the **right of the active receiver's RF spectrum (~1/3 width)**. The RF spectrum (`rx->vpaned`) is wrapped in a **windowless horizontal `GtkBox`** (`rx->wf_hpaned` — a box, despite the name); `receiver_ft8_waterfall_sync()` (`receiver.c`) packs/removes the FT8 waterfall as its second child, sized to 1/3 of the row by an `ft8_wf_box_alloc` size-allocate handler that tracks resizes. A `GtkBox` is used deliberately, **not** a `GtkPaned`: wrapping `vpaned` in a `GtkPaned` blanked the main waterfall (window-hierarchy quirk), whereas the windowless box leaves the vpaned's `GdkWindow` parenting unchanged. Shown only when the RX is in **DIGU *and* the FT8 panel is open** (`radio->ft8_panel_open`) — hooked from `receiver_mode_changed`, `radio_ft8_panel_sync`, and once after `create_visual`. **Span:** follows the DIGU receive filter width (`filter_low_a/high_a`), clamped to 0.5–5 kHz (`wf_span_hz`); `ft8_decoder_get_spectrum` returns up to 5 kHz and the widget shows the filter sub-span, rescaling its content to any width on resize. The old main-panadapter FT8 auto-zoom was removed (this waterfall replaces it).
- **`ft8_dxcc.c/h`** — **DXCC entity resolver** (backs the "new one" highlight). Parses AD1C's `cty.dat` country file (bundled in the repo root; ~346 entities) into an exact-callsign hash plus a prefix hash, and `ft8_dxcc_entity(call)` returns a DXCC entity index by exact-call override else **longest-prefix** match (faithfully reproducing cty.dat precedence — e.g. `UA9X…`→European Russia via the 4-char district prefix while bare `UA9`→Asiatic Russia; portable `SV9/DL1XX` handled by scanning `/`-segments). File is located at runtime via `$MACHPSDR_CTY` → `.app` `../Resources/cty.dat` → cwd `cty.dat` → `~/.local/share/machpsdr/` → `/usr/local/share`. `ft8_dxcc_init()` is called from `main.c` **before** `ft8_qso_init()` so the log's worked-before scan can tag worked entities. `ft8_dxcc_reload()`/`_count()`/`_path()` back the **Reload cty.dat** button + status label in Configure → FT8. Missing file degrades gracefully (no highlight, no crash). Compiled only with the `FT8` flag. SNR reporting is unrelated. — Worked-DXCC tracking lives in `ft8_qso.c`: a `worked_dxcc` set (entity indices) built alongside the worked-call set in `worked_load`/`worked_add`; `ft8_qso_new_dxcc(call)` = entity resolvable AND not yet worked, `ft8_qso_country(call)` returns the country name. The panel (`ft8_panel.c`) highlights new-DXCC rows with a goldenrod `cell-background` (`COL_NEWDX`) and shows the country as a hover tooltip (`COL_COUNTRY`, `on_query_tooltip`).
- **`ft8_dialog.c/h`** — **FT8 configuration page** (`create_ft8_dialog`, added to the Configure dialog as the **"FT8"** page). Holds the station **callsign** and **grid** (moved here from the panel — they are identity, not per-QSO controls; both entries force **uppercase** as you type, and **Enable Tx** in the panel is greyed out until both are set), the **DXCC** country-file status + Reload button (see `ft8_dxcc.c`), the **Network Logging** settings (enable, host/IP, port), and the PSK Reporter **Spot Reporting** toggle. Compiled only with the `FT8` flag.
- **`ft8_panel.c/h`** — Embedded **FT8 QSO panel** (Phase 2). A GtkWidget slotted into the RX stack *in place of the second receiver*. It is **opt-in**, not automatic: in DIGU the bottom-bar decode block shows a compact decode readout by default and a **"Show FT8 Panel"** toggle button (in the bottom-bar **SETUP** module, beside Configure/Add Receiver, wired via `ft8_expand_cb`) opens the panel; it appears only while `radio->ft8_panel_open` is set. `radio_ft8_panel_sync()` (in `radio.c`, hooked from `receiver_mode_changed`) creates/tears down the panel from `ft8_panel_open && mode==DIGU`, disables "Add Receiver" while it owns the slot, and force-closes it on leaving DIGU. Deep FT8 signal viewing is provided by the dedicated FT8 band waterfall beside the main waterfall (see `ft8_waterfall.c`), not by force-zooming the main panadapter (that auto-zoom was removed). The app-wide zoom cap was raised from 8× to 32× (`actions.c`, `midi3.c`; the `vfo.c` zoom menu adds quick x10/x12/x16/x32 jumps; `rx_panadapter.c` gridline divisors handle >8× via the `default` case). **Hard limit:** WDSP's analyzer keeps fixed `dMAX_PIXELS` (16384) internal buffers, and `rx->pixels == panadapter_width × zoom` is passed straight to `SetAnalyzer`; exceeding 16384 overruns those buffers and corrupts the heap (blank waterfall **and** broken RX/decode). So `receiver_change_zoom` **and** the panadapter `resize_timeout` clamp the zoom to `16384 / panadapter_width` — the effective max zoom is therefore width-dependent (deeper when the panadapter is narrow, e.g. with the FT8 panel open). A requested 32× is honoured only up to that ceiling; going deeper would need a span-based (`SetAnalyzer` fmin/fmax) rework rather than the pixels = width×zoom model. Callsign/grid are **no longer in the panel** — they live in Configure → FT8 (`ft8_dialog.c`); the panel's config row keeps only the operational TX offset/slot. While the panel is open the bottom-bar block switches to a **QSO status** readout (DX call, next Tx message, exchange state, offset/slot) rather than duplicating the list. Panel layout: config row on top, then a horizontal split with the Tx1–Tx6 message column on the **left** and the rolling band-activity list on the **right** (CQ rows green, to-me rows bold, "CQ only" filter via a `GtkTreeModelFilter`; double-click a row to work that station). **Enable Tx** / **Auto Seq** toggles, free-text entry, DX + status readout along the bottom. TX offset is also settable by **Shift+click** on the RX panadapter (`receiver_button_press_event_cb`), which draws a green "TX" marker (`rx_panadapter.c`).
- **`subrx.c`** — Sub-receiver support
- **`ext.c`** — Thread-safe UI dispatch helpers (`ext_*` functions wrap `g_idle_add()` calls)

### UI Dialogs
All dialogs follow the `*_dialog.c/h` naming convention: `radio_dialog`, `receiver_dialog`, `transmitter_dialog`, `midi_dialog`, `configure_dialog`, `bookmark_dialog`, `diversity_dialog`, `pa_dialog`, `eer_dialog`, `puresignal_dialog`, etc.

### Threading
Background tasks (protocol I/O, wisdom file creation) use `GThread`. UI updates must be dispatched to the GTK main thread via `g_idle_add()` or the `ext_*` wrappers in `ext.c` — never call GTK functions directly from protocol/audio threads.
