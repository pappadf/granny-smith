import { describe, it, expect } from 'vitest';
import { parseDisasmLine, parseDisasmBlock } from '@/lib/disasm';

describe('parseDisasmLine', () => {
  it('parses a typical row', () => {
    const r = parseDisasmLine('00400C  MOVE.L (A0)+,D1');
    expect(r).not.toBeNull();
    expect(r!.addr).toBe(0x40_0c);
    expect(r!.mnem).toBe('MOVE.L');
    expect(r!.ops).toBe('(A0)+,D1');
    expect(r!.cmt).toBe('');
  });

  it('parses a row with a comment', () => {
    const r = parseDisasmLine('00400E  LEA    $FC0000,A1 ; main rom base');
    expect(r!.mnem).toBe('LEA');
    expect(r!.ops).toBe('$FC0000,A1');
    expect(r!.cmt).toBe('main rom base');
  });

  it('tolerates a leading $ on the address', () => {
    const r = parseDisasmLine('$00400C  NOP');
    expect(r!.addr).toBe(0x40_0c);
    expect(r!.mnem).toBe('NOP');
  });

  it('returns null on garbage', () => {
    expect(parseDisasmLine('')).toBeNull();
    expect(parseDisasmLine('not a disasm line')).toBeNull();
  });
});

describe('parseDisasmBlock', () => {
  it('parses multi-line output and skips blanks', () => {
    const text = `00400C  MOVE.L D0,D1
00400E  RTS
\n00401A  NOP`;
    const rows = parseDisasmBlock(text);
    expect(rows.length).toBe(3);
    expect(rows[0].addr).toBe(0x40_0c);
    expect(rows[2].addr).toBe(0x40_1a);
  });

  it('parses the C debug.disasm format ($addr  hex  MNEM  ops)', () => {
    const text = `$00400C  4e75  RTS         D1,(A0)+
$00400E  4cdf  MOVEM.L     (SP)+,D0-D7/A0-A6
$004010  4e71  NOP         `;
    const rows = parseDisasmBlock(text);
    expect(rows.length).toBe(3);
    expect(rows[0].mnem).toBe('RTS');
    expect(rows[0].ops).toBe('D1,(A0)+');
    expect(rows[1].mnem).toBe('MOVEM.L');
    expect(rows[2].mnem).toBe('NOP');
  });

  it('parses the MMU-expanded format (L:$addr P:$addr  hex  MNEM  ops)', () => {
    const text = `L:$00400C P:$00400C  4e75  RTS         D1,(A0)+`;
    const rows = parseDisasmBlock(text);
    expect(rows.length).toBe(1);
    expect(rows[0].addr).toBe(0x40_0c);
    expect(rows[0].mnem).toBe('RTS');
    expect(rows[0].ops).toBe('D1,(A0)+');
  });

  it('tolerates an unresolved physical mapping (P:????????)', () => {
    const text = `L:$DEAD0000 P:????????  4e75  RTS         `;
    const rows = parseDisasmBlock(text);
    expect(rows.length).toBe(1);
    expect(rows[0].addr).toBe(0xdead_0000);
    expect(rows[0].mnem).toBe('RTS');
  });
});
