// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The record ring the page's transports share -- the TypeScript mirror of
// src/core/mailbox/mailbox_ring.h.  A byte ring of RECORDS {u32 kind, u32
// len} + payload inside shared wasm memory, with the writer's published
// byte count (HEAD) and the reader's consumed count (TAIL) in a control
// block the other side reads with Atomics.  The platen interpreter worker
// reads and writes it, the Voodoo2 GPU worker reads it, and the JS<->core
// mailbox is built on it.
//
// The rules (mailbox_ring.h states and enforces the same ones):
//   * `len` counts the header and is a multiple of 8, so every record
//     starts 8-aligned and a PAD's header always fits before the end;
//   * a record never wraps: a PAD (kind R_PAD) reaches the end first;
//   * HEAD and TAIL are free-running (mod 2^32); `(head - tail) >>> 0` is
//     what is published and not yet consumed;
//   * a reader that sees a bad len stops trusting the ring.
// Waiting and wake-ups are the transport's business, not this file's.

export const HDR_BYTES = 8;
export const R_PAD = 0;

// Bytes a text field of `n` occupies inside a payload (padded to 4).
export function pad4(n: number): number {
  return (n + 3) & ~3;
}

// A record's total length for `n` bytes of header plus payload (padded to 8).
export function pad8(n: number): number {
  return (n + 7) & ~7;
}

// Little-endian u32 access into a byte view (what both sides agree on).
export function getU32(u8: Uint8Array, at: number): number {
  return (u8[at] | (u8[at + 1] << 8) | (u8[at + 2] << 16) | (u8[at + 3] << 24)) >>> 0;
}

export function putU32(u8: Uint8Array, at: number, v: number): void {
  u8[at] = v & 0xff;
  u8[at + 1] = (v >>> 8) & 0xff;
  u8[at + 2] = (v >>> 16) & 0xff;
  u8[at + 3] = (v >>> 24) & 0xff;
}

// One byte ring inside a buffer: `base` is its byte offset in `u8`, `size`
// a power of two.
export interface Ring {
  u8: Uint8Array;
  base: number;
  size: number;
}

// The total length of a record with `words` header words and text fields
// of the given lengths (each padded to 4; the whole padded to 8).
export function recordBytes(words: number, textLens: number[]): number {
  let body = 4 * words;
  for (const n of textLens) body += pad4(n);
  return pad8(HDR_BYTES + body);
}

// Writes one record at monotonic position `wr` given the reader's `tail`
// (both mod 2^32): a PAD first when the record would cross the ring's end,
// then the header, the words and the text fields.  Returns the new write
// position, or -1 when there is no room (nothing written).  The caller
// publishes the position (HEAD) afterwards.
export function ringWrite(
  ring: Ring,
  wr: number,
  tail: number,
  kind: number,
  words: number[],
  texts: Uint8Array[],
): number {
  const len = recordBytes(
    words.length,
    texts.map((t) => t.length),
  );
  const mask = ring.size - 1;
  let at = wr & mask;
  // A PAD to the end first when the record would cross it; every record
  // starts 8-aligned, so the PAD's own header always fits
  const pad = at + len > ring.size ? ring.size - at : 0;
  const used = (wr - tail) >>> 0;
  if (ring.size - used < pad + len) return -1;
  if (pad) {
    putU32(ring.u8, ring.base + at, R_PAD);
    putU32(ring.u8, ring.base + at + 4, pad);
    wr = (wr + pad) >>> 0;
    at = 0;
  }
  let p = ring.base + at;
  putU32(ring.u8, p, kind);
  putU32(ring.u8, p + 4, len);
  p += HDR_BYTES;
  for (const w of words) {
    putU32(ring.u8, p, w);
    p += 4;
  }
  for (const t of texts) {
    ring.u8.set(t, p);
    p += pad4(t.length);
  }
  return (wr + len) >>> 0;
}

// Is a record header sound?  `off` is its offset in the ring, `avail` the
// bytes published past it.  A len under 8, not a multiple of 8, crossing
// the ring's end, or beyond what was published means the writer is broken
// and nothing after it can be trusted.
export function recordOk(len: number, off: number, size: number, avail: number): boolean {
  return len >= HDR_BYTES && (len & 7) === 0 && off + len <= size && len <= avail;
}

// A record read from a ring: its kind, its total length, and the absolute
// byte offset of its payload in the ring's view.
export interface RecordHeader {
  kind: number;
  len: number;
  payload: number;
}

// Reads the record header at monotonic position `rd` with `head` bytes
// published.  Throws on a framing violation (see recordOk).
export function ringRead(ring: Ring, rd: number, head: number): RecordHeader {
  const at = rd & (ring.size - 1);
  const kind = getU32(ring.u8, ring.base + at);
  const len = getU32(ring.u8, ring.base + at + 4);
  if (!recordOk(len, at, ring.size, (head - rd) >>> 0))
    throw new Error(`corrupt ring record (kind ${kind} len ${len} at ${at})`);
  return { kind, len, payload: ring.base + at + HDR_BYTES };
}
