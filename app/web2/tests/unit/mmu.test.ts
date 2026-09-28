import { describe, it, expect } from 'vitest';
import { decodeTc, decodeRootPointer } from '@/lib/mmu';

describe('decodeTc', () => {
  it('extracts the TC fields', () => {
    const t = decodeTc(0x80008307);
    expect(t.E).toBe(1);
    expect(t.SRE).toBe(0);
    expect(t.PS).toBe(0); // (0x80008307 >> 20) & 0xf == 0
    expect(t.TIA).toBe(8);
    expect(t.TID).toBe(7);
  });
});

describe('decodeRootPointer', () => {
  it('decodes limit/dt/pointer from the two words', () => {
    const r = decodeRootPointer(0x00000002, 0x001fe000);
    expect(r.dt).toBe(2);
    expect(r.pointer).toBe(0x001fe000);
    expect(r.limit).toBe(0);
  });
});
