import { describe, it, expect } from 'vitest';
import { dispatchCoreEvent } from '@/bus/emulator';
import { EVT_STATE } from '@/bus/mailbox';

// window.__gsCoreEvents, the trace the e2e specs read.  In turbo the core
// re-announces the speed figure as its measured ratio moves, a few times a
// second; with one cap over all events, those pushed the mode events a spec
// asserts on (shell-prompt: two stop_request mode_ended) out of the trace.

type Ev = { kind: string; event: string; data: Record<string, unknown> };
const trace = () => (window as unknown as { __gsCoreEvents?: Ev[] }).__gsCoreEvents ?? [];

describe('the core event trace', () => {
  it('keeps the rare events when a chatty one floods it', () => {
    dispatchCoreEvent(EVT_STATE, '{"event":"mode_ended","reason":"stop_request"}');
    dispatchCoreEvent(EVT_STATE, '{"event":"mode_started"}');
    dispatchCoreEvent(EVT_STATE, '{"event":"mode_ended","reason":"stop_request"}');
    for (let i = 0; i < 500; i++)
      dispatchCoreEvent(EVT_STATE, `{"event":"speed","x256":${256 + i}}`);
    const evs = trace();
    expect(evs.filter((e) => e.event === 'mode_ended')).toHaveLength(2);
    // The flood itself is still bounded, and keeps its newest entries.
    const speeds = evs.filter((e) => e.event === 'speed');
    expect(speeds).toHaveLength(64);
    expect(speeds[speeds.length - 1].data.x256).toBe(256 + 499);
  });
});
