import { describe, it, expect, vi } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { debugFrame, refreshDebugFrame, DISASM_ROWS, ROWS_BEFORE_PC } =
  await import('@/state/debugFrame.svelte');

// One debug.frame per pause, shared by the panes (11-WORK-ORDER D4, F-45).
describe('the shared debug frame', () => {
  it('coalesces overlapping refreshes into one fetch plus one re-run', async () => {
    bridge.reset();
    bridge.reply('machine.cpu.frame', { arch: 'm68k', pc: 0x400, regs: { pc: 0x400 }, rows: [] });
    // Three panes asking at once, as they did on every pause.
    await Promise.all([refreshDebugFrame(), refreshDebugFrame(), refreshDebugFrame()]);
    const frames = bridge.calls.filter((c) => c.path === 'machine.cpu.frame');
    expect(frames.length).toBe(2);
    expect(frames[0].args).toEqual({ count: DISASM_ROWS, before: ROWS_BEFORE_PC });
    expect(debugFrame.current?.pc).toBe(0x400);
  });
});
