import { describe, it, expect } from 'vitest';
import { fmtHex32, fmtHex16, fmtHex8, parseHex } from '@/lib/hex';

describe('fmtHex32', () => {
  it('pads to 8 chars and uppercases', () => {
    expect(fmtHex32(0xdead)).toBe('0000DEAD');
    expect(fmtHex32(0)).toBe('00000000');
    expect(fmtHex32(0xffffffff)).toBe('FFFFFFFF');
  });
});

describe('fmtHex16 / fmtHex8', () => {
  it('mask + pad', () => {
    expect(fmtHex16(0x1234)).toBe('1234');
    // 0xabcde masked to the low 16 bits → 0xbcde
    expect(fmtHex16(0xabcde)).toBe('BCDE');
    expect(fmtHex8(0xab)).toBe('AB');
  });
});

describe('parseHex', () => {
  it('accepts bare hex', () => {
    expect(parseHex('DEAD')).toBe(0xdead);
    expect(parseHex('0')).toBe(0);
  });
  it('strips leading $', () => {
    expect(parseHex('$BEEF')).toBe(0xbeef);
  });
  it('strips leading 0x', () => {
    expect(parseHex('0xCAFE')).toBe(0xcafe);
    expect(parseHex('0XCAFE')).toBe(0xcafe);
  });
  it('returns null on non-hex characters', () => {
    expect(parseHex('xyz')).toBeNull();
    expect(parseHex('')).toBeNull();
    expect(parseHex('  ')).toBeNull();
  });
  it('round-trips with fmtHex32', () => {
    const n = parseHex('0x40028e');
    expect(n).toBe(0x40028e);
    expect(fmtHex32(n!)).toBe('0040028E');
  });
});
