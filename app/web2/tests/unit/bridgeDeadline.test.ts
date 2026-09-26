import { describe, it, expect, vi, afterEach } from 'vitest';
import { onEmulatorCrash, watchRequest } from '@/bus/emulator';
import { machine } from '@/state/machine.svelte';

// Its own file: marking the bridge dead is once per page (module), and the
// crash tests in bridgeCrash.test.ts need a live one.
describe('the deadline (A6)', () => {
  afterEach(() => vi.useRealTimers());

  it('marks an ordinary request wedged past 120 visible seconds as a dead bridge', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    const seen: string[] = [];
    onEmulatorCrash((r) => seen.push(r));
    const stop = watchRequest('shell.run');
    vi.advanceTimersByTime(119_000);
    expect(seen).toEqual([]);
    expect(machine.status).not.toBe('crashed');
    vi.advanceTimersByTime(1_000);
    expect(seen).toEqual(["'shell.run' did not complete within 120 s"]);
    expect(machine.status).toBe('crashed');
    stop();
  });
});
