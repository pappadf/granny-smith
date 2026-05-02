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
  const hasCheckpoint = (await window.gsEval('checkpoint_probe')) === true;

  if (!hasCheckpoint) return false;

  const accept = await showCheckpointPrompt();
  if (!accept) {
    await window.gsEval('checkpoint_clear');
    toast('Starting fresh (checkpoint discarded)');
    return false;
  }

  hideRomOverlay();
  const ok = (await window.gsEval('checkpoint_load')) === true;
  if (!ok) return false;

  // The checkpoint preserves the scheduler's running flag.  Quick
  // checkpoints (captured while running) auto-resume; consolidated ones
  // (captured while paused) restore to paused.  Sync the JS-side flag
  // with the actual scheduler state so the UI stays consistent.
  const running = (await window.gsEval('running')) === true;
  setRunning(running);
  toast(running ? 'Resumed from saved checkpoint' : 'Restored checkpoint (paused)');
  return true;
}
