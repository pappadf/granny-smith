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

import { scratchPath } from '@/lib/opfsPaths';
import { gsEval, gsErrorText } from './emulator';
import { reconcileUiWithMachine } from './boot';
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
  await reconcileUiWithMachine('restore');

  const running = (await gsEval('scheduler.running')) === true;
  machine.status = running ? 'running' : 'paused';
  showNotification(
    running ? 'Resumed from saved checkpoint' : 'Restored checkpoint (paused)',
    'info',
  );
  return true;
}

// --- Save State (the toolbar's download) ---------------------------------

// The outcome of Save State: the name the browser downloads the file as, or
// which step failed and why.
export type SaveCheckpointResult =
  { ok: true; name: string } | { ok: false; step: 'save' | 'download'; message: string };

export async function saveCheckpoint(): Promise<SaveCheckpointResult> {
  const name = `saved-state-${compactTimestamp()}.bin`;
  // Written in the OPFS scratch area, not the memory-backed /tmp: a machine
  // with a large modified disk would otherwise hold its whole state in the
  // wasm heap while the download is made.  In a directory of its own, so no
  // other Save State shares the path and the download keeps the file's name.
  const dir = scratchPath('save');
  const tmpPath = `${dir}/${name}`;
  try {
    await gsEval('files.mkdir', [dir]);
    // Both methods return V_BOOL false on failure (a full quota, no machine,
    // a download that could not read the file back) — check each.
    const saved = await gsEval('checkpoint.save', [tmpPath]);
    if (saved !== true) return { ok: false, step: 'save', message: gsErrorText(saved) };
    const downloaded = await gsEval('files.download', [tmpPath]);
    if (downloaded !== true)
      return { ok: false, step: 'download', message: gsErrorText(downloaded) };
    return { ok: true, name };
  } finally {
    // The download has already copied it out, so remove it on every exit
    // (one left by a closed tab goes when the scratch area is emptied at
    // the next startup).
    await gsEval('files.rm', [dir]);
  }
}

// Local-time YYYYMMDD-HHMMSS for a download name.
function compactTimestamp(): string {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  return (
    `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-` +
    `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`
  );
}
