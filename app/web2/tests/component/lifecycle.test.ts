import { describe, it, expect, beforeEach } from 'vitest';
import {
  initEmulator,
  shutdownEmulator,
  pauseEmulator,
  resumeEmulator,
  saveCheckpoint,
} from '@/bus';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { _resetForTests, toasts } from '@/state/toasts.svelte';

beforeEach(() => {
  _resetForTests();
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  machine.mmuEnabled = false;
  stopDriveActivityMock();
});

describe('emulator lifecycle (mock)', () => {
  it('initEmulator sets machine to running with provided config', async () => {
    await initEmulator({
      model: 'Macintosh Plus',
      vrom: '(auto)',
      ram: '4 MB',
      fd: '',
      hd: 'hd1.img',
      cd: '',
    });
    expect(machine.status).toBe('running');
    expect(machine.model).toBe('Macintosh Plus');
    expect(machine.ram).toBe('4 MB');
    expect(machine.mmuEnabled).toBe(false);

    stopDriveActivityMock();
  });

  it('initEmulator fires a Machine started toast', async () => {
    await initEmulator({
      model: 'Macintosh SE/30',
      vrom: '(auto)',
      ram: '8 MB',
      fd: '',
      hd: '',
      cd: '',
    });
    expect(toasts.active.some((t) => t.msg === 'Machine started')).toBe(true);
    expect(machine.mmuEnabled).toBe(true);

    stopDriveActivityMock();
  });

  it('pause/resume flip machine.status', async () => {
    await initEmulator({
      model: 'Macintosh Plus',
      vrom: '(auto)',
      ram: '4 MB',
      fd: '',
      hd: '',
      cd: '',
    });
    await pauseEmulator();
    expect(machine.status).toBe('paused');
    await resumeEmulator();
    expect(machine.status).toBe('running');

    stopDriveActivityMock();
  });

  it('shutdownEmulator transitions to stopped (StatusBar stays visible)', async () => {
    await initEmulator({
      model: 'Macintosh Plus',
      vrom: '(auto)',
      ram: '4 MB',
      fd: '',
      hd: '',
      cd: '',
    });
    await shutdownEmulator();
    expect(machine.status).toBe('stopped');
    // 'stopped' is *not* 'no-machine'; spec §11 wants the StatusBar visible.
    expect(machine.status).not.toBe('no-machine');
  });

  it('saveCheckpoint returns a non-empty path string', async () => {
    const path = await saveCheckpoint();
    expect(path).toMatch(/^checkpoint-\d+\.gs-checkpoint$/);
  });
});
