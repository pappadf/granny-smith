import { describe, it, expect, vi } from 'vitest';
import { buildCommandsTree, type CommandNode } from '@/lib/commandsTree';

// The catalogue is now generated from the model (proposal §8.6), not a static
// constant. Mock the bus so buildCommandsTree walks a tiny synthetic tree:
//   root → machine → cpu (with a `step` method) ; root verbs `echo`/`download`
//   plus one alias. We assert the generated shape, not a hand-listed set.
vi.mock('@/bus/emulator', () => {
  const methodInfo = (name: string, doc = '', task = '') => ({
    name,
    verb: name,
    category: 'basic',
    task,
    doc,
    destructive: false,
    mutate: false,
    hidden: false,
    nargs: 0,
  });
  return {
    isModuleReady: () => true,
    gsEval: async (path: string, args?: unknown[]) => {
      // Root verbs.
      if (path === 'meta.methods') return ['echo'];
      if (path === 'meta.method_info') return methodInfo(String(args?.[0]), 'print args');
      // Root children.
      if (path === 'objects') return ['machine'];
      // machine: no methods, one child (cpu).
      if (path === 'machine.meta.methods') return [];
      if (path === 'machine.meta.children') return ['cpu'];
      // cpu: a `step` method, no children.
      if (path === 'machine.cpu.meta.methods') return ['step'];
      if (path === 'machine.cpu.meta.method_info')
        return methodInfo(String(args?.[0]), 'run N instructions');
      if (path === 'machine.cpu.meta.children') return [];
      if (path === 'shell.aliases') return ['pc=machine.cpu.pc'];
      return null;
    },
  };
});

function flatten(nodes: CommandNode[]): CommandNode[] {
  const out: CommandNode[] = [];
  const walk = (ns: CommandNode[]) =>
    ns.forEach((n) => {
      out.push(n);
      if (n.children) walk(n.children);
    });
  walk(nodes);
  return out;
}

describe('buildCommandsTree (model projection)', () => {
  it('generates command rows from object-node methods + root verbs', async () => {
    const tree = await buildCommandsTree();
    const inserts = flatten(tree).map((n) => n.insert);
    // A node method, reached by walking machine → cpu.
    expect(inserts).toContain('machine.cpu.step');
    // A global root verb.
    expect(inserts).toContain('echo');
  });

  it('includes an Aliases group and a Language (keywords) group', async () => {
    const tree = await buildCommandsTree();
    const groups = tree.map((n) => n.name);
    expect(groups).toContain('Aliases');
    expect(groups).toContain('Language');
    const lang = tree.find((g) => g.name === 'Language')!;
    expect(lang.children!.map((c) => c.insert)).toContain('while');
    const aliases = tree.find((g) => g.name === 'Aliases')!;
    expect(aliases.children!.map((c) => c.insert)).toContain('$pc');
  });
});
