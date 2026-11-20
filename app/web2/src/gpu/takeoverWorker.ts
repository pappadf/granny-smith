// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The transport half both WebGPU takeover workers share (the Voodoo2's,
// voodoo2Gpu.worker.ts, and the Rage 128's, rage128Gpu.worker.ts): the
// page's messages, attach and detach of the shared region, the consume
// loop, the record framing, ACK and TAIL, and the worker's diagnostics.
// A card supplies its device setup, its record handlers and its resource
// teardown (TakeoverCard) and calls runTakeoverWorker() once.
//
// Threading: the emulator side publishes bytes by advancing HEAD and
// notifying; the worker parks in Atomics.waitAsync on HEAD, consumes every
// complete record up to it, and advances TAIL as it goes so the producer
// can reuse the space.  A READBACK, FENCE or SHUTDOWN record carries a
// sequence number the worker stores into ACK when the work is done (after
// the GPU roundtrip, for a readback); the producer waits on ACK.
//
// The region's views are exported as plain module bindings (`u32`, `u8`,
// `ctrl`, `rbBase`, ...): a card's handlers read them directly, with no
// getter on the per-record path.

import {
  C_VERSION,
  C_RING_OFF,
  C_RING_SIZE,
  C_RB_OFF,
  C_RB_SIZE,
  C_HEAD,
  C_TAIL,
  C_ACK,
  C_STATUS,
  STATUS_DETACHED,
  STATUS_ATTACHED,
  STATUS_LOST,
} from './takeoverProtocol';
import { R_PAD, recordOk } from '@/bus/mailboxRing';

// What a card's worker supplies to the shared transport.
export interface TakeoverCard {
  name: string; // the log prefix ('voodoo2-gpu')
  magic: number; // the control block's MAGIC and PROTOCOL_VERSION
  version: number;
  fence: number; // its R_FENCE and R_SHUTDOWN record kinds
  shutdown: number;
  // Create the device and the card's pipelines on the overlay canvas;
  // false with unavailableReason() saying why.  A device loss after it
  // answered true goes to markLost().
  initDevice(canvas: OffscreenCanvas): Promise<boolean>;
  unavailableReason(): string;
  // A region was attached: reset the card's per-region state.
  attached(): void;
  // One record of the card's own kinds (`p` is its payload's word index,
  // `at` its byte address).  A promise for one whose work the drain must
  // wait out (a readback); false for a kind the card does not know.
  record(kind: number, p: number, at: number): void | Promise<void> | false;
  // Submit everything encoded so far; release every GPU resource.
  flush(): void;
  freeAll(): void;
  // The card's lines of the 'debug' and 'stats' answers (resource counts,
  // the readback in flight).
  debugInfo(): Record<string, unknown>;
  statsInfo(now: number): Record<string, unknown>;
}

// --- messages from the page -----------------------------------------------

interface InitMsg {
  type: 'init';
  canvas: OffscreenCanvas;
}
interface AttachMsg {
  type: 'attach';
  memory: WebAssembly.Memory | SharedArrayBuffer;
  ctrl: number;
}
interface DetachMsg {
  type: 'detach';
  ctrl: number;
}
interface DebugMsg {
  type: 'debug';
}
interface StatsMsg {
  type: 'stats';
}
type InMsg = InitMsg | AttachMsg | DetachMsg | DebugMsg | StatsMsg;

// --- attached region -----------------------------------------------------

export let ctrl: Int32Array | null = null; // the control block (Int32 for Atomics.wait)
export let u32: Uint32Array | null = null; // the whole heap
export let u8: Uint8Array | null = null;
export let ctrlBase = 0;
let ringBase = 0;
let ringSize = 0;
export let rbBase = 0;
export let rbSize = 0;
let consumed = 0; // bytes consumed (mod 2^32)
let attached = false;
let lost = false;
let loopRunning = false;
let lastKind = -1; // the last record kind consumed

let card: TakeoverCard;

// What this worker did since the page last asked (a 'stats' message; the
// page's perf monitor and diagnostics print it): records and bytes
// consumed, time spent consuming, and the card's own counts (readbacks and
// their latency, presents and how far the GPU's completion of them lags,
// draws), which its handlers add here.
export const st = {
  t0: performance.now(),
  records: 0,
  bytes: 0,
  drains: 0,
  drainMs: 0,
  drainMaxMs: 0,
  readbacks: 0,
  readbackMs: 0,
  readbackMaxMs: 0,
  presents: 0,
  presentLagMaxMs: 0,
  draws: 0,
  idleWaits: 0,
};
let drainingSince = 0; // performance.now() the current drain began (0: idle)

// The heap as the upload APIs want it: WebGPU accepts SharedArrayBuffer-
// backed views (AllowSharedBufferSource); the type declarations lag.
export function heap(): GPUAllowSharedBufferSource {
  return u8 as unknown as GPUAllowSharedBufferSource;
}

// Add to one of the control block's statistics words.
export function stat(word: number, delta: number): void {
  if (ctrl) Atomics.add(ctrl, word, delta);
}

// Tell the producer the work numbered `seq` is done.
export function ack(seq: number): void {
  if (!ctrl) return;
  Atomics.store(ctrl, C_ACK, seq | 0);
  Atomics.notify(ctrl, C_ACK);
}

// The transport is dead: wake whatever the producer waits on, so it sees
// LOST instead of waiting out its timeout.
function signalLost(): void {
  if (!ctrl) return;
  Atomics.store(ctrl, C_STATUS, STATUS_LOST);
  Atomics.notify(ctrl, C_STATUS);
  Atomics.notify(ctrl, C_TAIL);
  Atomics.notify(ctrl, C_ACK);
}

// The device is gone (its `lost` promise settled): tell the producer and
// the page, and consume nothing more.
export function markLost(reason: string): void {
  lost = true;
  signalLost();
  postMessage({ type: 'lost', reason });
}

// --- the consume loop --------------------------------------------------

function advanceTail(): void {
  Atomics.store(ctrl!, C_TAIL, consumed | 0);
  Atomics.notify(ctrl!, C_TAIL);
}

// One drain, timed for the stats.
async function drain(head: number): Promise<boolean> {
  const t0 = performance.now();
  drainingSince = t0;
  st.drains++;
  try {
    return await drainRecords(head);
  } finally {
    const ms = performance.now() - t0;
    drainingSince = 0;
    st.drainMs += ms;
    if (ms > st.drainMaxMs) st.drainMaxMs = ms;
  }
}

// Consume every complete record up to `head`.  Async only for the work a
// card's handler returns a promise for (the readbacks); the order of
// records is preserved because nothing else runs between awaits.
async function drainRecords(head: number): Promise<boolean> {
  const mask = ringSize - 1;
  while (consumed !== head) {
    const off = consumed & mask;
    const at = ringBase + off;
    const kind = u32![at >> 2];
    const len = u32![(at >> 2) + 1];
    // A record is a multiple of 8 (the record-ring rule every transport
    // shares), whole within the ring (the writer pads to the end rather
    // than wrap one), and no longer than what was published -- the checks
    // platen's ringRead makes.  Only a writer bug could break them, and a
    // record that does would read past the ring into the readback area and
    // mis-decode.
    if (!recordOk(len, off, ringSize, (head - consumed) >>> 0)) {
      console.error(`[${card.name}] corrupt record`, kind, len);
      return false;
    }
    const p = (at >> 2) + 2; // payload word index
    lastKind = kind;
    st.records++;
    st.bytes += len;
    if (kind === card.shutdown) {
      card.flush();
      card.freeAll();
      const seq = u32![p];
      consumed = (consumed + len) >>> 0;
      advanceTail();
      attached = false;
      Atomics.store(ctrl!, C_STATUS, STATUS_DETACHED);
      ack(seq);
      return false;
    }
    if (kind === card.fence) {
      card.flush();
      ack(u32![p]);
    } else if (kind !== R_PAD) {
      const r = card.record(kind, p, at);
      if (r === false) {
        console.error(`[${card.name}] unknown record kind`, kind);
        return false;
      }
      if (r) await r;
    }
    consumed = (consumed + len) >>> 0;
    advanceTail();
  }
  return true;
}

async function loop(): Promise<void> {
  if (loopRunning) return;
  loopRunning = true;
  try {
    while (attached && !lost) {
      const head = Atomics.load(ctrl!, C_HEAD) >>> 0;
      if (head === consumed) {
        st.idleWaits++;
        const r = Atomics.waitAsync(ctrl!, C_HEAD, head | 0, 1000);
        if (r.async) await r.value;
        continue;
      }
      if (!(await drain(head))) break;
    }
  } catch (e) {
    console.error(`[${card.name}] worker loop failed:`, e);
    signalLost();
  } finally {
    loopRunning = false;
  }
}

function attach(memory: WebAssembly.Memory | SharedArrayBuffer, ctrlPtr: number): void {
  const buffer = memory instanceof SharedArrayBuffer ? memory : memory.buffer;
  u8 = new Uint8Array(buffer);
  u32 = new Uint32Array(buffer);
  ctrl = new Int32Array(buffer, ctrlPtr, 32);
  ctrlBase = ctrlPtr;
  const w = ctrlPtr >> 2;
  if (u32[w] >>> 0 !== card.magic || u32[w + C_VERSION] !== card.version) {
    console.error(`[${card.name}] control block mismatch`, u32[w].toString(16), u32[w + C_VERSION]);
    return;
  }
  ringBase = ctrlBase + u32[w + C_RING_OFF];
  ringSize = u32[w + C_RING_SIZE];
  rbBase = ctrlBase + u32[w + C_RB_OFF];
  rbSize = u32[w + C_RB_SIZE];
  consumed = Atomics.load(ctrl, C_HEAD) >>> 0;
  card.attached();
  attached = true;
  Atomics.store(ctrl, C_STATUS, STATUS_ATTACHED);
  Atomics.notify(ctrl, C_STATUS);
  void loop();
}

function detach(): void {
  if (!attached) return;
  attached = false;
  card.flush();
  card.freeAll();
  if (ctrl) {
    Atomics.store(ctrl, C_STATUS, STATUS_DETACHED);
    Atomics.notify(ctrl, C_STATUS);
  }
  ctrl = null;
}

// The 'stats' answer, and the counters restarted.
function statsReply(): Record<string, unknown> {
  const now = performance.now();
  const reply = {
    type: 'stats',
    seconds: (now - st.t0) / 1000,
    ...st,
    t0: undefined,
    attached,
    lost,
    loopRunning,
    drainingForMs: drainingSince ? now - drainingSince : 0,
    consumed: consumed >>> 0,
    head: ctrl ? Atomics.load(ctrl, C_HEAD) >>> 0 : -1,
    lastKind,
    ...card.statsInfo(now),
  };
  for (const k of Object.keys(st) as (keyof typeof st)[]) st[k] = 0;
  st.t0 = now;
  return reply;
}

// Install the worker's message handler for `c`.
export function runTakeoverWorker(c: TakeoverCard): void {
  card = c;
  // An attach can arrive while the device is still being created (the
  // page may start the worker lazily, on the card's first attach): it
  // waits for the device instead of being dropped.
  let deviceReady: Promise<boolean> | null = null;
  let initFailure = '';
  self.onmessage = (ev: MessageEvent<InMsg>) => {
    const msg = ev.data;
    if (msg.type === 'init') {
      deviceReady = card.initDevice(msg.canvas).catch((e) => {
        initFailure = `WebGPU device setup failed: ${e}`;
        return false;
      });
      void deviceReady.then((ok) =>
        postMessage(
          ok
            ? { type: 'ready' }
            : { type: 'unavailable', reason: initFailure || card.unavailableReason() },
        ),
      );
    } else if (msg.type === 'attach') {
      void (deviceReady ?? Promise.resolve(false)).then((ok) => {
        if (ok && !lost) attach(msg.memory, msg.ctrl);
      });
    } else if (msg.type === 'detach') {
      if (ctrl && ctrlBase === msg.ctrl) detach();
    } else if (msg.type === 'stats') {
      postMessage(statsReply());
    } else if (msg.type === 'debug') {
      // Diagnostics: where the worker is (answered even while a readback
      // awaits, since the message handler is its own task).
      postMessage({
        type: 'debug',
        attached,
        lost,
        loopRunning,
        consumed,
        head: ctrl ? Atomics.load(ctrl, C_HEAD) >>> 0 : -1,
        ack: ctrl ? Atomics.load(ctrl, C_ACK) : -1,
        status: ctrl ? Atomics.load(ctrl, C_STATUS) : -1,
        lastKind,
        ...card.debugInfo(),
      });
    }
  };
}
