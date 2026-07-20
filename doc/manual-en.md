# MacHPSDR — User Manual (English)

MacHPSDR is a GTK-based SDR control application for HPSDR hardware (ANAN, Hermes,
Hermes-Lite 2, Orion, …) plus SoapySDR devices (RTL-SDR, LimeSDR, HackRF). It is a
macOS-focused fork of LinHPSDR. This manual describes the main functions in
general terms; it is not a per-button reference.

> Other languages: [Русский](./manual-ru.md) · [Українська](./manual-uk.md) · [Беларуская](./manual-be.md)

---

## 1. Starting the app

```bash
./machpsdr                 # normal start — discovers network + USB devices
./machpsdr --usb-only      # skip network discovery (USB/SoapySDR devices only)
./machpsdr --faker ft8.wav # no hardware: loop an I/Q WAV through the RX chain
```

On start MacHPSDR shows a **device-selection dialog** listing every radio it
found. Pick one to open it. With `--faker` the dialog is skipped and a synthetic
"Fake Noise SDR" starts straight away — useful for trying the UI without
hardware.

Running the `.app` bundle: double-click `MacHPSDR.app`, or, to pass flags,
call its launcher directly
(`MacHPSDR.app/Contents/MacOS/MacHPSDR --faker /abs/path.wav`). See the main
README for the details.

---

## 2. The main window

The window is organised top-to-bottom around one or more **receivers**:

- **VFO display** — the large frequency readout with VFO A/B, mode, filter and
  step. This is the tuning centre of each receiver.
- **Panadapter (RF spectrum)** — the live spectrum of the receiver's passband.
  The filter passband is shaded; the cursor shows where you are tuned.
- **Waterfall** — the scrolling spectrogram beneath the panadapter, colour-coded
  by signal strength.
- **Bottom bar** — grouped control "modules": band/mode/filter selectors, AGC,
  noise reduction, and a **SETUP** module with Configure, Add Receiver, Record,
  and (in DIGU) the FT8 panel toggle.

MacHPSDR is deliberately limited to **2 receivers** for now (a UI decision — the
underlying engine can do more). **Add Receiver** opens a second slice with its
own VFO, panadapter and waterfall.

---

## 3. Tuning and the VFO

- **Click / drag** on the panadapter or waterfall to tune; the scroll wheel
  steps in the current tuning step.
- **VFO A / B**, split, and A↔B swap are on the VFO widget and bottom bar.
- **Zoom** — the panadapter can be zoomed to inspect narrow signals (quick jumps
  to ×10/×12/×16/×32 from the VFO zoom menu). The practical maximum depends on
  the panadapter width (WDSP's analyzer has a fixed internal buffer, so a very
  wide window caps the zoom automatically).
- **Freetune** — a smooth continuous-tuning mode added in this fork.
- **PPM correction** — a frequency-calibration offset for the reference
  oscillator (Configure).

---

## 4. Receiver controls

- **Mode** — LSB, USB, CWL, CWU, AM, SAM, FMN, DIGU, DIGL, etc.
- **Filters** — bandwidth presets per mode, plus adjustable low/high edges. Extra
  attenuation filter controls (att10 / att20) were added in this fork.
- **AGC** — off / long / slow / medium / fast, with an adjustable threshold ("AGC
  gain") line drawn on the panadapter.
- **Noise handling** — NR / NR2 (noise reduction), NB / NB2 (noise blanker), ANF
  (automatic notch filter).
- **Volume / mute / squelch** — audible-output level and squelch per receiver.
- **Sub-receiver** — a second demodulator inside one receiver slice.

---

## 5. Waterfall themes

The waterfall colouring is selectable from several **themes**. The active theme
also drives the colours of the dedicated FT8 band waterfall, so both displays
stay visually consistent.

---

## 6. Transmitting (TX)

For HPSDR radios MacHPSDR provides a full TX path: TX filtering, CTCSS, EER, and
metering. PureSignal (adaptive predistortion, Protocol 1) is available as a
build option. HackRF TX is implemented (half-duplex) via SoapySDR. Keying is by
MOX, PTT, or a mapped MIDI/keyboard action.

---

## 7. FT8 / FT4 digital modes

MacHPSDR has a built-in FT8/FT4 engine (no external WSJT-X needed). Switch the
active receiver to **DIGU** to enable it.

- **Decoder** — automatically runs in DIGU. It decimates the demod audio, buffers
  15-second (FT8) or 7.5-second (FT4) UTC slots and decodes them in the
  background. FT8/FT4 is selected by the protocol switch in the FT8 panel; it
  decodes independently of the listening volume/mute.
- **Compact readout** — by default the bottom bar shows a small decode/QSO
  readout. **Show FT8 Panel** (SETUP module) opens the full panel in place of a
  second receiver.
- **FT8 panel** — protocol (FT8/FT4) selector, TX offset/slot, directed-CQ
  option, the Tx1–Tx6 message column, and a rolling band-activity list (CQ rows
  green, calls to you in bold, "CQ only" filter, double-click a row to work that
  station). New DXCC entities are highlighted (gold = new entity ever, blue = new
  on this band) with the country shown as a tooltip.
- **Auto-QSO** — with **Enable Tx** + **Auto Seq** the engine runs the standard
  WSJT-X exchange (CQ → grid → report → R-report → RR73 → 73) and logs completed
  QSOs to `~/.local/share/machpsdr/ft8_log.adi` (ADIF). Manual Tx1–Tx6 and free
  text are also available.
- **FT8 band waterfall** — a dedicated high-resolution spectrogram of the FT8
  passband appears to the right of the RF waterfall while the panel is open.
  Left-click it to set the TX offset on a clear frequency.
- **Station identity** — callsign and grid are set in **Configure → FT8**; Enable
  Tx is greyed out until both are filled in.
- **Network logging** — completed QSOs can be sent to a logger (Log4OM, N1MM+,
  JTAlert, GridTracker) as WSJT-X-compatible UDP ADIF messages (Configure → FT8).
- **PSK Reporter** — every decoded station can be reported to the PSK Reporter
  map. It only transmits once both callsign and grid are set (Configure → FT8).
- **DXCC** — the "new one" highlight uses AD1C's `cty.dat`; a **Reload cty.dat**
  button and status live in Configure → FT8.

---

## 8. I/Q + audio recorder

The **Record** button (SETUP module) captures the active receiver to
`~/.local/share/machpsdr/`:

- `rec_<UTC>_iq.wav` — off-air I/Q at the receiver's sample rate. It is written
  in the same 16-bit stereo format the `--faker` player reads, so a recording can
  be **replayed back through the fake device**.
- `rec_<UTC>_af.wav` — clean demodulated audio at 48 kHz.

Which streams are written and the output folder are set in
**Configure → Recording**.

---

## 9. Configuration

The **Configure** dialog groups settings into pages: Radio, Receiver,
Transmitter, MIDI, Bookmarks, Diversity, PA, EER, PureSignal, **FT8**, and
**Recording**. All settings are saved automatically to a per-device properties
file under `~/.local/share/machpsdr/` and restored on the next start.

---

## 10. MIDI and keyboard control

Any global **action** (tune, mode, filter, AGC, MOX, zoom, …) can be bound to a
keyboard shortcut or a MIDI control via **MIDI learn**. On macOS MIDI uses
CoreMIDI. Mappings are saved to `midi.props`.

---

## 11. Bookmarks

Frequencies of interest can be saved as **bookmarks** and recalled from the
bookmark dialog; bookmarks can also appear as markers on the panadapter.

---

## 12. Testing without hardware (fake device)

`--faker` starts a synthetic SDR that loops a 16-bit stereo I/Q WAV through the
full RX/decoder chain. Pass a file (`--faker ft8.wav`) or set `MACHPSDR_FAKE_IQ`;
with no file it falls back to `iq.wav`. The recording is resampled to the
receiver rate and auto-centred to baseband, then looped. Add `--revert-iq` to
swap I and Q if the sideband is inverted. This is the recommended way to try FT8
decoding, the recorder replay, and the UI without a radio.
