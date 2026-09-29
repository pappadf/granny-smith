import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  rootRows,
  expand,
  firstSentence,
  invalidate,
  invalidationFor,
  loadAliases,
  typeText,
  visible,
  type BrowserRow,
} from '@/lib/commandsTree';

// The browser is a structural projection of the model: mock the bus with a
// tiny tree and assert the rows it produces.
vi.mock('@/bus/emulator', () => {
  const t = (kind: string, presentation: string | null = null) => ({
    kind,
    width: 0,
    presentation,
    enum: null,
  });
  const members: Record<string, unknown[]> = {
    'meta.members': [
      {
        name: 'help',
        kind: 'method',
        category: 'basic',
        label: 'help',
        doc: 'Usage text. More.',
        task: 'shell',
      },
      {
        name: 'machine',
        kind: 'child',
        category: 'basic',
        label: 'M',
        doc: 'The computer',
        domain: 'machine',
        collection: false,
      },
      {
        name: 'debug',
        kind: 'child',
        category: 'basic',
        label: 'debug',
        doc: 'Debugger',
        domain: 'emulator',
        task: 'debug',
        collection: false,
      },
    ],
    'machine.meta.members': [
      {
        name: 'cpu',
        kind: 'child',
        category: 'basic',
        label: 'cpu',
        doc: 'CPU',
        task: 'debug',
        collection: false,
      },
      {
        name: 'drive',
        kind: 'child',
        category: 'basic',
        label: 'Drives',
        doc: 'Drives',
        task: 'storage',
        collection: true,
        indices: [0, 1],
        keys: null,
      },
    ],
    'machine.cpu.meta.members': [
      {
        name: 'pc',
        kind: 'attr',
        category: 'basic',
        label: 'pc',
        doc: 'Program counter',
        task: 'debug',
        readonly: false,
        type: t('uint', 'hex'),
      },
      {
        name: 'vbr',
        kind: 'attr',
        category: 'advanced',
        label: 'vbr',
        doc: 'Vector base',
        task: 'debug',
        readonly: false,
        type: t('uint', 'hex'),
      },
      {
        name: 'step',
        kind: 'method',
        category: 'basic',
        label: 'step',
        doc: 'Step',
        task: 'debug',
        hidden: false,
      },
      {
        name: 'run',
        kind: 'method',
        category: 'basic',
        label: 'run',
        doc: 'Plumbing',
        task: 'debug',
        hidden: true,
      },
    ],
    'machine.drive.meta.members': [
      {
        name: 'entries',
        kind: 'child',
        category: 'basic',
        label: 'entries',
        doc: '',
        task: 'storage',
        indexed: true,
        indices: [0, 1],
        keys: null,
        collection: false,
      },
    ],
  };
  return {
    isModuleReady: () => true,
    gsEval: async (path: string) => {
      if (path in members) return members[path];
      if (path === 'shell.alias.list')
        return [
          'pc=machine.cpu.pc (built-in)',
          'Ticks=debug.mac.globals.Ticks (built-in)',
          'mine=machine.cpu',
        ];
      if (path === 'shell.keywords') return [{ word: 'while', syntax: 'while <expr> { … }' }];
      return null;
    },
  };
});

beforeEach(() => invalidate(''));

const byName = (rows: BrowserRow[], name: string) => rows.find((r) => r.name === name)!;

describe('command browser rows (model projection)', () => {
  it('the root: its verbs, its children under domain dividers, then Aliases and Language', async () => {
    const rows = await rootRows();
    expect(rows.map((r) => `${r.kind}:${r.name}`)).toEqual([
      'method:help',
      'divider:Machine',
      'object:machine',
      'divider:Emulator',
      'object:debug',
      'group:Aliases',
      'group:Language',
    ]);
  });

  it('levels are path segments; hidden methods never appear', async () => {
    const machine = byName(await rootRows(), 'machine');
    const cpu = byName(await expand(machine), 'cpu');
    const rows = await expand(cpu);
    expect(rows.map((r) => r.path)).toEqual([
      'machine.cpu.pc',
      'machine.cpu.vbr',
      'machine.cpu.step',
    ]);
    const pc = byName(rows, 'pc');
    expect(pc.kind).toBe('attr');
    expect(typeText(pc.type)).toBe('uint, hex');
  });

  it('a collection expands to its live entries', async () => {
    const machine = byName(await rootRows(), 'machine');
    const drive = byName(await expand(machine), 'drive');
    expect(drive.kind).toBe('collection');
    const entries = await expand(drive);
    expect(entries.map((r) => r.path)).toEqual(['machine.drive[0]', 'machine.drive[1]']);
    expect(entries[0].kind).toBe('entry');
  });

  it('advanced members are visible only with the toggle', async () => {
    const machine = byName(await rootRows(), 'machine');
    const vbr = byName(await expand(byName(await expand(machine), 'cpu')), 'vbr');
    expect(visible(vbr, false)).toBe(false);
    expect(visible(vbr, true)).toBe(true);
  });

  it('aliases group into User, Built-in and Mac globals; keywords come from the model', async () => {
    const aliases = byName(await rootRows(), 'Aliases');
    const groups = await expand(aliases);
    expect(groups.map((g) => g.name)).toEqual(['User', 'Built-in', 'Mac globals']);
    expect((await expand(groups[0])).map((r) => r.name)).toEqual(['$mine']);
    expect((await expand(groups[1])).map((r) => r.name)).toEqual(['$pc']);
    expect((await expand(groups[2])).map((r) => r.name)).toEqual(['$Ticks']);
    const lang = await expand(byName(await rootRows(), 'Language'));
    expect(lang[0].name).toBe('while');
    expect(lang[0].doc).toBe('while <expr> { … }');
  });

  it('alias entries parse the built-in marker', async () => {
    expect(await loadAliases()).toContainEqual({
      name: 'pc',
      path: 'machine.cpu.pc',
      builtin: true,
    });
  });

  it('a row shows the first sentence of its doc', () => {
    expect(firstSentence('Usage text. More.')).toBe('Usage text.');
    expect(firstSentence('No period')).toBe('No period');
  });

  it('events map to the levels they change', () => {
    expect(invalidationFor('state:machine_booted')).toBe('');
    expect(invalidationFor('notify:media')).toBe('machine.scsi');
    expect(invalidationFor('notify:floppy')).toBe('machine.floppy');
    expect(invalidationFor('state:speed')).toBeNull();
  });
});
