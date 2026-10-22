# MacHPSDR browser remote

## Goal

MacHPSDR will contain its own web server and expose a complete browser control
surface: receiver state and controls, live panadapter and waterfall, receive
audio, transmit controls, meters and decoder output.  The browser is a second
front end to the same running radio, not a separate SDR process.

The implementation is deliberately incremental.  Every phase must leave a
usable, testable application and must not make the GTK or RX audio threads wait
for a network client.

Current status (2026-10-22): Phase 0 is complete and Phase 1 is implemented.
The headless protocol, routing and regression suites pass.  A live faker/browser
acceptance run remains to be performed on a normal desktop session; the
repository's isolated macOS test session cannot create the application's first
GTK window, before the radio and web server are started.

## Product scope

The finished remote should provide:

- a responsive desktop/tablet/phone interface;
- one panel per visible receiver, with VFO A/B, mode, filter, step, AGC, gain,
  squelch, mute, split/RIT/XIT and signal meter;
- live panadapter and waterfall with click/drag/scroll tuning;
- low-latency receive audio and, after an explicit safety opt-in, microphone/PTT;
- transmitter drive, tune and status controls on radios that can transmit;
- bookmarks, spots and the active decoder's output;
- reconnect and state resynchronisation without restarting the radio;
- multiple read-only viewers and at most one control lease holder.

VAC and arbitrary access to the host filesystem are out of scope.  The web
remote is not a general web host and serves only a fixed allow-list of bundled
assets.

## Safety and security invariants

1. The server is disabled by default.
2. Phase 1 binds only to `127.0.0.1`.  LAN binding is not enabled until token
   authentication exists.
3. Internet-facing operation is unsupported.  Remote use outside a trusted LAN
   should be placed behind a VPN or a TLS reverse proxy.
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

### Binary frames

Binary payloads use a 24-byte little-endian header:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | `char[4]` | `MPSD` magic |
| 4 | `uint8` | protocol version (`1`) |
| 5 | `uint8` | stream: `1` spectrum, `2` audio, `3` waterfall metadata |
| 6 | `uint8` | receiver index |
| 7 | `uint8` | flags |
| 8 | `uint32` | sequence number |
| 12 | `uint32` | sample/count field |
| 16 | `uint32` | rate or width |
| 20 | `uint32` | payload byte count |

Spectrum payloads are float32 dBm bins.  Audio is initially float32 mono at
48 kHz; the production transport will prefer Opus when a portable, bundled
encoder is available and retain PCM as the compatibility path.

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
- Per-client subscription, width and FPS limits (default 1024 bins / 15 fps).
- WebGL2/canvas panadapter and rolling waterfall.
- Click tune, drag tune, wheel step and filter-edge gestures.
- Drop old frames for a slow browser rather than queueing latency.

Acceptance: the faker trace in GTK and browser has matching peak positions and
dB scale; disconnecting or throttling a browser does not change RX cadence.

### Phase 3 - receive audio

- Subscribe per receiver; bounded queue and resampling where required.
- Web Audio playback with a jitter buffer, mute and volume local to the browser.
- PCM compatibility transport, followed by Opus if the bundle can carry it on
  all supported platforms.

Acceptance: continuous audio for 30 minutes, bounded memory, automatic recovery
after a tab sleeps, and no RX-thread socket writes.

### Phase 4 - complete receive controls

- Filters, step, AGC, AF/RF gain, attenuation, squelch, noise controls, RIT,
  split, sub-receiver and bookmarks.
- Server-pushed state events replace Phase 1 polling.
- Active decoder status and textual output.

Acceptance: two browsers stay consistent with GTK and with each other while
controls are changed from all three surfaces.

### Phase 5 - authenticated LAN operation

- Loopback/LAN selector and generated high-entropy bearer token.
- Pairing flow (QR/manual code), Origin validation and failed-auth throttling.
- Read-only viewers plus a renewable single control lease.
- Optional mDNS advertisement on trusted interfaces.

Acceptance: unauthenticated LAN requests receive no state, a stolen stale lease
cannot transmit, and loopback-only remains the default after upgrade.

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
- Receive-audio latency: 150-300 ms selectable target.
- Server memory: bounded by client count and configured stream buffers.
- Initial UI payload: under 250 KiB compressed; no CDN or runtime dependency.
- Supported browsers: current Safari, Chrome and Firefox; graceful Canvas/PCM
  fallback where WebGL2 or the preferred audio codec is unavailable.

## Test strategy

- Pure parser tests for HTTP, JSON commands and binary headers.
- Socket-pair tests for fragmented WebSocket messages and slow clients.
- Faker integration test: connect, obtain state, tune, change mode and verify
  the authoritative response.
- Deterministic spectrum test using a known faker tone.
- Audio sequence/jitter tests without opening a physical audio device.
- Sanitized `make SANITIZE=1 check` before enabling LAN or microphone/PTT.
