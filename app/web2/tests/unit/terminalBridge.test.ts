import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  registerTerminalInsert,
  insertIntoTerminal,
} from '@/components/panel-views/terminal/terminalBridge';

beforeEach(() => {
  registerTerminalInsert(null);
});

describe('terminalBridge', () => {
  it('insertIntoTerminal returns false when no pane is mounted', () => {
    expect(insertIntoTerminal('rom list')).toBe(false);
  });

  it('forwards text to the registered setter and returns true', () => {
    const setter = vi.fn();
    registerTerminalInsert(setter);
    expect(insertIntoTerminal('cpu.pc')).toBe(true);
    expect(setter).toHaveBeenCalledWith('cpu.pc');
  });

  it('null re-registration disables the bridge', () => {
    const setter = vi.fn();
    registerTerminalInsert(setter);
    registerTerminalInsert(null);
    expect(insertIntoTerminal('x')).toBe(false);
    expect(setter).not.toHaveBeenCalled();
  });

  it('re-registration replaces the previous setter', () => {
    const a = vi.fn();
    const b = vi.fn();
    registerTerminalInsert(a);
    registerTerminalInsert(b);
    insertIntoTerminal('hello');
    expect(a).not.toHaveBeenCalled();
    expect(b).toHaveBeenCalledWith('hello');
  });
});
