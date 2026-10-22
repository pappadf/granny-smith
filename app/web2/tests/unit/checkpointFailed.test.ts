import { describe, it, expect, beforeEach } from 'vitest';
import {
  noteCheckpointFailed,
  setCheckpointSaved,
  CHECKPOINT_FAILURE_REPEAT_MS,
} from '@/state/machine.svelte';
import { toasts, _resetForTests } from '@/state/toasts.svelte';

// S1018-05: an autosave the core could not write (notify:checkpoint_failed)
// is said, once per run of failures, not once per attempt.
describe('autosave failure toast', () => {
  beforeEach(() => {
    _resetForTests();
    // End any run left by an earlier test.
    setCheckpointSaved(0);
  });

  it('says the first failure, with its reason', () => {
    expect(noteCheckpointFailed('out of memory', 1000)).toBe(true);
    expect(toasts.active).toHaveLength(1);
    expect(toasts.active[0].severity).toBe('error');
    expect(toasts.active[0].msg).toMatch(/Automatic save failed \(out of memory\)/);
  });

  it('stays quiet while the failures repeat, and speaks again after a while', () => {
    expect(noteCheckpointFailed('x', 1000)).toBe(true);
    expect(noteCheckpointFailed('x', 6000)).toBe(false);
    expect(noteCheckpointFailed('x', 1000 + CHECKPOINT_FAILURE_REPEAT_MS - 1)).toBe(false);
    expect(noteCheckpointFailed('x', 1000 + CHECKPOINT_FAILURE_REPEAT_MS)).toBe(true);
  });

  it('a save that works ends the run: the next failure is said at once', () => {
    expect(noteCheckpointFailed('x', 1000)).toBe(true);
    setCheckpointSaved(12);
    expect(noteCheckpointFailed('x', 2000)).toBe(true);
  });
});
