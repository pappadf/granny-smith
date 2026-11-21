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
import { gsEval, gsErrorText, gsOk } from './emulator';
import { reconcileUiWithMachine } from './boot';
import { machine } from '@/state/machine.svelte';
import {
  checkpointPrompt,
  showCheckpointPrompt,
  resolveCheckpointPrompt,
} from '@/state/checkpointPrompt.svelte';
import { showNotification } from '@/state/toasts.svelte';
import { compactLocalStamp } from '@/lib/time';

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
  // Withdrawn by a newer prompt: that one owns the checkpoint now
  if (accept === null) return false;
  if (!accept) {
    const cleared = await gsEval('checkpoint.clear');
    if (gsOk(cleared)) showNotification('Starting fresh (checkpoint discarded)', 'info');
    else
      showNotification(
        `Starting fresh, but the checkpoint could not be discarded: ${gsErrorText(cleared)}`,
        'warning',
      );
    return false;
  }

  const ok = await gsEval('checkpoint.load');
  if (ok !== true) {
    showNotification(`Checkpoint load failed: ${gsErrorText(ok)}`, 'error');
    return false;
  }
  await reconcileUiWithMachine('restore');

  const running = machine.status === 'running';
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
  { ok: true; name: string } | { ok: false; step: 'save' | 'save to computer'; message: string };

export async function saveCheckpoint(): Promise<SaveCheckpointResult> {
  const name = `saved-state-${compactLocalStamp()}.bin`;
  // Written in the OPFS scratch area, not the memory-backed /tmp: a machine
  // with a large modified disk would otherwise hold its whole state in the
  // wasm heap while the download is made.  In a directory of its own, so no
  // other Save State shares the path and the download keeps the file's name.
  const dir = scratchPath('save');
  const tmpPath = `${dir}/${name}`;
  try {
    await gsEval('files.mkdir', [dir]);
    // Both methods return VK_BOOL false on failure (a full quota, no machine,
    // a download that could not read the file back) — check each.
    const saved = await gsEval('checkpoint.save', [tmpPath]);
    if (saved !== true) return { ok: false, step: 'save', message: gsErrorText(saved) };
    const downloaded = await gsEval('files.download', [tmpPath]);
    if (downloaded !== true)
      return { ok: false, step: 'save to computer', message: gsErrorText(downloaded) };
    return { ok: true, name };
  } finally {
    // The download has already copied it out, so remove it on every exit
    // (one left by a closed tab goes when the scratch area is emptied at
    // the next startup).
    await gsEval('files.rm', [dir]);
  }
}

// --- The Checkpoints panel ------------------------------------------------

// Save a labelled checkpoint into `dir` (made under `parent`): the machine's
// own checkpoint first, so its autosave (what a reload resumes) is this
// moment too -- checkpoint.snapshot returns once the file is complete --
// then a copy at `dir`/state.checkpoint.  A failed copy leaves no
// directory behind.  The reason is the core's, when it gave one.
export async function saveLabelledCheckpoint(
  label: string,
  parent: string,
  dir: string,
): Promise<{ ok: true } | { ok: false; reason?: string }> {
  if ((await gsEval('checkpoint.snapshot', [label])) !== true) return { ok: false };
  await gsEval('files.mkdir', [parent]);
  await gsEval('files.mkdir', [dir]);
  const ok = await gsEval('checkpoint.save', [`${dir}/state.checkpoint`]);
  if (ok !== true) {
    await gsEval('files.rm', [dir]);
    return { ok: false, reason: gsErrorText(ok) };
  }
  return { ok: true };
}

// Load the checkpoint in `dir`: true, or the core's reason.  gsEval never
// rejects: a transport failure is an {error} result too.
export async function loadCheckpointFrom(dir: string): Promise<true | string> {
  const ok = await gsEval('checkpoint.load', [`${dir}/state.checkpoint`]);
  return ok === true ? true : gsErrorText(ok);
}
