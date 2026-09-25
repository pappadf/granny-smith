import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { listBreakpoints, addBreakpoint, removeBreakpoint, removeBreakpointAt, writeRegister } =
  await import('@/bus/debug');

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
});
