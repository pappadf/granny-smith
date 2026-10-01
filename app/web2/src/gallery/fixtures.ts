// Fixture data for the gallery stories: debug frames, console entries and
// the like, shaped as the bus returns them.
import type { DebugFrame, DebugFrameRow } from '@/bus/debug';

// A 68K disassembly window around a PC; `mmu` adds physical addresses and an
// unmapped row.
export function m68kFrame(mmu = false): DebugFrame {
  const pc = 0x0040028e;
  const ops: Array<[string, string]> = [
    ['MOVE.L', 'D0,-(SP)'],
    ['LEA', '$1A(A5),A0'],
    ['MOVEQ', '#$00,D1'],
    ['JSR', '$00408A12'],
    ['TST.W', 'D0'],
    ['BEQ.S', '$004002A0'],
    ['MOVE.W', '(A0)+,D2'],
    ['CMPI.W', '#$4E75,D2'],
    ['BNE.S', '$0040029A'],
    ['RTS', ''],
    ['LINK', 'A6,#-$0010'],
    ['_ExitToShell', ''],
  ];
  const rows: DebugFrameRow[] = ops.map(([mnem, o], i) => {
    const addr = pc - 8 + i * 2;
    const valid = !(mmu && i === 10);
    return { addr, phys: mmu && valid ? addr + 0x10000000 : null, valid, mnem, ops: o };
  });
  const rawRegs: Record<string, number> = {
    ...Object.fromEntries(Array.from({ length: 8 }, (_, i) => [`d${i}`, 0x1000 * i + i])),
    ...Object.fromEntries(Array.from({ length: 8 }, (_, i) => [`a${i}`, 0x00400000 + 0x100 * i])),
    pc,
    sr: 0x2700,
    usp: 0x00fffe00,
    ssp: 0x00fffc00,
  };
  return {
    arch: 'm68k',
    pc,
    rawRegs,
    regs: null,
    rows,
    fpu: {
      prefix: 'FP',
      data: Array.from({ length: 8 }, (_, i) => ({
        hex: i === 1 ? '3fff_8000000000000000' : '0000_0000000000000000',
        val: i === 1 ? '1' : '0',
      })),
      control: [
        { name: 'fpcr', value: 0 },
        { name: 'fpsr', value: 0x08000000 },
        { name: 'fpiar', value: 0x00408a12 },
      ],
    },
  };
}
