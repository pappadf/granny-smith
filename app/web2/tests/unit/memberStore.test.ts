// The shared members cache: one structure read per path, dropped by prefix,
// by the core events that change a level, and (the collection levels) when
// a console job finishes; subscribers hear each change once it is dropped.
import { describe, it, expect, vi, beforeEach } from 'vitest';

type Listener = (ev: { kind: string; event: string; data: Record<string, unknown> }) => void;
const coreListeners = new Set<Listener>();
const jobListeners = new Set<() => void>();
const reads: string[] = [];

vi.mock('@/bus/emulator', () => ({
  onCoreEvent: (cb: Listener) => {
    coreListeners.add(cb);
    return () => coreListeners.delete(cb);
  },
  gsEval: async (path: string) => {
    reads.push(path);
    if (path === 'machine.meta.members')
      return [
        { name: 'cpu', kind: 'child', category: 'basic', label: 'cpu', doc: '' },
        {
          name: 'drive',
          kind: 'child',
          category: 'basic',
          label: 'drive',
          doc: '',
          indexed: true,
          indices: [0, 2],
          keys: null,
        },
      ];
    if (path === 'machine.cpu.meta.members')
      return [{ name: 'pc', kind: 'attr', category: 'basic', label: 'pc', doc: '' }];
    return [];
  },
}));

vi.mock('@/state/console.svelte', () => ({
  onConsoleJobDone: (cb: () => void) => {
    jobListeners.add(cb);
    return () => jobListeners.delete(cb);
  },
}));

import {
  collectionEntries,
  covers,
  invalidate,
  members,
  onMembersChanged,
  type MembersChange,
} from '@/bus/memberStore';

beforeEach(() => {
  invalidate('');
  reads.length = 0;
});

describe('memberStore', () => {
  it('reads a path once until it is dropped', async () => {
    await members('machine.cpu');
    await members('machine.cpu');
    expect(reads).toEqual(['machine.cpu.meta.members']);
    invalidate('machine');
    await members('machine.cpu');
    expect(reads).toHaveLength(2);
  });

  it('a prefix covers its members and entries, not its namesakes', () => {
    expect(covers('machine.scsi', 'machine.scsi')).toBe(true);
    expect(covers('machine.scsi', 'machine.scsi.device[0]')).toBe(true);
    expect(covers('machine.scsi', 'machine.scsi[1]')).toBe(true);
    expect(covers('machine.scsi', 'machine.scsi2')).toBe(false);
    expect(covers('', 'anything')).toBe(true);
  });

  it("a bare indexed member's entries come from its parent", async () => {
    const { entries, items } = await collectionEntries('machine.drive', []);
    expect(entries).toBeUndefined();
    expect(items).toEqual([
      { name: '[0]', path: 'machine.drive[0]', word: '0' },
      { name: '[2]', path: 'machine.drive[2]', word: '2' },
    ]);
  });

  it('events and finished jobs drop what they change, then tell subscribers', async () => {
    const heard: MembersChange[] = [];
    const off = onMembersChanged((c) => heard.push(c));
    await members('machine');
    await members('machine.cpu');
    for (const l of coreListeners) l({ kind: 'notify', event: 'floppy', data: {} });
    for (const l of coreListeners) l({ kind: 'notify', event: 'screen', data: {} });
    // A job drops the levels listing a collection: machine, not machine.cpu.
    for (const l of jobListeners) l();
    expect(heard).toEqual([
      { reload: false, dropped: ['machine.floppy'] },
      { reload: false, dropped: ['machine'] },
    ]);
    reads.length = 0;
    await members('machine.cpu');
    await members('machine');
    expect(reads).toEqual(['machine.meta.members']);

    for (const l of coreListeners) l({ kind: 'state', event: 'machine_booted', data: {} });
    expect(heard.at(-1)).toEqual({ reload: true, dropped: [] });
    await members('machine.cpu');
    expect(reads).toEqual(['machine.meta.members', 'machine.cpu.meta.members']);

    // The last subscriber leaving unhooks the store.
    off();
    expect(coreListeners.size).toBe(0);
    expect(jobListeners.size).toBe(0);
  });
});
