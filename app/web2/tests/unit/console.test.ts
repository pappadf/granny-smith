// The console state: each createConsole() is independent, runs its queue
// one job at a time, and dispose() stops it for good -- the model's frame
// cancelled, a job in flight finishing without touching the state.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const runs: string[] = [];
let release: (() => void) | null = null;

vi.mock('@/bus/emulator', () => ({
  getRuntimePrompt: () => 'gs>',
  isModuleReady: () => true,
  whenModuleReady: async () => {},
  shellInterrupt: async () => 'nothing',
  gsEvalLine: async (line: string) => {
    runs.push(line);
    await new Promise<void>((r) => (release = r));
  },
}));

import { createConsole, type Console } from '@/state/console.svelte';

const nextFrame = () => new Promise((r) => requestAnimationFrame(() => r(null)));
const texts = (c: Console) => c.state.entries.map((e) => [e.kind, e.text]);

let c: Console;
beforeEach(() => {
  runs.length = 0;
  release = null;
  c = createConsole();
});
afterEach(() => c.dispose());

describe('createConsole', () => {
  it('instances do not share entries or queues', async () => {
    const other = createConsole();
    c.notice('mine');
    await nextFrame();
    expect(texts(c)).toEqual([['text', 'mine']]);
    expect(texts(other)).toEqual([]);
    other.dispose();
  });

  it('runs submissions in order and reports each job done', async () => {
    const done = vi.fn();
    c.onJobDone(done);
    c.submit('echo one');
    c.submit('echo two');
    await vi.waitFor(() => expect(runs).toEqual(['echo one']));
    expect(c.state.queued).toBe(1);
    expect(c.state.runningSince).not.toBeNull();
    release?.();
    await vi.waitFor(() => expect(runs).toEqual(['echo one', 'echo two']));
    release?.();
    await vi.waitFor(() => expect(done).toHaveBeenCalledTimes(2));
    expect(c.state.runningSince).toBeNull();
    expect(c.state.prompt).toBe('gs>');
    await nextFrame();
    expect(texts(c)).toEqual([
      ['command', 'echo one'],
      ['command', 'echo two'],
    ]);
  });

  it('dispose cancels the pending frame', async () => {
    const cancel = vi.spyOn(globalThis, 'cancelAnimationFrame');
    c.notice('never shown');
    c.dispose();
    expect(cancel).toHaveBeenCalled();
    cancel.mockRestore();
    await nextFrame();
    expect(texts(c)).toEqual([]);
    c.model.push({ kind: 'print', line: 'late' });
    await nextFrame();
    expect(texts(c)).toEqual([]);
  });

  it('a job in flight at dispose ends without touching the state', async () => {
    const done = vi.fn();
    c.onJobDone(done);
    c.submit('echo one');
    c.submit('echo two');
    await vi.waitFor(() => expect(runs).toEqual(['echo one']));
    const since = c.state.runningSince;
    c.dispose();
    release?.();
    await new Promise((r) => setTimeout(r, 10));
    expect(runs).toEqual(['echo one']);
    expect(done).not.toHaveBeenCalled();
    expect(c.state.runningSince).toBe(since);
    c.submit('echo three');
    expect(runs).toEqual(['echo one']);
  });

  it('interrupt drops the queue and says when there is nothing to stop', async () => {
    c.submit('echo one');
    c.submit('echo two');
    await vi.waitFor(() => expect(runs).toEqual(['echo one']));
    await c.interrupt();
    expect(c.state.queued).toBe(0);
    release?.();
    await nextFrame();
    expect(texts(c)).toContainEqual([
      'text',
      '^C  (nothing to interrupt; Pause stops the machine)',
    ]);
    expect(runs).toEqual(['echo one']);
  });
});
