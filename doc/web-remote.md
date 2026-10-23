# MacHPSDR browser remote

## Goal

MacHPSDR will contain its own web server and expose a complete browser control
surface: receiver state and controls, live panadapter and waterfall, receive
audio, transmit controls, meters and decoder output.  The browser is a second
front end to the same running radio, not a separate SDR process.

The implementation is deliberately incremental.  Every phase must leave a
usable, testable application and must not make the GTK or RX audio threads wait
for a network client.

Current status (2026-10-23): Phases 0-3 are complete and live-confirmed on macOS
(control, panadapter/waterfall and WebRTC receive audio).  Phase 4 is in progress,
and the first Phase 5 slice now makes the listener reachable on every IPv4
interface with optional password protection.  The binary protocol, routing and
regression suites pass.  Per-slice live acceptance is recorded in the status
record; the repository's isolated macOS test session cannot create the
application's first GTK window, so live runs happen in a normal desktop session.

The continuously updated engineering handoff, verification record and exact
next task are in [web-remote-status.md](web-remote-status.md).

## Documentation contract

This file is the normative product, safety, architecture and wire-protocol
specification.  `web-remote-status.md` is the factual implementation record.
README.md describes only functionality available to an operator now.

Any change to browser-remote behaviour, protocol, architecture or delivery
phase must update this specification and the status record in the same commit.
The record must include the commands actually run, distinguish headless proof
from desktop/on-air verification, preserve unresolved limitations, and leave
one concrete next step for the next developer or coding agent.

Every operator-visible change must also update all supported user manuals in
the same commit: `manual-en.md`, `manual-ru.md`, `manual-uk.md` and
`manual-be.md`.  This includes controls, defaults, limitations, security/PTT
steps and troubleshooting.  A delivery phase is not complete until all four
manuals describe the same shipped behaviour.

## Product scope

The finished remote should provide:

- a responsive desktop/tablet/phone interface;
- one panel per visible receiver, with VFO A/B, mode, filter, step, AGC, gain,
  squelch, mute, split/RIT/XIT and signal meter;
- live panadapter and waterfall with click/drag/scroll tuning;
- low-latency receive audio and, after an explicit safety opt-in, microphone/PTT;
- adaptive low-bandwidth operation over an ordinary Internet connection without
  forwarding receiver I/Q or uncompressed display-rate data;
- transmitter drive, tune and status controls on radios that can transmit;
- bookmarks, spots and the active decoder's output;
- reconnect and state resynchronisation without restarting the radio;
- multiple read-only viewers and at most one control lease holder.

VAC and arbitrary access to the host filesystem are out of scope.  The web
remote is not a general web host and serves only a fixed allow-list of bundled
assets.

## Safety and security invariants

1. The server is disabled by default.
2. When enabled, the server listens on every IPv4 interface.  Password protection
   is operator-selectable and recommended; disabling it deliberately gives every
   host that can reach the port the same browser-control access.  The GTK status
   must make protected versus open operation visible.
3. Directly exposing the embedded HTTP listener to the Internet is unsupported.
   Internet use goes through a VPN or an authenticated HTTPS/WSS entry point;
   WebRTC media is encrypted with DTLS-SRTP and does not replace control-plane
   authentication.
4. Browser commands never touch GTK or radio state from a socket thread.  They
   use the existing `ext_*` main-loop dispatch pattern.
5. PTT is separately armed in the desktop application.  A disconnect, expired
   control lease or missing audio heartbeat unkeys the transmitter.
6. Network backpressure may drop spectrum/audio frames; it must never block the
   GTK, protocol, DSP or audio threads.
7. HTTP paths are fixed and cannot contain `..`, percent-decoded paths or user
   selected filenames.

## Architecture

### Components

- `web_server.c/.h`: listener, HTTP routing, WebSocket clients, lifecycle and
  radio-command dispatch.
- `web_protocol.c/.h`: protocol parsing/formatting that can be tested without a
  radio or GTK.  It owns protocol version 1 and the binary-frame headers.
- `web_dialog.c/.h`: Configure -> Network controls and status.
- `assets/web/`: dependency-free HTML/CSS/JavaScript application.  Assets are
  loaded from the repository during development and copied into the app bundle.
- Existing `tci_ws.c`: RFC 6455 framing.  Its framing primitives may be shared;
  TCI commands and browser commands remain separate protocols.

The first implementation uses the project's existing socket and GLib support,
not an additional HTTP library.  This keeps the macOS bundle and the Windows
cross-build self-contained.  The server accepts only GET/HEAD for known assets
and an Upgrade request for `/ws`.

The LAN slice uses HTTP Basic authentication over both asset requests and the
WebSocket Upgrade.  The username is fixed as `machpsdr`; the operator chooses a
password of at least eight characters.  Only a random salt and a
PBKDF2-HMAC-SHA256 result (100,000 rounds) are persisted.  Authentication can be
disabled, in which case the server sends no challenge.  Basic authentication is
not transport encryption: use it only on a trusted LAN or through a VPN/HTTPS
reverse proxy, and never forward the plain HTTP port to the Internet.  WebSocket
Origin must match the request Host (HTTP or HTTPS), so another web site cannot
reuse cached browser credentials to open the control socket.

### Threads and ownership

- One accept thread owns the listening socket.
- Each connected browser has a socket worker and a serialised send path.
- Radio mutations are heap-allocated commands passed to `g_idle_add()` and are
  revalidated on the GTK thread, exactly like TCI and rigctl.
- The GTK display timer publishes an immutable spectrum snapshot after
  `GetPixels()` succeeds.  A web sender reads the latest snapshot; it never
  calls WDSP itself.
- The RX audio thread feeds a bounded single-producer queue.  Encoding and
  socket writes happen elsewhere.

### Transport classes and bandwidth adaptation

Control/state and live media have different delivery rules:

- Commands, acknowledgements, state and authentication use the reliable
  WebSocket control channel.  They are small and must arrive in order.
- The Internet-grade media path is WebRTC: demodulated mono audio as an Opus RTP
  track, and spectrum rows on an unordered, limited-retransmission DataChannel.
  The spectrum channel uses `ordered: false` and a short
  `maxPacketLifeTime` rather than retransmitting a row after it is useful.
  The existing WebSocket remains signaling plus a localhost/LAN compatibility
  fallback.  A cross-platform dependency/packaging spike is required before a
  particular native WebRTC implementation is selected.
- The fallback may carry timestamped Opus packets in binary WebSocket frames.
  PCM exists only as a diagnostic/local compatibility mode, never the default
  WAN profile.  TCP head-of-line blocking means this fallback cannot claim the
  same loss behaviour as WebRTC.
- The server never transmits I/Q for the browser display.  It downsamples the
  already-fetched display bins to the subscribed width, quantizes each dB bin to
  one byte, and sends one spectrum row.  The browser draws both the panadapter
  and the next waterfall line from that same row; there is no waterfall video
  or second bulk stream.
- Audio has priority over spectrum.  On congestion, discard expired spectrum,
  then reduce spectrum FPS, then width.  Reduce Opus bitrate only after display
  traffic reaches its low profile.  Control/state is never discarded.
- Every media unit has a sequence number and monotonic capture timestamp.  A
  spectrum sender/client keeps the newest row only.  Audio uses a bounded
  adaptive jitter buffer; sequence gaps invoke Opus FEC/packet-loss concealment
  on WebRTC and never cause an unbounded wait.

Initial per-receiver profiles are budgets, not promises to fill the link:

| Profile | Spectrum | Spectrum payload | Opus target | Total target |
| --- | --- | ---: | ---: | ---: |
| Low | 256 bins × 5 fps | about 10 kbit/s | 16 kbit/s mono | at most 40 kbit/s |
| Balanced | 512 bins × 10 fps | about 41 kbit/s | 24 kbit/s mono | at most 80 kbit/s |
| High | 1024 bins × 15 fps | about 123 kbit/s | 48 kbit/s mono | at most 200 kbit/s |

Payload estimates are the uncompressed eight-bit rows and deliberately do not
depend on noise being compressible.  Protocol overhead fits inside the total
budget.  Opus uses 20 ms packets and constrained VBR.  DTX is off by default
because receiver noise and weak signals are meaningful; in-band FEC is useful on
the lossy WebRTC path and is not added to the reliable WebSocket fallback.

## Protocol version 1

The WebSocket subprotocol is `machpsdr.v1`.  Text frames are UTF-8 JSON; bulk
streams use binary frames.  Every connection starts with:

```json
{"type":"hello","protocol":1,"server":"MacHPSDR","capabilities":["control"]}
```

The client then requests a complete snapshot:

```json
{"type":"get_state","id":1}
```

The response contains a monotonically increasing revision, radio state and all
visible receivers.  Phase 1 exposes frequency, mode, meter, lock and MOX.  New
fields are additive; a v1 client must ignore unknown fields.

Commands have an id and receive either an acknowledgement or an error:

```json
{"type":"command","id":2,"action":"set_frequency","rx":0,"value":14074000}
{"type":"ack","id":2}
{"type":"error","id":2,"code":"invalid_value","message":"frequency is out of range"}
```

State changes are authoritative only when echoed by a later `state` or `event`
message.  The browser may render an optimistic value, but must replace it with
the server value.

Besides `set_frequency`, `set_mode`, `set_mox` and `set_filter`, protocol v1
carries an allow-listed family of scalar per-receiver controls, each a `command`
with an integer `value` and a receiver `rx`: `set_step` (Hz), `set_agc`
(0 Off … 4 Fast), `set_af_gain` and `set_squelch` (0-100), and `set_agc_gain`
(dB).  The server clamps each to the matching desktop control's own range on the
GTK thread, and reports the result in `state` as `step`, `agc`, `af_gain`,
`squelch` and `agc_gain`.  Unknown actions are rejected, not ignored.

A spectrum subscription is explicit and per receiver.  Profile names are
case-insensitive on input; acknowledgements and status use the canonical names.
Balanced is the browser default:

```json
{"type":"spectrum_subscribe","id":3,"rx":0,"profile":"Balanced"}
{"type":"spectrum_subscription","id":3,"rx":0,"enabled":true,"profile":"Balanced","bins":512,"fps":10,"dropped":0}
{"type":"spectrum_unsubscribe","id":4,"rx":0}
```

`spectrum_status` reports the current effective profile, bin and FPS caps and
the cumulative per-connection replacement/expiry drop count.  During
backpressure the caps may temporarily be below the selected profile.

### Binary frames

Binary DataChannel/WebSocket fallback payloads use a 32-byte little-endian
header.  It is encoded field-by-field; a compiler-dependent C struct is never
placed directly on the wire:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | `char[4]` | `MPSD` magic |
| 4 | `uint8` | protocol version (`1`) |
| 5 | `uint8` | stream: `1` spectrum, `2` fallback audio; other values reserved |
| 6 | `uint8` | receiver index |
| 7 | `uint8` | flags |
| 8 | `uint32` | sequence number |
| 12 | `uint64` | monotonic capture timestamp, microseconds |
| 20 | `uint32` | sample/bin count |
| 24 | `uint32` | sample rate or spectrum width |
| 28 | `uint32` | payload byte count |

Spectrum stream `1` begins with signed `int16 floor_db_x10` and unsigned
`uint16 span_db_x10`, followed by exactly `count` unsigned eight-bit bins.  A
bin reconstructs as `floor + value / 255 * span`.  Rows older than 500 ms may be
dropped by the sender or client and are never retransmitted merely to complete a
waterfall history.  `payload byte count` is therefore `4 + count`, and
`rate_or_width` equals `count`.  For spectrum, flag bits 0-1 carry the effective
profile (`0` Low, `1` Balanced, `2` High); the remaining flag bits are zero in
protocol v1.

The localhost WebSocket implementation treats three consecutive replacement,
expiry or over-100-ms send-pressure observations as congestion.  It first
reduces FPS to the next profile's rate, then reduces width; a quiet ten-second
interval advances recovery toward the requested profile.  Socket sends have a
250-ms timeout, and shutdown wakes and joins every sender before freeing its
single-row slots.  These constants define the current fallback behaviour, not
the future WebRTC congestion controller.

Audio stream `2` is the compatibility fallback.  Its codec and rate are agreed
by the subscription acknowledgement; production WAN audio is Opus, mono,
48 kHz clock, normally one 20 ms frame per payload.  The WebRTC RTP audio track
uses standard Opus framing rather than this private binary header.

## Delivery phases

### Phase 0 - specification and seams

- Record this contract.
- Reuse/test the WebSocket handshake primitive without coupling browser
  commands to TCI.
- Add a headless protocol/HTTP test target.

Acceptance: protocol parser and fixed-path HTTP router reject malformed,
oversized and traversal requests.

### Phase 1 - local control vertical slice

- Disabled-by-default localhost listener, configurable port.
- Serve the bundled responsive application.
- WebSocket hello/state exchange.
- Display receiver frequency, mode, S-meter and RX/TX state.
- Set VFO A and mode; toggle MOX only when the radio can transmit.
- Show connection/reconnect status in both GTK and browser.

Acceptance: with `--faker`, a browser can tune and change mode; GTK follows,
refreshing the page reconstructs state, malformed commands do not change it,
and `make check` passes.

### Phase 2 - panadapter and waterfall

- Publish the already-fetched WDSP bins from the display timer.
- Per-client Low/Balanced/High subscription; default 512 bins / 10 fps.
- Peak-preserving downsampling and eight-bit dB quantization; do not send I/Q,
  float32 bins or a separate waterfall image.
- WebGL2/canvas panadapter and rolling waterfall.
- Click tune, drag tune, wheel step and filter-edge gestures.
- Latest-only delivery, timestamps and automatic FPS/width downgrade under
  backpressure; old rows are dropped rather than queued or retransmitted.

Acceptance: the faker trace in GTK and browser has matching peak positions and
dB scale; disconnecting or throttling a browser does not change RX cadence.

### Phase 3 - receive audio

- Subscribe per receiver; bounded queue and resampling where required.
- Run a macOS/Linux/Windows packaging spike for the native WebRTC/Opus
  dependency before committing it to the bundle.
- WebRTC Opus mono track, 20 ms packets, adaptive bitrate and in-band FEC from
  measured loss; timestamped Opus-over-WebSocket fallback for local use.
- Adaptive 150-600 ms receive jitter target, packet-loss concealment, mute and
  volume local to the browser.  PCM is diagnostic/local only.

Acceptance: continuous audio for 30 minutes, bounded memory, automatic recovery
after a tab sleeps, no RX-thread socket writes, and the shaped-WAN target below
does not accumulate latency.

Implemented (2026-10-23, see the status record): libdatachannel + libopus
WebRTC Opus mono track, 20 ms packets, 32 kbps with in-band FEC; a bounded
200 ms per-session ring fed from the RX thread (no socket writes there) and
drained by a per-session encoder worker; auto-start per receiver with a local
mute and a one-tap autoplay unlock in the browser; the browser's own jitter
buffer and Opus PLC serve the 150-600 ms target.  Deliberately NOT built: the
Opus-over-WebSocket PCM/local fallback — WebRTC is the single audio transport
(see "Decisions" in the status record).  The operator confirmed the browser
audio live on 2026-10-23 (faker, macOS); the finer acceptance checks were waived
as in Phase 2.

### Phase 4 - complete receive controls

- Filters, step, AGC, AF/RF gain, attenuation, squelch, noise controls, RIT,
  split, sub-receiver and bookmarks.
- Server-pushed state events replace Phase 1 polling.
- Active decoder status and textual output.

Acceptance: two browsers stay consistent with GTK and with each other while
controls are changed from all three surfaces.

Implemented so far (2026-10-23, see the status record): server-pushed state
replaces the 500 ms poll (a GTK detector broadcasts a full `state` frame on any
discrete change and refreshes the S-meter at 5 Hz; the browser keeps `get_state`
for the connect snapshot and a quiet-link fallback), and the first receive
controls — tuning step, AGC mode, AF gain, squelch and AGC gain — ship as
allow-listed `command` actions whose read-back fields are in `state`, so a change
from any surface echoes to every browser. Still to come in this phase: filter
presets, RF gain/attenuation, the noise controls (NB/NR/ANF/SNB under the
DIGU/DIGL and decoder bypass rule), band up/down, RIT/split, sub-receiver,
bookmarks and decoder status/output.

### Phase 5 - authenticated LAN/Internet operation

- LAN listener with optional persisted password authentication.
- Pairing flow (QR/manual code) and failed-auth throttling.
- Read-only viewers plus a renewable single control lease.
- Optional mDNS advertisement on trusted interfaces.
- HTTPS/WSS termination or documented VPN deployment, plus WebRTC ICE/STUN and
  optional TURN for networks where direct media cannot connect.

Implemented so far (2026-10-23): the listener binds all IPv4 interfaces; optional
HTTP Basic authentication covers assets and WebSocket Upgrade, persists only a
salted PBKDF2 hash, and enforces same-origin WebSocket connections.  With
protection disabled the LAN service is intentionally open.  Pairing, throttling,
viewer roles, a control lease and Internet traversal remain future work.

Acceptance: with protection enabled, unauthenticated LAN requests receive no
state; a stolen stale lease cannot transmit; open mode is unambiguously reported
to the operator.

### Phase 6 - transmit audio and operational polish

- Explicit desktop-side remote-TX arm, control lease and visible remote-PTT
  indicator.
- Browser microphone permission, level meter and TX audio stream.
- Tune/drive/TX processing controls, emergency unkey and watchdog.
- Installable PWA shell, mobile layout and accessibility pass.

Acceptance: closing/crashing the controlling tab always unkeys; microphone
failure falls back to RX-only; on-air validation is recorded separately because
there is no TX hardware on the development machine.

## Non-functional targets

- Control round-trip on a LAN: under 100 ms at p95.
- Spectrum latency: under 250 ms at p95, with no unbounded queue.
- WAN Balanced profile: at most 80 kbit/s per receiver before IP/crypto
  overhead; Low profile at most 40 kbit/s.
- Receive-audio jitter target: adaptive 150-600 ms, with clock-drift correction
  and no growing buffer.
- Under a shaped 300 kbit/s path with 150 ms RTT, 50 ms jitter and 2% random
  media loss, spectrum age stays below 500 ms and audio remains continuous via
  FEC/PLC; exact audible acceptance is recorded after the transport spike.
- Server memory: bounded by client count and configured stream buffers.
- Initial UI payload: under 250 KiB compressed; no CDN or runtime dependency.
- Supported browsers: current Safari, Chrome and Firefox; Canvas and
  Opus-over-WebSocket local fallbacks where the preferred WebRTC path is
  unavailable.  PCM is diagnostic rather than an Internet fallback.

## Test strategy

- Pure parser tests for HTTP, JSON commands and binary headers.
- Socket-pair tests for fragmented WebSocket messages and slow clients.
- Faker integration test: connect, obtain state, tune, change mode and verify
  the authoritative response.
- Deterministic spectrum test using a known faker tone.
- Audio sequence/jitter tests without opening a physical audio device.
- Deterministic impairment tests for bandwidth, delay, jitter, packet loss,
  backpressure, adaptation hysteresis and recovery after a two-second outage.
- Sanitized `make SANITIZE=1 check` before enabling LAN or microphone/PTT.

## Standards rationale

- [RFC 6716](https://www.rfc-editor.org/rfc/rfc6716.html) defines Opus, 20 ms as
  the usual real-time trade-off, constrained VBR, FEC and packet-loss concealment.
- [RFC 7587](https://www.rfc-editor.org/rfc/rfc7587.html) defines Opus over RTP,
  including dynamic bitrate, DTX and in-band FEC negotiation.
- [RFC 7874](https://www.rfc-editor.org/rfc/rfc7874.html) requires WebRTC audio
  endpoints to implement Opus, avoiding a browser-specific decoder requirement.
- The [WebRTC Recommendation](https://www.w3.org/TR/webrtc/) defines unordered
  DataChannels and limited retransmission through `maxRetransmits` or
  `maxPacketLifeTime`, which match disposable spectrum rows.
