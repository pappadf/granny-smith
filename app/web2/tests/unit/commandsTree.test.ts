import { describe, it, expect, vi } from 'vitest';
import { buildCommandsTree, type CommandNode } from '@/lib/commandsTree';

// The catalogue is now generated from the model (proposal §8.6), not a static
// constant. Mock the bus so buildCommandsTree walks a tiny synthetic tree:
//   root → machine → cpu (with a `step` method) ; root verbs `echo`/`download`
//   plus one alias. We assert the generated shape, not a hand-listed set.
vi.mock('@/bus/emulator', () => {
  const method = (name: string, doc = '') => ({
    name,
    kind: 'method',
    category: 'basic',
    label: name,
    doc,
    verb: name,
    task: '',
    destructive: false,
    mutate: false,
    hidden: false,
    nargs: 0,
  });
  const child = (name: string) => ({
    name,
    kind: 'child',
    category: 'basic',
    label: name,
    doc: '',
  });
  return {
    isModuleReady: () => true,
    // One meta.members call per node: its methods and its children.
    gsEval: async (path: string) => {
      if (path === 'meta.members') return [method('echo', 'print args'), child('machine')];
      if (path === 'machine.meta.members') return [child('cpu')];
      if (path === 'machine.cpu.meta.members') return [method('step', 'run N instructions')];
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
