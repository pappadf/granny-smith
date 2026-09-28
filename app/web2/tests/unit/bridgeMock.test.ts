import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { gsEval, gsErrorText } = await import('@/bus/emulator');

describe('bridgeMock', () => {
  beforeEach(() => bridge.reset());

  it('records every call and answers from the route table', async () => {
    bridge.reply('machine.cpu.pc', 0x1234);
    bridge.reply('echo', (args: unknown[] | undefined) => args?.[0]);
    expect(await gsEval('machine.cpu.pc')).toBe(0x1234);
    expect(await gsEval('echo', ['x'])).toBe('x');
    expect(bridge.calls).toEqual([
      { path: 'machine.cpu.pc', args: undefined },
      { path: 'echo', args: ['x'] },
    ]);
  });

  it('answers an unknown path the way gs_eval does', async () => {
    const r = await gsEval('machine.cpu.d0 = 0x1');
    expect(r).toEqual({ error: "path 'machine.cpu.d0 = 0x1' did not resolve" });
    // The real gsErrorText is kept, so tests see the real message handling.
    expect(gsErrorText(r)).toContain('did not resolve');
  });
});
