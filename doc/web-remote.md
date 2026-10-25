# MacHPSDR browser remote

## Goal

MacHPSDR will contain its own web server and expose a complete browser control
surface: receiver state and controls, live panadapter and waterfall, receive
audio, transmit controls, meters and decoder output.  The browser is a second
front end to the same running radio, not a separate SDR process.

The implementation is deliberately incremental.  Every phase must leave a
usable, testable application and must not make the GTK or RX audio threads wait
for a network client.

Current status (2026-10-23): Phases 0-4 are complete and live-confirmed on macOS
(control, panadapter/waterfall, WebRTC receive audio and the receive-control
surface), and the first three Phase 5 slices make the listener reachable on every IPv4
interface with optional password protection, built-in HTTPS and a fixed
WebRTC UDP path for port forwarding.  The page's own address is advertised as
that path, without STUN or another external discovery service.  The binary
protocol, routing and regression suites pass.  Phase 6 now has its exclusive
renewable browser-TX lease, heartbeat/disconnect emergency unkey, visible
remote-PTT indication and browser microphone audio over the bidirectional
WebRTC Opus track, plus lease-gated Tune, drive, tune power, mic gain,
compressor, leveler and CESSB controls.  Phase 5 is complete in code
(2026-10-24): a single control lease with takeover, an optional view-only
login, and failed-login throttling; their protocol has been run against a live
faker server over HTTPS/WSS (2026-10-25), the browser-side button states not
yet.  The Windows build compiles them (UCRT64) without WebRTC audio.
Per-slice live acceptance is recorded in the status record.  A headless
`--faker` run serves the protocol for scripted checks; browser and RF checks
happen in the operator's desktop session.

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
3. The embedded listener uses HTTPS/WSS and may be forwarded directly to the
   Internet.  Password protection must remain enabled for a forwarded port.
   WebRTC media is separately encrypted with DTLS-SRTP and does not replace
   control-plane authentication.
4. Browser commands never touch GTK or radio state from a socket thread.  They
   use the existing `ext_*` main-loop dispatch pattern.
5. PTT is separately armed in the desktop application.  A disconnect, expired
   control lease or missing audio heartbeat unkeys the transmitter.
   A headless server (`--headless`, no window) has no checkbox to arm, so it
   stays disarmed unless launched with `--web-tx`, which arms that run only.
6. Network backpressure may drop spectrum/audio frames; it must never block the
   GTK, protocol, DSP or audio threads.
7. HTTP paths are fixed and cannot contain `..`, percent-decoded paths or user
   selected filenames.
8. At most one browser controls the radio (the control lease); the viewer
   login can never change radio state, not even to unkey.  Repeated wrong
   logins from one address are refused without computing the password hash.

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
(`/`, `app.css`, `app.js` and the PWA shell `manifest.webmanifest`, `sw.js`,
`icon-192.png`, `icon-512.png`, each matched whole) and an Upgrade request for
`/ws`.

The service worker is an install shell, never a radio cache: it stores only those
static assets (network-first, refreshed only from a successful answer), never
intercepts an online navigation (so the Basic challenge reaches the browser
untouched), and cannot see the WebSocket or WebRTC traffic.  A page started from
it shows Disconnected until the radio answers; reconnect always re-authenticates
and re-synchronises.  Browsers permit it only in a trusted secure context, so it
is inert on plain HTTP or with the automatic self-signed certificate.

The remote uses HTTP Basic authentication over both asset requests and the
WebSocket Upgrade.  The username defaults to `machpsdr` and is configurable; the
operator chooses a password of at least eight characters.  Only a random salt and a
PBKDF2-HMAC-SHA256 result (100,000 rounds) are persisted.  Authentication can be
disabled, in which case the server sends no challenge.  The listener itself is
TLS 1.2+; it creates a persistent self-signed certificate and private key on
first use, or loads a user-selected PEM bundle containing the certificate chain
and matching private key.  The UI exposes its SHA-256 fingerprint.  WebSocket
Origin must match the request Host (HTTPS), so another web site cannot
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

### Headless server

`--headless` runs the radio as this server alone.  The GTK widget tree is still
built (the application is written around it) but the window is never shown, so
nothing is mapped or rendered.  Because no panadapter allocation ever arrives,
`receiver_headless_geometry()` gives each receiver a fixed 2048-pixel analyzer
(one source bin per Ultra bin); the fps timer still fetches pixels, keeps the
auto dB scale (`update_rx_panadapter_headless()`) and publishes the spectrum,
but skips the panadapter, waterfall and S-meter rendering.  No local audio
output is opened or written; the saved `local_audio` choice is left untouched
for the next windowed run.  The server starts whatever `radio.web_enable` says
and does not write it.  The device is `--open`, `--faker` or the first real one
discovered; a miss exits instead of showing the selection window.  SIGINT and
SIGTERM run the normal save-and-stop path.  A display connection is still
required (GTK cannot initialise without one).

### Transport classes and bandwidth adaptation

Control/state and live media have different delivery rules:

- Commands, acknowledgements, state and authentication use the reliable
  WebSocket control channel.  They are small and must arrive in order.
- The Internet-grade media path is WebRTC: demodulated mono audio as an Opus RTP
  track, and spectrum rows on an unordered, limited-retransmission DataChannel.
  The spectrum channel uses `ordered: false` and a short
  `maxPacketLifeTime` rather than retransmitting a row after it is useful.
  The existing WebSocket remains the signaling and control path.  The selected
  native implementation is libdatachannel with libopus.  It uses one
  operator-configurable UDP-mux port for all server peer connections (8081 by
  default).  The server resolves the Host used for the accepted WebSocket
  and advertises that page address with the UDP port as an additional host
  candidate.  Forward that UDP port alongside the HTTPS TCP port for direct
  Internet use.  No STUN, TURN or other external service participates.
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
| Ultra | 2048 bins × 30 fps | about 492 kbit/s | 64 kbit/s mono | at most 700 kbit/s |

Ultra is the smooth/high-resolution tier for a LAN or an unconstrained
connection; it is never selected automatically by backpressure recovery (a
client reaches it only by subscribing to it) and degrades through High on
congestion like any other profile.  The source bins are the desktop
panadapter's own display pixels, so above the panadapter's width Ultra carries
no more real resolution than High — it is bandwidth spent on update rate and on
filling a wide browser window one-to-one.

Payload estimates are the uncompressed eight-bit rows and deliberately do not
depend on noise being compressible.  Protocol overhead fits inside the total
budget.  Opus uses 20 ms packets and constrained VBR.  DTX is off by default
because receiver noise and weak signals are meaningful; in-band FEC is useful on
the lossy WebRTC path and is not added to the reliable WebSocket fallback.

## Protocol version 1

The WebSocket subprotocol is `machpsdr.v1`.  Text frames are UTF-8 JSON; bulk
streams use binary frames.  Every connection starts with:

```json
{"type":"hello","protocol":1,"server":"MacHPSDR","capabilities":["control","spectrum","audio","tx_lease"],"audio_udp_port":8081}
```

`audio_udp_port` is the server's fixed WebRTC UDP-mux port.  The address is the
Host of this accepted WebSocket connection; the server resolves it and
advertises it as an ICE host candidate, so no external discovery is required.
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

`hello` carries `"role":"control"` or `"role":"view"` and the `control_lease`
capability.  A control connection sends `{"type":"control_acquire","id":4}`
(optionally `"force":true` to take control from its holder), renews with
`control_renew` at least every five seconds and relinquishes with
`control_release`; the server answers `{"type":"control_state","id":4,
"held":true,"timeout_ms":5000}` and sends an unsolicited `held:false` (id 0)
to a holder that loses control by takeover or expiry.  Without the lease a
radio-changing command fails with `control_required`, an acquire while held
with `control_busy`, and any non-receive request from a viewer with
`view_only`.  State reports `control_active` while someone holds control.

Remote TX has a connection-bound renewable lease.  A browser sends
`{"type":"lease_acquire","id":3}`, renews it at least every five seconds with
`lease_renew`, and may relinquish it with `lease_release`.  The server answers
with `{"type":"lease_state","id":3,"held":true,"timeout_ms":5000}`.  Only the
control holder may acquire it, and losing control drops it.  Only the TX holder
may set MOX true; any control-role connection may request MOX false as an
emergency unkey (a viewer may not).  Disconnect, expiry or withdrawal of the session-only desktop PTT arm
revokes the lease and dispatches an unkey through the GTK main loop.  State
reports `tx_lease_active` to all viewers and `remote_ptt` while browser MOX is
active.  `tx_media_active` reports that fresh browser microphone media is
arriving.  Lease ownership is never recoverable across a WebSocket reconnect.
After acquiring the lease the page requests microphone permission, adds its
mono audio track to the existing WebRTC session and shows a local input meter.
The server decodes incoming Opus on the libdatachannel worker, copies 48 kHz
samples into a bounded 250 ms ring and lets the transmitter audio thread pop
them at its existing microphone substitution point.  MOX-on is rejected until
fresh microphone media has arrived.  A 500 ms media gap while remote PTT is
active revokes the lease and emergency-unkeys; permission failure releases the
lease and leaves the page receive-only.  Codec and socket work never runs on
the GTK or transmitter audio threads.

Besides `set_frequency`, `set_mode`, `set_mox` and `set_filter`, protocol v1
carries an allow-listed family of scalar per-receiver controls, each a `command`
with an integer `value` and a receiver `rx`: `set_step` (Hz), `set_agc`
(0 Off … 4 Fast), `set_af_gain` and `set_squelch` (0-100), and `set_agc_gain`
(dB).  The server clamps each to the matching desktop control's own range on the
GTK thread, and reports the result in `state` as `step`, `agc`, `af_gain`,
`squelch` and `agc_gain`.  Unknown actions are rejected, not ignored.

The completed Phase 4 control family also includes `set_rf_gain` (the
device-reported Soapy gain range or the existing HPSDR attenuation/LNA range),
`set_subrx`, `set_subrx_mix` (0 hard left/right split through 100 mono blend)
and `recall_bookmark` (zero-based index from `radio.bookmarks`).  Receiver state
reports `rf_gain`, `rf_min`, `rf_max`, `rf_kind`, `subrx` and `subrx_mix`.
Radio state reports the saved bookmark names/frequencies and the active
decoder's `decoder_title`/plain-text `decoder_text`; these are pushed through
the same state-change detector as the other controls.

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
profile (`0` Low, `1` Balanced, `2` High, `3` Ultra); the remaining flag bits
are zero in protocol v1.  `count` is at most 2048 (the Ultra bin cap).

The WebSocket implementation treats three consecutive replacement,
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
- Per-client Low/Balanced/High/Ultra subscription; default 512 bins / 10 fps.
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

Implemented (2026-10-25): per-receiver recording in the browser.  A record
chip beside Mute records the received WebRTC stream with `MediaRecorder`
(Opus in WebM/Ogg, or AAC in MP4 on Safari) and saves the file on the viewing
device when stopped; nothing is sent to the radio, so a viewer may record.  The
recorder is fed through a Web Audio destination re-wired to each new session,
so a session rebuild (microphone, reconnect) is a gap in one file, not a second
file; Mute and AF gain do not change the recording.

### Phase 4 - complete receive controls

- Filters, step, AGC, AF/RF gain, attenuation, squelch, noise controls, RIT,
  split, sub-receiver and bookmarks.
- Server-pushed state events replace Phase 1 polling.
- Active decoder status and textual output.

Acceptance: a browser stays consistent with GTK while controls are changed from
either surface, and the broadcast regression proves the same authoritative
state is sent to every connected browser.

Implemented (2026-10-23, awaiting operator live acceptance; see the status
record): server-pushed state
replaces the 500 ms poll (a GTK detector broadcasts a full `state` frame on any
discrete change and refreshes the S-meter at 5 Hz; the browser keeps `get_state`
for the connect snapshot and a quiet-link fallback), and the first receive
controls — tuning step, AGC mode, AF/RF gain or attenuation, squelch and AGC
gain — ship as
allow-listed `command` actions whose read-back fields are in `state`, so a change
from any surface echoes to every browser. Filter presets, the noise controls
(NB/NR/ANF/SNB under the DIGU/DIGL and decoder bypass rule), a band dropdown,
per-digit VFO tuning (wheel over a digit, hover and type 0-9, as on the desktop),
RIT/split, sub-receiver enable/crossfeed, saved-bookmark recall and the active
decoder's textual readout are included.

### Phase 5 - authenticated LAN/Internet operation

- LAN listener with optional persisted password authentication.
- Failed-login throttling.
- Read-only viewers plus a renewable single control lease.
- Optional mDNS advertisement on trusted interfaces.
- HTTPS/WSS termination plus a fixed-port WebRTC ICE path that uses the page's
  own address and needs no third-party traversal service.

Implemented so far (2026-10-23): the TLS 1.2+ listener binds all IPv4 interfaces;
optional HTTP Basic authentication covers assets and WebSocket Upgrade, persists
only a salted PBKDF2 hash, and enforces same-origin WebSocket connections.  It
generates a self-signed certificate or loads a custom PEM bundle.  With
protection disabled the service is intentionally open.  Receive-audio ICE uses
one configurable UDP-mux port for every client and advertises the address from
which that client opened the page.  A direct Internet setup forwards both the
HTTPS TCP port and that WebRTC UDP port to the server; no STUN/TURN service is
used.

Implemented (2026-10-24): roles and throttling.  A connection's role comes from
the login it used: the main login is `control`, an optional second **viewer
login** (`web_view_user`/salt/hash, same PBKDF2 storage, username distinct from
the main one, in force only while protection is on) is `view`; with protection
off every connection is `control`.  Among control connections one holds the
**control lease** (`control_acquire`/`control_renew`/`control_release`, the same
connection-bound 5 s heartbeat as the TX lease); the page takes it on connect
when it is free, and `control_acquire` with `"force":true` takes it over.  Every
radio-changing request needs the lease; the TX lease can only be acquired by
the control holder, and losing control (release, takeover, expiry, disconnect)
drops the TX lease and so unkeys.  A viewer may only read state, subscribe to
spectrum and take receive audio.  The one access policy is
`web_request_access()`, checked once at the top of dispatch; a request type it
does not name needs the lease.  Failed logins are throttled per IPv4 source:
five wrong Authorization headers lock the address for 30 s, each further one
doubles the lock up to 15 min, a correct login or 15 quiet minutes clears it.
A locked address gets `429 Too Many Requests` with `Retry-After` before any hash
is computed.  A missing header is the browser's first try and is not a failure.
The table holds 64 addresses; when all are locked, the lockout ending soonest is
reused, so a flood of sources can shorten a lockout but never lock a fresh
address out.  Token/QR pairing was dropped by operator decision and is not
planned.

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

Implemented so far (2026-10-23): after the desktop session-only PTT arm, one
browser may take an exclusive TX lease.  It renews every two seconds and expires
after five; disconnect, expiry, explicit release or withdrawing the arm revokes
it and emergency-unkeys.  Every viewer receives the lease/remote-PTT state and
shows a visible `REMOTE PTT` indicator.  Once it owns the lease, the browser
requests microphone permission, displays a local level meter and sends mono
Opus on the bidirectional WebRTC audio track.  MOX requires fresh media; a
500 ms media gap during remote MOX revokes the lease and emergency-unkeys.  The
same lease exposes Tune, drive, tune power, microphone gain, compressor
gain/on-off, leveler and CESSB.  Tune does not require microphone media; Tune
off and MOX off remain emergency actions, and lease loss drops both.  The
PWA/accessibility pass is implemented too: an installable manifest and shell
service worker, a visible keyboard-focus ring on every control, arrow-key tuning
on the spectrum and RIT/XIT, names for the unlabelled selects, no live-region
churn, reduced-motion, finger-sized targets on touch and safe-area insets.

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
