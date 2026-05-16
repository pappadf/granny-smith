import { describe, it, expect } from 'vitest';
import { decodeTc, decodeCrpFromHex, dtName, fmtAddrPair, fmtRangePair } from '@/lib/mmu';
import { mmuLookup, mmuMap, mmuDescriptors, mmuRegs } from '@/bus/mockMmu';

describe('decodeTc', () => {
  it('extracts the prototype fixture fields', () => {
    const t = decodeTc(mmuRegs.tc); // 0x80008307
    expect(t.E).toBe(1);
    expect(t.SRE).toBe(0);
    expect(t.PS).toBe(0); // (0x80008307 >> 20) & 0xf == 0
    expect(t.TIA).toBe(8);
    expect(t.TID).toBe(7);
  });
});

describe('decodeCrpFromHex', () => {
  it('splits high/low + decodes limit/dt/pointer', () => {
    const r = decodeCrpFromHex('00000002_001FE000');
    expect(r.dt).toBe(2);
    expect(r.pointer).toBe(0x001fe000);
    expect(r.limit).toBe(0);
  });
});

describe('dtName', () => {
  it('maps DT values to canonical names', () => {
    expect(dtName(0)).toBe('INVALID');
    expect(dtName(1)).toBe('PAGE');
    expect(dtName(2)).toBe('TABLE8');
    expect(dtName(3)).toBe('TABLE4');
  });
});

describe('mmuLookup (mock fixtures)', () => {
  it('returns TT for low-memory addresses', () => {
    const r = mmuLookup(0x00001234);
    expect(r.valid).toBe(true);
    expect(r.kind).toBe('TT');
    expect(r.phys).toBe(0x00001234);
  });
  it('returns PT for the user-code range with rebased phys', () => {
    const r = mmuLookup(0x00400000);
    expect(r.valid).toBe(true);
    expect(r.kind).toBe('PT');
    expect(r.phys).toBe(0x50000000);
  });
  it('returns invalid for unmapped addresses', () => {
    const r = mmuLookup(0x20000000);
    expect(r.valid).toBe(false);
  });
});

describe('fmtAddrPair', () => {
  it('plain $hex with no lookup', () => {
    expect(fmtAddrPair(0x400)).toEqual({ logical: '$00000400' });
  });
  it('TT lookup → both columns + TT tag', () => {
    const p = fmtAddrPair(0x100, { valid: true, phys: 0x100, kind: 'TT' });
    expect(p.logical).toBe('L:$00000100');
    expect(p.physical).toBe('P:$00000100');
    expect(p.tag).toBe('TT');
  });
  it('invalid → INVALID tag, no physical', () => {
    const p = fmtAddrPair(0xdeadbeef, { valid: false });
    expect(p.tag).toBe('INVALID');
    expect(p.physical).toBeUndefined();
  });
});

describe('fmtRangePair', () => {
  it('mmu off → single column + size', () => {
    const r = fmtRangePair(0, 0x3ff, 0, false);
    expect(r.l).toBe('$00000000 – $000003FF');
    expect(r.p).toBeUndefined();
  });
  it('mmu on → both columns', () => {
    const r = fmtRangePair(0x400000, 0x4001ff, 0x50000000, true);
    expect(r.l).toBe('L:$00400000 – $004001FF');
    expect(r.p).toBe('P:$50000000 – $500001FF');
  });
});

describe('mmuMap fixture', () => {
  it('overlap filter returns ranges intersecting the window', () => {
    const r = mmuMap(0x00000000, 0x00ffffff);
    expect(r.length).toBeGreaterThan(0);
    expect(r.some((m) => m.lo === 0x00400000)).toBe(true);
  });
});

describe('mmuDescriptors fixture', () => {
  it('returns one row per descriptor cycling DT 0..3', () => {
    const rows = mmuDescriptors(0x001fe000, 4);
    expect(rows[0].dt).toBe(0);
    expect(rows[1].dt).toBe(1);
    expect(rows[2].dt).toBe(2);
    expect(rows[3].dt).toBe(3);
  });
});
