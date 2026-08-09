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
found. Pick one to open it. The list always ends with an **"I/Q Player"** entry —
a hardware-free player for a recorded I/Q file (see §14); select it to try the UI
or replay a capture without a radio. With `--faker` the dialog is skipped and the
I/Q Player starts straight away.

### Adding a device on another network

A radio that is not on your own subnet answers no scan — neither the USB one nor
the network one — so it has to be named. The selection window has a **Network
device** row for that:

1. pick the kind — currently **PlutoSDR**;
2. type its address, e.g. `192.168.36.190`;
3. press **Add** (or Enter).

It is tried straight away. If it answers it joins the list at once — no restart —
and you can select it and press **Start Radio**. If it does not, the line below
says why and nothing is saved, so a mistyped address cannot settle in and slow
every later start. Devices that do answer are remembered and tried again at each
start; select one and press **Forget** to drop it.

For a one-off you can still name a Pluto on the command line:

```bash
MACHPSDR_PLUTO_URI=ip:192.168.1.10 ./machpsdr
```

Give it as a URI (`ip:<address>`), not as a bare host name: a host name makes the
Pluto driver report a local connection alongside it, and SoapySDR gives that
priority over the address you asked for, so the app ends up looking for the radio
inside your own PC and reports *no device found in this context*. The older
`MACHPSDR_PLUTO_HOST=<host>` still works and is converted to the same URI.

If an attempt fails, restart the app before trying again: the Pluto driver keeps
its first (failed) connection for the life of the process.

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
  settings dialog) — the waterfall then fills the whole spectrum area. The
  receiver settings dialog's **Panadapter** section also offers **Panadapter
  Automatic** — a check box under the High/Low sliders that sets the dB scale
  from the signal itself (bottom on the band noise floor, top just above the
  strongest signal in view), widening quickly and narrowing slowly so the display
  does not jump; while it is on, High/Low and the panadapter's dB-scale scroll
  zone are inactive. The same section also offers a **Peak Hold**
  overlay (a light max-hold trace, with a **Peak Decay** slider for how fast the
  held peaks fall), a **Histogram** persistence display (a virtual-phosphor
  heat-coloured cloud showing where the trace has been, with a **Persistence
  Decay** slider) and **Detector** / **Averaging** drop-downs that change how the
  spectrum is computed; all are remembered per receiver.
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
  §11 (Configure → Display).

---

## 4. Receiver controls

- **Mode** — LSB, USB, CWL, CWU, AM, SAM, FMN, DIGU, DIGL, etc.
- **Filters** — bandwidth presets per mode, plus adjustable low/high edges. Extra
  attenuation filter controls (att10 / att20) were added in this fork.
- **AGC** — off / long / slow / medium / fast, with an adjustable threshold ("AGC
  gain") line drawn on the panadapter.
- **Noise handling** — NR / NR2 / NR3 / NR4 (noise reduction — NR3 is an RNNoise neural-net denoiser, NR4 is libspecbleach adaptive spectral subtraction; pick from the VFO NR menu), NB / NB2 (noise blanker), ANF
  (automatic notch filter) and **SNB** (Spectral Noise Blanker). Each is
  remembered per receiver between sessions.
- **Manual notch filters (MNF)** — place your own notches to kill a steady
  carrier or heterodyne. **Ctrl+click** on the RX spectrum drops a notch at that
  frequency; **Ctrl+click** on an existing notch removes it; **Ctrl+scroll** over
  one changes its width. A notch is drawn as a translucent red band with a centre
  line (grey while switched off) and is stored by absolute frequency, so it stays
  on the offending signal as you tune. Up to 16 per receiver, remembered between
  sessions. The **Manual Notch (MNF)** block in Configure → RX-N lists them for
  exact editing: switch a notch off without deleting it, type a frequency or
  width, choose the width new notches get, and flip a notch to **AF** — an AF
  notch keeps a fixed offset from the demodulated centre, so it rides the dial
  and always kills the same audio pitch instead of one RF frequency.
- **Audio Peak Filter (APF)** — a narrow audio peaking filter for CW that boosts
  the beat-note (centred on your sidetone pitch) to lift weak signals out of the
  noise. Enable it, and set its bandwidth (sharpness) and gain, in
  Configure → CW; it runs only in CW modes, and applies to the sub-receiver too
  (gated on the sub's own mode). Remembered per receiver.
- **Volume / mute / squelch** — audible-output level and squelch per receiver.
  The **SQL** bar is mode-aware: in FM it drives the FM noise squelch, in every
  other mode an amplitude / voice squelch that mutes until a signal exceeds the
  threshold. At its minimum the squelch is fully off (audio always passes). The
  setting is remembered **per mode**, and the **Squelch (AM/SSB)** block in
  Configure → RX-N sets the dB range the bar spans and the gate's max tail, so
  the amplitude squelch can be calibrated against a live band.
- **Sub-receiver (SUBRX)** — a second demodulator inside one receiver slice,
  switched on with the **SUBRX** button on the VFO; it has its own frequency
  (VFO B), mode, filter and AGC. The main RX plays on the left channel and the
  sub on the right; a **Sub-RX mix (split↔mono)** slider in the receiver settings
  crossfades from that channel split to an equal mono blend in both ears. Its
  on/off state and the mix position are remembered across restarts.
- **Diversity** *(experimental — needs testing on real hardware).* Combines two
  coherent ADC streams with adjustable gain/phase to null a local interferer or
  fight fading. It lives on the **Configure → Diversity** page: an **Enable
  diversity** checkbox (greyed out on devices that can't do it) turns it on —
  adding a hidden second receiver and the mixer — next to the gain/phase controls
  and a disclaimer. Protocol 1 only, needs a radio with two receivers/ADCs; not
  yet verified on hardware in this fork.
- **DX cluster spots** — when a DX cluster is connected (see Configure →
  Cluster), incoming spots are drawn on the RX panadapter as a short coloured
  tick with the callsign, colour-keyed by DXCC entity. **Left-click a spot
  marker** to tune the receiver straight onto that station (works in normal,
  ctun and freetune tuning). Spots disappear after 15 minutes.

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

**TX speech processing (Configure → TX).** A full transmit audio chain:
**CESSB** (controlled-envelope SSB overshoot control), **CFC** (a 5-band
Continuous Frequency Compressor at 200 / 1 k / 2 k / 3 k / 4 k Hz with pre-comp
and pre-emphasis), a **Phase Rotator** (voice-asymmetry reduction, adjustable
corner and stages), and a **10-band graphic equaliser** on both TX and RX.
Per-stage **Leveler / CFC / Compressor** gain-reduction meters (dB) are drawn
under the ALC line on the TX panadapter while transmitting. *These are built and
tested with the fake device but are not yet verified on the air — this fork has
no transmit hardware.*

---

## 7. Decoders and FT8 / FT4

The bottom bar's **Decode** module carries a **decoder selector**. No decoder runs
by default — pick one to start it. The selector only lists the decoders usable in
the current mode: **DIGU / DIGL** offers **Off / FT8 / FT4 / SSTV / WEFAX**; **NFM
(FMN)**, where only SSTV applies (VHF/ISS SSTV over narrow FM), offers just
**Off / SSTV / APT** (VHF/ISS SSTV over narrow FM, and NOAA weather-satellite
pictures). The image decoders (SSTV, WEFAX, APT) are covered in §8; FT8/FT4 below.
The choice is remembered between sessions.

In the **data modes (DIGU/DIGL)**, and whenever any decoder is running, **every
waveform-altering block is automatically bypassed** for that receiver — all four
NR modes (NR/NR2/NR3/NR4), the noise blankers (NB/NB2), the auto-notch (ANF), the
spectral noise blanker (SNB) and the manual notches. These are tuned for the ear
and would distort the signal a modem/decoder (or external software over VAC/TCI)
needs; only what the decoder actually needs — demodulation, the passband filter
and AGC — stays in. Your selections are kept (the VFO still shows them) and take
effect again the moment you leave the data mode and stop decoding.

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

**CW (Morse) decoder.** In the CW modes (CWL/CWU), the Decode block offers an
**Off / CW** selector. With **CW** selected, the receiver's audio is decoded to
text: the decoder locks onto the keyed tone, adapts to the sending speed
automatically, and turns the dots and dashes into letters. Press **Show CW** to
open a panel (in place of the second receiver, like the SSTV/FT8 panels) with
the scrolling decoded text, a live WPM / tone-frequency readout, and a **Clear**
button. The decoded text also flows into the Decode block's readout. Decoding
is automatic — no external program is needed. (It works best on a single,
well-tuned CW signal in a fairly narrow filter; heavy QRM or several
overlapping signals will garble the text.)

The Show-CW panel also lets you **send** CW. Its TX row has eight **message
memories** (buttons M1…M8), a **free-text** field, **Send** and **Stop**
buttons and a **WPM** control. Edit the memories in **Configure → CW**; the
token `%C` expands to your station callsign. Sending turns the text into a
keyed sidetone at the configured speed, weight and pitch, keys the transmitter
in CWL/CWU automatically, and un-keys it when the message finishes. *(The
encoder is verified by an internal encode-then-decode round-trip; the on-air
signal itself is not yet confirmed — this fork has no transmit hardware.)*

There is also a **software iambic keyer**. Set the keyer mode (Straight /
Mode A / Mode B), speed, weight and paddle reversal in **Configure → CW**. The
two paddles are the **`[`** (dot) and **`]`** (dash) keys — they only act while
the active receiver is in CWL/CWU — or a **MIDI** controller mapped to the
CW-left / CW-right actions (map them with the ONOFF option so both press and
release are sent). Squeezing both paddles gives proper iambic alternation with
dot/dash memory; Mode B adds the extra trailing element. Keying stops (and the
transmitter un-keys, after a short break-in hang) when you release the paddles.
*(The A/B timing is verified by a headless unit test; on-air behaviour with a
real paddle is unverified — no transmit hardware or physical paddle here.)*

**HFDL — aviation HF data link.** HFDL (ARINC 635) is the
short-message data link airliners use over the oceans when they are out of VHF
range: the ground stations broadcast their status, aircraft log on and off, and
ACARS messages — position reports, flight plans, company traffic — travel in
both directions. MacHPSDR decodes it out of the box.

Put the receiver in **DIGU** and choose **HFDL** from the
Decode selector; **Show HFDL** opens a panel (in the second-receiver slot, like
the other decoders), and the Decode block shows the signal level, frame count,
symbol throughput and the last few lines of decoded text. Tune the HFDL channel
the same way as any USB data signal — the decoder expects the 1440 Hz carrier
offset the standard uses, i.e. dial the *assigned* channel frequency. **With CTUN or freetune the
cursor is what selects the channel**, so you can leave the receiver where it is
and just put the cursor on the channel anywhere in the passband; without them the
channel is the centre, as before. One channel is decoded at a time — use **Scan
band** to decode more. You do not have to hit the channel exactly: the decoder
searches about **±600 Hz** around where you point, so clicking on the burst is
enough, and the Decode block shows the channel it settled on. The panel's
**channel drop-down** does that for you: pick, say, *11387 kHz – Riverhead, New
York* and press **Tune**. The list is built from the ground-station table, so it
follows the table received on air.

The panel has three tabs. **Messages** is the running decode; **Stations** lists
every known ground station with how long ago it was heard, how many frames it
sent, its UTC-sync flag and the frequencies it reports in use; **Aircraft** lists
the aircraft seen — ICAO address (once a logon reveals it), flight number, last
heard, and last reported position. **Log** appends every decoded message to
`~/.local/share/machpsdr/hfdl_log.txt`, which is what makes leaving the decoder
running unattended worthwhile; the setting is remembered.

**Scan band** decodes *every* known HFDL channel that falls inside the receiver's
passband at once, not just the one under the dial — an HF band packs about a
dozen channels into 100 kHz, and a 192 kHz receiver already has them all. Each
line is then labelled with the channel it came from. Every extra channel costs
roughly half a percent of a CPU core, and the setting is remembered.

What you get:

- **Squitters** — each ground station's periodic broadcast: station name, UTC
  sync, system-table version and the frequencies it is currently using.
- **Logon / logoff** — an aircraft's **ICAO address** as it joins or leaves a
  station, and the short ID assigned to it. Later messages from that aircraft
  are then shown with the ICAO address filled in automatically.
- **Position, performance and frequency reports** — flight ID, latitude,
  longitude, UTC time, the station and frequency in use, and which frequencies
  the aircraft can hear.
- **ACARS message text** — registration, label, flight ID, message number and
  the message body.

A report that names "frequency slot 1" is shown as the actual kHz. The slot map
comes from the **system table the ground stations broadcast**: the decoder
collects the parts, and once a complete table arrives it replaces the built-in
one and is listed in full (station, position, frequencies). Until then — and for
the station *names*, which the broadcast does not carry — a built-in snapshot is
used. A message **split over several ACARS blocks is reassembled**: each block
reports "Reassembly: in progress" and the block that completes the message prints
the whole text (a lost block is reported instead of being spliced over). An **ARINC-622** application inside the message text is decoded as
well: an **ADS-C** report is shown as position, altitude, time, flight ID,
predicted route, wind and temperature, with its own CRC checked. The two
file-carrying applications are decoded too. **MIAM** (labels MA and H1) covers
both the Single Transfer and the full file-transfer exchange — request, accept,
segments, abort, pause and resume; the segments are reassembled into the whole
file (the request is what announces its size, so a transfer joined late decodes
segment by segment but never completes), and the CORE payload inside is
decompressed and checked against its own ARINC CRC. **OHMA** (label H1) unpacks
its BASE64 + zlib envelope and shows the version, conversation id and payload;
an OHMA conversation split into parts is reassembled **even when the parts
arrive out of order**, which is the one place ACARS block reassembly cannot
help. FANS-1/A **CPDLC** — the controller-pilot conversation — is decoded too,
through a vendored FANS-1/A ASN.1 tree. A position report comes out as latitude,
longitude, time, flight level, the next fixes and their ETAs, wind and
temperature; a clearance or request comes out as the controller's own phrase
with its fields filled in, e.g. "AT [position] CONTACT [icaounitname]
[frequency]" followed by the fix, the facility and the frequency. Direction
matters and is taken from the HFDL frame: the same octets are a clearance as an
uplink and a request as a downlink.

*(Verified on a real off-air recording: on a 11387 kHz capture of the Riverhead,
New York ground station the decoder produced squitters, a ground-station uplink
carrying two logon confirmations with genuine ICAO addresses, aircraft position
and performance reports, and ACARS message text identical to a reference
decoder's output for the same frame.)*

**ACARS on VHF — the same messages, closer in.** Within range of a ground
station airliners use ACARS on VHF (129–137 MHz) instead of HFDL, and it carries
the same messages over a much simpler radio layer: 2400 bps MSK on an AM
carrier. MacHPSDR decodes it with the **same message layer as HFDL**, so the
header, multi-block reassembly, ARINC-622/ADS-C, CPDLC, MIAM and OHMA all come
out of a VHF message exactly as they do out of an HF one.

Put the receiver in **AM** and choose **ACARS** from the Decode selector;
**Show ACARS** opens the panel, and the Decode block shows the signal level, the
message count, how many blocks failed their CRC, the channel being decoded and
the last few lines of text. The decoder takes the raw I/Q, so with **CTUN or
freetune the cursor picks the channel** and the receiver can stay where it is;
the panel's channel drop-down and **Tune** do that for you (131.550 MHz is the
worldwide primary channel, and the list names the region each one belongs to).

**You cannot mistune it.** AM detection is the envelope of the signal, which
does not change when the carrier is off frequency, so anywhere inside the
channel decodes identically — there is no equivalent of HFDL's carrier search
because there is nothing to search for.

**Scan band** decodes every published channel that falls inside the receiver's
passband at once — they are 25 kHz apart, so a wide receiver holds several — and
each line is then labelled with the channel it came from.

Every message line starts with the time it was heard, the signal level and
whether the CRC checked out; a single bad bit is repaired using the CRC, and a
block still failing after that is counted but never printed as though it were a
message. **Log** appends everything to
`~/.local/share/machpsdr/acars_log.txt`. The **Aircraft** tab is the standing
picture next to the scrolling decode: registration, flight, last label, channel,
how long ago it was heard and how many messages it sent.

*(The demodulator and the framing follow `acarsdec`, the reference decoder for
this link, rather than the specification alone. Verified against real off-air
data: all seven messages in acarsdec's own four-channel test recording decode
with correct CRC — registrations, flight numbers and message text — through the
audio path, and again after being AM-remodulated onto a carrier 30 kHz off
centre at 192 kHz and at 2.4 MS/s. A real message also decodes in the running
application from a recording played through the I/Q Player. A live VHF antenna
would additionally cover the receive path at 131 MHz and Scan band on real
multi-channel traffic; that has not been done here.)*

---

## 8. SSTV, WEFAX and APT — images, weather fax and satellites

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
  provided. **Save** asks where to write a PNG; **Clear** resets. **Auto-save** (on
  by default) writes each picture out by itself just before the next
  transmission's VIS wipes it — pictures arrive back to back on a calling
  frequency, and without it keeping one meant sitting at the panel with the
  mouse; **Folder…** picks where (default `~/.local/share/machpsdr/sstv/`), and an
  explicit Clear does not save. The image scrolls and zooms: wheel, **Ctrl+wheel**
  about the pointer, drag to pan, double-click back to fit.
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
- **Contrast** / **Brightness** trim the exposure of a weak or hazy chart. They
  are applied on output, so the whole page re-maps rather than seaming.
- **Save** asks where to write a PNG; **Clear** starts over. **Auto-save page**
  (on by default) writes the page out by itself when the next start tone wipes it
  — taking fax unattended is the normal way to do it — and **Folder…** picks where
  (default `~/.local/share/machpsdr/wefax/`).
- The image scrolls and zooms (wheel / **Ctrl+wheel** / double-click to fit),
  which is what makes the native ~1810 px line readable in a small panel.

Tune the station in **DIGU/USB** ~1.9 kHz below its assigned frequency (so black
lands on 1500 Hz, white on 2300 Hz) and give it a **wide receive filter (~1.9 kHz,
e.g. 1000–2900 Hz)** — too narrow and the fast black↔white transitions smear.

---

### APT — NOAA weather-satellite pictures (receive only)

Pick **APT** from the selector (in **NFM** — the 137 MHz downlink is FM) and press
**Show APT**. Tune the satellite as it rises (NOAA-15 137.620, NOAA-18 137.9125,
NOAA-19 137.100 MHz) and the picture builds itself: the decoder finds the line sync
on its own, holds it through fades, and measures and cancels your receiver's clock
error, so the image does not slant. There is nothing to click.

- **View** shows the whole 2080-word line (both channels, sync bars and telemetry
  wedges) or channel **A** / **B** alone.
- **Slant ±** is a manual trim, there only if you want to nudge the automatic one.
- **Save** asks where to write a PNG, and always writes the **whole line** at full
  resolution whatever **View** is showing; **Clear** starts a new pass.
- **Contrast** / **Brightness** trim the automatic exposure. They are applied where
  the picture is handed to the screen, so moving a slider re-maps the whole image
  rather than leaving a seam at the line you moved it on.
- **Rotate** decides which way up the picture comes out. It is north-up only
  because the satellite was flying south; a northbound pass writes the same scan
  upside down. **North up** asks the orbit which way this pass went and turns the
  picture if it has to — it needs element sets, and leaves the picture alone
  rather than guess without them — while **180°** always turns it. The rotation
  goes with the picture into **Save** and auto-save, and the map turns with it;
  while a rotated pass is still coming in the newest lines arrive at the *top*.
- **Auto-save pass** (on by default) writes the picture to disk by itself when a
  pass ends: a retune, 30 seconds without sync, or the decoder switched off. The
  wipe that starts the next picture is automatic and a pass cannot be repeated, so
  without this an operator who stepped away comes back to an empty panel.
  **Folder…** chooses where (default `~/.local/share/machpsdr/apt/`). An explicit
  **Clear** does *not* save — it means "this one is rubbish, start again".
- **Map** draws the coastline, a 10° graticule and the ground track over the
  picture and reads out the position under the pointer, worked out from the
  satellite's orbit, the time each line arrived and the scan geometry.
  **Update** downloads the current element sets for the three APT satellites
  from celestrak.org and uses them at once; **TLE…** points at a file instead if
  you keep your own (the default is `~/.local/share/machpsdr/tle.txt`). Either
  way the satellite is taken from the frequency being decoded rather than typed
  in again, and the panel shows how old the element set is and says so past a
  week — which is when **Update** is worth pressing.
- **Time trim** is the control that matters. The orbit is good to about a
  kilometre, the clock is not, and one second is about seven kilometres along
  the track — the trim absorbs a stale element set, an unset PC clock and the
  audio-path delay at once, so nudge it until the coast sits on the coast.
- The image **scrolls and zooms**: wheel scrolls back through the pass, **Ctrl+wheel**
  zooms about the pointer up to full resolution, drag pans, double-click returns to
  fit. At the bottom the view keeps following the newest lines; scrolled up, it
  stays put. (The SSTV and WEFAX panels do the same.)
- The panel and the Decode block show **which frequency is being decoded** — the
  decoder follows the cursor, and one aimed elsewhere looks like a dead pass.
- The spectrum and the waterfall both draw the **window the decoder accepts** as
  a translucent band, captioned on the frequency ruler, so you can see the
  satellite fitting inside it: the signal is ~34 kHz wide, the window ~44 kHz.

A **new picture starts by itself** when this is plainly a different transmission:
you retuned the cursor by more than 50 kHz (the channels are 500 kHz apart, so it
cannot be the same satellite — an aim 15 kHz off the middle of the 34 kHz-wide
hump still counts as the same one), or the sync has been gone for over 30 seconds,
which no fade during a pass lasts. Moving from one satellite to the next therefore
gives two pictures instead of one strip with both passes stacked on a shared
exposure. One satellite is decoded at a time — whichever the cursor is on.

APT is the one decoder that does not listen to the demodulated audio: the signal is
~34 kHz wide — wider than the widest NFM filter and far narrower than WFM — so it
takes the raw I/Q and runs its own wideband-FM front-end. Three consequences: the
receive filter changes only what you hear, not the picture; Doppler over a pass needs no tuning at all (the
carrier shift goes out with the DC term, and the ±25 ppm it puts on the line
clock is absorbed by the servo that removes slant); and the receiver's sample rate must be at
least ~48 kHz — a wide DDC (192 kHz or more), or an SDR such as an RTL dongle over
SoapySDR. With CTUN/freetune the decoder follows the **cursor**, so the satellite
may sit anywhere in the visible span.

---

## 9. QO-100 (Es'hail-2)

QO-100 carries the only geostationary amateur transponder, so it is always in the
same place in the sky and never needs tracking. Its narrow-band transponder takes
**2400.000–2400.500 MHz** up and returns **10489.500–10490.000 MHz** down. It does
not invert, and the translation is a constant **8089.500 MHz**. The three beacons
sit at 10489.500 (CW), 10489.750 (400 bd BPSK) and 10490.000 (CW) — the outer two
mark the band edges.

Everything on the **Configure → Bands → QO-100** page exists because of the two
converters on either side of that transponder.

### Two converters, two VFOs

Receive arrives through a 10 GHz LNB (local oscillator typically 9750 MHz) and
transmit leaves through a completely separate 2.4 GHz transverter, so the
satellite needs two entries under **Transverters**. You do not have to type them:
fill in the two local oscillators — **Downlink converter (LNB) LO** (9750 MHz for
a standard universal LNB) and **Uplink converter LO** (leave it at 0 if your radio
reaches 2.4 GHz without a transverter) — and press **Create the two transverter
entries**. Everything else follows from the band plan.

VFO A then follows the receive entry and VFO B the transmit one, because each VFO
takes its converter from its own frequency. Pressing the button again updates the
same two rows rather than using up more slots, and keeps whatever LO error has
been measured, so it is safe to correct an LO figure later.

### Transponder mode

Tune the receiver anywhere on the downlink and press **Set up transponder mode**.
VFO B is put on the matching uplink and the two are linked with the SAT split, so
from then on tuning the receiver moves the transmitter with it. If your own
converters do not translate by exactly the standard amount, adjust
**Transponder offset**.

### Band plan on the spectrum

**Show the transponder band plan on the panadapter** draws the published plan as
tinted segments under the trace, with the beacons marked. The transponder is only
250 kHz wide and CW, digital modes and SSB each have their own part of it, so this
is the quickest way to see that you are about to call in the wrong section.

### Beacon level reference

The transponder is shared, and the rule is that your downlink must not be louder
than the beacon. No absolute figure in dBm can tell you that — it depends on your
dish, LNB and preamp — so **Show the beacon level as a reference line** measures
the beacon on your own display and draws a line at its level. Keep your signal
under it.

### Automatic LNB drift correction

An LNB's oscillator is a free-running device sitting outdoors. It is normally out
by anywhere from a few to some tens of kilohertz, and it moves: a few kHz over the
first half hour as the dish warms up, and again when the sun comes off it. Every
frequency the radio shows you is wrong by that amount.

Switch on **Correct the LNB's drift against a beacon** and the receiver finds the
beacon in the spectrum, compares where it is with where it should be, and trims
the difference out continuously. The correction is stored with the receive band,
so the next session starts already close. Your dial is never retuned, and the
uplink converter — a different box with a different error — is never touched.

Only the two CW beacons can be used as the reference. The middle beacon is BPSK
and has no carrier to measure. The status line shows the current error and how much
correction is being applied; **Re-acquire** starts the search again if you have
moved a long way.

The beacon must be inside the displayed span, and not right on the centre
frequency (where it cannot be told apart from the receiver's own DC spike) — the
status line will say so.

> The correction loop has been verified end to end against synthetic signals,
> including that it does not lock onto noise, but it has not yet been used on the
> real satellite.

---

## 10. I/Q + audio recorder

The **Record** button (SETUP module) captures the active receiver to
`~/.local/share/machpsdr/`:

- `rec_<UTC>_iq.wav` — off-air I/Q at the receiver's sample rate. It is written
  in the same 16-bit stereo format the `--faker` player reads, so a recording can
  be **replayed back through the fake device**.
- `rec_<UTC>_af.wav` — clean demodulated audio at 48 kHz.

Which streams are written and the output folder are set in
**Configure → Audio**.

---

## 11. Configuration

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

**DX cluster (Configure → Network).** Connect to a telnet DX cluster: enter the
host/IP, the port and your login callsign (leave the login blank to use the
station callsign from the FT8 page), then tick **Connect to DX cluster** — a
status line shows the connection state. **Show spots on panadapter** toggles
the overlay. Incoming `DX de …` spots are stored and shown on the RX
panadapter, colour-keyed by DXCC entity, and clicking a spot tunes to it; the
client reconnects automatically if the link drops.

**TCI server (Configure → Network).** MacHPSDR can act as a **TCI** (Expert
Electronics) control server so external loggers and skimmers — Log4OM, N1MM+,
SkookumLogger and others that speak TCI — can set and follow the radio over the
network, without a virtual serial or audio cable. Tick **Enable TCI server**,
optionally change the **port** (default 40001), and point the client software at
`ws://<this-computer>:40001`. The status line shows whether the server is
listening and how many clients are connected. When a client changes the VFO,
mode, PTT, RIT, XIT or split the radio follows, and when you tune, switch mode or
change RIT/XIT/split locally the change is pushed back to every connected client
so they stay in sync. Several
clients may connect at once, and if you run more than one receiver each is
exposed as a separate TCI *trx* that a client can drive and stream independently.
A client may also request the **live I/Q stream**
(`iq_start`): MacHPSDR then sends the off-air I/Q as TCI binary frames, so an
external CW/RTTY skimmer or panadapter can work from this receiver without a
virtual audio cable — run the receiver at 96 or 192 kHz for this, since TCI only
treats a stream above 48 kHz as I/Q. A client may also request the **RX audio
stream** (`audio_start`) to receive the demodulated audio, and stream **TX audio**
back so external digital-mode software can key and modulate the radio over TCI
instead of a virtual audio cable. The audio streams default to 48 kHz but a
client may ask for another rate (MacHPSDR resamples). A TCI client can also send
CW: a `cw_msg` command keys the built-in keyer to transmit the text (speed from
the CW settings, or set with `cw_macros_speed`). *The TX paths (audio and CW)
have not been tested on the air (no transmit hardware).*

---

## 12. MIDI and keyboard control

Any global **action** (tune, mode, filter, AGC, MOX, zoom, …) can be bound to a
keyboard shortcut or a MIDI control via **MIDI learn**. On macOS MIDI uses
CoreMIDI. Mappings are saved to `midi.props`.

---

## 13. Bookmarks

Frequencies of interest can be saved as **bookmarks** and recalled from the
bookmark dialog; bookmarks can also appear as markers on the panadapter.

---

## 14. Testing without hardware (I/Q Player)

The synthetic SDR is offered in the device list as **"I/Q Player"** (always last):
select it and click **Start Radio**. It loops a 16-bit stereo I/Q WAV through the
full RX/decoder chain, or plays a synthetic noise+tones test signal when no file
is set. **Choose the file in Configure → Radio** (the *I/Q Player* frame:
*Choose I/Q File…* / *Synthetic*) — the choice is remembered and can be swapped
**live while it plays**. You can also pass a file on the command line with
`--faker ft8.wav` (this skips the selection dialog) or set `MACHPSDR_FAKE_IQ`; the
command-line file takes precedence, and with no source it falls back to `iq.wav`.
The recording is resampled to the receiver rate and looped, and is played exactly
as recorded — the file centre becomes the receiver centre. When the signal you
want is *not* at the centre of the capture (an SDR usually records around its LO,
not around the station), set **Frequency offset (Hz)** in the same frame to shift
the recording so that signal lands in the middle of the span; the value is
remembered and applies live. `MACHPSDR_FAKE_OFFSET=<Hz>` does the same from the
command line. If the sideband is inverted, tick **Swap I & Q** in the radio dialog
to mirror the spectrum live. This is the recommended way to try FT8 decoding, the
recorder replay, and the UI without a radio.
