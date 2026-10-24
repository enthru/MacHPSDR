"use strict";

// MacHPSDR browser-remote service worker: an installable shell, never a cache of
// the radio.  Only the static files below are ever stored.  The WebSocket (all
// control, state and spectrum) and WebRTC media never pass through fetch, so they
// cannot be cached here; a page started from this cache shows "Disconnected"
// until the radio answers, and re-authenticates and re-synchronises as always.
const CACHE = "machpsdr-shell-v1";
const SHELL = ["/", "/app.css", "/app.js", "/manifest.webmanifest", "/icon-192.png", "/icon-512.png"];

self.addEventListener("install", event => {
  // A failed precache (e.g. the radio went away mid-install) must not fail the
  // install: the cache fills itself on the next online load.
  event.waitUntil(caches.open(CACHE).then(cache => cache.addAll(SHELL)).catch(() => {}).then(() => self.skipWaiting()));
});

self.addEventListener("activate", event => {
  event.waitUntil(caches.keys()
    .then(keys => Promise.all(keys.filter(key => key !== CACHE).map(key => caches.delete(key))))
    .then(() => self.clients.claim()));
});

self.addEventListener("fetch", event => {
  const request = event.request;
  if (request.method !== "GET") return;
  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return;
  if (request.mode === "navigate") {
    // Online, a navigation goes straight to the radio so its HTTP Basic challenge
    // reaches the browser untouched; the cached page is only for a device that
    // has no network at all.
    if (self.navigator.onLine) return;
    event.respondWith(caches.match("/").then(hit => hit || Response.error()));
    return;
  }
  if (!SHELL.includes(url.pathname) || url.search) return;
  // Network first, so an updated app.js takes effect on the next load; the copy
  // is a fallback, refreshed only from a successful (authenticated) answer.
  event.respondWith(fetch(request).then(response => {
    if (response.ok) {
      const copy = response.clone();
      caches.open(CACHE).then(cache => cache.put(url.pathname, copy)).catch(() => {});
    }
    return response;
  }).catch(() => caches.match(url.pathname).then(hit => hit || Response.error())));
});
