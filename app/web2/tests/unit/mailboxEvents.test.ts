import { describe, it, expect } from 'vitest';
import {
  Mailbox,
  MAGIC,
  VERSION,
  C_MAGIC,
  C_VERSION,
  C_REQ_OFF,
  C_REQ_SIZE,
  C_EVT_OFF,
  C_EVT_SIZE,
  C_EVT_HEAD,
  C_EVT_TAIL,
  C_REQ_HEAD,
  C_REQ_TAIL,
  C_STATUS,
  STATUS_ATTACHED,
  EVT_STATE,
  EVT_NOTIFY,
  EVT_RESULT,
  EVT_PROGRESS,
  REQ_ACK_BUF,
  REQ_EVAL,
} from '@/bus/mailbox';
import { getU32, ringRead, ringWrite, type Ring } from '@/bus/mailboxRing';

// A mailbox laid out here the way the core's constructor lays it out, with
// the test playing the core: it writes event records into the event ring
// and bumps EVT_HEAD.
const REQ = 4096;
const EVT = 4096;
const utf8 = new TextEncoder();

function layout(): {
  heap: SharedArrayBuffer;
  ctrlPtr: number;
  evt: Ring;
  req: Ring;
  ctrl: Int32Array;
  spare: number; // a heap offset past the rings, for staged buffers
} {
  const ctrlPtr = 64;
  const heap = new SharedArrayBuffer(ctrlPtr + 128 + REQ + EVT + 1024);
  const ctrl = new Int32Array(heap, ctrlPtr, 32);
  ctrl[C_MAGIC] = MAGIC | 0;
  ctrl[C_VERSION] = VERSION;
  ctrl[C_REQ_OFF] = 128;
  ctrl[C_REQ_SIZE] = REQ;
  ctrl[C_EVT_OFF] = 128 + REQ;
  ctrl[C_EVT_SIZE] = EVT;
  ctrl[C_STATUS] = STATUS_ATTACHED;
  const u8 = new Uint8Array(heap);
  return {
    heap,
    ctrlPtr,
    ctrl,
    evt: { u8, base: ctrlPtr + 128 + REQ, size: EVT },
    req: { u8, base: ctrlPtr + 128, size: REQ },
    spare: ctrlPtr + 128 + REQ + EVT,
  };
}

// The core's answer: {id, ok, json_len, out_len} + json + output.
function answer(
  l: ReturnType<typeof layout>,
  wr: number,
  id: number,
  json: string,
  output = '',
): number {
  const j = utf8.encode(json);
  const o = utf8.encode(output);
  const tail = Atomics.load(l.ctrl, C_EVT_TAIL) >>> 0;
  const next = ringWrite(l.evt, wr, tail, EVT_RESULT, [id, 1, j.length, o.length], [j, o]);
  if (next < 0) throw new Error('no room');
  Atomics.store(l.ctrl, C_EVT_HEAD, next | 0);
  Atomics.notify(l.ctrl, C_EVT_HEAD);
  return next;
}

// The core's side of the request ring: the next record's kind and words.
function nextRequest(
  l: ReturnType<typeof layout>,
  rd: number,
): { kind: number; words: number[]; next: number } {
  const head = Atomics.load(l.ctrl, C_REQ_HEAD) >>> 0;
  const rec = ringRead(l.req, rd, head);
  const words = [0, 1, 2].map((i) => getU32(l.req.u8, rec.payload + 4 * i));
  const next = (rd + rec.len) >>> 0;
  Atomics.store(l.ctrl, C_REQ_TAIL, next | 0);
  return { kind: rec.kind, words, next };
}

// The core's emit: {json_len} + json, published at once.
function emit(l: ReturnType<typeof layout>, wr: number, kind: number, json: string): number {
  const bytes = utf8.encode(json);
  const tail = Atomics.load(l.ctrl, C_EVT_TAIL) >>> 0;
  const next = ringWrite(l.evt, wr, tail, kind, [bytes.length], [bytes]);
  if (next < 0) throw new Error('no room');
  Atomics.store(l.ctrl, C_EVT_HEAD, next | 0);
  Atomics.notify(l.ctrl, C_EVT_HEAD);
  return next;
}

const tick = () => new Promise((r) => setTimeout(r, 0));

describe('core events on the mailbox', () => {
  it('a listener receives events with no request pending, in order, and the ring is consumed', async () => {
    const l = layout();
    const mb = new Mailbox(l.heap, l.ctrlPtr, 1);
    const seen: Array<[number, string]> = [];
    const off = mb.on((kind, json) => seen.push([kind, json]));
    let wr = 0;
    wr = emit(l, wr, EVT_STATE, '{"event":"mode_started","mode":1}');
    wr = emit(l, wr, EVT_NOTIFY, '{"event":"floppy"}');
    for (let i = 0; i < 20 && seen.length < 2; i++) await tick();
    expect(seen).toEqual([
      [EVT_STATE, '{"event":"mode_started","mode":1}'],
      [EVT_NOTIFY, '{"event":"floppy"}'],
    ]);
    expect(Atomics.load(l.ctrl, C_EVT_TAIL) >>> 0).toBe(wr);
    expect(mb.stats().dropped).toBe(0);
    off();
  });

  it('a listener that throws does not desynchronise the reader', async () => {
    const l = layout();
    const mb = new Mailbox(l.heap, l.ctrlPtr, 1);
    const seen: string[] = [];
    mb.on(() => {
      throw new Error('bad listener');
    });
    mb.on((_k, json) => seen.push(json));
    let wr = 0;
    wr = emit(l, wr, EVT_STATE, '{"a":1}');
    wr = emit(l, wr, EVT_STATE, '{"a":2}');
    for (let i = 0; i < 20 && seen.length < 2; i++) await tick();
    expect(seen).toEqual(['{"a":1}', '{"a":2}']);
    expect(Atomics.load(l.ctrl, C_EVT_TAIL) >>> 0).toBe(wr);
  });

  it('an answer carries what the leaf printed, and progress reaches the request that asked', async () => {
    const l = layout();
    const mb = new Mailbox(l.heap, l.ctrlPtr, 1);
    const progress: Array<[number, number]> = [];
    const p = mb.request('files.cp', '["a","b"]', 0, {
      onProgress: (done, total) => progress.push([done, total]),
    });
    let wr = 0;
    wr = emit(l, wr, EVT_PROGRESS, '{"id":1,"done":5,"total":10}');
    wr = emit(l, wr, EVT_PROGRESS, '{"id":99,"done":1,"total":1}'); // someone else's
    wr = answer(l, wr, 1, 'true', 'copied 1 file(s)\n');
    const r = await p;
    expect(r).toEqual({ ok: true, json: 'true', output: 'copied 1 file(s)\n' });
    expect(progress).toEqual([[5, 10]]);
    expect(Atomics.load(l.ctrl, C_EVT_TAIL) >>> 0).toBe(wr);
  });

  it('a spilled result is read from its staged buffer and the buffer is acknowledged', async () => {
    const l = layout();
    const mb = new Mailbox(l.heap, l.ctrlPtr, 1);
    const big = JSON.stringify({ rows: Array.from({ length: 40 }, (_, i) => `row ${i}`) });
    const bytes = utf8.encode(big);
    l.req.u8.set(bytes, l.spare);
    const p = mb.request('debug.disasm', '', 0);
    let rd = 0;
    const req = nextRequest(l, rd);
    rd = req.next;
    expect(req.kind).toBe(REQ_EVAL);
    answer(l, 0, req.words[0], `{"$buf":7,"ptr":${l.spare},"len":${bytes.length}}`);
    const r = await p;
    expect(r.ok).toBe(true);
    expect(r.json).toBe(big);
    // The ack names the handle.
    for (let i = 0; i < 20 && Atomics.load(l.ctrl, C_REQ_HEAD) >>> 0 === rd; i++) await tick();
    const ack = nextRequest(l, rd);
    expect(ack.kind).toBe(REQ_ACK_BUF);
    expect(ack.words[2]).toBe(7);
  });
});
