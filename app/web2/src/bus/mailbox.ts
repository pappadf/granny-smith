// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The mailbox: the page's side of how requests reach the emulator thread --
// the TypeScript mirror of src/core/mailbox/mailbox.h.  One control block
// and two record rings (bus/mailboxRing.ts) in the wasm heap: the page
// writes REQ_EVAL records into the request ring and wakes the worker; the
// emulator thread drains them at frame boundaries and writes EVT_RESULT
// records into the event ring, which a reader loop here turns back into
// the promises that asked.  Every request carries an id, so any number
// can be in flight and a late answer can never be mistaken for another
// call's.
//
// Nothing here knows what a request means; that is bus/emulator.ts (the
// gsEval contract) and, later, the job and event kinds.

import {
  HDR_BYTES,
  R_PAD,
  getU32,
  pad4,
  pad8,
  ringRead,
  ringWrite,
  type Ring,
} from './mailboxRing';

export const MAGIC = 0x47534d42; // 'GSMB'
export const VERSION = 8;

// Control-block word indices (Int32Array view at the control base).
export const C_MAGIC = 0;
export const C_VERSION = 1;
export const C_REQ_OFF = 2;
export const C_REQ_SIZE = 3;
export const C_EVT_OFF = 4;
export const C_EVT_SIZE = 5;
export const C_REQ_HEAD = 6;
export const C_REQ_TAIL = 7;
export const C_EVT_HEAD = 8;
export const C_EVT_TAIL = 9;
export const C_STATUS = 10;
export const C_HEARTBEAT = 11;
export const C_READY = 12;
export const C_GPU_AVAILABLE = 13;
export const C_STAT_REQUESTS = 14;
export const C_STAT_EVENTS = 15;
export const C_STAT_STALLS = 16;
export const C_STAT_DRAIN_US = 17;
export const C_STAT_BAD = 18;

export const STATUS_DETACHED = 0;
export const STATUS_ATTACHED = 1;
export const STATUS_LOST = 2;

// Record kinds.
export const REQ_EVAL = 1;
export const EVT_RESULT = 16;

// REQ_EVAL payload words: {id, client, deadline_ms, path_len, args_len} + path + args.
// EVT_RESULT payload words: {id, ok, json_len} + json.
const EVAL_WORDS = 5;
const RESULT_ID = 0;
const RESULT_OK = 1;
const RESULT_JSON_LEN = 2;
const RESULT_WORDS = 3;

// The core's limits (mailbox.h): a longer request is the caller's error.
export const PATH_MAX = 1023;
export const ARGS_MAX = 128 << 10;

export interface MailboxResult {
  ok: boolean;
  json: string;
}

export type MailboxFailure = 'deadline' | 'lost' | 'detached';

interface Pending {
  resolve(r: MailboxResult): void;
  fail(why: MailboxFailure): void;
  timer: ReturnType<typeof setTimeout> | null;
}

const utf8 = new TextEncoder();
const utf8dec = new TextDecoder();

export class Mailbox {
  private readonly ctrl: Int32Array;
  private readonly req: Ring;
  private readonly evt: Ring;
  private wr = 0; // request bytes written (mod 2^32), published as REQ_HEAD
  private rd = 0; // event bytes consumed, published as EVT_TAIL
  private nextId = 1;
  private readonly client: number;
  private readonly pending = new Map<number, Pending>();
  private readonly waiters: Array<() => void> = []; // requests waiting for ring room
  private failed: MailboxFailure | null = null;
  private reading = false;
  private onLost: ((why: string) => void) | null = null;

  // Binds to the control block at `ctrlPtr` in `heap`.  Throws on a MAGIC
  // or VERSION mismatch: the page and the core are out of step.
  constructor(heap: ArrayBufferLike, ctrlPtr: number, client: number) {
    this.ctrl = new Int32Array(heap, ctrlPtr, 32);
    this.client = client;
    const magic = this.ctrl[C_MAGIC] >>> 0;
    const version = this.ctrl[C_VERSION];
    if (magic !== MAGIC || version !== VERSION)
      throw new Error(
        `mailbox version mismatch: core has 0x${magic.toString(16)} v${version}, page expects v${VERSION}`,
      );
    const u8 = new Uint8Array(heap);
    this.req = { u8, base: ctrlPtr + this.ctrl[C_REQ_OFF], size: this.ctrl[C_REQ_SIZE] };
    this.evt = { u8, base: ctrlPtr + this.ctrl[C_EVT_OFF], size: this.ctrl[C_EVT_SIZE] };
    this.wr = Atomics.load(this.ctrl, C_REQ_HEAD) >>> 0;
    this.rd = Atomics.load(this.ctrl, C_EVT_TAIL) >>> 0;
  }

  // The core sets READY once leaves can be served.
  isReady(): boolean {
    return Atomics.load(this.ctrl, C_READY) !== 0;
  }

  // Parks until READY, in slices of `sliceMs`; resolves 'ready' or 'timed-out'.
  async waitReady(sliceMs: number): Promise<'ready' | 'timed-out'> {
    if (this.isReady()) return 'ready';
    const w = Atomics.waitAsync(this.ctrl, C_READY, 0, sliceMs);
    const outcome = w.async ? await w.value : w.value;
    return outcome === 'timed-out' ? 'timed-out' : 'ready';
  }

  heartbeat(): number {
    return Atomics.load(this.ctrl, C_HEARTBEAT) >>> 0;
  }

  status(): number {
    return Atomics.load(this.ctrl, C_STATUS);
  }

  setGpuAvailable(ok: boolean): void {
    Atomics.store(this.ctrl, C_GPU_AVAILABLE, ok ? 1 : 0);
  }

  stats(): { requests: number; events: number; stalls: number; drainMaxUs: number; bad: number } {
    return {
      requests: Atomics.load(this.ctrl, C_STAT_REQUESTS) >>> 0,
      events: Atomics.load(this.ctrl, C_STAT_EVENTS) >>> 0,
      stalls: Atomics.load(this.ctrl, C_STAT_STALLS) >>> 0,
      drainMaxUs: Atomics.load(this.ctrl, C_STAT_DRAIN_US) >>> 0,
      bad: Atomics.load(this.ctrl, C_STAT_BAD) >>> 0,
    };
  }

  inFlight(): number {
    return this.pending.size;
  }

  // Called once when the mailbox is marked LOST (by the core, or here on a
  // crash); every pending and future request fails.
  setLostHandler(cb: (why: string) => void): void {
    this.onLost = cb;
  }

  // Fails everything: pending promises, queued writers, and every request
  // from now on.
  markLost(why: MailboxFailure, reason: string): void {
    if (this.failed) return;
    this.failed = why;
    for (const [, p] of this.pending) {
      if (p.timer) clearTimeout(p.timer);
      p.fail(why);
    }
    this.pending.clear();
    for (const w of this.waiters.splice(0)) w();
    this.onLost?.(reason);
  }

  // Posts one gs_eval request and resolves with its answer.  `deadlineMs`
  // 0 means none; otherwise the promise rejects with 'deadline' after that
  // much wall time and the late answer, if it ever comes, is dropped.
  // Rejects with 'lost' / 'detached' when the mailbox is gone.
  async request(path: string, argsJson: string, deadlineMs: number): Promise<MailboxResult> {
    if (this.failed) throw this.failed;
    const pathBytes = utf8.encode(path);
    const argsBytes = utf8.encode(argsJson);
    if (pathBytes.length > PATH_MAX)
      throw new RangeError(`request path too large (${pathBytes.length} bytes > ${PATH_MAX})`);
    if (argsBytes.length > ARGS_MAX)
      throw new RangeError(`request arguments too large (${argsBytes.length} bytes > ${ARGS_MAX})`);
    const id = this.nextId++;
    if (this.nextId > 0x7fffffff) this.nextId = 1;
    const words = [id, this.client, deadlineMs >>> 0, pathBytes.length, argsBytes.length];
    // Write, waiting for room if the ring is full (the core drains every
    // frame, so this is rare; a full ring is a burst, not a stall).
    for (;;) {
      if (this.failed) throw this.failed;
      const tail = Atomics.load(this.ctrl, C_REQ_TAIL) >>> 0;
      const next = ringWrite(this.req, this.wr, tail, REQ_EVAL, words, [pathBytes, argsBytes]);
      if (next >= 0) {
        this.wr = next;
        break;
      }
      await this.waitForRoom(tail);
    }
    const answer = new Promise<MailboxResult>((resolve, reject) => {
      const p: Pending = {
        resolve,
        fail: (why) => reject(why),
        timer: null,
      };
      if (deadlineMs > 0) {
        p.timer = setTimeout(() => {
          if (this.pending.delete(id)) reject('deadline' as MailboxFailure);
        }, deadlineMs);
      }
      this.pending.set(id, p);
    });
    Atomics.store(this.ctrl, C_REQ_HEAD, this.wr | 0);
    Atomics.notify(this.ctrl, C_REQ_HEAD);
    void this.readLoop();
    return answer;
  }

  // Parks until the core moves REQ_TAIL past `tail` (it consumed something)
  // or the mailbox fails.
  private async waitForRoom(tail: number): Promise<void> {
    const w = Atomics.waitAsync(this.ctrl, C_REQ_TAIL, tail | 0, 1000);
    if (w.async) await w.value;
  }

  // Drains the event ring into the pending promises, then parks on
  // EVT_HEAD until there is more, for as long as something is pending.
  private async readLoop(): Promise<void> {
    if (this.reading) return;
    this.reading = true;
    try {
      while (this.pending.size > 0 && !this.failed) {
        const head = Atomics.load(this.ctrl, C_EVT_HEAD) >>> 0;
        if (head === this.rd) {
          if (Atomics.load(this.ctrl, C_STATUS) === STATUS_LOST) {
            this.markLost('lost', 'the core marked the mailbox lost');
            return;
          }
          const w = Atomics.waitAsync(this.ctrl, C_EVT_HEAD, head | 0, 1000);
          if (w.async) await w.value;
          continue;
        }
        this.drainEvents(head);
      }
    } finally {
      this.reading = false;
    }
  }

  private drainEvents(head: number): void {
    while (this.rd !== head) {
      let rec;
      try {
        rec = ringRead(this.evt, this.rd, head);
      } catch (e) {
        this.markLost('lost', `event ring corrupt: ${e instanceof Error ? e.message : String(e)}`);
        return;
      }
      if (rec.kind === EVT_RESULT) {
        const p = rec.payload;
        const id = getU32(this.evt.u8, p + 4 * RESULT_ID);
        const ok = getU32(this.evt.u8, p + 4 * RESULT_OK) !== 0;
        const n = getU32(this.evt.u8, p + 4 * RESULT_JSON_LEN);
        const pending = this.pending.get(id);
        if (pending) {
          // Copy out before consuming (the core may overwrite once TAIL
          // moves) -- and TextDecoder refuses a view over shared memory, so
          // slice(), which copies, not subarray().
          const json = utf8dec.decode(
            this.evt.u8.slice(p + 4 * RESULT_WORDS, p + 4 * RESULT_WORDS + n),
          );
          this.pending.delete(id);
          if (pending.timer) clearTimeout(pending.timer);
          pending.resolve({ ok, json });
        }
        // else: a late answer past its deadline, or an id we never issued -- dropped.
      } else if (rec.kind !== R_PAD) {
        // An event kind this page does not consume yet (later phases): skip.
      }
      this.rd = (this.rd + rec.len) >>> 0;
    }
    Atomics.store(this.ctrl, C_EVT_TAIL, this.rd | 0);
    Atomics.notify(this.ctrl, C_EVT_TAIL);
  }
}

// The bytes one REQ_EVAL occupies, for callers that size things.
export function requestBytes(pathLen: number, argsLen: number): number {
  return pad8(HDR_BYTES + 4 * EVAL_WORDS + pad4(pathLen) + pad4(argsLen));
}
