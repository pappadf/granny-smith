import { describe, it, expect } from 'vitest';
import {
  RECENT_MAX,
  findMissingFiles,
  forgetRecent,
  formatRelativeTime,
  parseRecent,
  pushRecent,
  recentKey,
  recentLabel,
  referencedFiles,
  type RecentMachine,
} from '@/lib/recentMachines';
import type { MachineConfig } from '@/bus/types';

const ROM = '/opfs/images/rom/iix-iicx-se30-97221136.rom';

// A IIcx with an 8•24 GC and a hard disk, as the New Machine dialog boots it.
function iicx(ram = '8192'): MachineConfig {
  return {
    model: 'iicx',
    rom: ROM,
    config: {
      model: 'iicx',
      options: { ram },
      floppies: { fd0: '800k' },
      storage: [{ bus: 'scsi', unit: 0, type: 'hd' }],
      startup: { bus: 'scsi', unit: 0 },
      cards: [{ slot: '9', card: 'gc824', options: {} }],
      displays: {},
    },
    floppies: {},
    media: [{ bus: 'scsi', unit: 0, type: 'hd', path: '/opfs/images/hd/System_7_1.img' }],
  };
}

function entry(config: MachineConfig, lastUsed = 0, label = config.model): RecentMachine {
  return { config, label, lastUsed };
}

describe('recent machines', () => {
  it('puts a new machine on top', () => {
    const list = pushRecent([entry({ model: 'plus', rom: 'a' })], entry(iicx()));
    expect(list.map((e) => e.config.model)).toEqual(['iicx', 'plus']);
  });

  it('moves an identical configuration to the top instead of adding it', () => {
    const plus = entry({ model: 'plus', rom: 'a' }, 1);
    let list = pushRecent([], entry(iicx(), 1));
    list = pushRecent(list, plus);
    list = pushRecent(list, entry(iicx(), 2, 'again'));
    expect(list).toHaveLength(2);
    expect(list[0]).toMatchObject({ label: 'again', lastUsed: 2 });
    expect(list[1]).toBe(plus);
  });

  it('treats key order as the same configuration, other values as different', () => {
    const a = iicx();
    const b = { media: a.media, floppies: a.floppies, config: a.config, rom: a.rom, model: 'iicx' };
    expect(recentKey(a)).toBe(recentKey(b));
    expect(recentKey(iicx('4096'))).not.toBe(recentKey(a));
    expect(pushRecent([entry(iicx('4096'))], entry(a))).toHaveLength(2);
  });

  it(`keeps the last ${RECENT_MAX}`, () => {
    let list: RecentMachine[] = [];
    for (let i = 0; i < RECENT_MAX + 3; i++) list = pushRecent(list, entry({ model: `m${i}` }));
    expect(list).toHaveLength(RECENT_MAX);
    expect(list[0].config.model).toBe(`m${RECENT_MAX + 2}`);
    expect(list.at(-1)?.config.model).toBe('m3');
  });

  it('forgets one entry by key', () => {
    const list = [entry(iicx()), entry({ model: 'plus', rom: 'a' })];
    const left = forgetRecent(list, recentKey(iicx()));
    expect(left.map((e) => e.config.model)).toEqual(['plus']);
    expect(forgetRecent(left, 'no such key')).toEqual(left);
  });

  it('drops malformed persisted entries', () => {
    const good = entry(iicx(), 5, 'IIcx');
    expect(parseRecent([good, null, { label: 'x', lastUsed: 1 }, { config: {} }])).toEqual([good]);
    expect(parseRecent('nonsense')).toEqual([]);
    expect(parseRecent(Array(RECENT_MAX + 2).fill(good))).toHaveLength(RECENT_MAX);
  });

  it('lists every referenced file, ROM first', () => {
    const c = { ...iicx(), floppies: { fd0: '/opfs/images/fd/Tools.dsk' } };
    expect(referencedFiles(c)).toEqual([
      ROM,
      '/opfs/images/fd/Tools.dsk',
      '/opfs/images/hd/System_7_1.img',
    ]);
  });

  it('names the first missing file, listing each directory once', async () => {
    const dirs: string[] = [];
    const list = async (dir: string) => {
      dirs.push(dir);
      const names = dir.endsWith('/rom') ? [ROM.split('/').pop()!] : [];
      return names.map((name) => ({ name, path: `${dir}/${name}`, kind: 'file' as const }));
    };
    const ok = entry({ model: 'plus', rom: ROM });
    const outside = entry({ model: 'se', rom: '/rom/elsewhere.rom' });
    const missing = await findMissingFiles([ok, entry(iicx()), outside], list);
    expect(missing).toEqual({ [recentKey(iicx())]: 'System_7_1.img' });
    expect(dirs.sort()).toEqual(['/opfs/images/hd', '/opfs/images/rom']);
  });

  it('labels a machine with its name, RAM, cards and first disk', () => {
    const label = recentLabel({
      name: 'Macintosh IIcx',
      ram: '8 MB',
      cards: ['8•24 GC'],
      config: iicx(),
    });
    expect(label).toBe('Macintosh IIcx · 8 MB · 8•24 GC · System_7_1.img');
    expect(recentLabel({ name: 'Macintosh Plus', ram: null, config: { model: 'plus' } })).toBe(
      'Macintosh Plus',
    );
  });

  it('formats relative times', () => {
    const now = 1_000_000_000_000;
    expect(formatRelativeTime(now - 10_000, now)).toBe('just now');
    expect(formatRelativeTime(now - 5 * 60_000, now)).toBe('5 minutes ago');
    expect(formatRelativeTime(now - 2 * 3600_000, now)).toBe('2 hours ago');
    expect(formatRelativeTime(now - 86400_000, now)).toBe('yesterday');
    expect(formatRelativeTime(now + 60_000, now)).toBe('just now');
  });
});
