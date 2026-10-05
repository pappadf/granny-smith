// The audio-out worklet's ring logic (src/audio/audioRing.ts) against a ring
// laid out in a plain SharedArrayBuffer the way em_audio.c lays it out.  The
// test plays the emulator: it writes frames and advances WRITE, and never
// touches READ -- the consumer is READ's only writer.

import { describe, it, expect } from 'vitest';
import { AudioRingConsumer } from '@/audio/audioRing';
import * as L from '@/bus/shmLayout';

const PTR = 256; // where the block sits in the "heap"
const FRAMES = 4096;
const RATE = 48000;

function makeRing() {
  const sab = new SharedArrayBuffer(PTR + L.ARING_HDR_BYTES + FRAMES * 2 * 2);
  const hdr = new Int32Array(sab, PTR, 16);
  hdr[L.ARING_W_MAGIC] = L.ARING_MAGIC | 0;
  hdr[L.ARING_W_VERSION] = L.ARING_VERSION;
  hdr[L.ARING_W_DATA_OFF] = L.ARING_HDR_BYTES;
  hdr[L.ARING_W_FRAMES] = FRAMES;
  hdr[L.ARING_W_VOL] = 7;
  hdr[L.ARING_W_OWNER] = 1;
  const data = new Int16Array(sab, PTR + L.ARING_HDR_BYTES, FRAMES * 2);
  // The producer: a square wave, so the DC blocker leaves something audible.
  let phase = 0;
  const push = (n: number) => {
    let w = hdr[L.ARING_W_WRITE] >>> 0;
    for (let i = 0; i < n; i++, w = (w + 1) >>> 0) {
      const s = (phase++ >> 4) & 1 ? 8000 : -8000;
      data[(w & (FRAMES - 1)) * 2] = s;
      data[(w & (FRAMES - 1)) * 2 + 1] = s;
    }
    Atomics.store(hdr, L.ARING_W_WRITE, w | 0);
    Atomics.add(hdr, L.ARING_W_PUSH_SEQ, 1);
  };
  const consumer = (owner = 1) =>
    new AudioRingConsumer(sab, PTR, {
      srcRate: RATE,
      dstRate: RATE,
      channels: 2,
      targetLatency: 0.01, // 480 frames
      owner,
    });
  const read = () => hdr[L.ARING_W_READ] >>> 0;
  const write = () => hdr[L.ARING_W_WRITE] >>> 0;
  return { sab, hdr, push, consumer, read, write };
}

const quantum = () => [new Float32Array(128), new Float32Array(128)];
const energy = (out: Float32Array[]) => out[0].reduce((a, x) => a + x * x, 0);

describe('AudioRingConsumer', () => {
  it('refuses a block it does not recognise', () => {
    const r = makeRing();
    r.hdr[L.ARING_W_VERSION] = 99;
    expect(() => r.consumer()).toThrow(/layout/);
  });

  it('holds the start gate until the target depth, then plays', () => {
    const r = makeRing();
    const c = r.consumer();
    r.push(100);
    const out = quantum();
    expect(c.process(out)).toBe(true);
    expect(energy(out)).toBe(0);
    // A consumer that did not exist when those frames were queued starts at
    // WRITE: what was queued before it is stale.
    expect(r.read()).toBe(r.write());
    r.push(600);
    c.process(out);
    expect(r.read()).toBeGreaterThan(r.write() - 600);
    expect(energy(out)).toBeGreaterThan(0);
  });

  it('is the only writer of READ and advances it at the stream rate', () => {
    const r = makeRing();
    const c = r.consumer();
    c.process(quantum()); // the initial flush
    r.push(1000);
    const before = r.read();
    for (let q = 0; q < 4; q++) {
      r.push(128);
      c.process(quantum());
    }
    const moved = (r.read() - before) >>> 0;
    expect(moved).toBeGreaterThanOrEqual(4 * 128 - 4);
    expect(moved).toBeLessThanOrEqual(4 * 128 + 4);
  });

  it('keeps its place across the uint32 wrap of both indices', () => {
    const r = makeRing();
    const c = r.consumer();
    const near = 0xffffff00 | 0;
    r.hdr[L.ARING_W_WRITE] = near;
    r.hdr[L.ARING_W_READ] = near;
    c.process(quantum());
    r.push(1000); // WRITE wraps past zero
    expect(r.write()).toBeLessThan(0x1000);
    let played = 0;
    for (let q = 0; q < 4; q++) {
      const out = quantum();
      c.process(out);
      played += energy(out);
    }
    expect(played).toBeGreaterThan(0);
    const depth = (r.write() - r.read()) >>> 0;
    expect(depth).toBeGreaterThan(1000 - 4 * 128 - 8);
    expect(depth).toBeLessThan(1000 - 4 * 128 + 8);
  });

  it('resyncs when the producer has lapped it', () => {
    const r = makeRing();
    const c = r.consumer();
    c.process(quantum());
    r.push(FRAMES + 1000); // overwrote the oldest frames
    c.process(quantum());
    const depth = (r.write() - r.read()) >>> 0;
    expect(depth).toBeLessThanOrEqual(480);
    expect(depth).toBeGreaterThan(480 - 140);
  });

  it('drops the backlog when the producer asks for a new stream', () => {
    const r = makeRing();
    const c = r.consumer();
    c.process(quantum());
    r.push(2000);
    c.process(quantum());
    expect(r.read()).not.toBe(r.write());
    Atomics.add(r.hdr, L.ARING_W_RESET_GEN, 1);
    const out = quantum();
    c.process(out);
    expect(r.read()).toBe(r.write());
    expect(energy(out)).toBe(0); // re-gated
  });

  it('a rate change flushes and re-gates', () => {
    const r = makeRing();
    const c = r.consumer();
    c.process(quantum());
    r.push(2000);
    c.process(quantum());
    c.setRate(24000);
    c.process(quantum());
    expect(r.read()).toBe(r.write());
  });

  it('a replaced worklet stops and leaves READ alone', () => {
    const r = makeRing();
    const old = r.consumer(1);
    old.process(quantum());
    r.push(2000);
    r.hdr[L.ARING_W_OWNER] = 2;
    const before = r.read();
    expect(old.process(quantum())).toBe(false);
    expect(r.read()).toBe(before);
    const next = r.consumer(2);
    expect(next.process(quantum())).toBe(true);
  });

  it('retire() stops it too', () => {
    const r = makeRing();
    const c = r.consumer();
    c.retire();
    expect(c.process(quantum())).toBe(false);
  });

  it('reports the fill level every 32 quanta', () => {
    const r = makeRing();
    const c = r.consumer();
    c.process(quantum());
    for (let q = 0; q < 32; q++) {
      r.push(128);
      c.process(quantum());
    }
    expect(r.hdr[L.ARING_W_FILL_PM]).toBeGreaterThan(0);
  });
});
