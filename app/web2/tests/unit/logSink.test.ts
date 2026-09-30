// What the core prints goes straight into the app's console, which exists
// before any view mounts.
import { describe, it, expect, vi } from 'vitest';
import { routePrintLine, routeErrorLine } from '@/bus/logSink';
import { appConsole } from '@/state/console.svelte';

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
