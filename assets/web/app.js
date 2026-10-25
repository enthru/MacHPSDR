"use strict";

const modes = ["LSB", "USB", "DSB", "CWL", "CWU", "FMN", "AM", "DIGU", "SPEC", "DIGL", "SAM", "DRM", "WFM"];
const cards = new Map();
let socket, reconnectTimer, stateWatchdog, leaseRenewTimer, radioState;
let nextId = 1, socketGeneration = 0;
let capabilities = [];
let leaseHeld = false;
// Phase 5 roles.  role comes from the login (hello); a "view" login can never
// control.  controlHeld is the one-controller lease among "control" logins.
let role = "control", controlHeld = false, controlRenewTimer;
let micStream = null, micContext = null, micAnalyser = null, micMeterTimer = null;
let micReady = false, micGeneration = 0;
const connection = document.querySelector("#connection");
const receivers = document.querySelector("#receivers");
const empty = document.querySelector("#empty");
const template = document.querySelector("#receiver-template");
const mox = document.querySelector("#mox");
const tune = document.querySelector("#tune");
const txControl = document.querySelector("#tx-control");
const controlBtn = document.querySelector("#control");
const txSettings = document.querySelector("#tx-settings");
const drive = document.querySelector("#drive"), tuneDrive = document.querySelector("#tune-drive");
const micGain = document.querySelector("#mic-gain"), compLevel = document.querySelector("#comp-level");
const compressor = document.querySelector("#compressor"), leveler = document.querySelector("#leveler");
const cessb = document.querySelector("#cessb");
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
// The readout's 13 characters, as the desktop's freq_digit_at() indexes them:
// the place value of each digit, 0 for a separator.
const FREQ_PLACES = [1e10, 1e9, 1e8, 1e7, 1e6, 0, 1e5, 1e4, 1e3, 0, 100, 10, 1];
const FREQ_MAX = 99999999999;
// The digit typing targets: the one under the pointer, then (after each typed
// digit) the next to its right, as the desktop's vfo_type_digit() does.
// {card, index}, or null once the pointer leaves the readout.
let digitCursor = null;
function setDigitCursor(card, index) {
  document.querySelectorAll(".freq-digit.digit-cursor").forEach(el => el.classList.remove("digit-cursor"));
  digitCursor = card && index >= 0 ? { card, index } : null;
  if (digitCursor) card.querySelectorAll(".freq-a > span")[index].classList.add("digit-cursor");
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
function controlLeased() { return capabilities.includes("control_lease"); }
// Without the lease (or on a server that predates it) nothing here may send a
// radio-changing command; MOX/Tune OFF stays open to every control login as the
// emergency unkey, exactly as the server's access table has it.
function canControl() { return !controlLeased() || controlHeld; }
function setControlHeld(held) {
  controlHeld = held;
  clearTimeout(controlRenewTimer);
  if (held && socket?.readyState === WebSocket.OPEN)
    controlRenewTimer = setTimeout(() => send({ type: "control_renew" }), 2000);
  // The server drops the TX lease with control and need not say so separately.
  if (!held && leaseHeld) setLeaseHeld(false);
  renderControl();
}
function renderControl() {
  const leased = controlLeased();
  controlBtn.hidden = !leased;
  const busy = !controlHeld && !!radioState?.control_active;
  controlBtn.classList.toggle("held", controlHeld);
  controlBtn.classList.toggle("busy", busy && role === "control");
  controlBtn.disabled = role !== "control";
  controlBtn.textContent = role !== "control" ? "View only" :
                           controlHeld ? "In control" : busy ? "Take over" : "Take control";
  controlBtn.title = role !== "control" ? "This login can watch and listen, never control" :
                     controlHeld ? "This browser controls the radio — click to hand control back" :
                     busy ? "Another browser has control — click to take it over (it stops transmitting)" :
                            "Take control of the radio";
  const locked = !canControl();
  for (const card of cards.values()) {
    card.classList.toggle("no-control", locked);
    for (const el of card.querySelectorAll("button, select, input")) {
      // Mute and the spectrum profile are this browser's own; dead chips stay dead.
      if (el.classList.contains("audio-toggle") || el.classList.contains("spectrum-profile") ||
          el.classList.contains("dead")) continue;
      el.disabled = locked;
    }
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
  const viewer = role !== "control";
  txControl.disabled = !radio.can_transmit || busy || !canControl();
  txControl.classList.toggle("held", leaseHeld);
  txControl.textContent = leaseHeld ? (micReady ? "Release TX control" : "Starting microphone…") :
                          (busy ? "TX control in use" : "Take TX control");
  mox.disabled = viewer || !radio.can_transmit || (!radio.mox && (!leaseHeld || !txMediaReady(radio)));
  mox.classList.toggle("on", radio.mox);
  mox.textContent = radio.mox ? "MOX ON" : "MOX";
  tune.disabled = viewer || !radio.can_transmit || (!leaseHeld && !radio.tune);
  tune.classList.toggle("on", !!radio.tune);
  tune.textContent = radio.tune ? "TUNE ON" : "TUNE";
  for (const input of txSettings.querySelectorAll("input")) input.disabled = !radio.can_transmit || !leaseHeld;
  const setTxRange = (input, valueId, value) => {
    if (document.activeElement !== input) input.value = String(value ?? 0);
    document.querySelector(valueId).textContent = input.value;
  };
  setTxRange(drive,"#drive-value",radio.drive);
  setTxRange(tuneDrive,"#tune-drive-value",radio.tune_drive);
  setTxRange(micGain,"#mic-gain-value",radio.mic_gain);
  setTxRange(compLevel,"#comp-level-value",radio.comp_level);
  if (document.activeElement !== compressor) compressor.checked = !!radio.compressor;
  if (document.activeElement !== leveler) leveler.checked = !!radio.leveler;
  if (document.activeElement !== cessb) cessb.checked = !!radio.cessb;
  remotePtt.hidden = !radio.remote_ptt;
  micLevelWrap.hidden = !leaseHeld;
}
function command(action, fields) {
  const unkey = (action === "set_mox" || action === "set_tune") && fields?.value === false;
  if (!canControl() && !(unkey && role === "control")) return;
  send({ type: "command", action, ...fields });
}
function subscribe(card, rx) {
  if (card.dataset.subscription === String(socketGeneration)) return;
  card.dataset.subscription = String(socketGeneration);
  send({ type: "spectrum_subscribe", rx, profile: card.querySelector(".spectrum-profile").value });
}
// The browser twin of the desktop's scroll_notches(): a wheel event becomes at
// most one notch.  A mouse detent (line/page mode, or a legacy wheelDelta that
// is a whole multiple of 120) is one notch as before.  A trackpad delivers dozens
// of small pixel deltas per swipe plus momentum, so those are accumulated on the
// element and a notch is emitted each WHEEL_PIXEL_STEP; whole notches beyond one
// are discarded rather than banked, and a reversal starts afresh.  Returns +1 for
// up/away (deltaY < 0), -1 for down, 0 for no notch (incl. a horizontal swipe).
const WHEEL_PIXEL_STEP = 16;
function wheelNotches(el, event) {
  const dy = event.deltaY;
  if (!dy) return 0;
  const legacy = Math.abs(event.wheelDeltaY || 0);
  if (event.deltaMode !== 0 || (legacy >= 120 && legacy % 120 === 0)) {
    el._wheelAcc = 0;
    return dy < 0 ? 1 : -1;
  }
  let acc = el._wheelAcc || 0;
  if ((acc > 0 && dy < 0) || (acc < 0 && dy > 0)) acc = 0;
  acc += dy;
  if (Math.abs(acc) < WHEEL_PIXEL_STEP) { el._wheelAcc = acc; return 0; }
  el._wheelAcc = acc % WHEEL_PIXEL_STEP;
  return acc < 0 ? 1 : -1;
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
  const stepBy = steps => {
    if (!canControl()) return;          // no optimistic retune without control
    const value = Math.max(1, card._rx.frequency + steps * Math.max(1, card._rx.step));
    card._rx.frequency = value;
    command("set_frequency", { rx: rxId, value });
  };
  canvas.addEventListener("wheel", event => {
    if (!card._rx) return;
    event.preventDefault();
    const notch = wheelNotches(canvas, event);
    if (notch) stepBy(notch);
  }, { passive: false });
  // Keyboard twin of the wheel: left/right one step, Shift for ten.
  canvas.addEventListener("keydown", event => {
    if (!card._rx || (event.key !== "ArrowLeft" && event.key !== "ArrowRight")) return;
    event.preventDefault();
    stepBy((event.key === "ArrowRight" ? 1 : -1) * (event.shiftKey ? 10 : 1));
  });
}

function showFrequency(card, hz) {
  const text = formatFreq(hz);
  card.querySelectorAll(".freq-a > span").forEach((span, i) => {
    if (span.textContent !== text[i]) span.textContent = text[i];
  });
}
// A digit key while the pointer is over a VFO digit types into it -- unless the
// viewer is typing into a field of the page.
document.addEventListener("keydown", event => {
  if (!digitCursor || event.ctrlKey || event.metaKey || event.altKey) return;
  if (!/^[0-9]$/.test(event.key)) return;
  const target = event.target;
  if (target instanceof HTMLInputElement || target instanceof HTMLSelectElement ||
      target instanceof HTMLTextAreaElement || target?.isContentEditable) return;
  if (!digitCursor.card.isConnected || digitCursor.card.querySelector(".freq-a").hidden) {
    setDigitCursor(null, -1); return;
  }
  event.preventDefault();
  digitCursor.card._typeDigit(digitCursor.index, Number(event.key));
});

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
    setDigitCursor(null, -1);
    editor.value = String(card._rx?.frequency ?? "");
    display.hidden = true; editor.hidden = false; editor.focus(); editor.select();
  };
  const closeEditor = () => { editor.hidden = true; display.hidden = false; };
  const applyEditor = () => {
    const value = Number(editor.value.replace(/[^0-9]/g, ""));
    if (Number.isSafeInteger(value) && value > 0) command("set_frequency", { rx: rx.id, value });
    closeEditor();
  };
  // Per-digit tuning, the desktop VFO's twin: one span per readout character
  // (render() rewrites their text). The wheel over a digit tunes by its place
  // value; a typed 0-9 overwrites the digit under the pointer and moves one to
  // the right. Both send an absolute set_frequency, so ctun/freetune and the VFO
  // lock are the server's business exactly as for the type-in editor.
  FREQ_PLACES.forEach(place => {
    const span = document.createElement("span");
    span.textContent = place ? "0" : ".";
    if (place) span.className = "freq-digit";
    display.append(span);
  });
  const digitSpans = [...display.children];
  const retune = value => {
    if (!card._rx || card._rx.locked || !canControl()) return false;
    value = Math.max(1, Math.min(FREQ_MAX, value));
    if (value === card._rx.frequency) return false;
    card._rx.frequency = value;           // optimistic, so a burst compounds
    showFrequency(card, value);
    command("set_frequency", { rx: rx.id, value });
    return true;
  };
  display.addEventListener("mouseover", event => {
    const i = digitSpans.indexOf(event.target);
    if (i >= 0 && FREQ_PLACES[i]) setDigitCursor(card, i);
  });
  display.addEventListener("mouseleave", () => { if (digitCursor?.card === card) setDigitCursor(null, -1); });
  display.addEventListener("wheel", event => {
    const i = digitSpans.indexOf(event.target);
    if (i < 0 || !FREQ_PLACES[i] || !card._rx) return;
    event.preventDefault();
    let notch = wheelNotches(display, event);
    if (radioState?.vfo_scroll_reversed) notch = -notch;
    if (notch) retune(card._rx.frequency + notch * FREQ_PLACES[i]);
  }, { passive: false });
  card._typeDigit = (index, n) => {
    if (!card._rx) return;
    const place = FREQ_PLACES[index];
    const current = Math.floor(card._rx.frequency / place) % 10;
    retune(card._rx.frequency + (n - current) * place);
    let next = index + 1;
    while (next < FREQ_PLACES.length && !FREQ_PLACES[next]) next++;
    if (next < FREQ_PLACES.length) setDigitCursor(card, next);
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
  // Band: a dropdown of the bands the desktop menu offers (set_band, which
  // refuses a locked VFO), and the split toggle.
  const bandSel = card.querySelector(".band-sel");
  bandSel.addEventListener("change", () => command("set_band", { rx: rx.id, value: Number(bandSel.value) }));
  card.querySelector(".split-chip").addEventListener("click", () => command("set_split", { rx: rx.id, value: card._rx?.split ? 0 : 1 }));
  // RIT/XIT: click toggles on/off, wheel (or up/down arrows when focused) nudges
  // the offset by 10 Hz (the desktop's rit_step), clamped server-side to +/-10 kHz.
  const offsetChip = (sel, enableAction, valueAction, field, onField) => {
    const el = card.querySelector(sel);
    const nudge = up => {
      const next = (Number(card._rx?.[field]) || 0) + (up ? 10 : -10);
      command(valueAction, { rx: rx.id, value: Math.max(-10000, Math.min(10000, next)) });
    };
    el.addEventListener("click", () => command(enableAction, { rx: rx.id, value: card._rx?.[onField] ? 0 : 1 }));
    el.addEventListener("wheel", event => {
      event.preventDefault();
      const notch = wheelNotches(el, event);
      if (notch) nudge(notch > 0);
    }, { passive: false });
    el.addEventListener("keydown", event => {
      if (event.key !== "ArrowUp" && event.key !== "ArrowDown") return;
      event.preventDefault(); nudge(event.key === "ArrowUp");
    });
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
    if (editor.hidden) showFrequency(card, rx.frequency);
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
    // Band dropdown: rebuilt when the offered list changes (a transverter row
    // named or cleared), then set to this receiver's band.
    const bandSel = card.querySelector(".band-sel");
    const bands = state.radio.bands;
    if (Array.isArray(bands)) {
      const key = bands.map(b => b.i + "=" + b.n).join("|");
      if (bandSel._key !== key) {
        bandSel._key = key;
        bandSel.textContent = "";
        bands.forEach(b => bandSel.add(new Option(b.n, b.i)));
      }
    }
    if (rx.band_id !== undefined && document.activeElement !== bandSel) {
      // A band outside the list (never expected) shows its name rather than a
      // wrong row.
      if (![...bandSel.options].some(o => o.value === String(rx.band_id)))
        bandSel.add(new Option(rx.band || "?", rx.band_id));
      bandSel.value = String(rx.band_id);
    }
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
  renderControl();                       /* also renders the TX bar */
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
  if (!card._connected) { button.disabled = true; button.textContent = "⏳"; button.setAttribute("aria-pressed", "false"); button.title = "Connecting audio…"; button.setAttribute("aria-label", button.title); return; }
  button.disabled = false;
  button.textContent = card._muted ? "🔇" : "🔈";
  button.title = card._muted ? "Unmute received audio" : "Mute received audio";
  button.setAttribute("aria-label", button.title);
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
      role = message.role === "view" ? "view" : "control";
      updateAudioAvailability();
      // A control login takes control on connect if nobody has it; if someone
      // does, the reply is control_busy and the button offers a takeover.
      if (controlLeased() && role === "control") send({ type: "control_acquire" });
      renderControl();
    }
    if (message.type === "lease_state") setLeaseHeld(!!message.held);
    if (message.type === "control_state") setControlHeld(!!message.held);
    if (message.type === "state") { render(message); armStateWatchdog(); }
    if (message.type === "spectrum_status" || message.type === "spectrum_subscription") updateSpectrumStatus(message);
    if (message.type === "rtc_offer") handleRtcOffer(message.rx, message.sdp);
    if (message.type === "rtc_candidate") handleRtcCandidate(message.rx, message.candidate, message.mid);
    // The local RTCPeerConnection's connectionstatechange drives the button; the
    // server's rtc_state is just a log line here.
    if (message.type === "error") {
      if (["lease_lost", "lease_required", "not_armed"].includes(message.code)) setLeaseHeld(false);
      if (["control_busy", "control_lost", "control_required"].includes(message.code)) setControlHeld(false);
      // A busy controller at connect is the normal case for a second browser,
      // shown by the button; it is not a failed command.
      if (message.code !== "control_busy") revision.textContent = `Command failed: ${message.message}`;
    }
  });
  socket.addEventListener("close", () => {
    clearTimeout(stateWatchdog); setLeaseHeld(false); setControlHeld(false); setConnection(navigator.onLine ? "Disconnected" : "Offline"); closeAllAudio(); reconnectTimer = setTimeout(connect, 1500);
  });
  socket.addEventListener("error", () => socket.close());
}

mox.addEventListener("click", () => {
  if (radioState?.can_transmit && (leaseHeld || radioState.mox)) command("set_mox", { value: !radioState.mox });
});
tune.addEventListener("click", () => {
  if (radioState?.can_transmit && (leaseHeld || radioState.tune)) command("set_tune", { value: !radioState.tune });
});
for (const [input, valueId, action] of [
  [drive,"#drive-value","set_drive"], [tuneDrive,"#tune-drive-value","set_tune_drive"],
  [micGain,"#mic-gain-value","set_mic_gain"], [compLevel,"#comp-level-value","set_comp_level"]
]) {
  input.addEventListener("input", () => { document.querySelector(valueId).textContent = input.value; });
  input.addEventListener("change", () => { if (leaseHeld) command(action,{ value: Number(input.value) }); });
}
for (const [input, action] of [[compressor,"set_compressor"],[leveler,"set_leveler"],[cessb,"set_cessb"]])
  input.addEventListener("change", () => { if (leaseHeld) command(action,{ value: input.checked ? 1 : 0 }); });
txControl.addEventListener("click", () => {
  if (!canControl()) return;
  send({ type: leaseHeld ? "lease_release" : "lease_acquire" });
});
controlBtn.addEventListener("click", () => {
  if (role !== "control") return;
  if (controlHeld) send({ type: "control_release" });
  else send({ type: "control_acquire", force: !!radioState?.control_active });
});
// One tap unlocks audio for the whole tab, then the prompt never returns.
enableSound.addEventListener("click", () => {
  audioUnlocked = true;
  enableSound.hidden = true;
  for (const card of cards.values()) if (card._audio && !card._muted) card._audio.play().catch(() => {});
});
connect();
// Installable shell (manifest + sw.js).  Browsers allow a service worker only in
// a secure context -- HTTPS with a certificate they trust, or localhost -- so on
// plain-HTTP LAN or a self-signed certificate this quietly does nothing and the
// page works exactly as before.
if ("serviceWorker" in navigator && window.isSecureContext)
  navigator.serviceWorker.register("/sw.js").catch(error => console.info("MacHPSDR: no offline shell:", error.message));
