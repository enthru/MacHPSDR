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
- Phase 1: implemented and headlessly verified.
- Phase 2: implemented and headlessly verified.
- Phase 1/2 desktop acceptance: pending on a normal graphical session.
- Next implementation slice: run and record the Phase 2 faker/browser desktop
  acceptance described below before beginning receive audio.
- Not implemented yet: RX audio, full RX controls, pushed
  state events, authenticated LAN access, control leases and browser TX audio.

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

## Exact next slice: Phase 2 desktop acceptance

On a normal graphical desktop, start `MACHPSDR_WEB=8080 ./machpsdr --faker`, open
the page in current Safari and one Chromium/Firefox browser, and record:

1. matching GTK/browser faker peak X position and dB scale for Low, Balanced and
   High, plus successful click/drag/wheel/filter-edge gestures;
2. measured binary payload bitrate, delivered FPS and capture-to-draw age for
   all three profiles;
3. RX/display cadence while the browser reads normally, is throttled for at
   least 20 seconds and is disconnected, confirming drops/adaptation without a
   cadence change or memory growth;
4. profile recovery after pressure clears and clean application shutdown with a
   stalled client.

Only after these observations are recorded should Phase 2 be marked complete
and the Phase 3 WebRTC/Opus packaging spike begin.

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
