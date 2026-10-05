// The microphone ring's producer-side slot arithmetic (state/micRing.ts) at
// the uint32 wrap.  The indices run free in Int32 words, so past 2^31 they
// read back negative; the slot must still be the index masked (the C
// consumer, em_mic_ring.c, masks the same way -- tests/unit/suites/mic_ring).

import { describe, it, expect } from 'vitest';
import { micRingSlot } from '@/state/micRing';

describe('micRingSlot', () => {
  const LEN = 32768;

  it('is the index masked', () => {
    expect(micRingSlot(0, 0, LEN)).toBe(0);
    expect(micRingSlot(5, 3, LEN)).toBe(8);
    expect(micRingSlot(LEN - 1, 1, LEN)).toBe(0);
  });

  it('never goes negative once the index word does', () => {
    // WR as Atomics.load returns it after 2^31 samples.
    const wr = 0x7ffffffe | 0;
    for (let i = 0; i < 8; i++) {
      const slot = micRingSlot(wr, i, LEN);
      expect(slot).toBeGreaterThanOrEqual(0);
      expect(slot).toBe((0x7ffffffe + i) % LEN);
    }
    const neg = 0xfffffffe | 0; // -2
    expect(micRingSlot(neg, 0, LEN)).toBe(LEN - 2);
    expect(micRingSlot(neg, 2, LEN)).toBe(0); // wraps to 0x100000000
  });
});
