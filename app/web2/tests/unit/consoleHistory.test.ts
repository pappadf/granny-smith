// The console's input history: browse with a draft, a cap of 500, kept in
// localStorage under gs.console.history.
import { describe, it, expect, beforeEach } from 'vitest';
import { ConsoleHistory, HISTORY_KEY, HISTORY_MAX } from '@/lib/consoleHistory';

class MemStorage {
  data = new Map<string, string>();
  getItem(k: string) {
    return this.data.get(k) ?? null;
  }
  setItem(k: string, v: string) {
    this.data.set(k, v);
  }
}

let store: MemStorage;
beforeEach(() => {
  store = new MemStorage();
});
const make = () => new ConsoleHistory(store as unknown as Storage);

describe('ConsoleHistory', () => {
  it('browses back and forward, restoring the draft', () => {
    const h = make();
    h.push('one');
    h.push('two');
    expect(h.prev('typing')).toBe('two');
    expect(h.prev('two')).toBe('one');
    expect(h.prev('one')).toBeNull();
    expect(h.next()).toBe('two');
    expect(h.next()).toBe('typing');
    expect(h.next()).toBeNull();
  });

  it('persists across instances as a JSON array; a block is one entry', () => {
    make().push('if true {\n  echo x\n}');
    expect(JSON.parse(store.getItem(HISTORY_KEY)!)).toEqual(['if true {\n  echo x\n}']);
    expect(make().prev('')).toBe('if true {\n  echo x\n}');
  });

  it('keeps the last 500, skips blanks and immediate repeats', () => {
    const h = make();
    for (let i = 0; i < HISTORY_MAX + 20; i++) h.push(`c${i}`);
    h.push('  ');
    h.push(`c${HISTORY_MAX + 19}`);
    expect(h.list().length).toBe(HISTORY_MAX);
    expect(h.list()[0]).toBe('c20');
  });

  it('survives unreadable storage', () => {
    store.setItem(HISTORY_KEY, '{not json');
    expect(make().list()).toEqual([]);
  });
});
