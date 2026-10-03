import { describe, it, expect } from 'vitest';
import { decodeTc, decodeRootPointer, formatEntry, fmtSize } from '@/lib/mmu';

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

describe('formatEntry', () => {
  it('lays out a table level: index, where it was read, what it held, where it led', () => {
    const e = formatEntry('level', {
      name: 'A',
      index: 8,
      addr: 0x40800070,
      desc: 0x40800019,
      type: 'page',
      wp: false,
      u: true,
      phys: 0x40826cc0,
    });
    expect(e.title).toBe('LEVEL A');
    expect(e.main).toBe('[8] @$40800070 = $40800019 page → P:$40826CC0');
    expect(e.extra).toEqual(['U']);
  });

  it('shows a fault reason and a long descriptor pair', () => {
    const e = formatEntry('pteg', {
      name: 'secondary',
      addr: 0x4effc0,
      desc: 0x80000000,
      desc_lo: 0x1,
      reason: 'no matching PTE',
      hash: 0x7ffff,
    });
    expect(e.main).toBe('@$004EFFC0 = $80000000 $00000001 (no matching PTE)');
    expect(e.extra).toEqual(['hash=$0007FFFF']);
  });
});

describe('fmtSize', () => {
  it('uses K/M/G when exact, hex otherwise', () => {
    expect(fmtSize(0x100000)).toBe('1M');
    expect(fmtSize(2 ** 32)).toBe('4G');
    expect(fmtSize(0x4000)).toBe('16K');
    expect(fmtSize(0x1234)).toBe('$1234');
  });
});
