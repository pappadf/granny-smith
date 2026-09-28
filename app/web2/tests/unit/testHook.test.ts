import { describe, it, expect, vi } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { installEvalHookForAutomation } = await import('@/bus/testHook');

// A minimal window stand-in with a controllable webdriver flag.
function fakeWindow(webdriver: boolean): Window {
  return { navigator: { webdriver } } as unknown as Window;
}

type Hooked = { __gsEvalForTests?: (p: string, a?: unknown[]) => Promise<unknown> };

describe('installEvalHookForAutomation', () => {
  it('does nothing for an ordinary browser', () => {
    const win = fakeWindow(false);
    expect(installEvalHookForAutomation(win)).toBe(false);
    expect((win as unknown as Hooked).__gsEvalForTests).toBeUndefined();
  });

  it('exposes the real bridge under automation', async () => {
    bridge.reset();
    bridge.reply('machine.cpu.pc', 0x400);
    const win = fakeWindow(true);
    expect(installEvalHookForAutomation(win)).toBe(true);
    const hook = (win as unknown as Hooked).__gsEvalForTests!;
    expect(await hook('machine.cpu.pc')).toBe(0x400);
    expect(bridge.paths()).toEqual(['machine.cpu.pc']);
  });
});
