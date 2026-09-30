// The shared tree state: open levels, the rows on screen, and a refresh
// that re-reads every open level without losing one opened meanwhile.
import { describe, it, expect } from 'vitest';
import { TreeState } from '@/lib/treeState.svelte';

interface Row {
  key: string;
  expandable: boolean;
}

// A tree `a` → `a.x` → `a.x.1`, `b` → `b.y`; each load can be held.
function source() {
  const kids: Record<string, string[]> = {
    '': ['a', 'b'],
    a: ['a.x'],
    'a.x': ['a.x.1'],
    b: ['b.y'],
  };
  const loads: string[] = [];
  const held: Array<() => void> = [];
  let hold = false;
  const level = async (key: string): Promise<Row[]> => {
    loads.push(key);
    if (hold) await new Promise<void>((r) => held.push(r));
    return (kids[key] ?? []).map((k) => ({ key: k, expandable: k in kids }));
  };
  return {
    loads,
    held,
    setHold: (h: boolean) => (hold = h),
    src: {
      root: () => level(''),
      load: (row: Row) => level(row.key),
      rows: (l: Row[]) => l,
    },
  };
}

const keys = (t: TreeState<Row, Row[]>) => t.flat.map((f) => `${f.depth}:${f.row.key}`);

describe('TreeState', () => {
  it('flattens the open levels depth-first', async () => {
    const s = source();
    const t = new TreeState<Row, Row[]>(s.src);
    await t.refresh();
    expect(keys(t)).toEqual(['0:a', '0:b']);
    await t.open(t.findRow('a')!);
    await t.open(t.findRow('a.x')!);
    expect(keys(t)).toEqual(['0:a', '1:a.x', '2:a.x.1', '0:b']);
    await t.toggle(t.findRow('a')!);
    expect(keys(t)).toEqual(['0:a', '0:b']);
  });

  it('a refresh re-reads every open level, siblings in parallel', async () => {
    const s = source();
    const t = new TreeState<Row, Row[]>({ ...s.src, defaultOpen: () => true });
    await t.refresh();
    expect(keys(t)).toEqual(['0:a', '1:a.x', '2:a.x.1', '0:b', '1:b.y']);
    s.loads.length = 0;
    s.setHold(true);
    const done = t.refresh();
    await Promise.resolve();
    s.held.shift()!(); // the root
    await new Promise((r) => setTimeout(r));
    // Both open siblings are asked before either answers.
    expect(s.loads).toEqual(['', 'a', 'b']);
    s.setHold(false);
    while (s.held.length) {
      s.held.shift()!();
      await new Promise((r) => setTimeout(r));
    }
    await done;
  });

  it('a level opened while a refresh runs survives it', async () => {
    const s = source();
    const t = new TreeState<Row, Row[]>(s.src);
    await t.refresh();
    await t.open(t.findRow('a')!);
    s.setHold(true);
    const tick = () => new Promise((r) => setTimeout(r));
    const pass = t.refresh();
    await tick();
    s.held.shift()!(); // the root: the pass now waits on `a`
    await tick();
    expect(s.loads.at(-1)).toBe('a');
    const open = t.open(t.findRow('b')!);
    await tick();
    s.held.pop()!(); // `b` answers first
    await open;
    s.setHold(false);
    s.held.shift()!(); // then the pass's `a`
    await pass;
    expect(keys(t)).toEqual(['0:a', '1:a.x', '0:b', '1:b.y']);
  });

  it('keeps the open rows where it is told', async () => {
    const s = source();
    const record: Record<string, boolean> = { a: true };
    const t = new TreeState<Row, Row[]>({ ...s.src, expanded: () => record });
    await t.refresh();
    expect(keys(t)).toEqual(['0:a', '1:a.x', '0:b']);
    t.close(t.findRow('a')!);
    expect(record.a).toBe(false);
  });
});
