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

  it('a heartbeat that stands still for 3 visible seconds with requests pending is a stall', () => {
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

  it('a stall is not the end: the watch goes on, and reports the heartbeat coming back', () => {
    vi.useFakeTimers();
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    let heartbeat = 1;
    let inFlight = 1;
    const stalls: string[] = [];
    const resumes: number[] = [];
    const stop = watchHeartbeat(
      () => ({ heartbeat, inFlight }),
      (r) => stalls.push(r),
      (ms) => resumes.push(ms),
    );
    vi.advanceTimersByTime(4_000);
    expect(stalls).toHaveLength(1);
    // Still stalled, the requests timed out: one stall, not one a second.
    inFlight = 0;
    vi.advanceTimersByTime(10_000);
    expect(stalls).toHaveLength(1);
    expect(resumes).toEqual([]);
    // The core comes back: the stall ends, with how long it lasted.
    heartbeat++;
    vi.advanceTimersByTime(1_000);
    expect(resumes).toHaveLength(1);
    expect(resumes[0]).toBeGreaterThanOrEqual(13_000);
    // ...and a second stall is a new one.
    inFlight = 1;
    vi.advanceTimersByTime(4_000);
    expect(stalls).toHaveLength(2);
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
