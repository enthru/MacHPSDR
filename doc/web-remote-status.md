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

Baseline commit: `7072382` (`Add localhost browser remote foundation`), pushed
to `origin/master` on 2026-10-22.

- Phase 0: complete.
- Phase 1: implemented and headlessly verified.
- Phase 1 desktop acceptance: pending on a normal graphical session.
- Next implementation slice: Phase 2, panadapter and waterfall transport.
- Not implemented yet: spectrum/waterfall, RX audio, full RX controls, pushed
  state events, authenticated LAN access, control leases and browser TX audio.

The first slice provides a disabled-by-default listener bound to
`127.0.0.1`, bundled static assets, the `machpsdr.v1` WebSocket subprotocol,
state polling, VFO-A frequency and mode commands, meters, and MOX.  Browser PTT
is additionally gated by `web_tx_enable`, a desktop checkbox that is deliberately
session-only and resets to false on every application start.

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

## Exact next slice: Phase 2

Implement spectrum transport before audio or LAN access:

1. Find the existing receiver display-timer path that calls `GetPixels()` and
   publish the bins it has already fetched.  The web path must not call WDSP.
2. Add explicit binary-header encode/decode helpers and tests in
   `web_protocol.c/.h` for the 24-byte `MPSD` header specified in
   `web-remote.md`; do not cast a C struct onto the wire.
3. Add validated `spectrum_subscribe`/`spectrum_unsubscribe` requests with a
   receiver, requested width and FPS.  Clamp v1 to 1024 bins and 15 fps by
   default, with documented hard maxima.
4. Maintain one latest immutable spectrum snapshot per visible receiver.  Each
   client may have at most one unsent spectrum frame per receiver; replacement
   drops the older frame and increments an observable drop counter.
5. Send binary frames only from a web worker/sender, never from the display
   callback.  Define lock ownership and shutdown order before adding the first
   queue.
6. Render the panadapter and rolling waterfall from the same bins in
   `assets/web/`; start with Canvas 2D and preserve a future WebGL path.
7. Add deterministic headless tests for header endianness, sequence/count/size,
   width/FPS clamping and latest-frame replacement.  Then perform the normal
   desktop faker acceptance and record measured FPS/latency here.

Phase 2 is complete only when a throttled or disconnected browser cannot alter
the receiver/display cadence and the browser peak position matches the faker
signal.

## Update checklist

For every browser-remote commit:

1. Read this record and `web-remote.md` before editing.
2. Keep safety invariants and protocol examples synchronized with code.
3. Update Current handoff, Decisions (when needed), Verification record and
   Exact next slice in this file.
4. Update README only for newly usable operator functionality.
5. Run the narrow harness while iterating, then `make` and the proportionate
   regression suite; use full `make check` for a completed phase.
6. Record caveats as pending verification rather than implied success.
7. Commit and push according to `AGENTS.md`, reporting branch and commit.
