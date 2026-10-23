"use strict";

const modes = ["LSB", "USB", "DSB", "CWL", "CWU", "FMN", "AM", "DIGU", "SPEC", "DIGL", "SAM", "DRM", "WFM"];
const cards = new Map();
let socket, reconnectTimer, stateWatchdog, leaseRenewTimer, radioState;
let nextId = 1, socketGeneration = 0;
let capabilities = [];
let leaseHeld = false;
let micStream = null, micContext = null, micAnalyser = null, micMeterTimer = null;
let micReady = false, micGeneration = 0;
const connection = document.querySelector("#connection");
const receivers = document.querySelector("#receivers");
const empty = document.querySelector("#empty");
const template = document.querySelector("#receiver-template");
const mox = document.querySelector("#mox");
const txControl = document.querySelector("#tx-control");
const remotePtt = document.querySelector("#remote-ptt");
const micLevelWrap = document.querySelector("#mic-level-wrap");
const micLevel = document.querySelector("#mic-level");
const revision = document.querySelector("#revision");
const decoderTitle = document.querySelector("#decoder-title");
const decoderText = document.querySelector("#decoder-text");
const enableSound = document.querySelector("#enable-sound");

// 135205200 Hz -> "00135.205.200": three-digit groups from the right, the top
// group zero-padded so the readout keeps the desktop's fixed width.
function formatFreq(hz) {
  const d = String(Math.max(0, Math.round(hz))).padStart(11, "0").slice(-11);
  return `${d.slice(0, 5)}.${d.slice(5, 8)}.${d.slice(8, 11)}`;
}
function formatWidth(hz) {
  const w = Math.abs(Math.round(hz));
  return w >= 1000 ? `${(w / 1000).toFixed(w % 1000 ? 1 : 0)}k` : `${w}`;
}
// Browsers block autoplay with sound until the page has had a user gesture.
// Once any play() succeeds (or the viewer taps the affordance), the whole tab is
// unlocked and the prompt never returns.
let audioUnlocked = false;

function setConnection(text, online = false) {
  connection.textContent = text;
  connection.classList.toggle("offline", !online);
}
function send(message) {
  if (socket?.readyState !== WebSocket.OPEN) return false;
  socket.send(JSON.stringify({ id: nextId++, ...message }));
  return true;
}
function setLeaseHeld(held) {
  const changed = leaseHeld !== held;
  leaseHeld = held;
  clearTimeout(leaseRenewTimer);
  if (held && socket?.readyState === WebSocket.OPEN)
    leaseRenewTimer = setTimeout(() => send({ type: "lease_renew" }), 2000);
  if (changed) {
    if (held) startMicrophone();
    else stopMicrophone();
  }
  if (radioState) renderTx(radioState);
}
function txMediaReady(radio) {
  return micReady && !!radio.tx_media_active &&
         [...cards.values()].some(card => card._pc && card._connected);
}
function renderTx(radio) {
  const leaseAvailable = capabilities.includes("tx_lease");
  const busy = !!radio.tx_lease_active && !leaseHeld;
  txControl.hidden = !leaseAvailable;
  txControl.disabled = !radio.can_transmit || busy;
  txControl.classList.toggle("held", leaseHeld);
  txControl.textContent = leaseHeld ? (micReady ? "Release TX control" : "Starting microphone…") :
                          (busy ? "TX control in use" : "Take TX control");
  mox.disabled = !radio.can_transmit || !leaseHeld || !txMediaReady(radio);
  mox.classList.toggle("on", radio.mox);
  mox.textContent = radio.mox ? "MOX ON" : "MOX";
  remotePtt.hidden = !radio.remote_ptt;
  micLevelWrap.hidden = !leaseHeld;
}
function command(action, fields) { send({ type: "command", action, ...fields }); }
function subscribe(card, rx) {
  if (card.dataset.subscription === String(socketGeneration)) return;
  card.dataset.subscription = String(socketGeneration);
  send({ type: "spectrum_subscribe", rx, profile: card.querySelector(".spectrum-profile").value });
}
function canvasX(event, canvas) {
  const rect = canvas.getBoundingClientRect();
  return Math.max(0, Math.min(rect.width, event.clientX - rect.left));
}
function displayGeometry(card) {
  const state = card._rx;
  if (!state) return null;
  const span = state.display_high - state.display_low;
  return span > 0 ? { low: state.display_low, high: state.display_high, span } : null;
}
function filterPixels(card) {
  const geometry = displayGeometry(card);
  if (!geometry) return null;
  const width = card.querySelector(".spectrum").getBoundingClientRect().width;
  return {
    left: (card._rx.frequency + card._rx.filter_low - geometry.low) / geometry.span * width,
    right: (card._rx.frequency + card._rx.filter_high - geometry.low) / geometry.span * width
  };
}

function installSpectrumGestures(card, rxId) {
  const canvas = card.querySelector(".spectrum");
  const sendDrag = (drag, x) => {
    const now = performance.now();
    if (now - drag.lastSent < 45) return;
    drag.lastSent = now;
    const geometry = displayGeometry(card);
    const width = canvas.getBoundingClientRect().width;
    if (!geometry || width <= 0) return;
    const delta = Math.round((x - drag.startX) / width * geometry.span);
    if (drag.kind === "left") command("set_filter", { rx: rxId, low: drag.low + delta, high: drag.high });
    else if (drag.kind === "right") command("set_filter", { rx: rxId, low: drag.low, high: drag.high + delta });
    else command("set_frequency", { rx: rxId, value: Math.max(1, Math.round(drag.frequency - delta)) });
  };
  canvas.addEventListener("pointerdown", event => {
    if (event.button !== 0 || !card._rx) return;
    const x = canvasX(event, canvas), edges = filterPixels(card);
    let kind = "tune";
    if (edges && Math.abs(x - edges.left) <= 10) kind = "left";
    else if (edges && Math.abs(x - edges.right) <= 10) kind = "right";
    canvas._drag = { kind, startX: x, moved: false, lastSent: 0,
      frequency: card._rx.frequency, low: card._rx.filter_low, high: card._rx.filter_high };
    canvas.setPointerCapture(event.pointerId);
  });
  canvas.addEventListener("pointermove", event => {
    const drag = canvas._drag;
    if (!drag) return;
    const x = canvasX(event, canvas);
    if (Math.abs(x - drag.startX) >= 4) drag.moved = true;
    if (drag.moved) sendDrag(drag, x);
  });
  canvas.addEventListener("pointerup", event => {
    const drag = canvas._drag;
    if (!drag) return;
    const x = canvasX(event, canvas);
    if (drag.moved) sendDrag({ ...drag, lastSent: 0 }, x);
    else {
      const geometry = displayGeometry(card), width = canvas.getBoundingClientRect().width;
      if (geometry && width > 0)
        command("set_frequency", { rx: rxId, value: Math.max(1, Math.round(geometry.low + x / width * geometry.span)) });
    }
    canvas._drag = null;
  });
  canvas.addEventListener("pointercancel", () => { canvas._drag = null; });
  canvas.addEventListener("wheel", event => {
    if (!card._rx) return;
    event.preventDefault();
    const direction = event.deltaY < 0 ? 1 : -1;
    const value = Math.max(1, card._rx.frequency + direction * Math.max(1, card._rx.step));
    card._rx.frequency = value;
    command("set_frequency", { rx: rxId, value });
  }, { passive: false });
}

function makeCard(rx) {
  const card = template.content.firstElementChild.cloneNode(true);
  card.dataset.rx = rx.id;
  card.querySelector(".rx-name").textContent = `RX${rx.id + 1}`;
  const select = card.querySelector(".mode");
  modes.forEach((name, value) => select.add(new Option(name, value)));
  select.addEventListener("change", () => command("set_mode", { rx: rx.id, value: Number(select.value) }));
  const profile = card.querySelector(".spectrum-profile");
  profile.addEventListener("change", () => send({ type: "spectrum_subscribe", rx: rx.id, profile: profile.value }));
  // Big VFO-A readout: click (or focus+Enter) swaps in an inline numeric editor.
  const display = card.querySelector(".freq-a");
  const editor = card.querySelector(".freq-edit");
  const openEditor = () => {
    editor.value = String(card._rx?.frequency ?? "");
    display.hidden = true; editor.hidden = false; editor.focus(); editor.select();
  };
  const closeEditor = () => { editor.hidden = true; display.hidden = false; };
  const applyEditor = () => {
    const value = Number(editor.value.replace(/[^0-9]/g, ""));
    if (Number.isSafeInteger(value) && value > 0) command("set_frequency", { rx: rx.id, value });
    closeEditor();
  };
  display.addEventListener("click", openEditor);
  display.addEventListener("keydown", event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); openEditor(); } });
  editor.addEventListener("keydown", event => {
    if (event.key === "Enter") applyEditor();
    else if (event.key === "Escape") closeEditor();
  });
  editor.addEventListener("blur", applyEditor);
  // Scalar receiver controls. Sliders send live (throttled) while dragging and
  // once more on release; the select controls send on change. render() writes
  // each back from server state, skipping whichever one the user is holding.
  const throttle = fn => { let t = 0; return v => { const n = performance.now(); if (n - t < 60) return; t = n; fn(v); }; };
  const bindSlider = (inputSel, valSel, action) => {
    const el = card.querySelector(inputSel), out = card.querySelector(valSel);
    const push = throttle(v => command(action, { rx: rx.id, value: v }));
    el.addEventListener("input", () => { out.textContent = el.value; push(Number(el.value)); });
    el.addEventListener("change", () => command(action, { rx: rx.id, value: Number(el.value) }));
  };
  bindSlider(".af-gain", ".af-gain-val", "set_af_gain");
  bindSlider(".squelch", ".squelch-val", "set_squelch");
  bindSlider(".agc-gain", ".agc-gain-val", "set_agc_gain");
  bindSlider(".rf-gain", ".rf-gain-val", "set_rf_gain");
  bindSlider(".subrx-mix", ".subrx-mix-val", "set_subrx_mix");
  const agc = card.querySelector(".agc");
  agc.addEventListener("change", () => command("set_agc", { rx: rx.id, value: Number(agc.value) }));
  const step = card.querySelector(".step");
  step.addEventListener("change", () => command("set_step", { rx: rx.id, value: Number(step.value) }));
  // NB/NR have several states, so they are dropdowns like mode/AGC/step/filter
  // (off/NB/NB2 and off/NR/NR2/NR3/NR4). SNB/ANF are binary, so they stay toggle
  // chips. All honour the bypass rule: the selection is recorded exactly as on the
  // desktop, only muted (dimmed) while suppressed in DIGU/DIGL or under a decoder.
  const nbSel = card.querySelector(".nb-sel");
  nbSel.addEventListener("change", () => command("set_nb", { rx: rx.id, value: Number(nbSel.value) }));
  const nrSel = card.querySelector(".nr-sel");
  nrSel.addEventListener("change", () => command("set_nr", { rx: rx.id, value: Number(nrSel.value) }));
  const toggleChip = (sel, action, field) =>
    card.querySelector(sel).addEventListener("click", () =>
      command(action, { rx: rx.id, value: card._rx?.[field] ? 0 : 1 }));
  toggleChip(".snb-chip", "set_snb", "snb");
  toggleChip(".anf-chip", "set_anf", "anf");
  // Filter width: a preset dropdown (the mode's filters[] list) applied through
  // the desktop's receiver_filter_changed choke point. The panadapter edge drag
  // still works; this is the reliable width control.
  const filterSel = card.querySelector(".filter-sel");
  filterSel.addEventListener("change", () => command("set_filter_preset", { rx: rx.id, value: Number(filterSel.value) }));
  // Band up/down (set_band, which refuses a locked VFO) and split toggle.
  card.querySelector(".band-down").addEventListener("click", () => command("band_step", { rx: rx.id, value: -1 }));
  card.querySelector(".band-up").addEventListener("click", () => command("band_step", { rx: rx.id, value: 1 }));
  card.querySelector(".split-chip").addEventListener("click", () => command("set_split", { rx: rx.id, value: card._rx?.split ? 0 : 1 }));
  // RIT/XIT: click toggles on/off, wheel nudges the offset by 10 Hz (the desktop's
  // rit_step), clamped server-side to +/-10 kHz.
  const offsetChip = (sel, enableAction, valueAction, field, onField) => {
    const el = card.querySelector(sel);
    el.addEventListener("click", () => command(enableAction, { rx: rx.id, value: card._rx?.[onField] ? 0 : 1 }));
    el.addEventListener("wheel", event => {
      event.preventDefault();
      const next = (Number(card._rx?.[field]) || 0) + (event.deltaY < 0 ? 10 : -10);
      command(valueAction, { rx: rx.id, value: Math.max(-10000, Math.min(10000, next)) });
    }, { passive: false });
  };
  offsetChip(".rit-chip", "set_rit_enable", "set_rit", "rit", "rit_on");
  offsetChip(".xit-chip", "set_xit_enable", "set_xit", "xit", "xit_on");
  card.querySelector(".subrx-chip").addEventListener("click", () =>
    command("set_subrx", { rx: rx.id, value: card._rx?.subrx ? 0 : 1 }));
  card.querySelector(".bookmark-sel").addEventListener("change", event => {
    if (event.target.value !== "") command("recall_bookmark", { rx: rx.id, value: Number(event.target.value) });
    event.target.value = "";
  });
  card._audio = card.querySelector(".rx-audio");
  card._muted = false;
  const audioBtn = card.querySelector(".audio-toggle");
  audioBtn.hidden = !capabilities.includes("audio");
  audioBtn.addEventListener("click", () => toggleMute(card));
  installSpectrumGestures(card, rx.id);
  receivers.append(card);
  cards.set(rx.id, card);
  subscribe(card, rx.id);
  // Audio auto-starts: there is no "Listen" step. The per-receiver button is a
  // mute/unmute for the audio already flowing.
  if (capabilities.includes("audio")) startAudio(card, rx.id);
  return card;
}

function render(state) {
  radioState = state.radio;
  const live = new Set(state.receivers.map(rx => rx.id));
  for (const [id, card] of cards) if (!live.has(id)) {
    send({ type: "spectrum_unsubscribe", rx: id });
    if (card._pc) { send({ type: "audio_unsubscribe", rx: id }); try { card._pc.close(); } catch {} card._pc = null; }
    card.remove(); cards.delete(id);
  }
  for (const rx of state.receivers) {
    const card = cards.get(rx.id) || makeCard(rx);
    card._rx = rx;
    subscribe(card, rx.id);
    card.classList.toggle("active", state.radio.active_rx === rx.id);
    const editor = card.querySelector(".freq-edit");
    if (editor.hidden) card.querySelector(".freq-a").textContent = formatFreq(rx.frequency);
    card.querySelector(".mode").value = String(rx.mode);
    // Filter presets: rebuild the dropdown when the mode's list changes, then
    // reflect the current preset (unless the viewer is holding the select open).
    const filterSel = card.querySelector(".filter-sel");
    if (Array.isArray(rx.filters)) {
      const key = rx.mode + ":" + rx.filters.length;
      if (filterSel._key !== key) {
        filterSel._key = key;
        filterSel.textContent = "";
        // The preset title already IS the width label (e.g. "2.7k"), so show it
        // as-is — appending formatWidth() repeated it ("2.7k (2.7k)").
        rx.filters.forEach((f, i) => filterSel.add(new Option(f.n, i)));
      }
      if (rx.filter !== undefined && document.activeElement !== filterSel) filterSel.value = String(rx.filter);
    }
    // S-meter: map dBm onto the S1..S9+60 scale (S9 = -73 dBm, 6 dB per S-unit).
    const pos = Math.max(0, Math.min(1, (rx.meter_db + 121) / 108));
    card.querySelector(".smeter-bar span").style.width = `${pos * 100}%`;
    const sText = rx.meter_db >= -73
      ? `S9+${Math.round(rx.meter_db + 73)}`
      : `S${Math.max(1, Math.min(9, Math.round((rx.meter_db + 121) / 6) + 1))}`;
    card.querySelector(".smeter-read").textContent = `${sText} · ${Math.round(rx.meter_db)} dBm`;
    card.querySelector(".lock-chip").classList.toggle("chip-on", !!rx.locked);
    // Reflect server state onto the controls, but never yank one the user is
    // actively dragging/opening (active element) out from under them.
    const setSlider = (inputSel, valSel, value) => {
      const el = card.querySelector(inputSel);
      if (value === undefined) return;
      if (document.activeElement !== el) el.value = String(value);
      card.querySelector(valSel).textContent = el.value;
    };
    const setSelect = (sel, value) => {
      const el = card.querySelector(sel);
      if (value !== undefined && document.activeElement !== el) el.value = String(value);
    };
    setSlider(".af-gain", ".af-gain-val", rx.af_gain);
    setSlider(".squelch", ".squelch-val", rx.squelch);
    setSlider(".agc-gain", ".agc-gain-val", rx.agc_gain);
    const rf = card.querySelector(".rf-gain");
    if (rx.rf_min !== undefined) rf.min = String(rx.rf_min);
    if (rx.rf_max !== undefined) rf.max = String(rx.rf_max);
    card.querySelector(".rf-label").textContent = rx.rf_kind === "attenuation" ? "ATT" : "RF";
    setSlider(".rf-gain", ".rf-gain-val", rx.rf_gain);
    setSlider(".subrx-mix", ".subrx-mix-val", rx.subrx_mix);
    setSelect(".agc", rx.agc);
    setSelect(".step", rx.step);
    // Noise controls: NB/NR dropdowns reflect the state (highlight the wrapper when
    // active); SNB/ANF chips highlight; all dim when the radio is suppressing them
    // (shown, not pretended-applied — see noise_bypassed).
    setSelect(".nb-sel", rx.nb);
    setSelect(".nr-sel", rx.nr);
    card.querySelector(".nb-wrap").classList.toggle("chip-on", (rx.nb || 0) > 0);
    card.querySelector(".nr-wrap").classList.toggle("chip-on", (rx.nr || 0) > 0);
    card.querySelector(".snb-chip").classList.toggle("chip-on", !!rx.snb);
    card.querySelector(".anf-chip").classList.toggle("chip-on", !!rx.anf);
    for (const sel of [".nb-wrap", ".nr-wrap", ".snb-chip", ".anf-chip"])
      card.querySelector(sel).classList.toggle("chip-suppressed", !!rx.noise_bypassed);
    // Band name, split and RIT/XIT. The offset chips show the value when on; XIT
    // is only meaningful on a transmit-capable radio.
    if (rx.band !== undefined) card.querySelector(".band-name").textContent = rx.band;
    card.querySelector(".split-chip").classList.toggle("chip-on", !!rx.split);
    const fmtOffset = v => (v >= 0 ? "+" : "") + v;
    const ritChip = card.querySelector(".rit-chip");
    ritChip.textContent = rx.rit_on ? `RIT ${fmtOffset(rx.rit || 0)}` : "RIT";
    ritChip.classList.toggle("chip-on", !!rx.rit_on);
    const xitChip = card.querySelector(".xit-chip");
    xitChip.textContent = rx.xit_on ? `XIT ${fmtOffset(rx.xit || 0)}` : "XIT";
    xitChip.classList.toggle("chip-on", !!rx.xit_on);
    xitChip.classList.toggle("chip-suppressed", !state.radio.can_transmit);
    card.querySelector(".subrx-chip").classList.toggle("chip-on", !!rx.subrx);
    card.querySelector(".subrx-mix-wrap").classList.toggle("chip-suppressed", !rx.subrx);
    const bookmarkSel = card.querySelector(".bookmark-sel");
    const bookmarkKey = JSON.stringify(state.radio.bookmarks || []);
    if (bookmarkSel._key !== bookmarkKey) {
      bookmarkSel._key = bookmarkKey;
      bookmarkSel.textContent = "";
      bookmarkSel.add(new Option("—", ""));
      for (const b of state.radio.bookmarks || [])
        bookmarkSel.add(new Option(`${b.name || "Bookmark"} · ${formatFreq(b.frequency)}`, b.id));
    }
  }
  empty.hidden = state.receivers.length !== 0;
  renderTx(state.radio);
  decoderTitle.textContent = state.radio.decoder_title || "";
  decoderText.textContent = state.radio.decoder_text || "";
  decoderTitle.hidden = !state.radio.decoder_text;
  decoderText.hidden = !state.radio.decoder_text;
  revision.textContent = `State revision ${state.revision}`;
}

function waterfallColor(value) {
  const t = value / 255;
  if (t < .3) return [0, Math.round(t / .3 * 65), Math.round(30 + t / .3 * 130)];
  if (t < .65) return [Math.round((t - .3) / .35 * 35), Math.round(65 + (t - .3) / .35 * 190), Math.round(160 - (t - .3) / .35 * 100)];
  return [Math.round(35 + (t - .65) / .35 * 220), Math.round(255 - (t - .65) / .35 * 85), Math.round(60 - (t - .65) / .35 * 40)];
}

function drawSpectrum(card, bins, floor, span) {
  const canvas = card.querySelector(".spectrum"), rect = canvas.getBoundingClientRect();
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const width = Math.max(1, Math.round(rect.width * dpr)), height = Math.max(1, Math.round(rect.height * dpr));
  const panHeight = Math.round(height * .20);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width; canvas.height = height;
    card._waterfall = document.createElement("canvas");
    card._waterfall.width = width; card._waterfall.height = Math.max(1, height - panHeight);
  }
  const ctx = canvas.getContext("2d", { alpha: false }), wf = card._waterfall;
  const wctx = wf.getContext("2d", { alpha: false });
  wctx.drawImage(wf, 0, 0, width, wf.height - 1, 0, 1, width, wf.height - 1);
  const row = wctx.createImageData(width, 1);
  for (let x = 0; x < width; x++) {
    const value = bins[Math.min(bins.length - 1, Math.floor(x * bins.length / width))];
    const [r, g, b] = waterfallColor(value), p = x * 4;
    row.data[p] = r; row.data[p + 1] = g; row.data[p + 2] = b; row.data[p + 3] = 255;
  }
  wctx.putImageData(row, 0, 0);
  ctx.fillStyle = "#030a0f"; ctx.fillRect(0, 0, width, panHeight);
  ctx.strokeStyle = "#17303f"; ctx.lineWidth = dpr;
  for (let i = 1; i < 5; i++) { const y = i * panHeight / 5; ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke(); }
  const edges = filterPixels(card);
  if (edges) {
    ctx.fillStyle = "rgba(69,217,255,.10)";
    ctx.fillRect(edges.left * dpr, 0, (edges.right - edges.left) * dpr, panHeight);
    ctx.strokeStyle = "rgba(69,217,255,.75)";
    for (const edge of [edges.left, edges.right]) { ctx.beginPath(); ctx.moveTo(edge * dpr, 0); ctx.lineTo(edge * dpr, panHeight); ctx.stroke(); }
  }
  ctx.strokeStyle = "#68f59b"; ctx.lineWidth = Math.max(1, 1.25 * dpr); ctx.beginPath();
  for (let i = 0; i < bins.length; i++) {
    const x = i / Math.max(1, bins.length - 1) * width;
    const db = floor + bins[i] / 255 * span;
    const y = Math.max(0, Math.min(panHeight, (floor + span - db) / span * panHeight));
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke(); ctx.drawImage(wf, 0, panHeight);
  ctx.strokeStyle = "rgba(255,255,255,.5)"; ctx.beginPath(); ctx.moveTo(width / 2, 0); ctx.lineTo(width / 2, panHeight); ctx.stroke();
}

function handleBinary(buffer) {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 36) return;
  const view = new DataView(buffer);
  if (view.getUint8(0) !== 77 || view.getUint8(1) !== 80 || view.getUint8(2) !== 83 || view.getUint8(3) !== 68 ||
      view.getUint8(4) !== 1 || view.getUint8(5) !== 1) return;
  const rx = view.getUint8(6), count = view.getUint32(20, true), payload = view.getUint32(28, true);
  if (count === 0 || count > 2048 || payload !== count + 4 || buffer.byteLength !== 32 + payload) return;
  const card = cards.get(rx);
  if (!card) return;
  const timestamp = Number(view.getBigUint64(12, true));
  const floor = view.getInt16(32, true) / 10, span = view.getUint16(34, true) / 10;
  if (!(span > 0)) return;
  const bins = new Uint8Array(buffer, 36, count), arrival = performance.now() * 1000;
  const offset = arrival - timestamp;
  card._clockOffset = Math.min(card._clockOffset ?? offset, offset);
  card._age = Math.max(0, (offset - card._clockOffset) / 1000);
  card._frames = (card._frames || []).filter(t => arrival - t < 1000000);
  card._frames.push(arrival);
  drawSpectrum(card, bins, floor, span);
}

function updateSpectrumStatus(message) {
  const card = cards.get(message.rx);
  if (!card) return;
  const fps = card._frames?.length || 0;
  const age = Number.isFinite(card._age) ? `${Math.round(card._age)} ms` : "—";
  card.querySelector(".spectrum-status").textContent = `${message.profile} · ${fps} fps · ${age} · ${message.dropped} dropped`;
  card.querySelector(".spectrum-profile").value = message.profile;
}

// The button shows, and flips, the mute state; while the track is still coming
// up it is disabled and reads "Connecting…".
function refreshAudioButton(card) {
  const button = card.querySelector(".audio-toggle");
  if (!button) return;
  if (!card._connected) { button.disabled = true; button.textContent = "⏳"; button.setAttribute("aria-pressed", "false"); button.title = "Connecting audio…"; return; }
  button.disabled = false;
  button.textContent = card._muted ? "🔇" : "🔈";
  button.title = card._muted ? "Unmute received audio" : "Mute received audio";
  button.setAttribute("aria-pressed", card._muted ? "true" : "false");
}
function updateAudioAvailability() {
  const available = capabilities.includes("audio");
  for (const [rxId, card] of cards) {
    const button = card.querySelector(".audio-toggle");
    if (button) button.hidden = !available;
    // A hello arriving after the cards were built (e.g. a reconnect) is where a
    // newly-available audio capability turns into a running stream.
    if (available && !card._pc) startAudio(card, rxId);
  }
}
// Attempt playback; if the browser blocks it for want of a gesture, raise the
// one-tap affordance. A muted element always plays, so this only ever prompts
// for audible sound the viewer has not yet allowed.
function tryPlay(card) {
  if (!card._audio) return;
  card._audio.play().then(() => { audioUnlocked = true; enableSound.hidden = true; })
                    .catch(() => { if (!card._muted) enableSound.hidden = false; });
}
function stopAudio(card, rxId) {
  send({ type: "audio_unsubscribe", rx: rxId });
  if (card._pc) { try { card._pc.close(); } catch {} card._pc = null; }
  if (card._audio) card._audio.srcObject = null;
  card._connected = false;
  refreshAudioButton(card);
}
function restartAudioSessions() {
  if (socket?.readyState !== WebSocket.OPEN || !capabilities.includes("audio")) return;
  for (const [rxId, card] of cards) {
    stopAudio(card, rxId);
    startAudio(card, rxId);
  }
}
function stopMicrophone() {
  micGeneration++;
  micReady = false;
  clearInterval(micMeterTimer); micMeterTimer = null;
  if (micStream) for (const track of micStream.getTracks()) track.stop();
  micStream = null; micAnalyser = null;
  if (micContext) micContext.close().catch(() => {});
  micContext = null; micLevel.value = 0; micLevelWrap.hidden = true;
  restartAudioSessions();
}
async function startMicrophone() {
  const generation = ++micGeneration;
  micReady = false;
  if (radioState) renderTx(radioState);
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: {
      channelCount: 1, echoCancellation: false, noiseSuppression: false,
      autoGainControl: false
    }});
    if (!leaseHeld || generation !== micGeneration) {
      for (const track of stream.getTracks()) track.stop();
      return;
    }
    micStream = stream;
    micContext = new AudioContext();
    const source = micContext.createMediaStreamSource(stream);
    micAnalyser = micContext.createAnalyser();
    micAnalyser.fftSize = 512;
    source.connect(micAnalyser);
    const samples = new Float32Array(micAnalyser.fftSize);
    micMeterTimer = setInterval(() => {
      if (!micAnalyser) return;
      micAnalyser.getFloatTimeDomainData(samples);
      let sum = 0;
      for (const value of samples) sum += value * value;
      micLevel.value = Math.min(1, Math.sqrt(sum / samples.length) * 4);
    }, 100);
    micReady = true;
    restartAudioSessions();
    if (radioState) renderTx(radioState);
  } catch (error) {
    revision.textContent = `Microphone unavailable: ${error?.message || error}`;
    send({ type: "lease_release" });
    setLeaseHeld(false);
  }
}
function startAudio(card, rxId) {
  // The server is the offerer; we create the connection, then ask for audio and
  // answer the offer it sends back over the WebSocket.
  const pc = new RTCPeerConnection();
  card._pc = pc;
  card._connected = false;
  refreshAudioButton(card);
  if (leaseHeld && micReady && micStream)
    for (const track of micStream.getAudioTracks()) pc.addTrack(track, micStream);
  pc.addEventListener("track", event => {
    card._audio.srcObject = event.streams[0];
    card._audio.muted = card._muted;
    tryPlay(card);
  });
  pc.addEventListener("icecandidate", event => {
    if (event.candidate)
      send({ type: "rtc_candidate", rx: rxId, candidate: event.candidate.candidate, mid: event.candidate.sdpMid || "" });
  });
  pc.addEventListener("connectionstatechange", () => {
    if (!card._pc) return;
    if (pc.connectionState === "connected") { card._connected = true; refreshAudioButton(card); tryPlay(card); }
    else if (["failed", "disconnected", "closed"].includes(pc.connectionState)) { card._connected = false; refreshAudioButton(card); }
    if (radioState) renderTx(radioState);
  });
  send({ type: "audio_subscribe", rx: rxId });
}
function toggleMute(card) {
  card._muted = !card._muted;
  if (card._audio) {
    card._audio.muted = card._muted;
    // Unmuting is a user gesture, so a stream parked by a blocked autoplay can
    // start now.
    if (!card._muted) tryPlay(card);
  }
  refreshAudioButton(card);
}
async function handleRtcOffer(rxId, sdp) {
  const card = cards.get(rxId);
  if (!card || !card._pc || !sdp) return;
  try {
    await card._pc.setRemoteDescription({ type: "offer", sdp });
    const answer = await card._pc.createAnswer();
    await card._pc.setLocalDescription(answer);
    send({ type: "rtc_answer", rx: rxId, sdp: card._pc.localDescription.sdp });
  } catch { card._connected = false; refreshAudioButton(card); }
}
async function handleRtcCandidate(rxId, candidate, mid) {
  const card = cards.get(rxId);
  if (!card || !card._pc || !candidate) return;
  try { await card._pc.addIceCandidate({ candidate, sdpMid: mid || null }); } catch {}
}
function closeAllAudio() {
  for (const card of cards.values()) {
    if (card._pc) { try { card._pc.close(); } catch {} card._pc = null; }
    if (card._audio) card._audio.srcObject = null;
    card._connected = false;
    refreshAudioButton(card);
  }
}

// The server pushes `state` on change plus a steady meter refresh, so the page
// no longer polls.  This watchdog is the fallback the server contract keeps:
// if no state arrives for a few refresh intervals the link has gone quiet, so
// re-request a snapshot before giving up on it.
function armStateWatchdog() {
  clearTimeout(stateWatchdog);
  stateWatchdog = setTimeout(() => {
    if (socket?.readyState === WebSocket.OPEN) { send({ type: "get_state" }); armStateWatchdog(); }
  }, 2000);
}

function connect() {
  clearTimeout(reconnectTimer); clearTimeout(stateWatchdog); setConnection("Connecting…");
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${scheme}://${location.host}/ws`, "machpsdr.v1");
  socket.binaryType = "arraybuffer";
  socket.addEventListener("open", () => {
    socketGeneration++; setConnection("Connected", true); send({ type: "get_state" }); armStateWatchdog();
  });
  socket.addEventListener("message", event => {
    if (typeof event.data !== "string") { handleBinary(event.data); return; }
    let message; try { message = JSON.parse(event.data); } catch { return; }
    if (message.type === "hello") {
      capabilities = message.capabilities || [];
      updateAudioAvailability();
      if (radioState) renderTx(radioState);
    }
    if (message.type === "lease_state") setLeaseHeld(!!message.held);
    if (message.type === "state") { render(message); armStateWatchdog(); }
    if (message.type === "spectrum_status" || message.type === "spectrum_subscription") updateSpectrumStatus(message);
    if (message.type === "rtc_offer") handleRtcOffer(message.rx, message.sdp);
    if (message.type === "rtc_candidate") handleRtcCandidate(message.rx, message.candidate, message.mid);
    // The local RTCPeerConnection's connectionstatechange drives the button; the
    // server's rtc_state is just a log line here.
    if (message.type === "error") {
      if (["lease_lost", "lease_required", "not_armed"].includes(message.code)) setLeaseHeld(false);
      revision.textContent = `Command failed: ${message.message}`;
    }
  });
  socket.addEventListener("close", () => {
    clearTimeout(stateWatchdog); setLeaseHeld(false); setConnection("Disconnected"); closeAllAudio(); reconnectTimer = setTimeout(connect, 1500);
  });
  socket.addEventListener("error", () => socket.close());
}

mox.addEventListener("click", () => {
  if (radioState?.can_transmit && leaseHeld) command("set_mox", { value: !radioState.mox });
});
txControl.addEventListener("click", () => {
  send({ type: leaseHeld ? "lease_release" : "lease_acquire" });
});
// One tap unlocks audio for the whole tab, then the prompt never returns.
enableSound.addEventListener("click", () => {
  audioUnlocked = true;
  enableSound.hidden = true;
  for (const card of cards.values()) if (card._audio && !card._muted) card._audio.play().catch(() => {});
});
connect();
