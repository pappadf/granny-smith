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
  C_STATUS,
  STATUS_ATTACHED,
  EVT_STATE,
  EVT_NOTIFY,
} from '@/bus/mailbox';
import { ringWrite, type Ring } from '@/bus/mailboxRing';

// A mailbox laid out here the way the core's constructor lays it out, with
// the test playing the core: it writes event records into the event ring
// and bumps EVT_HEAD.
const REQ = 4096;
const EVT = 4096;
const utf8 = new TextEncoder();

function layout(): { heap: SharedArrayBuffer; ctrlPtr: number; evt: Ring; ctrl: Int32Array } {
  const ctrlPtr = 64;
  const heap = new SharedArrayBuffer(ctrlPtr + 128 + REQ + EVT);
  const ctrl = new Int32Array(heap, ctrlPtr, 32);
  ctrl[C_MAGIC] = MAGIC | 0;
  ctrl[C_VERSION] = VERSION;
  ctrl[C_REQ_OFF] = 128;
  ctrl[C_REQ_SIZE] = REQ;
  ctrl[C_EVT_OFF] = 128 + REQ;
  ctrl[C_EVT_SIZE] = EVT;
  ctrl[C_STATUS] = STATUS_ATTACHED;
  const u8 = new Uint8Array(heap);
  return { heap, ctrlPtr, ctrl, evt: { u8, base: ctrlPtr + 128 + REQ, size: EVT } };
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
});
