// Phase 3: the bus is now real (no stub state changes), so lifecycle calls
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
import { _resetForTests } from '@/state/toasts.svelte';

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
    await initEmulator({
      model: 'Macintosh Plus',
      vrom: '(auto)',
      ram: '4 MB',
      floppies: [],
      hd: '(none)',
      cd: '(none)',
    });
    // machine state remains as the test reset left it (status still
    // 'no-machine' since gsEval returns null without a Module).
    expect(machine.status).toBe('no-machine');
  });

  it('pause/resume/shutdown each resolve to undefined (no Module)', async () => {
    await expect(pauseEmulator()).resolves.toBeUndefined();
    await expect(resumeEmulator()).resolves.toBeUndefined();
    // shutdownEmulator updates machine.status optimistically since the user
    // expects the Welcome view to come back; the gsEval call inside is a
    // no-op without a Module.
    await shutdownEmulator();
    expect(machine.status).toBe('stopped');
  });
});
