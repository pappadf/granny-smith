// What the core prints goes straight into the app's console, which exists
// before any view mounts.
import { describe, it, expect, vi } from 'vitest';
import { routePrintLine, routeErrorLine, routeLogPrint } from '@/bus/logSink';
import { appConsole } from '@/state/console.svelte';
import { logs, appendLog, clearLogs } from '@/state/logs.svelte';

describe('logSink', () => {
  it('feeds the app console model directly', () => {
    const push = vi.spyOn(appConsole.model, 'push');
    routePrintLine('booting');
    routeErrorLine('warning');
    expect(push.mock.calls.map((c) => c[0])).toEqual([
      { kind: 'print', line: 'booting' },
      { kind: 'stderr', line: 'warning' },
    ]);
    push.mockRestore();
  });

  it('what arrives before any view is shown once a frame passes', async () => {
    appConsole.model.clear();
    routePrintLine('early');
    await new Promise((r) => requestAnimationFrame(() => r(null)));
    expect(appConsole.state.entries.map((e) => e.text)).toEqual(['early']);
  });
});

describe('log print events (S1018-04)', () => {
  it('a stdout=on log line reaches the console as print lines', () => {
    const push = vi.spyOn(appConsole.model, 'push');
    routeLogPrint('[via] 9 one\n');
    routeLogPrint('[via] 9 two\nthree\n');
    expect(push.mock.calls.map((c) => c[0])).toEqual([
      { kind: 'print', line: '[via] 9 one' },
      { kind: 'print', line: '[via] 9 two' },
      { kind: 'print', line: 'three' },
    ]);
    push.mockRestore();
  });
});

describe('the Logs buffer cap', () => {
  const flush = () => new Promise((r) => requestAnimationFrame(() => r(null)));
  const line = (i: number) => ({ ts: i, cat: 'via', level: 9, msg: `m${i}` });

  it('keeps the dropped notice first, counting every overflow (S1018-87)', async () => {
    clearLogs();
    for (let i = 0; i < 5003; i++) appendLog(line(i));
    await flush();
    expect(logs.entries.length).toBe(5001);
    expect(logs.entries[0].msg).toBe('[3 older entries dropped — buffer capped at 5000 lines]');
    expect(logs.entries[1].msg).toBe('m3');
    for (let i = 5003; i < 5010; i++) appendLog(line(i));
    await flush();
    expect(logs.entries.length).toBe(5001);
    expect(logs.entries[0].msg).toBe('[10 older entries dropped — buffer capped at 5000 lines]');
    expect(logs.entries[1].msg).toBe('m10');
    expect(logs.entries[5000].msg).toBe('m5009');
    clearLogs();
  });

  it('a flood between frames holds no more than the buffer can show (S1018-04)', async () => {
    clearLogs();
    for (let i = 0; i < 50_000; i++) appendLog(line(i));
    await flush();
    expect(logs.entries.length).toBe(5001);
    expect(logs.entries[0].msg).toBe('[45000 older entries dropped — buffer capped at 5000 lines]');
    expect(logs.entries[1].msg).toBe('m45000');
    expect(logs.entries[5000].msg).toBe('m49999');
    clearLogs();
  });
});
