// The bus is real (no stub state changes), so lifecycle calls
// against the un-booted bus return null without touching machine state.
// Real-emulator coverage stays manual in the browser; bus internals are
// covered via the unit suites (machineId, urlMedia.parse, archive, etc.).

import { describe, it, expect, beforeEach } from 'vitest';
import {
  initEmulator,
  shutdownEmulator,
  pauseEmulator,
  resumeEmulator,
  isModuleReady,
} from '@/bus';
import { machine } from '@/state/machine.svelte';
import { _resetForTests, toasts } from '@/state/toasts.svelte';

beforeEach(() => {
  _resetForTests();
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  machine.mmuEnabled = false;
});

describe('emulator lifecycle (no Module in jsdom)', () => {
  it('isModuleReady returns false before bootstrap', () => {
    expect(isModuleReady()).toBe(false);
  });

  it('initEmulator is a no-op against an un-booted bus', async () => {
    await initEmulator({ model: 'plus' });
    // machine state remains as the test reset left it (status still
    // 'no-machine' since gsEval returns null without a Module).
    expect(machine.status).toBe('no-machine');
  });

  it('pause/resume/shutdown each resolve to undefined (no Module)', async () => {
    await expect(pauseEmulator()).resolves.toBeUndefined();
    await expect(resumeEmulator()).resolves.toBeUndefined();
    // Without a Module the stop fails ({error}); shutdownEmulator must say so
    // rather than show a machine that is still running as stopped.
    await shutdownEmulator();
    expect(machine.status).toBe('no-machine');
    expect(toasts.active.some((t) => /Stop failed: emulator not ready/.test(t.msg))).toBe(true);
  });
});
