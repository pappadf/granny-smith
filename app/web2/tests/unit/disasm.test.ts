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
});
