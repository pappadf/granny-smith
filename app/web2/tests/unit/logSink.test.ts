// Output printed before any console exists is kept and replayed when one
// registers, within a bound.
import { describe, it, expect } from 'vitest';
import { routePrintLine, routeErrorLine, setConsoleSink } from '@/bus/logSink';
import type { ConsoleRecord } from '@/lib/consoleModel';

describe('logSink', () => {
  it('replays the backlog to the first sink, then writes through', () => {
    setConsoleSink(null);
    routePrintLine('booting');
    routeErrorLine('warning');
    const got: ConsoleRecord[] = [];
    setConsoleSink((r) => got.push(r));
    expect(got).toEqual([
      { kind: 'print', line: 'booting' },
      { kind: 'stderr', line: 'warning' },
    ]);
    routePrintLine('live');
    expect(got[2]).toEqual({ kind: 'print', line: 'live' });
    setConsoleSink(null);
  });

  it('keeps only the newest records when nobody listens for long', () => {
    setConsoleSink(null);
    for (let i = 0; i < 2500; i++) routePrintLine(`l${i}`);
    const got: string[] = [];
    setConsoleSink((r) => got.push(r.kind === 'print' ? r.line : '?'));
    expect(got.length).toBe(2000);
    expect(got[0]).toBe('l500');
    expect(got[got.length - 1]).toBe('l2499');
    setConsoleSink(null);
  });
});
