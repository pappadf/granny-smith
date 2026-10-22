import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  registerConsoleInput,
  writeToConsole,
  focusConsole,
  type ConsoleInputApi,
} from '@/components/panel-views/terminal/terminalBridge';

function fakeInput(): ConsoleInputApi {
  return { replaceToken: vi.fn(), focusEnd: vi.fn(), focus: vi.fn() };
}

beforeEach(() => registerConsoleInput(null));

describe('terminalBridge', () => {
  it('does nothing while no console is mounted', () => {
    expect(writeToConsole('machine.')).toBe(false);
    expect(focusConsole()).toBe(false);
  });

  it('writes the token and hands focus over', () => {
    const api = fakeInput();
    registerConsoleInput(api);
    expect(writeToConsole('machine.')).toBe(true);
    expect(api.replaceToken).toHaveBeenCalledWith('machine.');
    expect(focusConsole()).toBe(true);
    expect(api.focusEnd).toHaveBeenCalled();
    // After an insert: focus with the cursor left where the insert put it.
    expect(focusConsole(false)).toBe(true);
    expect(api.focus).toHaveBeenCalledTimes(1);
    expect(api.focusEnd).toHaveBeenCalledTimes(1);
  });
});
