import { describe, it, expect } from 'vitest';
import {
  decodeTc,
  decodeRootPointer,
  formatEntry,
  fmtSize,
  formatLisaSegment,
  lisaSegmentType,
  lisaSpaceLabel,
} from '@/lib/mmu';

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

// The Lisa's segment descriptors in words, for a viewer who is no MMU expert.
describe('formatLisaSegment', () => {
  it('reads a code segment: number, logical base, type, length, physical base', () => {
    const r = formatLisaSegment({
      segment: 1,
      context: 1,
      base: 0x20000,
      sor: 0x1fa,
      slr: 0x5f9,
      type: 'ram-ro',
      limit: 249,
      phys: 0x3f400,
      first_page: 0,
      pages: 7,
    });
    expect(r).toEqual({
      segment: 1,
      logical: '$020000',
      type: 'read-only RAM',
      present: true,
      length: '7 pages (3.5 KB)',
      physical: '$03F400',
      raw: 'SOR=$1FA SLR=$5F9',
    });
  });

  it('says a segment is not present, with no length or physical base', () => {
    const r = formatLisaSegment({ segment: 5, base: 0xa0000, sor: 0, slr: 0xc00, type: 'invalid' });
    expect(r.type).toBe('not present');
    expect(r.present).toBe(false);
    expect(r.length).toBe('');
    expect(r.physical).toBe('');
  });

  it('starts a stack at its boundary page', () => {
    const r = formatLisaSegment({
      segment: 126,
      base: 126 * 0x20000,
      sor: 0x400,
      slr: 0x600,
      type: 'ram-rw-stack',
      limit: 0,
      phys: 0x80000,
      first_page: 255,
      pages: 1,
    });
    expect(r.logical).toBe('$FDFE00');
    expect(r.physical).toBe('$09FE00');
    expect(r.length).toBe('1 page (512 B)');
    expect(r.type).toBe('stack (read-write RAM)');
  });

  it('names every type and space', () => {
    expect(['ram-rw', 'ram-ro-stack', 'io', 'special'].map(lisaSegmentType)).toEqual([
      'read-write RAM',
      'stack (read-only RAM)',
      'I/O',
      'ROM',
    ]);
    expect(['ram', 'io', 'rom', 'mmureg'].map(lisaSpaceLabel)).toEqual([
      'RAM',
      'I/O',
      'ROM',
      'MMU registers',
    ]);
  });
});
