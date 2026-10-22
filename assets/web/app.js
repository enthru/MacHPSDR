"use strict";

const modes = ["LSB", "USB", "DSB", "CWL", "CWU", "FMN", "AM", "DIGU", "SPEC", "DIGL", "SAM", "DRM", "WFM"];
const cards = new Map();
let socket;
let reconnectTimer;
let pollTimer;
let nextId = 1;
let radioState;

const connection = document.querySelector("#connection");
const receivers = document.querySelector("#receivers");
const empty = document.querySelector("#empty");
const template = document.querySelector("#receiver-template");
const mox = document.querySelector("#mox");
const revision = document.querySelector("#revision");

function setConnection(text, online = false) {
  connection.textContent = text;
  connection.classList.toggle("offline", !online);
}

function send(message) {
  if (socket?.readyState !== WebSocket.OPEN) return false;
  socket.send(JSON.stringify({ id: nextId++, ...message }));
  return true;
}

function command(action, fields) {
  send({ type: "command", action, ...fields });
}

function makeCard(rx) {
  const card = template.content.firstElementChild.cloneNode(true);
  card.dataset.rx = rx.id;
  card.querySelector(".rx-name").textContent = `RX ${rx.id + 1}`;
  const select = card.querySelector(".mode");
  modes.forEach((name, value) => select.add(new Option(name, value)));
  select.addEventListener("change", () => command("set_mode", { rx: rx.id, value: Number(select.value) }));
  const input = card.querySelector(".frequency");
  const tune = () => {
    const value = Number(input.value.replace(/[^0-9]/g, ""));
    if (Number.isSafeInteger(value) && value > 0) command("set_frequency", { rx: rx.id, value });
  };
  card.querySelector(".apply-frequency").addEventListener("click", tune);
  input.addEventListener("keydown", event => { if (event.key === "Enter") tune(); });
  receivers.append(card);
  cards.set(rx.id, card);
  return card;
}

function render(state) {
  radioState = state.radio;
  const live = new Set(state.receivers.map(rx => rx.id));
  for (const [id, card] of cards) if (!live.has(id)) { card.remove(); cards.delete(id); }
  for (const rx of state.receivers) {
    const card = cards.get(rx.id) || makeCard(rx);
    card.classList.toggle("active", state.radio.active_rx === rx.id);
    const input = card.querySelector(".frequency");
    if (document.activeElement !== input) input.value = String(rx.frequency);
    card.querySelector(".mode").value = String(rx.mode);
    card.querySelector(".meter-text").textContent = `${rx.meter_db.toFixed(1)} dB`;
    const level = Math.max(0, Math.min(100, (rx.meter_db + 140) / 1.2));
    card.querySelector(".meter span").style.width = `${level}%`;
    card.querySelector(".lock-state").textContent = rx.locked ? "VFO locked" : "";
  }
  empty.hidden = state.receivers.length !== 0;
  mox.disabled = !state.radio.can_transmit;
  mox.classList.toggle("on", state.radio.mox);
  mox.textContent = state.radio.mox ? "MOX ON" : "MOX";
  revision.textContent = `State revision ${state.revision}`;
}

function connect() {
  clearTimeout(reconnectTimer);
  clearInterval(pollTimer);
  setConnection("Connecting…");
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${scheme}://${location.host}/ws`, "machpsdr.v1");
  socket.addEventListener("open", () => {
    setConnection("Connected", true);
    send({ type: "get_state" });
    pollTimer = setInterval(() => send({ type: "get_state" }), 500);
  });
  socket.addEventListener("message", event => {
    if (typeof event.data !== "string") return;
    let message;
    try { message = JSON.parse(event.data); } catch { return; }
    if (message.type === "state") render(message);
    if (message.type === "error") revision.textContent = `Command failed: ${message.message}`;
  });
  socket.addEventListener("close", () => {
    clearInterval(pollTimer);
    setConnection("Disconnected");
    reconnectTimer = setTimeout(connect, 1500);
  });
  socket.addEventListener("error", () => socket.close());
}

mox.addEventListener("click", () => {
  if (radioState?.can_transmit) command("set_mox", { value: !radioState.mox });
});

connect();
