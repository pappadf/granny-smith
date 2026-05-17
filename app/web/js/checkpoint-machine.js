// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Machine-id management for the per-machine checkpoint directory.
//
// The id+timestamp pair lives in localStorage and is rotated only on
// explicit "new machine" actions (config-dialog confirm or URL-driven media
// setup).  Page reloads preserve it.  The C side reads it via the
// `checkpoint --machine <id> <created>` shell command and uses it to root
// /opfs/checkpoints/<id>-<created>/ for state.checkpoint and image deltas.

const KEY = 'gs.checkpoint.machine';   // stored as JSON: { id, created }

// Generate 8 random bytes encoded as 16 hex chars.
function randomId() {
  const bytes = new Uint8Array(8);
  crypto.getRandomValues(bytes);
  return [...bytes].map(b => b.toString(16).padStart(2, '0')).join('');
}

// Compose a UTC timestamp in compact ISO 8601 form: "20260430T153045Z".
function nowStamp() {
  const d = new Date();
  const pad = n => String(n).padStart(2, '0');
  return `${d.getUTCFullYear()}${pad(d.getUTCMonth() + 1)}${pad(d.getUTCDate())}` +
         `T${pad(d.getUTCHours())}${pad(d.getUTCMinutes())}${pad(d.getUTCSeconds())}Z`;
}

// Read the existing machine record from localStorage, or mint one if absent.
// Always returns a `{ id, created }` object; never throws.
export function getOrCreateMachine() {
  const raw = localStorage.getItem(KEY);
  if (raw) {
    try {
      const parsed = JSON.parse(raw);
      if (parsed && parsed.id && parsed.created) return parsed;
    } catch (_) {}
  }
  const m = { id: randomId(), created: nowStamp() };
  localStorage.setItem(KEY, JSON.stringify(m));
  return m;
}

// Replace the persisted machine record with a fresh one and return it.
// Caller decides whether to also reload the page; in-process rotation is
// not supported by the C side (§3.1).
export function rotateMachine() {
  const m = { id: randomId(), created: nowStamp() };
  localStorage.setItem(KEY, JSON.stringify(m));
  return m;
}

// Rotate the persisted machine record and reload the page.  This is the
// only supported way to switch to a new machine — the C side does not
// support in-process rotation.  See §2.4 for the call sites.
export function rotateAndReload() {
  rotateMachine();
  location.reload();
}
