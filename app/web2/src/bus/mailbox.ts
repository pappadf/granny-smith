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
export const VERSION = 9;

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
export const C_STAT_DROPPED = 19;

export const STATUS_DETACHED = 0;
export const STATUS_ATTACHED = 1;
export const STATUS_LOST = 2;

// Record kinds.
export const REQ_EVAL = 1;
export const REQ_SCRIPT = 2;
export const REQ_CANCEL = 3;
export const REQ_MODE_STOP = 4;
export const REQ_ACK_BUF = 5;
export const EVT_RESULT = 16;
export const EVT_PROGRESS = 17;
export const EVT_STATE = 18;
export const EVT_NOTIFY = 19;
export const EVT_LOG = 20;

// REQ_EVAL payload words: {id, client, deadline_ms, path_len, args_len} + path + args.
// REQ_SCRIPT payload words: {id, client, deadline_ms, src_len} + src.
// REQ_CANCEL payload words: {id, client, target_id}; REQ_MODE_STOP: {id, client, owner};
// REQ_ACK_BUF: {id, client, handle} (a transfer buffer was consumed).
// EVT_RESULT payload words: {id, ok, json_len, out_len} + json + output (each
// padded to 4).  The JSON is at most the core's result limit
// (GS_MBX_RESULT_MAX); a larger result arrives as an error naming its size.
// EVT_PROGRESS / EVT_STATE / EVT_NOTIFY / EVT_LOG payload words: {json_len} + json;
// progress is {"id": request, "done": n, "total": n}.  A job's EVT_LOG
// records are "output" (its printed text) and, among them at the positions
// they describe, the annotations "value_begin" / "value" (bracketing a value
// the REPL printed; "json" is the value as tagged JSON) and "error" (a
// statement error whose text went to stderr: file, line, message, lines).
const EVAL_WORDS = 5;
const RESULT_ID = 0;
const RESULT_OK = 1;
const RESULT_JSON_LEN = 2;
const RESULT_OUT_LEN = 3;
const RESULT_WORDS = 4;
const EVENT_JSON_LEN = 0;
const EVENT_WORDS = 1;

// The core's limits (mailbox.h): a longer request is the caller's error.
export const PATH_MAX = 1023;
export const ARGS_MAX = 128 << 10;
export const SCRIPT_MAX = 256 << 10;

export interface MailboxResult {
  ok: boolean;
  json: string;
  // What the leaf printed while it ran (out.h), when the core captures
  // it; empty otherwise.
  output: string;
}

export type MailboxFailure = 'deadline' | 'lost' | 'detached';

// A core event as it comes off the ring: the record kind and its JSON text.
export type EventListener = (kind: number, json: string) => void;

// An I/O job's progress: `done` of `total` (bytes, files; 0 when unknown).
export type ProgressListener = (done: number, total: number) => void;

export interface RequestOptions {
  // Who asks (default: this mailbox's client); a mode a request starts
  // belongs to it.
  client?: number;
  // Progress of an I/O job (meta.method_info `io`), as the core reports it.
  onProgress?: ProgressListener;
}

interface Pending {
  resolve(r: MailboxResult): void;
  fail(why: MailboxFailure): void;
  timer: ReturnType<typeof setTimeout> | null;
  progress?: ProgressListener;
}

// The core's heap, fresh each time: under ALLOW_MEMORY_GROWTH the buffer a
// view was made over can be replaced, so a transfer buffer (anywhere in the
// heap, unlike the rings) is read through a view made at that moment.
export type HeapAccessor = () => ArrayBufferLike;

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
  private readonly listeners = new Set<EventListener>();
  private readonly liveHeap: HeapAccessor;

  // Binds to the control block at `ctrlPtr` in `heap`.  Throws on a MAGIC
  // or VERSION mismatch: the page and the core are out of step.  `liveHeap`
  // returns the heap as it is now (transfer buffers are read through it);
  // default: the buffer given.
  constructor(heap: ArrayBufferLike, ctrlPtr: number, client: number, liveHeap?: HeapAccessor) {
    this.ctrl = new Int32Array(heap, ctrlPtr, 32);
    this.client = client;
    this.liveHeap = liveHeap ?? (() => heap);
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

  stats(): {
    requests: number;
    events: number;
    stalls: number;
    drainMaxUs: number;
    bad: number;
    dropped: number;
  } {
    return {
      requests: Atomics.load(this.ctrl, C_STAT_REQUESTS) >>> 0,
      events: Atomics.load(this.ctrl, C_STAT_EVENTS) >>> 0,
      stalls: Atomics.load(this.ctrl, C_STAT_STALLS) >>> 0,
      drainMaxUs: Atomics.load(this.ctrl, C_STAT_DRAIN_US) >>> 0,
      bad: Atomics.load(this.ctrl, C_STAT_BAD) >>> 0,
      dropped: Atomics.load(this.ctrl, C_STAT_DROPPED) >>> 0,
    };
  }

  // Subscribes to the core's own events (EVT_STATE / EVT_NOTIFY / EVT_LOG),
  // which arrive whether or not a request is pending, so the reader loop
  // runs for as long as anyone listens.  Returns the unsubscribe.
  on(cb: EventListener): () => void {
    this.listeners.add(cb);
    void this.readLoop();
    return () => {
      this.listeners.delete(cb);
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

  // Posts one object_eval request and resolves with its answer.  `deadlineMs`
  // 0 means none; otherwise the promise rejects with 'deadline' after that
  // much wall time and the late answer, if it ever comes, is dropped.
  // Rejects with 'lost' / 'detached' when the mailbox is gone.  `client`
  // names who asks (default: this mailbox's client); a mode a request
  // starts belongs to it.
  async request(
    path: string,
    argsJson: string,
    deadlineMs: number,
    clientOrOptions: number | RequestOptions = this.client,
  ): Promise<MailboxResult> {
    const opts: RequestOptions =
      typeof clientOrOptions === 'number' ? { client: clientOrOptions } : clientOrOptions;
    const pathBytes = utf8.encode(path);
    const argsBytes = utf8.encode(argsJson);
    if (pathBytes.length > PATH_MAX)
      throw new RangeError(`request path too large (${pathBytes.length} bytes > ${PATH_MAX})`);
    if (argsBytes.length > ARGS_MAX)
      throw new RangeError(`request arguments too large (${argsBytes.length} bytes > ${ARGS_MAX})`);
    return this.post(
      REQ_EVAL,
      opts.client ?? this.client,
      deadlineMs,
      [pathBytes.length, argsBytes.length],
      [pathBytes, argsBytes],
      undefined,
      opts.onProgress,
    );
  }

  // Tells the core a transfer buffer has been consumed: the core hands it
  // back to the I/O job that fills it.  Resolves true if it was one.
  async ackBuf(handle: number): Promise<boolean> {
    const r = await this.post(REQ_ACK_BUF, this.client, 10_000, [handle], []);
    return r.ok && r.json === 'true';
  }

  // A view of `len` bytes of the core's heap at `ptr`, as it is now.
  heapBytes(ptr: number, len: number): Uint8Array {
    return new Uint8Array(this.liveHeap(), ptr, len);
  }

  // Posts a script as a job: the answer comes when the job ends (the
  // shell's prompt, or an error), however long that takes -- no deadline.
  // `onId` receives the request id first, so the caller can cancel it.
  async script(src: string, client: number, onId?: (id: number) => void): Promise<MailboxResult> {
    const bytes = utf8.encode(src);
    if (bytes.length > SCRIPT_MAX)
      throw new RangeError(`script too large (${bytes.length} bytes > ${SCRIPT_MAX})`);
    return this.post(REQ_SCRIPT, client, 0, [bytes.length], [bytes], onId);
  }

  // Cancels a job of `client` by its request id.  Resolves true if one was.
  async cancel(client: number, targetId: number): Promise<boolean> {
    const r = await this.post(REQ_CANCEL, client, 10_000, [targetId], []);
    return r.ok && r.json === 'true';
  }

  // Stops a running mode owned by `owner` (0: any).  Resolves true if one was.
  async modeStop(client: number, owner: number): Promise<boolean> {
    const r = await this.post(REQ_MODE_STOP, client, 10_000, [owner], []);
    return r.ok && r.json === 'true';
  }

  private async post(
    kind: number,
    client: number,
    deadlineMs: number,
    tailWords: number[],
    texts: Uint8Array[],
    onId?: (id: number) => void,
    progress?: ProgressListener,
  ): Promise<MailboxResult> {
    if (this.failed) throw this.failed;
    const id = this.nextId++;
    if (this.nextId > 0x7fffffff) this.nextId = 1;
    const words =
      kind === REQ_EVAL || kind === REQ_SCRIPT
        ? [id, client, deadlineMs >>> 0, ...tailWords]
        : [id, client, ...tailWords];
    onId?.(id);
    // Write, waiting for room if the ring is full (the core drains every
    // frame, so this is rare; a full ring is a burst, not a stall).
    for (;;) {
      if (this.failed) throw this.failed;
      const tail = Atomics.load(this.ctrl, C_REQ_TAIL) >>> 0;
      const next = ringWrite(this.req, this.wr, tail, kind, words, texts);
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
        progress,
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
      while ((this.pending.size > 0 || this.listeners.size > 0) && !this.failed) {
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
        const outLen = getU32(this.evt.u8, p + 4 * RESULT_OUT_LEN);
        const pending = this.pending.get(id);
        if (pending) {
          // Copy out before consuming (the core may overwrite once TAIL
          // moves) -- and TextDecoder refuses a view over shared memory, so
          // slice(), which copies, not subarray().
          const jsonAt = p + 4 * RESULT_WORDS;
          const json = utf8dec.decode(this.evt.u8.slice(jsonAt, jsonAt + n));
          const outAt = jsonAt + pad4(n);
          const output = outLen ? utf8dec.decode(this.evt.u8.slice(outAt, outAt + outLen)) : '';
          this.pending.delete(id);
          if (pending.timer) clearTimeout(pending.timer);
          pending.resolve({ ok, json, output });
        }
        // else: a late answer past its deadline, or an id we never issued -- dropped.
      } else if (rec.kind === EVT_PROGRESS) {
        const p = rec.payload;
        const n = getU32(this.evt.u8, p + 4 * EVENT_JSON_LEN);
        const json = utf8dec.decode(
          this.evt.u8.slice(p + 4 * EVENT_WORDS, p + 4 * EVENT_WORDS + n),
        );
        try {
          const d = JSON.parse(json) as { id?: number; done?: number; total?: number };
          const pending = typeof d.id === 'number' ? this.pending.get(d.id) : undefined;
          if (pending?.progress) pending.progress(d.done ?? 0, d.total ?? 0);
        } catch {
          // a malformed progress record is nobody's failure
        }
      } else if (rec.kind === EVT_STATE || rec.kind === EVT_NOTIFY || rec.kind === EVT_LOG) {
        const p = rec.payload;
        const n = getU32(this.evt.u8, p + 4 * EVENT_JSON_LEN);
        const json = utf8dec.decode(
          this.evt.u8.slice(p + 4 * EVENT_WORDS, p + 4 * EVENT_WORDS + n),
        );
        for (const cb of this.listeners) {
          // A listener that throws must not desynchronise the ring position.
          try {
            cb(rec.kind, json);
          } catch (e) {
            console.error('mailbox event listener failed:', e);
          }
        }
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
