import { describe, it, expect } from 'vitest';
import { bootstrap, whenModuleReady, getBootState } from '@/bus/emulator';

// A boot that cannot start must say so (A5, F-37): whenModuleReady
// rejects with the reason, the boot state records it, and automation gets
// window.__gsBootError.  Before, the promise stayed pending forever.
//
// Under jsdom the dynamic import of the emulator's main.mjs cannot succeed,
// so this is a real bootstrap failing on a real path, not a simulated one.
describe('bootstrap failure', () => {
  it('rejects the ready signal with the reason instead of hanging', async () => {
    await expect(bootstrap(document.createElement('canvas'))).rejects.toBeTruthy();
    await expect(whenModuleReady()).rejects.toBeInstanceOf(Error);
    const state = getBootState();
    expect(state.phase).toBe('failed');
    const reason = state.phase === 'failed' ? state.reason : '';
    expect(reason.length).toBeGreaterThan(0);
    expect((window as unknown as { __gsBootError?: string }).__gsBootError).toBe(reason);
  });

  it('does not try to start a second time after failing', async () => {
    await expect(bootstrap(document.createElement('canvas'))).resolves.toBeUndefined();
  });
});
