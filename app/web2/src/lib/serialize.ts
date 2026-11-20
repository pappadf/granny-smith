// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// One run at a time, the latest intent last: `coalesced(fn)` returns a
// function that never overlaps two runs of `fn`.  A call arriving while one
// is in flight waits for it, then runs `fn` again, so the final run always
// sees the state as it is after the last call.  The camera and microphone
// reconcile their device with it: two overlapping reconciles each acquired
// a device (camera), or one closed the other's AudioContext mid-construction
// (microphone).
export function coalesced(fn: () => Promise<void>): () => Promise<void> {
  let running: Promise<void> | null = null;
  return async function run(): Promise<void> {
    // Wait out the run in flight; another waiter may start the next one
    // first, in which case wait for that one too.
    while (running) {
      const inFlight = running;
      await inFlight;
      if (running === inFlight) break;
    }
    running = fn();
    try {
      await running;
    } finally {
      running = null;
    }
  };
}
