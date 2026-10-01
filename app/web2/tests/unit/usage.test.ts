// shell.usage answers: kept per path (a failed one is asked again), the
// argument span in UTF-16, and the text as runs with the argument cut out.
import { describe, it, expect, vi, beforeEach } from 'vitest';

const asked: string[] = [];
let answer: unknown = null;

vi.mock('@/bus/emulator', () => ({
  gsEval: async (path: string, args: unknown[] = []) => {
    if (path === 'shell.usage') asked.push(String(args[0]));
    return answer;
  },
}));

import { argSpan16, forgetUsage, loadUsageInfo, usageRuns, type UsageInfo } from '@/lib/usage';

beforeEach(() => {
  forgetUsage();
  asked.length = 0;
  answer = null;
});

describe('usage', () => {
  it('asks once per path; a missing answer is asked again', async () => {
    expect(await loadUsageInfo('a.b')).toBeNull();
    answer = { text: 'a.b <x>', signature: 'a.b <x>', arg_spans: [[4, 7]] };
    const u = await loadUsageInfo('a.b');
    expect(u?.argSpans).toEqual([[4, 7]]);
    await loadUsageInfo('a.b');
    expect(asked).toEqual(['a.b', 'a.b']);
    forgetUsage();
    await loadUsageInfo('a.b');
    expect(asked).toHaveLength(3);
  });

  it('argument spans are converted from UTF-8 bytes', () => {
    // `é` is two bytes, one code unit.
    const u: UsageInfo = {
      text: 'é.f <p> [q]',
      signature: 'é.f <p> [q]',
      argSpans: [[5, 8], null],
    };
    expect(argSpan16(u, 0)).toEqual([4, 7]);
    expect(argSpan16(u, 1)).toBeNull();
    expect(argSpan16(u, null)).toBeNull();
    expect(argSpan16({ ...u, signature: '' }, 0)).toBeNull();
  });

  it('runs cut the marked argument out of the signature line', () => {
    const u: UsageInfo = {
      text: 'm.f <p> [q]\nMore',
      signature: 'm.f <p> [q]',
      argSpans: [
        [4, 7],
        [8, 11],
      ],
    };
    const runs = usageRuns(u, { 0: [{ from: 2, to: 3, cls: 'method' }] }, 1);
    expect(runs[0]).toEqual([
      { text: 'm.', cls: null, mark: false },
      { text: 'f', cls: 'method', mark: false },
      { text: ' <p> ', cls: null, mark: false },
      { text: '[q]', cls: null, mark: true },
    ]);
    expect(runs[1]).toEqual([{ text: 'More', cls: null, mark: false }]);
  });
});
