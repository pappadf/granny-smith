import { describe, it, expect, beforeEach } from 'vitest';
import { dispatchCoreEvent } from '@/bus/emulator';
import { EVT_STATE } from '@/bus/mailbox';
import { machine } from '@/state/machine.svelte';

// A machine that switches its own supply off (the Lisa's OS, through the
// COPS) reports it with a `power` state event: the page marks it powered
// off (the status bar says so) until a later power-on.
describe('the power state event', () => {
  beforeEach(() => {
    machine.status = 'running';
    machine.poweredOff = false;
  });

  it('marks the machine powered off, and no longer running', () => {
    dispatchCoreEvent(EVT_STATE, '{"event":"power","state":"off"}');
    expect(machine.poweredOff).toBe(true);
    expect(machine.status).toBe('paused');
    dispatchCoreEvent(EVT_STATE, '{"event":"mode_ended","reason":"stop_request"}');
    expect(machine.status).toBe('paused');
  });

  it('clears the mark when the machine is switched on again', () => {
    dispatchCoreEvent(EVT_STATE, '{"event":"power","state":"off"}');
    dispatchCoreEvent(EVT_STATE, '{"event":"power","state":"on"}');
    expect(machine.poweredOff).toBe(false);
    dispatchCoreEvent(EVT_STATE, '{"event":"mode_started"}');
    expect(machine.status).toBe('running');
  });
});
