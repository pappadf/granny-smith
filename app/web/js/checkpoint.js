// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Checkpoint resume orchestration: probes for existing checkpoints and offers
// the user a choice to resume or start fresh.
import { setRunning } from './emulator.js';
import { showCheckpointPrompt } from './dialogs.js';
import { toast, hideRomOverlay } from './ui.js';

// Probe for a background checkpoint and offer resume if found.
// Returns true if the user resumed from a checkpoint.
export async function maybeOfferBackgroundCheckpoint() {
  // Probe for a valid checkpoint via the C-side load-state probe command.
  // With OPFS, the filesystem is always up to date — no sync needed.
  let hasCheckpoint = (await window.runCommand('load-state probe')) === 0;

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
  const rc = await window.runCommand('load-state');
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
