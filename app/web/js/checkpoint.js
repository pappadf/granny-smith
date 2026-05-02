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
// Uses runCommand rather than gsEval because this fires during the boot
// window between Module-ready and the worker's main loop becoming
// active, where ccall-based gsEval requests are not yet served.
// runCommand naturally waits via the cmd_pending flag the main loop
// polls. The matching root methods (checkpoint_probe / _clear / _load /
// `running`) are still registered for the post-boot callers (drop.js).
export async function maybeOfferBackgroundCheckpoint() {
  let hasCheckpoint = (await window.runCommand('checkpoint --probe')) === 0;

  if (!hasCheckpoint) return false;

  const accept = await showCheckpointPrompt();
  if (!accept) {
    // User declined: clear all checkpoint files
    await window.runCommand('checkpoint clear');
    toast('Starting fresh (checkpoint discarded)');
    return false;
  }

  hideRomOverlay();
  // Auto-load the latest valid checkpoint (no filename needed)
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
