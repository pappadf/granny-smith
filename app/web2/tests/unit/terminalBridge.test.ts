import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  registerConsoleInput,
  writeToConsole,
  isBrowserWriting,
  snapshotConsole,
  restoreConsole,
  focusConsole,
  type ConsoleInputApi,
} from '@/components/panel-views/terminal/terminalBridge';

function fakeInput(): ConsoleInputApi & { seen: boolean[] } {
  const seen: boolean[] = [];
  return {
    seen,
    replaceToken: vi.fn(() => void seen.push(isBrowserWriting())),
    focusEnd: vi.fn(),
    getState: vi.fn(() => ({ text: 'abc', cursor: 1 })),
    restore: vi.fn(() => void seen.push(isBrowserWriting())),
  };
}

beforeEach(() => registerConsoleInput(null));

describe('terminalBridge', () => {
  it('does nothing while no console is mounted', () => {
    expect(writeToConsole('machine.')).toBe(false);
    expect(focusConsole()).toBe(false);
    expect(snapshotConsole()).toBeNull();
  });

  it("forwards writes, marked as the browser's own while they run", () => {
    const api = fakeInput();
    registerConsoleInput(api);
    expect(writeToConsole('machine.cpu.')).toBe(true);
    expect(api.replaceToken).toHaveBeenCalledWith('machine.cpu.');
    expect(api.seen).toEqual([true]);
    expect(isBrowserWriting()).toBe(false);
  });

  it('snapshots and restores the input, and hands focus over', () => {
    const api = fakeInput();
    registerConsoleInput(api);
    const s = snapshotConsole();
    expect(s).toEqual({ text: 'abc', cursor: 1 });
    restoreConsole(s!);
    expect(api.restore).toHaveBeenCalledWith({ text: 'abc', cursor: 1 });
    focusConsole();
    expect(api.focusEnd).toHaveBeenCalled();
  });
});
