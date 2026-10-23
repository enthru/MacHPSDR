# Browser remote implementation record

This is the durable handoff for the MacHPSDR browser remote.  It is written for
humans and coding agents, including Claude and Codex.  Read it together with
[web-remote.md](web-remote.md) before changing this subsystem.

## Sources of truth

- `web-remote.md` is normative: product scope, safety invariants, architecture,
  protocol and acceptance criteria.
- This file records what exists, what was actually verified, known limitations,
  decisions already made and the exact next implementation slice.
- `README.md` is operator-facing and must describe only functionality that is
  currently available.
- Code and tests win when this record is accidentally stale; fix the record in
  the same commit instead of silently carrying the discrepancy forward.

Do not create a second roadmap or private-only implementation history.  Append
material decisions and verification results here, while keeping the current
handoff sections accurate.

## Current handoff

Baseline before the current work: `7072382` (`Add localhost browser remote
foundation`), pushed to `origin/master` on 2026-10-22.

- Phase 0: complete.
- Phase 1: implemented, headlessly verified and confirmed live.
- Phase 2: implemented, headlessly verified and confirmed live (operator ran the
  faker on macOS 2026-10-22/23; the receiver card, panadapter and waterfall
  render and tune in the browser).  The quantitative measurements in "Exact next
  slice" were waived by the operator rather than recorded.
- Phase 3 (receive audio over WebRTC) is COMPLETE and confirmed live.  Done, all
  on `origin/master`: slice 1
  (libdatachannel build integration behind an auto-enabling `WEBRTC` make flag),
  slice 2a (the server `WEB_RTC_SESSION` offer/Opus-track + the `webrtc_offline`
  loopback-connect harness), slice 2b (signaling over the WebSocket and the
  browser `RTCPeerConnection`; live connect confirmed by the operator
  2026-10-23), and slice 3 (the Opus audio feed itself + the auto-start/mute
  browser UX).  Slice 3 is proven natively: `webrtc_offline --selftest` also
  feeds real-time audio into the server session and pulls RTP off the answerer's
  track — 75 packets, PT 111, 0 stray, 50.0/s over 1.50 s, zero loss.  The
  operator confirmed on 2026-10-23 that the browser audio works live (faker on
  macOS); the finer checks (sustained duration, mute/unmute, tab sleep/resume)
  were not separately reported, as in Phase 2.
- Decisions realised in slice 3 (were "binding", now shipped):
  - There is no local/PCM audio mode.  WebRTC is the single audio transport; the
    WebSocket stays the signaling/control plane.
  - No explicit "Listen" button.  Audio auto-starts when a receiver card appears
    (`audio_subscribe` from `makeCard`/`updateAudioAvailability`); the
    per-receiver button is mute/unmute (local `<audio>.muted`).  Browsers block
    autoplay without a gesture, so a blocked first `play()` raises a one-tap
    "Tap to enable sound" affordance, then never again (`audioUnlocked`).
  - The `WEBRTC` make flag auto-enables when `build/libdatachannel` is present
    (run `tools/build-libdatachannel.sh` once), so a plain `make` includes it;
    `make WEBRTC_INCLUDE=off` forces it off.  libopus is a hard companion of the
    flag, found through `pkg-config` (brew `opus` / `libopus-dev` / mingw `opus`).
    The operator's `./machpsdr` is a WebRTC build.
- How the audio flows: `process_rx_buffer()` taps the clean 48 kHz
  interleaved-stereo demod block (beside the AF recorder, before the software
  listen gain) and calls `web_server_audio_tap()`, which fans it to every client
  subscribed to that receiver.  `web_rtc_session_feed()` takes the left channel
  into a bounded 200 ms ring under `queue_mutex` (never encodes or writes a
  socket on the RX thread — decision #6/#14).  A per-session worker thread drains
  the ring, encodes 20 ms Opus frames (`libopus`, 48 kHz mono, 32 kbps, in-band
  FEC), sets the RTP timestamp and pushes each frame into the track's Opus
  packetizer (PT 111, SSRC `0x4d485344`).  Lifetime: `web_rtc_teardown()` nulls
  `client->rtc` under `queue_mutex` (the lock the tap holds) and frees — joining
  the worker — with no lock held, so the audio thread never touches a freed
  session and the join never stalls the RX thread.
- Phase 4 implementation is COMPLETE and operator live-confirmed.  Its
  first slice is done on `origin/master`:
  server-pushed state replaces the browser's 500 ms `get_state` poll.  A GTK
  main-loop detector (`web_state_tick`, 50 ms) broadcasts a full `state` frame
  the instant a discrete field moves (frequency, mode, MOX, active RX, lock,
  step, filter, display window, receiver add/remove) and otherwise refreshes the
  S-meter every 200 ms; the detector compares a meter-free signature so meter
  noise never reads as a change, and `web_revision` now counts real changes, not
  ticks.  The browser sends one `get_state` on connect for the snapshot and keeps
  it as a 2 s quiet-link watchdog fallback; the periodic poll is gone.
- Phase 4 slice 2 is done on `origin/master`: the first receive controls.  Five
  allow-listed scalar `command` actions — `set_step`, `set_agc`, `set_af_gain`,
  `set_squelch`, `set_agc_gain` — parse to one `WEB_REQUEST_SET_CONTROL`/
  `WEB_CONTROL` in `web_protocol.c`, dispatch through a single
  `ext_set_rx_control` on the GTK thread, and are applied through the SAME choke
  points the desktop UI uses (`set_agc`/`set_squelch`/`receiver_set_agc_gain`/
  `receiver_set_volume`), clamped to each control's UI range.  Their read-back
  fields (`step`, `agc`, `af_gain`, `squelch`, `agc_gain`) are in `state` and in
  the detector signature, so a change from any surface echoes to every browser.
  The browser card gained step/AGC selects and AF-gain/squelch/AGC-gain sliders.
- Phase 4 slice 3 is done on `origin/master`: the noise controls (NB, NR, ANF,
  SNB).  Four more allow-listed actions — `set_nb` (0/1/2), `set_nr` (0..4 via
  `receiver_set_nr_mode`), `set_anf`, `set_snb` — go through the same
  `WEB_CONTROL` -> `ext_set_rx_control` path and are applied through
  `update_noise`/`receiver_set_nr_mode`, the exact VFO-menu choke points, so the
  bypass rule applies to a remote change too: in DIGU/DIGL or under a decoder the
  run flags are forced off while the selection is kept.  The state gains `nb`,
  `nr`, `anf`, `snb` and `noise_bypassed` (a new public `receiver_dsp_bypassed()`
  reads the one `bypass_stream_dsp` predicate); the browser shows NB and NR as
  dropdowns (like mode/AGC/step/filter — operator asked for a list over a
  click-cycle for a multi-state control) and SNB/ANF as toggle chips, dimmed
  (`chip-suppressed`) when `noise_bypassed` so a suppressed selection is shown, not
  pretended applied.  `web_offline` covers parse of each action plus the rx-required
  and boolean-rejected guards.
- Phase 5 slice 1 is implemented in the current work: the HTTP/WebSocket
  listener binds all IPv4 interfaces.  Configure -> Network can set, disable or
  clear password protection; authentication uses HTTP Basic with configurable
  username (default `machpsdr`), and only a UUID salt plus a 100,000-round PBKDF2-HMAC-SHA256 hash
  are persisted.  Protection covers every bundled asset and the WebSocket
  Upgrade.  WebSocket Origin must equal the request Host (HTTP or HTTPS).  When
  protection is disabled the service sends no challenge and is intentionally
  open to every host that can reach the port.
- Phase 5 slice 2 adds a built-in OpenSSL TLS 1.2+ transport for every asset and
  WebSocket frame.  First use creates a persistent 2048-bit RSA self-signed PEM
  bundle with a ten-year certificate and mode 0600; Configure -> Network may
  instead select one PEM bundle containing a certificate chain and matching
  private key.  The login username is configurable and persisted.  The TLS
  layer is inside MacHPSDR, so forwarding its TCP port requires no reverse proxy.
- Phase 5 slice 3 adds the Internet receive-audio path.  Every server-side
  libdatachannel peer uses one persisted UDP-mux port (8081 by default), so the
  router needs one stable UDP forwarding rule rather than a dynamic range.  The
  server resolves the Host of each accepted WebSocket and advertises that
  same page address with the UDP port as an additional ICE host candidate.  No
  STUN/TURN or third-party discovery service is used.  The status and Configure
  -> Network show both ports and tell the operator to forward HTTPS/TCP plus
  WebRTC/UDP.
- Phase 4 slice 4 is done on `origin/master`: filter-width presets, band up/down,
  split and RIT/XIT.  Seven more SET_CONTROL actions — `set_filter_preset` (index
  into the mode's `filters[]`, applied via `receiver_filter_changed`), `band_step`
  (+1/-1 through `next_band`/`previous_band` + `set_band`, which refuses a locked
  VFO), `set_split`, `set_rit`/`set_rit_enable` and `set_xit`/`set_xit_enable` (RIT
  on the receiver, XIT on the transmitter; both reach the hardware through
  `frequency_changed`+`update_frequency` like the VFO row).  State gains `band`,
  `split`, `rit`/`rit_on`, `xit`/`xit_on`, `filter` (current preset) and a
  `filters` array (each preset's name + edges) so the browser offers a reliable
  width dropdown rather than only the edge drag — the fix for "the bandwidth does
  not change" (a thin edge on a 20%-tall panadapter is impractical to grab, so
  drags read as tune).  The previously-greyed SPLIT/RIT/XIT chips and new band
  down/up + filter dropdown are live; `web_offline` gains seven cases.
- Phase 4 slice 5 is implemented in the current work: a device-shaped RF
  gain/attenuation slider, sub-receiver enable plus 0..100 stereo/mono
  crossfeed, saved-bookmark listing/recall and the active decoder's status and
  plain-text output.  Each command uses the desktop field/choke point; no web
  hardware path was added.  Soapy bounds come from the discovered gain-element
  ranges, HPSDR-family bounds match Configure -> Radio, and actual RF hardware
  effect is not a separate requirement of the web adapter.  Bookmarks are
  loaded once on the GTK thread and recall honours VFO lock.  The decoder cache
  is populated on the GTK timer, so socket workers never inspect GTK widgets.
- Phase 6 slice 1 is implemented in the current work: after the existing
  session-only desktop PTT arm, one browser may take an exclusive renewable TX
  lease.  It renews every two seconds and expires after five.  Disconnect,
  expiry, explicit release or withdrawing the desktop arm revokes ownership and
  emergency-unkeys through `ext_set_mox` on the GTK thread.  MOX-on requires the
  live holder (MOX-off stays available as an emergency action); every viewer sees
  the active-lease state and a `REMOTE PTT` indicator while browser MOX is on.
  The lease is tied to the WebSocket and cannot survive reconnect.
- Phase 6 slice 2 is implemented in the current work: the browser asks for its
  microphone only after it owns the TX lease, shows a local level meter, and
  renegotiates the existing audio session as bidirectional Opus.  libdatachannel
  decodes incoming RTP on its worker and performs only a bounded copy into a
  250 ms 48 kHz mono ring; the transmitter audio thread pops that ring at its
  existing microphone substitution point.  MOX-on requires fresh media.  A
  500 ms media gap while browser PTT is active revokes the lease and unkeys;
  permission failure releases the lease and leaves receive audio running.  The
  pushed `tx_media_active` field makes the page wait for an actual incoming
  media packet, rather than only ICE connection state, before enabling MOX.
- Phase 6 slice 3 is implemented in the current work: the lower bar exposes
  Tune, drive, tune power, microphone gain, compressor gain/on-off, leveler and
  CESSB.  State remains authoritative and every mutation is dispatched to the
  GTK thread, where it reuses the desktop WDSP/hardware appliers.  All changes
  require the live TX lease.  Tune-on also requires the lease but not microphone
  media; Tune-off and MOX-off remain emergency actions.  Lease loss now drops
  both Tune and MOX, so the emergency path no longer assumes MOX is the only RF
  source.
- Not implemented yet: failed-auth throttling, token/QR pairing, general
  viewer/controller roles and the Phase 6 PWA/accessibility polish.

The browser remote provides a disabled-by-default LAN listener, bundled static
assets, the `machpsdr.v1` WebSocket subprotocol,
server-pushed state, VFO-A frequency and mode commands, meters, and MOX.  Browser PTT
is additionally gated by `web_tx_enable`, a desktop checkbox that is deliberately
session-only and resets to false on every application start.  Phase 2 adds one
quantized latest-only spectrum row per subscribed receiver, a Canvas panadapter
and browser-derived waterfall, Low/Balanced/High/Ultra profiles, adaptive fallback,
click/drag/wheel tuning and draggable filter edges.

## File map

| Area | Files | Responsibility |
| --- | --- | --- |
| Normative design | `doc/web-remote.md` | Requirements, invariants, protocol, phases |
| Implementation handoff | `doc/web-remote-status.md` | Current state, decisions, proof, next step |
| HTTP/JSON protocol | `src/proto/web_protocol.c/.h` | Strict flat request parser and fixed route allow-list |
| Server | `src/proto/web_server.c/.h` | LAN listener, asset/WebSocket authentication, clients, state and command dispatch |
| Shared WebSocket primitive | `src/proto/tci_ws.c/.h` | RFC 6455 framing plus reusable Upgrade response |
| Desktop configuration | `src/ui/web_dialog.c/.h`, `src/ui/configure_dialog.c` | Enable, port, status and session PTT arm |
| Persistent radio settings | `src/core/radio.c/.h`, `src/core/radio_state.c` | Persist enable/port; never persist PTT arm |
| Browser application | `assets/web/` | Dependency-free responsive HTML/CSS/JavaScript client |
| Headless tests | `tools/web_offline.c`, `Makefile` | Parser and route rejection tests in `make check` |
| Packages | `Makefile`, `tools/build-appimage.sh` | Install the three browser assets |

## Decisions that must survive handoff

1. Browser control is a separate versioned protocol, not a browser pretending
   to be a TCI client.  Only the tested WebSocket framing/handshake primitive is
   shared with TCI.
2. The enabled TLS listener is LAN/Internet capable.  Password protection is optional by
   operator request; when off, the UI/status and manuals must say that every host
   reaching the port can control the radio.  Origin checks prevent browser CSRF
   but are not authentication.  HTTP Basic credentials travel only inside the
   built-in TLS connection; a forwarded Internet port must keep authentication on.
3. Network threads never mutate GTK or radio state directly.  Commands use the
   existing heap-owned `g_idle_add(ext_*)` pattern.
4. The server is a fixed asset host, not a filesystem server.  Routes remain an
   allow-list; queries, percent paths and traversal are rejected.
5. PTT requires the non-persistent desktop arm even on loopback.  Browser MOX
   additionally requires the connection-bound TX lease; disconnect, its
   five-second heartbeat timeout or withdrawal of the arm emergency-unkeys.
6. Spectrum and audio producers must never write sockets or wait for clients.
   They publish into bounded latest-value/bounded-duration buffers; slow clients
   lose old data instead of adding latency to DSP, audio or GTK threads.
7. Wire fields are additive within protocol v1.  Incompatible meaning or binary
   layout changes require a new WebSocket subprotocol version.
8. Keep the page dependency-free and package-local: no CDN, external font,
   framework build chain or runtime download.
9. Internet media is not I/Q and not waterfall video.  Send demodulated Opus
   audio plus one quantized spectrum row from which the browser draws both
   panadapter and waterfall.
10. Reliable WebSocket traffic is the control/signaling plane.  WebRTC is the
    target WAN media plane because browsers provide Opus, jitter buffering,
    congestion handling and lossy/unordered DataChannels there.  Keep the
    application framing usable over WebSocket as a local fallback, but do not
    claim TCP head-of-line behaviour is loss tolerant.
11. Audio wins congestion arbitration.  Spectrum is latest-only and adapts in
    this order: drop expired rows, reduce FPS, reduce bins.  Audio bitrate falls
    only after the display reaches its Low profile; commands/state never drop.
12. WebRTC uses `libdatachannel` plus `libopus`; the dependency spike and native
    loopback/RTP proof are recorded below.
13. Operator documentation ships atomically in English, Russian, Ukrainian and
    Belarusian.  A feature is not complete if any of `manual-en.md`,
    `manual-ru.md`, `manual-uk.md` or `manual-be.md` is stale.
14. The display timer makes only a calibrated bounded copy after its existing
    `GetPixels()` call and never waits for a sender.  Each client owns one sender,
    one serialized WebSocket send mutex and one replaceable `GBytes` reference
    per RX.  Shutdown wakes senders and waits for all client workers before
    releasing the global latest snapshots.
15. Backpressure is observable.  Replacement and expiry increment the client/RX
    drop count; three pressure observations first lower FPS, then bins, and ten
    healthy seconds recover toward the selected profile.  A binary send may
    block only its web sender and has a 250 ms socket timeout.

## Verification record

### 2026-10-23 — Phase 6 slice 3: remaining transmit controls

Added lease-gated Tune, drive, tune power, microphone gain, compressor
gain/on-off, leveler and CESSB to the pushed radio state, protocol allow-list and
responsive lower bar.  The GTK-thread applier uses the same WDSP calls and
Protocol 2/Soapy drive updates as the desktop controls.  Tune-on is lease-gated
but does not require microphone media.  Tune-off and MOX-off are emergency
actions; disconnect, timeout, release or desktop disarm now schedules both
`set_tune(FALSE)` and `ext_set_mox(FALSE)`.

Verified in this work session on macOS: normal and ASan/UBSan
`web_offline --selftest` pass all 73 cases, including Tune and global TX-control
parsing; bundled Node.js accepts `assets/web/app.js` with `--check`;
`make check-build` builds every configured harness; `make -j4` compiles and
links the full WebRTC/Soapy build; the bidirectional WebRTC loopback still
passes 75/75 receive packets and 72,000 decoded TX samples; TLS and all four
localized-manual checks pass; and `git diff --check` passes.  NOT verified live: control
read-back in a real browser, Tune/MOX emergency-off against a running desktop,
physical drive changes, processed microphone audio or any on-air effect.

### 2026-10-23 — Phase 6 slice 2: browser microphone TX audio

Extended the single WebRTC audio m-line to sendrecv.  The page requests a mono
microphone only after acquiring the connection-bound TX lease, exposes a local
input meter, restarts the peer with that track, and keeps MOX disabled until the
media connection is ready.  The server decodes browser Opus on the
libdatachannel callback thread into a bounded 48 kHz mono ring; the transmitter
uses those samples at its existing microphone choke point.  MOX-on is rejected
without fresh media, and a 500 ms media timeout during remote PTT revokes the
lease and schedules emergency unkey.

Verified in this work session on macOS: `make -j4` compiles and links the full
WebRTC build and `make check-build` builds every configured harness;
`webrtc_offline --selftest` passes offer/answer/ICE/DTLS and both
media directions, receiving all 75 server-to-browser Opus RTP packets with no
sequence/timestamp errors and decoding 72,000 browser-to-server samples at RMS
0.141; `web_offline --selftest` passes all 71 cases, `web_tls_offline --selftest`
and `i18n_offline --selftest` pass; and `git diff --check` passes.  The JavaScript syntax check could not use
the system `node` command because it is not installed; the repository's bundled
runtime accepts `assets/web/app.js` with `--check`.  A local static-page visual
check confirms the lower TX/status bar layout and caught/fixed the MIC meter's
hidden-state CSS.  NOT verified live: browser
permission UX, the MIC meter, restart/lease-loss behaviour against a running
desktop, or any on-air modulation/unkey effect.

### 2026-10-23 — Phase 6 slice 1: TX lease and emergency unkey

Implemented an exclusive browser-TX lease ahead of the deferred general Phase 5
role model.  The protocol adds `lease_acquire`, `lease_renew` and
`lease_release`; ownership is the upgraded WebSocket itself, with a two-second
browser renewal and five-second server deadline.  Only the live holder can set
MOX true, while MOX false remains an emergency action.  Disconnect, expiry,
explicit release and clearing the session-only desktop PTT arm all revoke the
lease and schedule `ext_set_mox(FALSE)` on the GTK main loop.  State broadcasts
`tx_lease_active` and `remote_ptt`; the page adds Take/Release TX control and a
visible REMOTE PTT indicator.

Verified in this work session on macOS: `make check-build` builds every configured
harness; normal and ASan/UBSan `web_offline --selftest` runs pass all 71 cases,
including the three lease lifecycle messages; `make -j4` compiles and links the
full WebRTC build (the forced all-object rebuild repeats the pre-existing unused
FT8 helper warning); `web_tls_offline --selftest` passes; and, with permission to
bind its fixed UDP test port, `webrtc_offline --selftest` passes its ICE/DTLS,
advertised-address and 75-packet Opus RTP checks.  The bundled Node.js accepts
`assets/web/app.js` with `--check`, and `git diff --check` passes.  NOT verified live: two-browser lease exclusion, disconnect or
five-second-timeout unkey against a running desktop, the remote-PTT indicator,
or any on-air effect.  Browser microphone/TX audio is not part of this slice.

### 2026-10-23 — operator live-confirmed Phases 4 and 5

The operator confirmed on 2026-10-23 that everything shipped in Phases 4 (pushed
state, all receive controls, noise controls, filter/band/split/RIT/XIT) and 5
(LAN listener, password, TLS, fixed-port WebRTC audio) works live.  The
"NOT verified live" / "operator step" caveats in the per-slice entries below are
therefore cleared; they are kept as the implementation history, not as open
items.  All test hardware is present on this machine (HackRF, Pluto over USB) —
the only thing still absent is an HPSDR radio (and thus the two-receiver HPSDR
pair), so every remaining HPSDR-specific claim stays emulator-only.

### 2026-10-23 — Phase 5 slice 1: LAN listener and optional password

The browser remote now listens on every IPv4 interface when enabled.  Configure
-> Network can set a password (minimum eight characters), turn the password
challenge off without deleting it, or clear it.  The username is fixed as
`machpsdr`.  HTTP Basic protects all three bundled assets and the WebSocket
Upgrade; disabled authentication sends no challenge and intentionally leaves the
service open to every reachable LAN host.  The status distinguishes `password
protected` from `open`.  WebSocket Origin validation now accepts only an HTTP or
HTTPS origin whose authority exactly matches the request Host, rather than a
hard-coded localhost origin.  CSP permits the page's `ws:`/`wss:` connection.

The clear-text password is never put in `RADIO` or the property file.  Setting a
password creates a UUID salt and persists only that salt plus a
PBKDF2-HMAC-SHA256 result (100,000 rounds); comparison of the fixed-length hash is
constant-time.  At that point Basic authentication still ran over plain HTTP;
the following TLS slice replaces that limitation.  PTT remains separately
session-armed.

### 2026-10-23 — Phase 5 slice 2: built-in HTTPS/WSS

The embedded server now performs TLS 1.2+ itself using OpenSSL; every HTTP asset,
401 exchange and WebSocket frame uses the encrypted transport.  With no custom
path, first start generates a persistent 2048-bit RSA self-signed certificate
and private key in one mode-0600 PEM bundle under the application data directory.
Configure -> Network accepts an alternative PEM bundle containing the certificate
chain and matching private key, validates it before switching, exposes the
SHA-256 fingerprint, and allows a configurable login username.

Verified on macOS: `make -j4` completes; `web_tls_offline` creates/loads the
self-signed bundle, completes a real TLS client/server handshake over a socket
pair, and round-trips encrypted data.  NOT yet verified in this isolated session:
the GTK certificate controls, a real browser warning/login/WSS session, public
TCP port forwarding, or audio through NAT.  Spectrum/control use the one forwarded
TCP port.  This was the exact limitation addressed by Phase 5 slice 3 below.

Verified on macOS:

- `make -j4` completed and linked the WebRTC-enabled application;
- `make check-build` built every configured offline harness;
- `web_offline --selftest` passes all 52 cases, including exact LAN/HTTPS Origin
  matches, cross-site and host-suffix rejection, a standard PBKDF2 vector,
  correct credentials, wrong password and wrong username;
- `make SANITIZE=1 web-offline && ./web_offline --selftest` passes under
  AddressSanitizer/UndefinedBehaviorSanitizer;
- every configured `make check` harness except `qo100_offline` was then run
  directly and passed;
- `git diff --check` passes.

The complete `make check` and `make SANITIZE=1 check` recipes were attempted but
did not complete: in both configurations the pre-existing `qo100_offline` binary
stopped making progress after its `beacon lock stopped` diagnostic (normal run
was interrupted after 30 seconds there; sanitized run after about 90 seconds).
No web test had failed.  NOT yet verified in this isolated session: opening the
GTK settings page, receiving a 401 challenge in a real browser, the authenticated
WebSocket Upgrade, or connecting over a second physical LAN device.  Those are
operator desktop tests; no live/on-air claim is made.

### 2026-10-23 — Phase 5 slice 3: direct fixed-port WebRTC audio

Receive audio can now follow the browser remote through an ordinary router port
forward.  Configure -> Network persists a **WebRTC audio UDP port** (default
8081).  The native offerer enables libdatachannel/libjuice UDP multiplexing and
constrains candidate gathering to that one port for every client.  For each
WebSocket the server resolves the HTTP Host through which that browser opened
the page, then advertises that address and the UDP port as an additional ICE
host candidate.  The additive `hello.audio_udp_port` field exposes the selected
port.  Editing it restarts the listener immediately.  No STUN, TURN or other
third-party service is contacted.

Verified on macOS: `make -j4` links the WebRTC-enabled application and `make
check-build` builds every configured harness.  The native `webrtc_offline
--selftest`, run with permission to bind UDP 49001, reports a host candidate on
exactly that port, a successful offer/answer/ICE/DTLS loopback connection, and
the explicitly advertised page address on that port, followed by 75/75 Opus RTP
packets at 49.9/s with PT 111, no other payloads and no bad sequence/timestamp
steps.  `web_offline --selftest` passes all 52
protocol/routing/authentication cases;
`web_tls_offline` completes its certificate/handshake round trip; JavaScriptCore
parses `assets/web/app.js`; and `git diff --check` passes.  NOT verified in this
isolated session: real router TCP/UDP forwarding, browser audio from outside the
LAN, or split-DNS behaviour.

### 2026-10-23 — Browser page re-laid-out to mirror the desktop window

Front-end only (`assets/web/index.html`, `app.css`, `app.js`); no C, protocol or
behaviour change, so no rebuild and no `make check` impact.  The page now follows
the desktop main-window layout instead of the earlier card grid:

- a VFO bar — navigation chips, the big cyan VFO-A readout in the desktop's
  `00135.205.200` grouping (click to edit inline), an orange VFO-B placeholder,
  AF/AGC gain sliders with a mute button, and an S1..S9+60 S-meter;
- the mode/status chip strip (LOCK, mode, filter width, NB/NR/SNB/ANF, AGC,
  RIT/XIT/CTUN/DUP/BMK, squelch);
- a full-width panadapter over the waterfall, split **20/80** per operator choice
  (`drawSpectrum`'s `panHeight`);
- a bottom TRANSMIT (MOX) / STATUS bar.

Only the controls the protocol already carries are wired (VFO-A frequency, mode,
step, AGC, AF/AGC gain, squelch, mute, panadapter tuning gestures, MOX); the
desktop buttons with no protocol action yet (A>B/A<B/A<>B, SPLIT, NB/NR/SNB/ANF,
RIT/XIT/CTUN/DUP/BMK, VFO B) are shown **greyed** as a deliberate skeleton the
operator asked to keep.  The deferred polish (per-digit frequency editing, real
VFO B, wiring each greyed chip) is parked in `.claude/notes/backlog.md`.

Verified: rendered in a local browser against a hand-injected `state` frame and a
synthetic `MPSD` spectrum stream — the VFO bar holds one line at laptop widths,
the big readout/S-meter/chip strip render, and the panadapter trace + rolling
waterfall draw at the 20/80 split with the filter passband and centre line in
place.  The four manuals and README are intentionally unchanged (no control,
default, limitation or gesture changed — only the visual arrangement).  NOT yet
done: live acceptance on the faker (operator step, as in earlier phases).

### 2026-10-23 — Phase 4 slice 4: filter presets, band, split, RIT/XIT

The operator reported that the bandwidth "does not change" from the browser.  The
`set_filter` edge-drag path is correct in code, but grabbing a ~thin filter edge
within 10 px on a 20%-tall panadapter registers as a tune, so the width rarely
moves.  The fix is an explicit width control; this slice adds it plus band/split/
RIT/XIT.

- Protocol (`web_protocol.c/.h`): seven new `WEB_CONTROL` values and allow-list
  actions — `set_filter_preset`, `band_step`, `set_split`, `set_rit`,
  `set_rit_enable`, `set_xit`, `set_xit_enable` — all on the generic SET_CONTROL
  path (integer value, rx required, boolean rejected).
- GTK apply (`ext.c`): `set_filter_preset` → `receiver_filter_changed(idx)` (the
  VFO filter menu's own choke point: sets `filter_a`, remembers it per mode,
  applies the `filters[]` edges, FMN deviation included); `band_step` → `set_band`
  via `next_band`/`previous_band`, which already refuses a locked VFO; `set_split`
  toggles `rx->split` keeping `split_last`; RIT writes `rx->rit`/`rit_enabled` and
  XIT writes the transmitter's `xit`/`xit_enabled`, both followed by
  `frequency_changed`+`update_frequency` so the offset actually reaches the
  hardware and the dial repaints.  All clamped (RIT/XIT ±10 kHz, preset to
  `FILTERS`).
- State (`web_server.c`): `append_state_body` emits `band` (`band_display_name`),
  `split`, `rit`/`rit_on`, `xit`/`xit_on`, `filter` and the mode's `filters` array
  (name + edges).  All join the discrete signature, so a change from any surface
  pushes to every client; the preset list rebuilds in the browser only when the
  mode changes.
- Browser (`assets/web/`): the static filter chip becomes a preset `<select>`;
  band down/up chips (with a band-name chip) and a live SPLIT toggle; RIT/XIT chips
  click-to-arm and wheel-to-nudge by 10 Hz, showing the offset when on (XIT dimmed
  without a TX-capable radio).  Same commit, on operator feedback: the slice-3
  NB/NR click-cycle chips became dropdowns too, for consistency with every other
  multi-state control (mode/AGC/step/filter); SNB/ANF stay toggle chips.

Verified on macOS: `make` links clean (WebRTC auto-enabled); `web_offline
--selftest` passes with seven new cases (each action parses to its control, and
`band_step` without `rx` is rejected), and again under ASan/UBSan; the application
relinks; JavaScriptCore parses `assets/web/app.js`; `props`/`keybind`/`nr`/`agc`/
`tci` offline harnesses pass.  As before, `make check` was not run to completion
(the pre-existing `qo100_offline` stall precedes `web_offline`), so web and
representative harnesses were run directly.  NOT verified live: a desktop run
exercising the dropdown, band chips, split and RIT/XIT against GTK and a second
browser, and — the motivating report — that the width dropdown actually changes
the passband on the air (operator step).  The four manuals and README were updated
in the same commit.

### 2026-10-23 — Phase 4 slice 3: noise controls (NB/NR/ANF/SNB)

NB, NR, ANF and SNB are now controllable from the browser, under the same
DIGU/DIGL-and-decoder bypass rule the desktop obeys.

- Protocol (`web_protocol.c/.h`): four new `WEB_CONTROL` values and allow-list
  entries (`set_nb`/`set_nr`/`set_anf`/`set_snb`).  They reuse the generic
  SET_CONTROL path, so the integer-value and `rx`-required guards already apply and
  a boolean is still rejected.
- GTK apply (`ext.c`): `ext_set_rx_control` gains the four cases, each guarded on
  `rx->channel>=0` and applied through the VFO menu's own choke point —
  `rx->nb/nb2`+`update_noise` for NB (0/1/2), `receiver_set_nr_mode` for NR (0..4),
  `rx->anf`/`rx->snb`+`update_noise` for the toggles.  Because the apply runs
  through `update_noise`, the bypass rule forces the WDSP run flags off in DIGU/DIGL
  or under a decoder exactly as a desktop toggle would, while the selection is kept.
- State (`web_server.c`): `append_state_body` emits `nb`, `nr`, `anf`, `snb` and
  `noise_bypassed`.  The last reads a new public `receiver_dsp_bypassed()`
  (receiver.c/h) that returns the single `bypass_stream_dsp` predicate, so there is
  one spelling of the rule.  All five join the discrete signature, so a change from
  the desktop or another browser pushes to every client.
- Browser (`assets/web/`): the four previously-greyed chips are live — NB and NR
  click-cycle (off/NB/NB2 and off/NR/NR2/NR3/NR4, labelled from state), SNB and ANF
  toggle; all four highlight (`chip-on`) when active and dim (`chip-suppressed`)
  when `noise_bypassed`, staying settable like the desktop menu.

Verified on macOS: `make` links clean (WebRTC auto-enabled); `web_offline
--selftest` passes with the new noise cases (NB/NR/ANF/SNB parse, rx-required and
boolean-rejected), and again under ASan/UBSan; the application relinks;
JavaScriptCore parses `assets/web/app.js`; `tci`/`props`/`keybind`/`nr`/`agc`/
`dcblock`/`reconnect`/`rec` offline harnesses pass.  `make check` was NOT run to
completion: it reaches the pre-existing `qo100_offline` hang (the "beacon lock
stopped" stall recorded above) before `web_offline`, which runs after it; the web
and representative harnesses were therefore run directly.  NOT yet verified live: a
desktop run exercising the chips against GTK and a second browser, and the
suppressed-in-DIGU/DIGL behaviour on the air (operator step, as in prior phases).
The four manuals and README were updated in the same commit.

### 2026-10-23 — Phase 4 slice 2: first receive controls

Step, AGC mode, AF gain, squelch and AGC gain are now controllable from the
browser, on top of the pushed-state foundation.

- Protocol (`web_protocol.c/.h`): a `WEB_CONTROL` enum and
  `WEB_REQUEST_SET_CONTROL`.  The parser keeps its allow-list discipline — a small
  table maps `set_step`/`set_agc`/`set_af_gain`/`set_squelch`/`set_agc_gain` to the
  enum and every other `command` action is still rejected — and the existing
  generic checks require an integer `value` and an `rx`, rejecting a boolean.
- GTK apply (`ext.c`): one `ext_set_rx_control` re-validates the receiver with
  `receiver_is_live()`, then applies each control through the UI's own choke point
  (`set_agc`, `set_squelch`, `receiver_set_agc_gain`, `receiver_set_volume`) and
  clamps to that control's range (AF/squelch 0-100→0-1, AGC 0-4, AGC gain
  [-20,120] dB, step >0), ending in `update_vfo` — so a remote change behaves
  exactly like moving the desktop knob, not just writing a field.
- State: `append_state_body` now emits `step`, `agc`, `af_gain`, `squelch` and
  `agc_gain`; these are part of the discrete signature, so a desktop or other-browser
  change pushes to all clients.
- Browser (`assets/web/`): the receiver card gained a step select, an AGC-mode
  select and AF-gain/squelch/AGC-gain range sliders (throttled live sends plus a
  send on release); `render()` writes each back from state, skipping whichever
  control the viewer is actively holding.

Verified on macOS: `make` links clean (WebRTC auto-enabled); `make check` passes
(exit 0); `web_offline --selftest` passes with eight new control cases (parse of
each action, the `rx`-required and boolean-rejected guards, and rejection of an
unknown control action).  NOT yet verified live: a desktop faker run exercising
the sliders and selects against GTK and a second browser (operator test, as in
prior phases).  Range/limit behaviour is harness/implementation-only until then.

### 2026-10-23 — Phase 4 slice 1: server-pushed state replaces polling

The browser no longer polls `get_state` every 500 ms; the server pushes state on
change.

- Server (`web_server.c`): `ws_send_state`'s body is refactored into
  `append_state_body(include_meter)` + `build_state_frame()`, shared by the
  `get_state` reply and the push.  `web_state_tick()` is a `g_timeout_add` (50 ms)
  armed in `web_server_start` and torn down in `web_server_stop` (both GTK-thread
  only, so the source cannot be mid-tick at teardown).  Each tick skips out when
  no client is upgraded; otherwise it builds the meter-free signature and pushes a
  full `state` frame when the signature changed (and bumps `web_revision`) or when
  200 ms have passed (meter refresh, same revision).  `web_broadcast_text()`
  snapshots the client set under `web_clients_mutex`, then sends with the mutex
  dropped — `client_send` is serialized per client by `send_mutex` and re-checks
  `closing`/`fd`, so a client released right after the snapshot is a safe no-op and
  a slow send never stalls the accept thread.  The design is self-correcting: a new
  mutation site cannot be "left off a list" of notify calls, unlike TCI's scattered
  `tci_notify_*`.  `get_state` now reports the current revision without bumping it.
- Browser (`assets/web/app.js`): the 500 ms `setInterval` poll is removed.  One
  `get_state` is sent on connect for the initial snapshot; a 2 s watchdog, re-armed
  on every received `state`, re-requests a snapshot if the link goes quiet (it never
  fires in healthy operation, since the server refreshes the meter at 5 Hz).

Verified on macOS: both `make` (WebRTC auto-enabled, libdatachannel+libopus linked)
and the link succeed; `make check` passes (exit 0, every self-test); `web_offline
--selftest` passes (0 failures); `app.js`'s added block is brace-balanced (the
raw `{`/`}` count carries the same +2 string-literal skew as the committed file).
This is a responsiveness/architecture change with no new operator control, default,
limitation or step, so the four manuals and README are intentionally unchanged.
NOT yet verified: the live browser round-trip — control echo latency, two-browser
consistency and a live S-meter under the push — needs a desktop faker run (next,
operator test), as in Phases 2 and 3.  The full Phase 4 acceptance (two browsers
consistent with GTK and each other across all three surfaces) also waits on the
rest of the control surface.

### 2026-10-23 — Phase 3 slice 2b: signaling over the WebSocket + browser peer

The WebRTC session is now reachable from a browser: offer/answer/ICE are
carried over the existing control WebSocket and the browser builds a real
`RTCPeerConnection`.

- Protocol: `audio_subscribe`/`audio_unsubscribe` and the `rtc_answer`/
  `rtc_candidate` signaling replies, with a JSON escaper for outgoing offers and
  span-then-unescape parsing so a large escaped SDP cannot leak on a bad message
  (`web_offline` covers all of this).
- Server (`web_server.c`, under `#ifdef WEBRTC`): one `WEB_RTC_SESSION` per
  client.  `audio_subscribe` creates the session and offers; the signal callback
  wraps the offer/candidate as `rtc_offer`/`rtc_candidate` text frames through the
  serialized `client_send`; `rtc_answer`/`rtc_candidate` feed the session; the
  session is town down on `audio_unsubscribe` and on disconnect with no lock
  held (so joining libdatachannel threads cannot deadlock on `send_mutex`).  The
  `hello` advertises the `audio` capability only in a WEBRTC build; a default
  build answers `webrtc_unavailable`.
- Browser (`assets/web/`): a per-receiver "Listen" button (shown only when the
  server advertises `audio`) creates the peer, answers the server's offer,
  trickles candidates and attaches the received track to an `<audio>` element.

Verified on macOS: both `make` (no datachannel linked) and `make
WEBRTC_INCLUDE=WEBRTC` (linked) build clean; `make check` and the webrtc
loopback harness still pass; `web_offline` covers the new messages; app.js
balances.  NOT yet verified: the live browser round-trip to the connected state
— that needs a desktop run of a WEBRTC build (next, operator test).  There is no
audio in the track yet (silent until slice 3), so this slice proves the
connection, not sound; the four manuals are intentionally deferred to slice 3,
when audio is actually operator-usable.

### 2026-10-23 — Phase 3 slice 2a: native WebRTC session + connect harness

The server side of one peer connection per browser audio subscription now
exists and is proven to connect, without a browser.

- `src/proto/web_rtc.c/.h` gains a `WEB_RTC_SESSION`: the offering (server) role.
  It creates a PeerConnection (auto-negotiation off), adds an Opus send-only
  audio track (PT 111, fixed SSRC for the slice-3 packetizer), emits the SDP
  offer and ICE candidates through a structured signal callback, and accepts the
  browser's answer and remote candidates.  Callbacks run on libdatachannel
  worker threads and only hand structured strings back to the owner — they never
  touch GTK or the socket directly, matching the server's existing dispatch
  rules.
- `tools/webrtc_offline.c` (`make WEBRTC_INCLUDE=WEBRTC webrtc-offline`) drives
  that session as the server and stands in for the browser with a second
  libdatachannel peer (the answerer) built from the raw C API, wiring the two
  peers' signaling together in-process.

Verified on macOS: `./webrtc_offline --selftest` reports
`offer/answer/ICE/DTLS loopback connect PASS` — both peers reach the connected
state over loopback, exercising offer/answer exchange, ICE candidate trickle and
the DTLS handshake.  The harness is opt-in (needs the WEBRTC dependency) and is
deliberately not in `make check`.  Not yet done: wiring the session into
`web_server`/`web_protocol` and the browser `RTCPeerConnection` (slice 2b), and
feeding Opus RTP into the track from the AF tap (slice 3).

### 2026-10-23 — Phase 3 slice 1: libdatachannel build integration

The WebRTC dependency is now buildable and linkable, off by default.

- `tools/build-libdatachannel.sh` builds libdatachannel `v0.24.6` (and the
  usrsctp/libsrtp it statically links, on OpenSSL) from source into
  `build/libdatachannel`, following the `build-soapy-plutosdr.sh` precedent; the
  tag is pinned so a CI cache keys on the script.
- `Makefile` gains a `WEBRTC` feature flag modelled on `FREEDV`, wiring
  `-D WEBRTC`, the prefix include/lib paths and an rpath (`@loader_path`/`$ORIGIN`)
  into every platform's link.  It AUTO-ENABLES when `build/libdatachannel` is
  present (run the build script once, then a plain `make` includes it with no
  flag); a tree without the dependency links without it, and `make
  WEBRTC_INCLUDE=off` forces it off.
- `src/proto/web_rtc.c/.h` wraps the libdatachannel C API: logger setup and a
  `web_rtc_selftest()` that creates and destroys a real `rtcPeerConnection`.

Verified on macOS: the dependency builds (exit 0); a standalone C program against
`rtc/rtc.h` links and creates a PeerConnection at runtime; `make
WEBRTC_INCLUDE=WEBRTC` links the application against `@rpath/libdatachannel.0.dylib`
(confirmed with `otool -L`), while the default `make` does not link it and
`web_offline --selftest` still passes.  The dependency's only non-system runtime
dylib is OpenSSL (already bundled by `make app`).  Not yet done: `make app`
bundling of `libdatachannel.dylib`, CI build on Linux/Windows, and any signaling
or audio — those are the next slices.

### 2026-10-22 — Phase 3 WebRTC dependency spike

Decision #12 required a measured dependency spike before selecting a native
WebRTC implementation.  Result:

- **Selected: `libdatachannel`.**  It is the lightest native WebRTC stack with
  a C API (`rtc/rtc.h`) and macOS/Linux/Windows support — the alternatives are
  Google `libwebrtc` (enormous) or non-C runtimes (Pion/Go, aiortc/Python).
- **License: MPL-2.0** since 0.18, which is GPL-compatible; this tree is already
  effectively GPLv3 via the HFDL port, so there is no new licensing obstacle.
- **Transitive deps:** `usrsctp` (BSD; already present, brew 0.9.5.0),
  `libsrtp` (BSD; needed for the SRTP media path), a DTLS backend (`openssl`
  4.0.3 is present; Mbed TLS/GnuTLS are alternatives), and `libjuice` for ICE
  (bundled as a submodule).  Packaged shared build is ~2.8 MB (Arch 0.24.5).
- **Packaging:** not in Homebrew or the mingw repos, so it is built from source
  via CMake, following the `tools/build-soapy-plutosdr.sh` precedent rather than
  vendoring the C++ tree into the repo; `make app` then bundles the dylib plus
  openssl/usrsctp/srtp, and CI builds it on Linux/Windows.
- **Localhost testability:** WebRTC connects on loopback with host ICE
  candidates and needs no STUN/TURN.  The spike originally left WAN traversal
  to Phase 5; the shipped design uses the known page address and fixed forwarded
  UDP port instead of a traversal service.

No code, build script or dependency has been committed from this spike yet; it
records the decision only.

### 2026-10-22 — locale-independent state JSON (bug fix)

The `state` frame formatted `meter_db` with `g_snprintf("%.1f", …)`.  GTK runs
under the operator's locale, so on a comma-decimal machine (`ru_RU`, confirmed
with `LC_NUMERIC=ru_RU.UTF-8`) it emitted `"meter_db":-200,0`, i.e. invalid
JSON.  The browser's `JSON.parse` failed silently (`catch { return; }`), so
`render()` never ran: the page connected and then sat on "Waiting for radio
state" with no receiver card, which also meant no panadapter and no waterfall.
`ws_send_state()` now formats the one float with `g_ascii_formatd`, matching the
tree's rule for every ASCII interchange format.  Verified: `g_ascii_formatd`
gives `-200.0` and `g_snprintf("%.1f")` gives `-200,0` under this locale; the
binary relinks; `web_offline --selftest` passes.  Live browser render of the
restored card/panadapter/waterfall is still to be confirmed from a desktop
session running the relinked binary.

### 2026-10-22 — phases 0 and 1

Verified on macOS:

```text
make -j4
make check
make i18n-offline web-offline tci-offline
./i18n_offline --selftest
./web_offline --selftest
./tci_offline --selftest
git diff --check
```

Results:

- the application links successfully;
- all project self-tests pass;
- all 19 browser protocol/route cases pass, including malformed types,
  duplicate fields, oversize input, traversal and percent-encoded paths;
- TCI WebSocket framing tests still pass after handshake extraction;
- translation-table validation passes;
- no whitespace errors were reported.

Not verified in that session:

- A live faker/browser connection.  The isolated macOS runner raises an AppKit
  `NSInternalInconsistencyException` while GTK creates the first discovery
  window, before a RADIO or the web listener exists.  Re-test from a normal
  desktop session; do not attribute this pre-server failure to the web code and
  do not mark desktop acceptance complete until frequency/mode round trips are
  observed in both GTK and the browser.
- Real-radio or on-air behaviour.

### 2026-10-22 — Phase 2 headless verification

Verified on macOS:

```text
make -j4
make check
./web_offline --selftest
git diff --check
python3 -m http.server 8765 --bind 127.0.0.1 --directory assets/web
```

Results:

- the complete application builds and links;
- `make check` completed with exit status 0 and every project self-test passed;
- all 29 browser protocol/route cases pass, including subscription validation,
  exact MPSD little-endian sequence/timestamp/count/size fields, bad magic,
  profile limits, peak-preserving quantization and latest-frame replacement;
- the three browser assets loaded in the in-app browser from the temporary
  static server, the responsive shell rendered, and JavaScript ran through the
  expected WebSocket failure/reconnect state (`Disconnected`); Node.js was not
  installed, so no separate `node --check` was performed;
- no whitespace errors were reported.

Not verified in this session:

- A live faker/browser spectrum connection.  The existing isolated macOS
  AppKit limitation occurs before a RADIO or listener exists, so the desktop
  acceptance below remains mandatory and no bitrate, delivered FPS,
  capture-to-draw age, peak alignment or throttled-client cadence result is
  claimed.
- Real-radio or on-air behaviour.

### 2026-10-22 — Phase 2 headless implementation

Implemented:

- the receiver display timer publishes the calibrated, already-fetched visible
  bins only after `GetPixels()` succeeds; it does not call WDSP again;
- field-by-field 32-byte little-endian `MPSD` header helpers, profile validation,
  peak-preserving reduction and eight-bit floor/span quantization;
- explicit per-RX subscribe/unsubscribe, default Balanced selection in the page,
  a hard 1024-bin/15-fps ceiling, latest-only replacement and drop reporting;
- a dedicated sender per WebSocket client, serialized text/binary writes,
  500-ms expiry, staged FPS/width fallback, hysteretic recovery and bounded
  shutdown;
- a dependency-free Canvas 2D panadapter and rolling waterfall generated from
  the same row, with click/drag/wheel tuning and draggable filter edges;
- matching operator documentation in all four supported manuals.

Headless verification performed in this session is recorded below.  It does not
prove GTK/browser peak alignment, measured browser bitrate or unchanged RX
cadence under a throttled real socket.

## Verification record

### 2026-10-23 — Phase 3 slice 3 (Opus audio into the track) implemented

The server now produces receive audio and the browser auto-plays it.  Native
proof: `make WEBRTC_INCLUDE=WEBRTC webrtc-offline && ./webrtc_offline --selftest`
reports both the loopback connect PASS and, with real-time synthetic audio fed
into the live `web_rtc.c` session, RTP arriving on the answerer's track at
**75 packets, PT 111, 0 stray, 50.0/s over 1.50 s, zero loss** (two runs).  The
RTP/RTCP demux in the harness correctly discards the SR reporter's sender
reports.  `make check` passes; `make` (WebRTC auto-enabled) links the app with
libopus.  The four manuals, README and this record were updated in the same
commit.  **Live acceptance: the operator confirmed on 2026-10-23 that the
browser audio works (faker on macOS).**

Remaining caveats, waived like Phase 2's measurements: the finer behaviours were
not separately reported by the operator — sustained-duration continuity, the
autoplay-unlock affordance, mute/unmute, tab sleep/resume and multi-receiver
audio are confirmed only as "audio works", the rest harness/implementation-only.

### 2026-10-23 — Phase 4 slice 5: remaining receive surface

Implemented the final Phase 4 surface: `set_rf_gain`, `set_subrx`,
`set_subrx_mix` and `recall_bookmark`, with matching state fields and browser
controls.  The radio state now carries saved bookmark names/frequencies plus the
active decoder title/plain-text output.  Bookmark recall and the desktop list
share one implementation, including the VFO-lock guard; decoder text is cached
on the GTK timer instead of reading widgets from a socket thread.

Verified in this work session on macOS: `make -j4` links the full
WebRTC/Soapy/decoder-enabled application; `make check-build` builds every
configured harness; `web_offline --selftest` passes all 70 cases, including the
four new allow-listed controls; bundled Node.js accepts `assets/web/app.js` with
`--check`; `git diff --check` passes; and every configured offline harness except
`qo100_offline` was run directly and passed.  `make check` was attempted, but the
pre-existing `qo100_offline` hang recurred immediately after its `beacon lock
stopped` diagnostic and was interrupted after 30 seconds; no web test failed.
NOT yet verified live: moving the new controls in
a real browser, observing the Soapy/HPSDR front-end read-back, sub-RX audio,
bookmark recall or live decoder text.  No physical-HPSDR RF-effect claim is
needed for this web adapter; the existing desktop hardware path is reused.

## Exact next slice: Phase 6 — PWA and accessibility polish

Add an installable manifest and service-worker shell without caching live
control/media traffic, then complete keyboard/focus labels, reduced-motion and
small-screen checks.  Keep offline installation separate from radio-state
claims and preserve the rule that reconnect always re-authenticates and
re-synchronises state.  The remaining Phase 5 pairing/roles/throttling work is
intentionally deferred by operator request.

### 2026-10-22 — WAN/jitter design decision

The specification now makes the Internet budget part of the protocol rather
than a later optimization.  The selected direction is reliable WebSocket for
control/signaling, WebRTC Opus for production receive audio, and an
unordered/limited-retransmission DataChannel for disposable spectrum rows.
Waterfall pixels are generated in the browser and are never a second stream.

At the time of this design decision no encoder, WebRTC dependency, spectrum
binary frame or impairment result existed.  Phase 2 has since added the
transport-independent spectrum format over the current WebSocket; the WebRTC
dependency and impairment spike still belongs to Phase 3.

### 2026-10-22 — localized operator documentation requirement

The documentation contract now requires every operator-visible browser-remote
change to update the English, Russian, Ukrainian and Belarusian manuals in the
same commit.  Phase 1 has been backfilled in all four manuals, including the
localhost URL/default port, available controls, session-only PTT arm, current
localhost limitation and the then-not-yet-implemented spectrum/audio features.
The Phase 2 change above replaces that operator text in all four manuals with
the shipped panadapter/waterfall profiles and gestures; receive audio remains
unimplemented.

This was a documentation-only change.  Validation checks all four files for one
Browser Remote section and one `MACHPSDR_WEB` entry, plus `git diff --check`;
no code or runtime behaviour changed.

## Update checklist

For every browser-remote commit:

1. Read this record and `web-remote.md` before editing.
2. Keep safety invariants and protocol examples synchronized with code.
3. Update Current handoff, Decisions (when needed), Verification record and
   Exact next slice in this file.
4. Update README only for newly usable operator functionality.
5. Update all four localized user manuals for every operator-visible change.
6. Run the narrow harness while iterating, then `make` and the proportionate
   regression suite; use full `make check` for a completed phase.
7. Record caveats as pending verification rather than implied success.
8. Commit and push according to `AGENTS.md`, reporting branch and commit.
