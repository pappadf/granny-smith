import { describe, it, expect } from 'vitest';
import { commandsTree, type CommandNode } from '@/lib/commandsTree';

function walk(nodes: CommandNode[], visit: (n: CommandNode, depth: number) => void, depth = 0) {
  for (const n of nodes) {
    visit(n, depth);
    if (n.children) walk(n.children, visit, depth + 1);
  }
}

describe('commandsTree', () => {
  it('has at least the six canonical categories', () => {
    const names = commandsTree.map((n) => n.name);
    for (const expected of ['Scheduler', 'Debugger', 'Logging', 'Storage', 'Mac', 'General']) {
      expect(names).toContain(expected);
    }
  });

  it('top-level categories carry a description but no insert', () => {
    for (const cat of commandsTree) {
      expect(cat.desc.length).toBeGreaterThan(0);
      expect(cat.insert).toBeUndefined();
      expect(cat.children?.length ?? 0).toBeGreaterThan(0);
    }
  });

  it('every leaf node has both insert and a non-empty description', () => {
    walk(commandsTree, (n, depth) => {
      if (depth === 0) return; // categories
      if (!n.children?.length) {
        expect(n.insert, `${n.name} (depth ${depth}) should have insert`).toBeTruthy();
        expect(n.desc.length).toBeGreaterThan(0);
      }
    });
  });

  it('subcommands prefix the parent command name when applicable (`info mmu`, …)', () => {
    const debugger_ = commandsTree.find((c) => c.name === 'Debugger')!;
    const info = debugger_.children!.find((c) => c.name === 'info')!;
    expect(info.children?.length ?? 0).toBeGreaterThan(0);
    for (const sub of info.children!) {
      expect(sub.name.startsWith('info ')).toBe(true);
      expect(sub.insert?.startsWith('info ')).toBe(true);
    }
  });
});
