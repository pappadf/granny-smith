// The input assist: one shell.complete per text and cursor, shared by the
// browser sync and Tab; highlighting after the pause, the newest text only.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const completes: Array<[string, number]> = [];
let answer: unknown = { candidates: [], span: { start: 0, end: 0 }, context: {} };

vi.mock('@/bus/emulator', () => ({
  tabComplete: async (line: string, cursor: number) => {
    completes.push([line, cursor]);
    return answer;
  },
  gsEval: async (path: string, args: unknown[] = []) =>
    path === 'shell.highlight'
      ? [{ start: 0, end: String(args[0]).length, class: 'number' }]
      : null,
}));

import { InputAssist } from '@/components/panel-views/terminal/inputAssist.svelte';
import { terminalSync } from '@/state/terminalSync.svelte';

let assist: InputAssist;
const highlighted: string[] = [];

beforeEach(() => {
  vi.useFakeTimers();
  completes.length = 0;
  highlighted.length = 0;
  answer = { candidates: [], span: { start: 0, end: 0 }, context: {} };
  assist = new InputAssist(() => ({ setHighlight: (text) => void highlighted.push(text) }));
});
afterEach(() => {
  assist.dispose();
  vi.useRealTimers();
});

describe('InputAssist', () => {
  it('Tab reuses the answer the browser sync asked for', async () => {
    assist.onChange('mach', 4);
    await vi.advanceTimersByTimeAsync(50);
    expect(completes).toEqual([['mach', 4]]);
    expect(terminalSync.line).toBe('mach');
    await assist.complete('mach', 4);
    expect(completes).toEqual([['mach', 4]]);
    // Another cursor is another question.
    await assist.complete('mach', 2);
    expect(completes).toEqual([
      ['mach', 4],
      ['mach', 2],
    ]);
  });

  it('the sync reuses what Tab asked', async () => {
    await assist.complete('cpu', 3);
    assist.onChange('cpu', 3);
    await vi.advanceTimersByTimeAsync(50);
    expect(completes).toEqual([['cpu', 3]]);
  });

  it('no answer, or forget(), asks again', async () => {
    answer = null;
    await assist.complete('x', 1);
    await assist.complete('x', 1);
    expect(completes.length).toBe(2);
    answer = { candidates: [], span: { start: 0, end: 0 }, context: {} };
    await assist.complete('x', 1);
    assist.forget();
    await assist.complete('x', 1);
    expect(completes.length).toBe(4);
  });

  it('highlights after the pause, only the newest text, and keeps its spans', async () => {
    assist.onChange('1', 1);
    assist.onChange('12', 2);
    await vi.advanceTimersByTimeAsync(50);
    expect(highlighted).toEqual(['12']);
    expect(assist.spansFor('12')).toEqual([{ from: 0, to: 2, cls: 'number' }]);
    expect(assist.spansFor('1')).toBeUndefined();
  });

  it('after dispose nothing is asked', async () => {
    assist.onChange('m', 1);
    assist.dispose();
    await vi.advanceTimersByTimeAsync(50);
    expect(completes).toEqual([]);
    expect(highlighted).toEqual([]);
  });
});
