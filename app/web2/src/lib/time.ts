// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The page's date-and-time stamps.  Most become file names, so each format
// is fixed: change one only with the names it produces.

// Two digits, zero-padded.
export function pad2(n: number): string {
  return String(n).padStart(2, '0');
}

// Local "YYYYMMDD-HHMMSS": a download's name (saved states, the log).
export function compactLocalStamp(d: Date = new Date()): string {
  return (
    `${d.getFullYear()}${pad2(d.getMonth() + 1)}${pad2(d.getDate())}-` +
    `${pad2(d.getHours())}${pad2(d.getMinutes())}${pad2(d.getSeconds())}`
  );
}

// Local "YYYY-MM-DD HH:MM", with `seconds` ":SS" too: what the panels show.
export function localStamp(d: Date, seconds = false): string {
  const hm = `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  return (
    `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${hm}` +
    (seconds ? `:${pad2(d.getSeconds())}` : '')
  );
}

// Compact ISO 8601 UTC: "20260430T153045Z".
export function utcCompactStamp(d: Date = new Date()): string {
  return (
    `${d.getUTCFullYear()}${pad2(d.getUTCMonth() + 1)}${pad2(d.getUTCDate())}` +
    `T${pad2(d.getUTCHours())}${pad2(d.getUTCMinutes())}${pad2(d.getUTCSeconds())}Z`
  );
}
