import { describe, it, expect, vi, afterEach } from 'vitest';
import {
  gsEval,
  markBridgeDead,
  onEmulatorCrash,
  isWorkerCrashLine,
  watchRequest,
} from '@/bus/emulator';
import { machine } from '@/state/machine.svelte';
import { bridgeBusy } from '@/state/activity.svelte';

// Slow is not dead.  A long request raises a notice and is
// never abandoned; a crashed worker fails every request at once.

describe('the busy notice', () => {
  afterEach(() => vi.useRealTimers());

  it('appears after five visible seconds, and goes when the request ends', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    const stop = watchRequest('debug.step');
    vi.advanceTimersByTime(4_000);
    expect(bridgeBusy.path).toBeNull();
    vi.advanceTimersByTime(1_000);
    expect(bridgeBusy.path).toBe('debug.step');
    expect(bridgeBusy.seconds).toBe(5);
    stop();
    expect(bridgeBusy.path).toBeNull();
  });

  it('waits longer for a legitimately long request, and ignores hidden time', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'hidden', configurable: true });
    const stop = watchRequest('checkpoint.save');
    vi.advanceTimersByTime(120_000); // hidden: does not count
    expect(bridgeBusy.path).toBeNull();
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    vi.advanceTimersByTime(29_000);
    expect(bridgeBusy.path).toBeNull();
    vi.advanceTimersByTime(1_000);
    expect(bridgeBusy.path).toBe('checkpoint.save');
    stop();
  });
});

describe('the deadline', () => {
  afterEach(() => vi.useRealTimers());

  it('never applies to a legitimately long request', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    const stop = watchRequest('storage.cp');
    vi.advanceTimersByTime(600_000);
    expect(machine.status).not.toBe('crashed');
    stop();
  });
});

describe('a crashed worker', () => {
  it('recognises the glue crash lines', () => {
    expect(isWorkerCrashLine('worker sent an error! main.mjs:1: RuntimeError: unreachable')).toBe(
      true,
    );
    expect(isWorkerCrashLine('Aborted(Cannot enlarge memory arrays)')).toBe(true);
    expect(isWorkerCrashLine('floppy: inserted disk')).toBe(false);
  });

  it('fails every request at once and tells the page', async () => {
    const seen: string[] = [];
    onEmulatorCrash((r) => seen.push(r));
    markBridgeDead('worker sent an error! trap');
    markBridgeDead('a second report'); // only the first counts
    expect(seen).toEqual(['worker sent an error! trap']);
    expect(machine.status).toBe('crashed');
    const r = await gsEval('machine.cpu.pc');
    expect(r).toEqual({ error: 'emulator crashed: worker sent an error! trap', transport: true });
  });
});
