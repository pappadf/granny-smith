import { describe, it, expect, vi, afterEach } from 'vitest';
import { onEmulatorCrash, watchHeartbeat, watchRequest } from '@/bus/emulator';
import { machine } from '@/state/machine.svelte';

// Its own file: marking the bridge dead is once per page (module), and the
// crash tests in bridgeCrash.test.ts need a live one.
describe('the deadline and the heartbeat', () => {
  afterEach(() => vi.useRealTimers());

  it('a wedged request no longer kills the bridge: only its own caller fails', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    const seen: string[] = [];
    onEmulatorCrash((r) => seen.push(r));
    const stop = watchRequest('shell.run');
    vi.advanceTimersByTime(600_000);
    expect(seen).toEqual([]);
    expect(machine.status).not.toBe('crashed');
    stop();
  });

  it('a heartbeat that stands still for 3 visible seconds with requests pending is a dead worker', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    let heartbeat = 10;
    let inFlight = 0;
    const stalls: string[] = [];
    const stop = watchHeartbeat(
      () => ({ heartbeat, inFlight }),
      (r) => stalls.push(r),
    );
    // Nothing pending: a still heartbeat means an idle worker, not a dead one.
    vi.advanceTimersByTime(10_000);
    expect(stalls).toEqual([]);
    // Pending and beating: fine.
    inFlight = 1;
    for (let i = 0; i < 5; i++) {
      heartbeat++;
      vi.advanceTimersByTime(1_000);
    }
    expect(stalls).toEqual([]);
    // Pending and still: dead after three samples.
    vi.advanceTimersByTime(2_000);
    expect(stalls).toEqual([]);
    vi.advanceTimersByTime(1_000);
    expect(stalls).toEqual(['emulator not responding: no heartbeat for 3 s with requests pending']);
    stop();
  });

  it('hidden time does not count against the heartbeat', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'hidden', configurable: true });
    const stalls: string[] = [];
    const stop = watchHeartbeat(
      () => ({ heartbeat: 1, inFlight: 1 }),
      (r) => stalls.push(r),
    );
    vi.advanceTimersByTime(60_000);
    expect(stalls).toEqual([]);
    stop();
  });
});
