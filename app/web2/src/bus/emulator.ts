// Emulator bus — Phase 2 stub. Drives machine.svelte.ts state directly
// without touching WASM. Phase 3 replaces these bodies with real Emscripten
// module init + gsEval calls; the function signatures stay the same so
// components don't change.

import { machine, startDriveActivityMock, stopDriveActivityMock } from '@/state/machine.svelte';
import { showNotification } from '@/state/toasts.svelte';
import { modelHasMmu } from '@/lib/machine';
import type { MachineConfig } from './types';

export async function initEmulator(config: MachineConfig): Promise<void> {
  machine.model = config.model;
  machine.ram = config.ram;
  machine.mmuEnabled = modelHasMmu(config.model);
  machine.status = 'running';
  startDriveActivityMock();
  showNotification('Machine started', 'info');
}

export async function shutdownEmulator(): Promise<void> {
  machine.status = 'stopped';
  stopDriveActivityMock();
  showNotification('Machine stopped', 'info');
}

export async function pauseEmulator(): Promise<void> {
  machine.status = 'paused';
  stopDriveActivityMock();
}

export async function resumeEmulator(): Promise<void> {
  machine.status = 'running';
  startDriveActivityMock();
}

// Returns the mock checkpoint path. Toast is fired at the call site so the
// caller can do the disable / re-enable UX pattern.
export async function saveCheckpoint(): Promise<string> {
  return `checkpoint-${Date.now()}.gs-checkpoint`;
}
