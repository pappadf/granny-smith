import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const {
  listBreakpoints,
  addBreakpoint,
  removeBreakpoint,
  removeBreakpointAt,
  writeRegister,
  stepInto,
  loadDebugFrame,
} = await import('@/bus/debug');
const { debug } = await import('@/state/debug.svelte');

// Teach the double two sparse breakpoints, ids 2 and 5 (0, 1, 3, 4 removed).
function twoBreakpoints(): void {
  bridge.reply('debug.breakpoints.meta.indices', (args: unknown[] | undefined) =>
    args?.[0] === 'entries' ? [2, 5] : { error: 'x' },
  );
  const entries: Record<
    number,
    { addr: number; enabled: boolean; condition: string; hit_count: number }
  > = {
    2: { addr: 0x400100, enabled: true, condition: '', hit_count: 0 },
    5: { addr: 0x400200, enabled: false, condition: 'd0 == 1', hit_count: 3 },
  };
  for (const [id, e] of Object.entries(entries)) {
    for (const [k, v] of Object.entries(e))
      bridge.reply(`debug.breakpoints.entries[${id}].${k}`, v);
  }
}

describe('bus/debug against the real object-model paths', () => {
  beforeEach(() => bridge.reset());

  it('lists breakpoints by their live ids, never by count', async () => {
    twoBreakpoints();
    const list = await listBreakpoints();
    expect(list.map((b) => b.id)).toEqual([2, 5]);
    expect(list[1]).toMatchObject({
      addr: 0x400200,
      enabled: false,
      condition: 'd0 == 1',
      hits: 3,
    });
    // The path that never resolved is gone.
    expect(bridge.paths().some((p) => p.includes('.count'))).toBe(false);
  });

  it('removes by id through the entry, and reports a core error as failure', async () => {
    bridge.reply('debug.breakpoints.entries[5].remove', null); // V_NONE success
    expect(await removeBreakpoint(5)).toBe(true);
    expect(bridge.calls.at(-1)).toEqual({
      path: 'debug.breakpoints.entries[5].remove',
      args: undefined,
    });
    // No route: the double answers { error } as the core does.
    expect(await removeBreakpoint(9)).toBe(false);
    // Nothing ever calls add to remove.
    expect(bridge.paths()).not.toContain('debug.breakpoints.add');
  });

  it('removes by address by finding the id first', async () => {
    twoBreakpoints();
    bridge.reply('debug.breakpoints.entries[5].remove', null);
    expect(await removeBreakpointAt(0x400200)).toBe(true);
    expect(bridge.paths()).toContain('debug.breakpoints.entries[5].remove');
    expect(await removeBreakpointAt(0x999999)).toBe(false);
  });

  it('reports a failed add', async () => {
    expect(await addBreakpoint(0x400100)).toBe(false);
    bridge.reply('debug.breakpoints.add', { kind: 'object' });
    expect(await addBreakpoint(0x400100, ' d0 == 1 ')).toBe(true);
    expect(bridge.calls.at(-1)).toEqual({
      path: 'debug.breakpoints.add',
      args: [0x400100, 'd0 == 1'],
    });
  });

  it('writes a register through the typed setter', async () => {
    bridge.reply('machine.cpu.d0', null);
    expect(await writeRegister('d0', 0x1234)).toBe(true);
    expect(bridge.calls.at(-1)).toEqual({ path: 'machine.cpu.d0', args: [0x1234] });
  });

  it('refuses a register name that is not one identifier, without calling the core', async () => {
    expect(await writeRegister('d0 = 1', 5)).toBe(false);
    expect(await writeRegister('mmu.tc', 5)).toBe(false);
    expect(bridge.calls).toEqual([]);
  });

  it('reports a register the core refuses', async () => {
    expect(await writeRegister('r99', 1)).toBe(false);
  });

  it('steps, and makes the panes re-fetch', async () => {
    bridge.reply('debug.step', true);
    const gen = debug.refreshGen;
    await stepInto(1);
    expect(bridge.calls).toEqual([{ path: 'debug.step', args: [1] }]);
    expect(debug.refreshGen).toBe(gen + 1);
  });

  it('asks for the frame by named arguments', async () => {
    bridge.reply('machine.cpu.frame', { arch: 'm68k', pc: 0x400, regs: { pc: 0x400 }, rows: [] });
    await loadDebugFrame();
    await loadDebugFrame(0x1000, 8);
    expect(bridge.calls.map((c) => c.args)).toEqual([{ count: 32 }, { count: 8, addr: 0x1000 }]);
  });

  it('never fills a 68K register view on another architecture', async () => {
    bridge.reply('machine.cpu.frame', {
      arch: 'ppc',
      pc: 0xfff0345c,
      regs: { r1: 0x1234, pc: 0xfff0345c, lr: 0 },
      rows: [{ addr: 0xfff0345c, phys: 0xfff0345c, valid: true, mnem: 'b', ops: '' }],
      fpu: { fpr: [], fpscr: 0 },
    });
    const f = await loadDebugFrame();
    expect(f?.arch).toBe('ppc');
    expect(f?.regs).toBeNull(); // not d0..a7 of zeros
    expect(f?.rawRegs.r1).toBe(0x1234);
    expect(f?.pc).toBe(0xfff0345c);
    // The PPC FPU block maps onto the generic shape, not the 68K one.
    expect(f?.fpu).toEqual({ prefix: 'FPR', data: [], control: [{ name: 'fpscr', value: 0 }] });
  });

  // An auxiliary core answers the same frame (D7): the AV DSP3210.
  it("reads an auxiliary core's frame from its own node", async () => {
    bridge.reply('machine.dsp.frame', {
      arch: 'dsp3210',
      pc: 0x50030000,
      regs: { r1: 7, pc: 0x50030000, emr: 0x8000 },
      rows: [{ addr: 0x50030000, phys: 0x50030000, valid: true, mnem: 'r1 = r2', ops: '' }],
      fpu: { a: [{ hex: '40000000_81', val: '1' }] },
    });
    const f = await loadDebugFrame(undefined, 8, 2, 'dsp');
    expect(bridge.calls).toEqual([{ path: 'machine.dsp.frame', args: { count: 8, before: 2 } }]);
    expect(f?.arch).toBe('dsp3210');
    expect(f?.regs).toBeNull();
    expect(f?.rawRegs.emr).toBe(0x8000);
    expect(f?.fpu).toEqual({ prefix: 'A', data: [{ hex: '40000000_81', val: '1' }], control: [] });
  });

  it('refuses a core name that is not one identifier, without calling the core', async () => {
    expect(await loadDebugFrame(undefined, 8, 0, 'cpu.mmu')).toBeNull();
    expect(bridge.calls).toEqual([]);
  });
});
