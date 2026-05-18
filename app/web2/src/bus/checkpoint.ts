// Checkpoint resume orchestration. Port of app/web/js/checkpoint.js.
//
// Flow at boot (see main.ts):
//   1. After bus.bootstrap(), call maybeOfferBackgroundCheckpoint().
//   2. It probes via gsEval('checkpoint.probe'). If false, returns false.
//   3. Else it opens the prompt via state.showCheckpointPrompt(); the
//      CheckpointResumePrompt component renders, the user picks Resume or
//      Discard, the component calls state.resolveCheckpointPrompt(accept).
//   4. On Resume, gsEval('checkpoint.load') runs and the bus updates
//      machine.status from scheduler.running.

import { gsEval } from './emulator';
import { machine } from '@/state/machine.svelte';
import {
  checkpointPrompt,
  showCheckpointPrompt,
  resolveCheckpointPrompt,
} from '@/state/checkpointPrompt.svelte';
import { showNotification } from '@/state/toasts.svelte';

export function isResumePending(): boolean {
  return checkpointPrompt.shown;
}

export function resolveResume(accept: boolean): void {
  resolveCheckpointPrompt(accept);
}

// Returns true when the user accepted the resume (i.e. machine is now live).
export async function maybeOfferBackgroundCheckpoint(): Promise<boolean> {
  const probe = await gsEval('checkpoint.probe');
  if (probe !== true) return false;

  const accept = await showCheckpointPrompt();
  if (!accept) {
    await gsEval('checkpoint.clear');
    showNotification('Starting fresh (checkpoint discarded)', 'info');
    return false;
  }

  const ok = (await gsEval('checkpoint.load')) === true;
  if (!ok) {
    showNotification('Checkpoint load failed', 'error');
    return false;
  }

  const running = (await gsEval('scheduler.running')) === true;
  machine.status = running ? 'running' : 'paused';
  showNotification(
    running ? 'Resumed from saved checkpoint' : 'Restored checkpoint (paused)',
    'info',
  );
  return true;
}
