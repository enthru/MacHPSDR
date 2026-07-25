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
  The filter passband is shaded; the cursor shows where you are tuned. It can be
  turned off per receiver (a **Show Panadapter** check box in the receiver
  settings dialog) — the waterfall then fills the whole spectrum area.
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
- **Tuning on the VFO display itself** — three ways:
  - **Mouse wheel** over a digit steps by that digit's place value: up tunes up,
    down tunes down (consistent for VFO A/B and in ctun/freetune; the digit under
    the pointer is hit-tested from the text, so it is exact).
  - **Hover a digit and press a number key** to overwrite that digit in place;
    the cursor then advances one digit to the right (SDR#-style), so you can fill
    a frequency left-to-right.
  - **Left-click** the frequency to open a small entry field pre-filled with the
    current frequency in MHz — edit or retype it (`.` or `,` decimal, and the
    grouped `14.074.000` style are both accepted) and press Enter, Esc to cancel.
    The **band-stack menu** is on the **right-click**.
  - Tuning is clamped to **0 – 6 GHz** (or the device's own upper limit if
    lower), so a stray edit can't run the VFO off to a nonsense frequency.
- **VFO A / B**, split, and A↔B swap are on the VFO widget and bottom bar.
- **Zoom** — the panadapter can be zoomed to inspect narrow signals (quick jumps
  to ×10/×12/×16/×32 from the VFO zoom menu). The practical maximum depends on
  the panadapter width (WDSP's analyzer has a fixed internal buffer, so a very
  wide window caps the zoom automatically).
- **Freetune** — a smooth continuous-tuning mode added in this fork.
- **PPM correction & auto-calibration** — corrects the reference-oscillator
  error (fractional ppm; applied on Protocol 1, Protocol 2 and SoapySDR alike).
  It can be measured automatically from a time-signal station's carrier — see
  §10 (Configure → Misc).

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
- **Sub-receiver (SUBRX)** — a second demodulator inside one receiver slice,
  switched on with the **SUBRX** button on the VFO; it has its own frequency
  (VFO B), mode, filter and AGC. The main RX plays on the left channel and the
  sub on the right; a **Sub-RX mix (split↔mono)** slider in the receiver settings
  crossfades from that channel split to an equal mono blend in both ears. Its
  on/off state and the mix position are remembered across restarts.
- **Diversity** *(experimental — needs testing on real hardware).* Combines two
  coherent ADC streams with adjustable gain/phase to null a local interferer or
  fight fading. Turned on with the **DIV** button on the VFO: it adds a hidden
  second receiver and creates the mixer, after which a **DMIX-0** page with the
  gain/phase controls and a disclaimer appears in Configure. Protocol 1 only,
  needs a radio with two receivers/ADCs; not yet verified on hardware in this fork.

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

## 7. Decoders and FT8 / FT4

The bottom bar's **Decode** module carries a **decoder selector**. No decoder runs
by default — pick one to start it. The selector only lists the decoders usable in
the current mode: **DIGU / DIGL** offers **Off / FT8 / FT4 / SSTV / WEFAX**; **NFM
(FMN)**, where only SSTV applies (VHF/ISS SSTV over narrow FM), offers just
**Off / SSTV**. The image decoders (SSTV, WEFAX) are covered in §8; FT8/FT4 below.
The choice is remembered between sessions.

MacHPSDR has a built-in FT8/FT4 engine (no external WSJT-X needed): pick **FT8** or
**FT4** from the selector with the active receiver in **DIGU**.

- **Decoder** — decimates the demod audio, buffers 15-second (FT8) or 7.5-second
  (FT4) UTC slots and decodes them in the background, independently of the
  listening volume/mute.
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

## 8. SSTV and WEFAX — images and weather fax

Two self-contained image decoders share the Decode-module selector with FT8. Like
FT8 they run at full audio level regardless of the volume/mute, so you can decode
silently. Each opens an image panel in place of the second receiver.

### SSTV — analogue images (receive and transmit)

Pick **SSTV** from the selector and press **Show SSTV** to open the panel. SSTV
works in **DIGU/DIGL** for **HF** SSTV (SSB — the calling frequency is 14.230 MHz)
and in **FMN** for **VHF/ISS** SSTV (the ISS transmits on **145.800 MHz FM**, so
use FMN for Robot 36 / PD120).

- **Receive** — the decoder auto-detects the mode from the VIS header and paints
  the picture line-by-line. Supported: **Martin M1/M2**, **Scottie S1/S2/DX**,
  **Robot 36/72**, **PD50/90/120/160/180/240** — which covers ISS SSTV (Robot 36
  MAI-75, PD120 ARISS events). **Auto** works even on FM, where de-emphasis smears
  the VIS header, by recognising the mode from its sync-pulse line period, so you
  rarely need to choose anything. A **Mode** override, an automatic slant corrector
  with a manual **Slant ±** trim on top, and **AFC** (tracks ISS Doppler) are
  provided. **Save** writes a PNG to `~/.local/share/machpsdr/sstv/`; **Clear**
  resets.
- **Transmit** — the panel's **Tx** row: pick a **mode**, **Load…** any image
  (fitted to the mode's geometry preserving aspect ratio — letter-/pillar-boxed
  with black, not stretched — and previewed), and press **Send**. It transmits a
  standard VIS header plus the scan lines through the normal phone TX chain, so it
  works on any protocol — DIGU/DIGL for HF, FMN for VHF FM. MOX is keyed
  automatically for the length of the picture (a progress bar tracks it) and
  dropped at the end, with a safety watchdog; **Stop** aborts.

### WEFAX — HF radiofax / weather charts (receive only)

Pick **WEFAX** from the selector (in **DIGU/DIGL** — HF fax is USB) and press
**Show WEFAX**. Unlike SSTV, WEFAX is a *continuous* scan (weather charts and
satellite images from stations such as DWD Hamburg, NMG/NHC, Northwood, …), so the
picture scrolls as it arrives rather than being a fixed frame. It is designed to
just work:

- **Auto-phase** (on by default) finds the fax's recurring vertical reference (its
  black margin) and pulls it to the left edge itself, so the picture self-aligns
  with no clicking.
- **Auto-start** (on by default) spots the transmission's start tone (300 Hz for
  IOC 576 / 675 Hz for IOC 288) to begin a fresh page and seed the AFC.
- **LPM** (60/90/**120**/240) and **IOC** (**576**/288) set the line timing;
  120 lpm / IOC 576 is the weather-fax standard and the default.
- An **auto-exposure AFC** keeps the white background correct (drift can't wash the
  picture grey), and **Denoise** removes impulse specks while keeping thin chart
  lines. **Invert** flips a reversed (wrong-sideband) signal to a positive
  black-on-white image.
- To do it by hand: untick Auto-phase and **click the image** to set the left
  margin, use **Start** to begin a page, **Slant ±** to deskew.
- **Save** writes a PNG to `~/.local/share/machpsdr/wefax/`; **Clear** starts over.

Tune the station in **DIGU/USB** ~1.9 kHz below its assigned frequency (so black
lands on 1500 Hz, white on 2300 Hz) and give it a **wide receive filter (~1.9 kHz,
e.g. 1000–2900 Hz)** — too narrow and the fast black↔white transitions smear.

---

## 9. I/Q + audio recorder

The **Record** button (SETUP module) captures the active receiver to
`~/.local/share/machpsdr/`:

- `rec_<UTC>_iq.wav` — off-air I/Q at the receiver's sample rate. It is written
  in the same 16-bit stereo format the `--faker` player reads, so a recording can
  be **replayed back through the fake device**.
- `rec_<UTC>_af.wav` — clean demodulated audio at 48 kHz.

Which streams are written and the output folder are set in
**Configure → Recording**.

---

## 10. Configuration

The **Configure** dialog groups settings into pages: Radio, Receiver,
Transmitter, MIDI, Bookmarks, Diversity, PA, EER, PureSignal, **FT8**,
**Recording**, and **Misc**. All settings are saved automatically to a
per-device properties file under `~/.local/share/machpsdr/` and restored on the
next start.

The **Misc** page holds the colour skin, custom attenuator-button labels,
Broadcast-FM options, and **Frequency Calibration (PPM)**: pick a
time/frequency-standard station (RWM, WWV, CHU, BPM on HF; MSF, DCF77, Droitwich
on LF) and press **Calibrate** to measure its carrier and set the oscillator
correction automatically, **Tune** to zero-beat it by ear, or type the value in
manually.

---

## 11. MIDI and keyboard control

Any global **action** (tune, mode, filter, AGC, MOX, zoom, …) can be bound to a
keyboard shortcut or a MIDI control via **MIDI learn**. On macOS MIDI uses
CoreMIDI. Mappings are saved to `midi.props`.

---

## 12. Bookmarks

Frequencies of interest can be saved as **bookmarks** and recalled from the
bookmark dialog; bookmarks can also appear as markers on the panadapter.

---

## 13. Testing without hardware (fake device)

`--faker` starts a synthetic SDR that loops a 16-bit stereo I/Q WAV through the
full RX/decoder chain. Pass a file (`--faker ft8.wav`) or set `MACHPSDR_FAKE_IQ`;
with no file it falls back to `iq.wav`. The recording is resampled to the
receiver rate and auto-centred to baseband, then looped. If the sideband is
inverted, tick **Swap I & Q** in the radio dialog to mirror the spectrum live.
This is the recommended way to try FT8 decoding, the recorder replay, and the UI
without a radio.
