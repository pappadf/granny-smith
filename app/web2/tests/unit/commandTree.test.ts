// The command browser's tree store, without the DOM: sections and their
// defaults, opening a path by key, following the console (marks, the row
// to select, a stale update dropped), and re-reading on model changes.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

type Listener = (ev: { kind: string; event: string; data: Record<string, unknown> }) => void;
const coreListeners = new Set<Listener>();
const reads: string[] = [];
let floppyEntries = [0, 1];
// A shell.alias.list answer to hold back (a slow follow).
let holdAliases: Promise<void> | null = null;

vi.mock('@/bus/emulator', () => {
  const child = (name: string, extra: Record<string, unknown> = {}) => ({
    name,
    kind: 'child',
    category: 'basic',
    label: name,
    doc: name,
    ...extra,
  });
  const method = (name: string) => ({
    name,
    kind: 'method',
    category: 'basic',
    label: name,
    doc: '',
  });
  const members: Record<string, () => unknown[]> = {
    'meta.members': () => [
      method('help'),
      child('machine', { domain: 'machine' }),
      child('debug', { domain: 'emulator' }),
    ],
    'machine.meta.members': () => [
      child('cpu'),
      child('floppy'),
      child('category', { collection: true, indices: null, keys: ['scsi'] }),
    ],
    'machine.cpu.meta.members': () => [
      { name: 'pc', kind: 'attr', category: 'basic', label: 'pc', doc: '' },
      method('step'),
      method('stop'),
    ],
    'machine.floppy.meta.members': () => [
      child('drive', { indexed: true, indices: floppyEntries, keys: null }),
    ],
    'machine.category.meta.members': () => [
      child('entries', { indexed: true, indices: null, keys: ['scsi'] }),
    ],
    'debug.meta.members': () => [method('step')],
  };
  return {
    isModuleReady: () => true,
    onCoreEvent: (cb: Listener) => {
      coreListeners.add(cb);
      return () => coreListeners.delete(cb);
    },
    gsEval: async (path: string) => {
      reads.push(path);
      if (path in members) return members[path]();
      // `s` runs machine.cpu.step: its row carries that path.
      if (path === 'shell.command.list')
        return [{ name: 's', target: 'machine.cpu.step', doc: '', builtin: true }];
      if (path === 'shell.alias.list') {
        if (holdAliases) await holdAliases;
        return ['pc=machine.cpu.pc (built-in)', 'mine=machine.cpu'];
      }
      if (path === 'shell.keywords') return [];
      return null;
    },
  };
});

vi.mock('@/state/console.svelte', () => ({ onConsoleJobDone: () => () => {} }));

import { invalidate } from '@/bus/memberStore';
import { CommandTree } from '@/state/commandTree.svelte';
import type { CompletionResult } from '@/bus/emulator';

let ct: CommandTree;
beforeEach(async () => {
  invalidate('');
  floppyEntries = [0, 1];
  holdAliases = null;
  ct = new CommandTree();
  await ct.reload();
  reads.length = 0;
});
afterEach(() => ct.dispose());

const shown = () => ct.tree.flat.map((f) => `${f.depth}:${f.row.key}`);

function completion(
  partial: { start: number; end: number },
  names: string[],
  method: string | null = null,
  argIndex: number | null = null,
): CompletionResult {
  return {
    candidates: names.map((text) => ({ text, kind: 'attr', doc: '' })),
    span: partial,
    context: { method, argIndex, argName: null },
  } as CompletionResult;
}

describe('CommandTree', () => {
  it('sections are keyed section:*; the member sections start open', () => {
    expect(ct.tree.rootRows.map((r) => [r.key, ct.tree.isOpen(r)])).toEqual([
      ['section:commands', true],
      ['section:machine', true],
      ['section:emulator', true],
      ['section:aliases', false],
      ['section:language', false],
    ]);
    // A section's rows share its indent.
    expect(shown()).toEqual([
      '0:section:commands',
      '0:help',
      '0:cmd:s',
      '0:section:machine',
      '0:machine',
      '0:section:emulator',
      '0:debug',
      '0:section:aliases',
      '0:section:language',
    ]);
  });

  it('a collapsed section stays collapsed across a reload', async () => {
    ct.tree.close(ct.tree.findRow('section:machine')!);
    await ct.reload();
    expect(ct.tree.isOpen(ct.tree.findRow('section:machine')!)).toBe(false);
  });

  it('openPath walks by key: a command carrying the path is not the member', async () => {
    const row = await ct.openPath('machine.cpu.step');
    expect(row?.key).toBe('machine.cpu.step');
    expect(row?.kind).toBe('method');
    expect(shown()).toContain('2:machine.cpu.step');
  });

  it('openPath stops at the deepest row shown; openLast leaves the last closed', async () => {
    expect((await ct.openPath('machine.cpu.nope'))?.key).toBe('machine.cpu');
    const cat = await ct.openPath('machine.category', { openLast: false });
    expect(cat?.key).toBe('machine.category');
    expect(ct.tree.isOpen(cat!)).toBe(false);
    expect((await ct.openPath('machine.category["scsi"]'))?.key).toBe('machine.category["scsi"]');
  });

  it('follows the typed token: marks the matches, dims the rest, answers the row to select', async () => {
    const a = await ct.follow(
      'machine.cpu.st',
      completion({ start: 12, end: 14 }, ['step', 'stop']),
    );
    expect(a).toEqual({
      kind: 'select',
      row: expect.objectContaining({ key: 'machine.cpu.step' }),
      arg: null,
    });
    expect([...ct.matchKeys]).toEqual(['machine.cpu.step', 'machine.cpu.stop']);
    expect([...ct.otherKeys]).toEqual(['machine.cpu.pc']);
    // The marks follow the level as it is re-read.
    ct.tree.close(ct.tree.findRow('machine')!);
    expect(ct.matchKeys.size).toBe(2);
  });

  it('at the root, marks root members across the sections and opens the match’s section', async () => {
    ct.tree.close(ct.tree.findRow('section:machine')!);
    const a = await ct.follow('mach', completion({ start: 0, end: 4 }, ['machine.']));
    expect(a?.kind === 'select' && a.row.key).toBe('machine');
    expect(ct.tree.isOpen(ct.tree.findRow('section:machine')!)).toBe(true);
    expect([...ct.otherKeys]).toEqual(['help', 'cmd:s', 'debug']);
  });

  it("in a method's arguments: that method, with the argument", async () => {
    const a = await ct.follow(
      'machine.cpu.step 3',
      completion({ start: 18, end: 18 }, [], 'machine.cpu.step', 0),
    );
    expect(a).toEqual({
      kind: 'select',
      row: expect.objectContaining({ key: 'machine.cpu.step' }),
      arg: 0,
    });
  });

  it('a $ token selects the alias in its group', async () => {
    const a = await ct.follow('$mi', completion({ start: 0, end: 3 }, ['$mine']));
    expect(a?.kind === 'select' && a.row.key).toBe('alias:mine');
    expect(ct.tree.isOpen(ct.tree.findRow('group:aliases:user')!)).toBe(true);
  });

  it('an unknown parent, or an emptied input, drops the marks', async () => {
    await ct.follow('machine.cpu.st', completion({ start: 12, end: 14 }, ['step']));
    expect(ct.matchKeys.size).toBe(1);
    expect(await ct.follow('nope.x', completion({ start: 5, end: 6 }, []))).toBeNull();
    expect(ct.matchKeys.size).toBe(0);
    await ct.follow('machine.cpu.st', completion({ start: 12, end: 14 }, ['step']));
    expect(await ct.follow('  ', null)).toEqual({ kind: 'close' });
    expect(ct.matchKeys.size).toBe(0);
  });

  it('an update overtaken by a newer one answers nothing', async () => {
    let release!: () => void;
    holdAliases = new Promise<void>((r) => (release = r));
    const slow = ct.follow('$mi', completion({ start: 0, end: 3 }, ['$mine']));
    const fast = ct.follow('machine.cpu.st', completion({ start: 12, end: 14 }, ['step']));
    expect((await fast)?.kind).toBe('select');
    release();
    expect(await slow).toBeNull();
    expect([...ct.matchKeys]).toEqual(['machine.cpu.step']);
  });

  it('a core event re-reads the open levels it drops', async () => {
    await ct.openPath('machine.cpu');
    await ct.openPath('machine.floppy.drive');
    expect(shown()).toContain('3:machine.floppy.drive[1]');
    reads.length = 0;
    floppyEntries = [0, 1, 2];
    for (const l of coreListeners) l({ kind: 'notify', event: 'floppy', data: {} });
    await vi.waitFor(() => expect(shown()).toContain('3:machine.floppy.drive[2]'));
    // Only the dropped levels were asked again.
    expect(reads).not.toContain('machine.cpu.meta.members');
  });
});
