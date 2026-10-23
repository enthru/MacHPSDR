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
- Phase 4 is IN PROGRESS.  Its first slice is done on `origin/master`:
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
- Not implemented yet (rest of Phase 4 and beyond): filter presets, RF
  gain/attenuation, the noise controls (NB/NR/ANF/SNB under the DIGU/DIGL and
  decoder bypass rule), band up/down, RIT/split, sub-receiver, bookmarks and
  decoder status/output; authenticated LAN access, control leases and browser TX
  audio.

The local remote provides a disabled-by-default listener bound to
`127.0.0.1`, bundled static assets, the `machpsdr.v1` WebSocket subprotocol,
state polling, VFO-A frequency and mode commands, meters, and MOX.  Browser PTT
is additionally gated by `web_tx_enable`, a desktop checkbox that is deliberately
session-only and resets to false on every application start.  Phase 2 adds one
quantized latest-only spectrum row per subscribed receiver, a Canvas panadapter
and browser-derived waterfall, Low/Balanced/High profiles, adaptive fallback,
click/drag/wheel tuning and draggable filter edges.

## File map

| Area | Files | Responsibility |
| --- | --- | --- |
| Normative design | `doc/web-remote.md` | Requirements, invariants, protocol, phases |
| Implementation handoff | `doc/web-remote-status.md` | Current state, decisions, proof, next step |
| HTTP/JSON protocol | `src/proto/web_protocol.c/.h` | Strict flat request parser and fixed route allow-list |
| Server | `src/proto/web_server.c/.h` | Loopback listener, assets, WebSocket clients, state and command dispatch |
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
2. Until token authentication exists, the listener stays on loopback.  Do not
   add a `0.0.0.0` convenience switch or treat Origin checks as authentication.
3. Network threads never mutate GTK or radio state directly.  Commands use the
   existing heap-owned `g_idle_add(ext_*)` pattern.
4. The server is a fixed asset host, not a filesystem server.  Routes remain an
   allow-list; queries, percent paths and traversal are rejected.
5. PTT requires the non-persistent desktop arm even on loopback.  Future remote
   TX also requires a lease, heartbeat/watchdog and emergency unkey.
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
12. No WebRTC library has been selected yet.  `libdatachannel` plus `libopus` is
    a candidate combination because the former has a C API and
    macOS/Linux/Windows support, but adoption requires a measured bundle-size,
    license, build and browser-interoperability spike.
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
  candidates and needs no STUN/TURN; TURN belongs to the Phase 5 WAN/auth work.

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

## Exact next slice: Phase 4 — more receive controls

Slices 1 (pushed state) and 2 (step/AGC/AF gain/squelch/AGC gain) are shipped and
live-pending above.  The next slice extends the same mechanism — allow-listed
`command` action → `WEB_CONTROL` → `ext_set_rx_control` → UI choke point, with a
read-back field in `append_state_body` — to the remaining receive controls:

- the noise controls (NB, NR, ANF, SNB).  These MUST honour `bypass_stream_dsp()`
  — in DIGU/DIGL or under a decoder the run flags are forced off, so the browser
  has to show them disabled/suppressed rather than pretend they took effect
  (reuse `update_noise`/`receiver_notch_sync`; see the bypass rule in CLAUDE.md).
- RF gain / attenuation (hardware-dependent; gate on what the device exposes),
  and filter presets alongside the existing draggable edges.
- band up/down and RIT/split, routed through `set_band`/the RIT appliers so the
  VFO-lock rule still refuses a locked dial.

Then sub-receiver and bookmarks, and finally decoder status/textual output.  Each
operator-visible slice updates the four manuals and README.  Authenticated LAN
access and control leases remain gated behind their own phase and the loopback-only
rule (decision #2/#5).

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
