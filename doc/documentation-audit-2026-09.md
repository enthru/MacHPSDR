# Documentation review — 4 September 2026

Scope: 20 July–4 September 2026, 579 commits through `a7e1377`.
Reviewed the commit history by feature area, compared user-facing changes with
README and all four manuals, and checked relevant implementation and UI labels.
This is a documentation review, not a new hardware validation or a code audit.
Existing uncommitted QO-100 and receiver work is outside this review.

## Gaps corrected

| Area | Documentation correction | Supporting commits |
|---|---|---|
| Logging | Category selection, examples, environment precedence, synchronization at DEBUG; distinguish `--debug` from `-v`. | `a7e1377` |
| SoapySDR rates | Device rate applies during reception, updates spans, refuses changes during TX and reports failures. | `8276de2`, `1bcf1ce` |
| DC removal / hardware AGC | Radio-page DC removal, default, LO-centred 20 Hz corner, live application and saved hardware AGC. | `59325c2`, `8636e10`, `281ba8e` |
| Tuning | Device/transverter-aware frequency ceiling, LOCK coverage and exceptions, freetune span drag preserving the station. | `6d60487`, `38f61c7`, `f91d4f0` |
| Noise reduction / audio | NR3 depth, NR4 smoothing default and saved values, SUBRX updates, mute, FM audio AGC. | `c49109e`, `9c9ba73`, `a29a83c`, `d0ad6b6` |
| TX | DAC backoff control, range, per-device defaults and distinction from Drive. | `e9105f6`, `cbd4d70` |
| QO-100 | Correction interval, acquisition exceptions, minimum span, CW fallback and confirmed return to the selected middle beacon. | `669e1a4`, `bd8af19`, `979be89`, `b06c870`, `cfef595` |
| TCI audio | Default AGC path, automatic level limiting, independence from speaker volume, optional pre-AGC path and overload limitations. | `b1f1c7f`, `716d7dc`, `2f3e9fc`, `6a79cb5`, `08c71bd` |
| Protocol 2 | Experimental PureSignal and Diversity; DDC0/DDC1 and two-ADC requirements. Hardware validation remains outstanding. | `e4f4855`, `d77f86c`, `29d45b6`, `b143b06` |
| Display | Phase scope sources, interface font, DX-spot placement and label appearance; correct CW APF / Network page paths. | `852f3fb`, `c31a94e`, `71dd3f8`, `de37421`, `a03f215`, `f9d9530` |
| Recorder | Automatic numbered WAV continuations around 3.75 GiB per stream, individually replayable. | `19276e1` |
| CW verification | Remove stale README claim of synthetic-only decoding; real off-air recordings were already documented elsewhere. | `bb551af` |

The broken README link to the removed `MacOS.md` now points to the current
macOS packaging section.

## Coverage retained

The existing documentation already covers the GTK4 interface, decoder selector,
FT8/FT4 QSO workflow, SSTV receive/transmit, WEFAX, APT mapping and image saving,
CW memories/keyer, HFDL/ACARS applications and scanning, I/Q Player seeking,
manual notches/APF/squelch, TX processing, LINK and hotkeys, shared SoapySDR
receivers, network device discovery, TCI control/IQ, and platform packages/builds.
Superseded intermediate implementations were checked against the final code;
internal fixes and CI changes do not each become a user-manual entry.

All 44 literal `MACHPSDR_*` environment variables read by tracked application and
tool sources are represented in README and the four manuals. Legacy subsystem
diagnostic switches remain distinct from the new general debug category filter.

Validation: Markdown whitespace, numbered manual sections, local links and
cross-language coverage of the added controls and environment variables.
No application rebuild is required for these Markdown-only changes.
