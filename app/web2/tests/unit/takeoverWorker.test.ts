// @vitest-environment node
//
// gpu/takeoverWorker.ts, the transport both WebGPU takeover workers share,
// driven without a GPU: a fake card, a hand-built control block and ring
// in a SharedArrayBuffer, records written with mailboxRing.ringWrite.  It
// checks the record order and framing, TAIL and ACK, FENCE and SHUTDOWN,
// a corrupt record, an attach that arrives before the device is ready,
// and the 'unavailable' reason.

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { ringWrite, type Ring } from '@/bus/mailboxRing';
import {
  C_MAGIC,
  C_VERSION,
  C_RING_OFF,
  C_RING_SIZE,
  C_RB_OFF,
  C_RB_SIZE,
  C_HEAD,
  C_TAIL,
  C_ACK,
  C_STATUS,
  STATUS_ATTACHED,
  STATUS_DETACHED,
} from '@/gpu/takeoverProtocol';
import type { TakeoverCard } from '@/gpu/takeoverWorker';

const MAGIC = 0x54455354; // 'TEST'
const VERSION = 7;
const R_A = 1;
const R_SLOW = 2; // the card answers with a promise (a readback)
const R_FENCE = 3;
const R_SHUTDOWN = 4;
const CTRL = 64; // control block address in the fake heap
const RING_OFF = 256; // ring, relative to the control block
const RING_SIZE = 1024;

let posted: Record<string, unknown>[] = [];
let onmessage: ((ev: { data: unknown }) => void) | null = null;

beforeEach(() => {
  posted = [];
  vi.resetModules();
  // The worker globals the transport uses.
  vi.stubGlobal('postMessage', (m: Record<string, unknown>) => posted.push(m));
  vi.stubGlobal('self', {
    set onmessage(f: (ev: { data: unknown }) => void) {
      onmessage = f;
    },
  });
});

afterEach(() => {
  vi.unstubAllGlobals();
});

// The control block a producer sets up before it asks for an attach.
function heap(): { sab: SharedArrayBuffer; ctrl: Int32Array; ring: Ring } {
  const sab = new SharedArrayBuffer(CTRL + RING_OFF + RING_SIZE + 256);
  const ctrl = new Int32Array(sab, CTRL, 32);
  ctrl[C_MAGIC] = MAGIC;
  ctrl[C_VERSION] = VERSION;
  ctrl[C_RING_OFF] = RING_OFF;
  ctrl[C_RING_SIZE] = RING_SIZE;
  ctrl[C_RB_OFF] = RING_OFF + RING_SIZE;
  ctrl[C_RB_SIZE] = 256;
  return { sab, ctrl, ring: { u8: new Uint8Array(sab), base: CTRL + RING_OFF, size: RING_SIZE } };
}

// A card that records what it was handed.
function fakeCard(opts: { ready?: Promise<boolean>; reason?: string } = {}) {
  const seen: number[][] = [];
  const calls: string[] = [];
  let u32: Uint32Array | null = null;
  let release: (() => void) | null = null;
  const card: TakeoverCard = {
    name: 'test-gpu',
    magic: MAGIC,
    version: VERSION,
    fence: R_FENCE,
    shutdown: R_SHUTDOWN,
    initDevice: () => opts.ready ?? Promise.resolve(true),
    unavailableReason: () => opts.reason ?? '',
    attached: () => calls.push('attached'),
    record(kind, p) {
      seen.push([kind, u32![p]]);
      if (kind === R_SLOW) return new Promise<void>((r) => (release = r));
      return kind === R_A ? undefined : false;
    },
    flush: () => calls.push('flush'),
    freeAll: () => calls.push('freeAll'),
    debugInfo: () => ({ things: seen.length }),
    statsInfo: () => ({ things: seen.length }),
  };
  return {
    card,
    seen,
    calls,
    setHeap: (sab: SharedArrayBuffer) => (u32 = new Uint32Array(sab)),
    release: () => release?.(),
  };
}

async function start(card: TakeoverCard): Promise<void> {
  const { runTakeoverWorker } = await import('@/gpu/takeoverWorker');
  runTakeoverWorker(card);
}

const send = (data: unknown) => onmessage!({ data });
const tick = () => new Promise((r) => setTimeout(r, 5));

// Publish records ([kind, ...words]) and wake the worker.
function publish(h: ReturnType<typeof heap>, wr: number, recs: number[][]): number {
  for (const [kind, ...words] of recs) {
    wr = ringWrite(h.ring, wr, Atomics.load(h.ctrl, C_TAIL) >>> 0, kind, words, []);
    expect(wr).toBeGreaterThanOrEqual(0);
  }
  Atomics.store(h.ctrl, C_HEAD, wr | 0);
  Atomics.notify(h.ctrl, C_HEAD);
  return wr;
}

describe('takeover worker transport', () => {
  it('consumes records in order, advances TAIL, acks FENCE and detaches on SHUTDOWN', async () => {
    const f = fakeCard();
    await start(f.card);
    const h = heap();
    f.setHeap(h.sab);
    send({ type: 'init', canvas: {} });
    send({ type: 'attach', memory: h.sab, ctrl: CTRL });
    await tick();
    expect(posted[0]).toEqual({ type: 'ready' });
    expect(Atomics.load(h.ctrl, C_STATUS)).toBe(STATUS_ATTACHED);
    expect(f.calls).toEqual(['attached']);

    let wr = publish(h, 0, [
      [R_A, 11],
      [R_A, 12],
      [R_FENCE, 5],
    ]);
    await tick();
    expect(f.seen).toEqual([
      [R_A, 11],
      [R_A, 12],
    ]);
    expect(Atomics.load(h.ctrl, C_ACK)).toBe(5);
    expect(Atomics.load(h.ctrl, C_TAIL) >>> 0).toBe(wr);

    // 24-byte records, enough to wrap the 1 KB ring: the PAD the writer
    // puts before the end is skipped, never handed on.
    for (let i = 0; i < 60; i++) {
      wr = publish(h, wr, [[R_A, 100 + i, 0, 0]]);
      await tick();
    }
    expect(f.seen.slice(2).map((r) => r[1])).toEqual([...Array(60).keys()].map((i) => 100 + i));

    wr = publish(h, wr, [[R_SHUTDOWN, 9]]);
    await tick();
    expect(Atomics.load(h.ctrl, C_ACK)).toBe(9);
    expect(Atomics.load(h.ctrl, C_STATUS)).toBe(STATUS_DETACHED);
    expect(Atomics.load(h.ctrl, C_TAIL) >>> 0).toBe(wr);
    expect(f.calls.slice(-3)).toEqual(['flush', 'flush', 'freeAll']);
  });

  it('waits out a record whose handler returns a promise before the next', async () => {
    const f = fakeCard();
    await start(f.card);
    const h = heap();
    f.setHeap(h.sab);
    send({ type: 'init', canvas: {} });
    send({ type: 'attach', memory: h.sab, ctrl: CTRL });
    await tick();
    publish(h, 0, [
      [R_SLOW, 1],
      [R_A, 2],
    ]);
    await tick();
    expect(f.seen).toEqual([[R_SLOW, 1]]);
    f.release();
    await tick();
    expect(f.seen).toEqual([
      [R_SLOW, 1],
      [R_A, 2],
    ]);
  });

  it('stops at a corrupt record or an unknown kind', async () => {
    const err = vi.spyOn(console, 'error').mockImplementation(() => {});
    const f = fakeCard();
    await start(f.card);
    const h = heap();
    f.setHeap(h.sab);
    send({ type: 'init', canvas: {} });
    send({ type: 'attach', memory: h.sab, ctrl: CTRL });
    await tick();
    const wr = publish(h, 0, [[99, 1]]);
    await tick();
    expect(err).toHaveBeenCalledWith('[test-gpu] unknown record kind', 99);
    expect(Atomics.load(h.ctrl, C_TAIL) >>> 0).not.toBe(wr);
    err.mockRestore();
  });

  it('holds an attach that arrives while the device is still being made', async () => {
    let resolve: (ok: boolean) => void = () => {};
    const f = fakeCard({ ready: new Promise((r) => (resolve = r)) });
    await start(f.card);
    const h = heap();
    f.setHeap(h.sab);
    send({ type: 'init', canvas: {} });
    send({ type: 'attach', memory: h.sab, ctrl: CTRL }); // before the device
    await tick();
    expect(Atomics.load(h.ctrl, C_STATUS)).toBe(0);
    resolve(true);
    await tick();
    expect(Atomics.load(h.ctrl, C_STATUS)).toBe(STATUS_ATTACHED);
  });

  it('says why there is no device', async () => {
    const f = fakeCard({ ready: Promise.resolve(false), reason: 'no adapter' });
    await start(f.card);
    send({ type: 'init', canvas: {} });
    await tick();
    expect(posted).toEqual([{ type: 'unavailable', reason: 'no adapter' }]);
  });

  it('answers stats with the transport counters and the card lines', async () => {
    const f = fakeCard();
    await start(f.card);
    const h = heap();
    f.setHeap(h.sab);
    send({ type: 'init', canvas: {} });
    send({ type: 'attach', memory: h.sab, ctrl: CTRL });
    await tick();
    publish(h, 0, [
      [R_A, 1],
      [R_A, 2],
    ]);
    await tick();
    send({ type: 'stats' });
    const s = posted.at(-1)!;
    expect(s.type).toBe('stats');
    expect(s.records).toBe(2);
    expect(s.things).toBe(2);
    send({ type: 'stats' });
    expect(posted.at(-1)!.records).toBe(0); // restarted
  });
});
