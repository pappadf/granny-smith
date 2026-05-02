// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Checkpoint resume orchestration: probes for existing checkpoints and offers
// the user a choice to resume or start fresh.
import { setRunning } from './emulator.js';
import { showCheckpointPrompt } from './dialogs.js';
import { toast, hideRomOverlay } from './ui.js';

// Probe for a background checkpoint and offer resume if found.
// Returns true if the user resumed from a checkpoint.
//
// Stays on runCommand for the same boot-window reason as main.js's
// register_machine call: switching to gsEval reorders this against
// OPFS state in a way that causes the resume dialog to fire on stale
// checkpoints from a previous session and block UI tests. The
// matching root methods (checkpoint_probe / _clear / _load / running)
// are still registered for post-boot callers (drop.js).
export async function maybeOfferBackgroundCheckpoint() {
  let hasCheckpoint = (await window.runCommand('checkpoint --probe')) === 0;

  if (!hasCheckpoint) return false;

  const accept = await showCheckpointPrompt();
  if (!accept) {
    await window.runCommand('checkpoint clear');
    toast('Starting fresh (checkpoint discarded)');
    return false;
  }

  hideRomOverlay();
  const rc = await window.runCommand('checkpoint --load');
  if (rc !== 0) return false;

  // The checkpoint preserves the scheduler's running flag.  Quick
  // checkpoints (captured while running) auto-resume; consolidated ones
  // (captured while paused) restore to paused.  Sync the JS-side flag
  // with the actual scheduler state so the UI stays consistent.
  const running = (await window.runCommand('status')) === 1;
  setRunning(running);
  toast(running ? 'Resumed from saved checkpoint' : 'Restored checkpoint (paused)');
  return true;
}
