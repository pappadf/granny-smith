// @vitest-environment node
//
// lib/serialize.ts: overlapping calls never overlap runs, and the last run
// starts after the last call, so it sees the latest intent.  Also the
// shared-heap block claim (bus/shmLayout.ts lazyShmBlock) both AV
// transports use.

import { describe, it, expect, vi } from 'vitest';
import { coalesced } from '@/lib/serialize';
import { lazyShmBlock } from '@/bus/shmLayout';

describe('coalesced', () => {
  it('runs one at a time and ends with a run that saw the last intent', async () => {
    let intent = 0;
    let active = 0;
    let maxActive = 0;
    const seen: number[] = [];
    const gates: (() => void)[] = [];
    const run = coalesced(async () => {
      active++;
      maxActive = Math.max(maxActive, active);
      const at = intent;
      await new Promise<void>((r) => gates.push(r));
      seen.push(at);
      active--;
    });

    intent = 1;
    const a = run();
    intent = 2;
    const b = run(); // arrives mid-flight
    // Release runs as they appear until every call has settled.
    let settled = false;
    const all = Promise.all([a, b]).then(() => (settled = true));
    while (!settled) {
      await new Promise((r) => setTimeout(r, 0));
      gates.shift()?.();
    }
    await all;
    expect(maxActive).toBe(1);
    expect(seen.at(-1)).toBe(2);
  });

  it('a failing run does not wedge the next call', async () => {
    let n = 0;
    const run = coalesced(async () => {
      n++;
      if (n === 1) throw new Error('first');
    });
    await expect(run()).rejects.toThrow('first');
    await run();
    expect(n).toBe(2);
  });
});

describe('lazyShmBlock', () => {
  const MAGIC = 0x41424344;
  function heapWith(magic: number, version: number): Int32Array {
    const i32 = new Int32Array(new SharedArrayBuffer(64));
    i32[4] = magic; // the block at byte 16
    i32[5] = version;
    i32[6] = 77;
    return i32;
  }

  it('checks once per announcement and reads the layout once', () => {
    const onClaim = vi.fn();
    const onMismatch = vi.fn();
    const b = lazyShmBlock({ magic: MAGIC, version: 3, onClaim, onMismatch });
    const i32 = heapWith(MAGIC, 3);
    expect(b.claim(i32)).toBe(0); // nothing announced yet
    b.announce(16);
    expect(b.claim(i32)).toBe(16);
    expect(b.claim(i32)).toBe(16);
    expect(onClaim).toHaveBeenCalledTimes(1);
    expect(onClaim).toHaveBeenCalledWith(i32, 4);
    expect(onMismatch).not.toHaveBeenCalled();
  });

  it('refuses a block from another build, and says so once', () => {
    const onClaim = vi.fn();
    const onMismatch = vi.fn();
    const b = lazyShmBlock({ magic: MAGIC, version: 3, onClaim, onMismatch });
    const i32 = heapWith(MAGIC, 4);
    b.announce(16);
    expect(b.claim(i32)).toBe(0);
    expect(b.claim(i32)).toBe(0);
    expect(onMismatch).toHaveBeenCalledTimes(1);
    expect(onClaim).not.toHaveBeenCalled();
  });
});
