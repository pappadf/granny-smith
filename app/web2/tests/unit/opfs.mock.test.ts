import { describe, it, expect, beforeEach } from 'vitest';
import { MockOpfs, setOpfsBackend, opfs } from '@/bus/opfs';
import type { RecentEntry } from '@/bus/types';

beforeEach(() => setOpfsBackend(new MockOpfs()));

describe('MockOpfs', () => {
  it('scanRoms returns at least one ROM with expected shape', async () => {
    const roms = await opfs.scanRoms();
    expect(roms.length).toBeGreaterThan(0);
    expect(roms[0]).toHaveProperty('name');
    expect(roms[0]).toHaveProperty('path');
    expect(roms[0]).toHaveProperty('size');
    expect(typeof roms[0].size).toBe('number');
  });

  it.each(['rom', 'vrom', 'fd', 'hd', 'cd'] as const)(
    'scanImages(%s) returns a non-empty list',
    async (cat) => {
      const list = await opfs.scanImages(cat);
      expect(list.length).toBeGreaterThan(0);
      expect(list[0].kind).toBe('file');
      expect(list[0].path).toContain(`/opfs/images/${cat}/`);
    },
  );

  it('readJson("/opfs/config/recent.json") returns prototype-style recents', async () => {
    const recents = await opfs.readJson<RecentEntry[]>('/opfs/config/recent.json');
    expect(Array.isArray(recents)).toBe(true);
    expect(recents!.length).toBeGreaterThanOrEqual(1);
    expect(recents![0]).toHaveProperty('model');
    expect(recents![0]).toHaveProperty('ram');
    expect(recents![0]).toHaveProperty('media');
    expect(recents![0]).toHaveProperty('lastUsedAt');
  });

  it('readJson returns null for unknown paths', async () => {
    expect(await opfs.readJson('/opfs/does/not/exist.json')).toBeNull();
  });

  it('writeJson + readJson round-trips a value', async () => {
    await opfs.writeJson('/opfs/test/value.json', { hello: 'world' });
    const result = await opfs.readJson<{ hello: string }>('/opfs/test/value.json');
    expect(result).toEqual({ hello: 'world' });
  });
});
