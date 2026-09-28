// Output printed before any terminal exists is kept and replayed when one
// registers, within a bound.
import { describe, it, expect } from 'vitest';
import { routePrintLine, setTerminalSink } from '@/bus/logSink';

describe('logSink', () => {
  it('replays the backlog to the first sink, then writes through', () => {
    setTerminalSink(null);
    routePrintLine('booting');
    routePrintLine('ready');
    const got: string[] = [];
    setTerminalSink((l) => got.push(l));
    expect(got).toEqual(['booting', 'ready']);
    routePrintLine('live');
    expect(got).toEqual(['booting', 'ready', 'live']);
    setTerminalSink(null);
  });

  it('keeps only the newest lines when nobody listens for long', () => {
    setTerminalSink(null);
    for (let i = 0; i < 2500; i++) routePrintLine(`l${i}`);
    const got: string[] = [];
    setTerminalSink((l) => got.push(l));
    expect(got.length).toBe(2000);
    expect(got[0]).toBe('l500');
    expect(got[got.length - 1]).toBe('l2499');
    setTerminalSink(null);
  });
});
