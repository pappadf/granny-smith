// bus/vfs.ts's path helpers: the one de-duplicating name scheme every store
// route uses ("name 2.ext", with the same-contents check), the one recursive
// file walk, and the one files.rm wrapper.
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';
import { fakeHeap, installFakeCoreFs } from '../helpers/fakeCoreFs';

const heap = fakeHeap();
vi.mock('@/bus/emulator', async () => ({
  ...(await (await import('../helpers/bridgeMock')).emulatorModule()),
  getModuleHeap: () => heap,
}));

const { uniquePath, walkFiles, rmPath } = await import('@/bus/vfs');

let files: Map<string, Uint8Array>;
beforeEach(() => {
  bridge.reset();
  files = installFakeCoreFs(heap);
});

describe('uniquePath', () => {
  it('is the name itself when nothing is there', async () => {
    expect(await uniquePath('/d', 'disk.dmg')).toEqual({ path: '/d/disk.dmg', same: false });
  });

  it('numbers the stem Finder style past every name taken', async () => {
    files.set('/d/disk.dmg', new Uint8Array([1]));
    files.set('/d/disk 2.dmg', new Uint8Array([2]));
    expect(await uniquePath('/d', 'disk.dmg')).toEqual({ path: '/d/disk 3.dmg', same: false });
    files.set('/d/README', new Uint8Array([1]));
    expect((await uniquePath('/d', 'README')).path).toBe('/d/README 2');
  });

  it('answers a byte-identical file already there with sameAs', async () => {
    files.set('/d/disk.dmg', new Uint8Array([1]));
    files.set('/d/disk 2.dmg', new Uint8Array([2]));
    files.set('/s/new', new Uint8Array([2]));
    expect(await uniquePath('/d', 'disk.dmg', { sameAs: '/s/new' })).toEqual({
      path: '/d/disk 2.dmg',
      same: true,
    });
  });
});

describe('walkFiles', () => {
  beforeEach(() => {
    const tree: Record<string, unknown> = {
      '/a': [
        { name: '.', kind: 'directory' },
        { name: '..', kind: 'directory' },
        { name: 'x', kind: 'file' },
        { name: 'sub', kind: 'directory' },
        { name: 'y', kind: 'file' },
      ],
      '/a/sub': [{ name: 'z', kind: 'file' }],
      '/bad': [{ name: 'unreadable', kind: 'directory' }],
      '/bad/unreadable': { error: 'image is busy' },
    };
    bridge.reply('files.list', (args: unknown) => tree[(args as [string])[0]]);
  });

  it('lists every file depth first, skipping . and ..', async () => {
    expect(await walkFiles('/a')).toEqual(['/a/x', '/a/sub/z', '/a/y']);
  });

  it('gives paths relative to the root on request', async () => {
    expect(await walkFiles('/a', { relative: true })).toEqual(['x', 'sub/z', 'y']);
  });

  it('throws, naming the directory, when one cannot be listed', async () => {
    await expect(walkFiles('/bad')).rejects.toThrow('cannot list /bad/unreadable: image is busy');
  });
});

describe('rmPath', () => {
  it('removes the tree and says so', async () => {
    files.set('/d/a', new Uint8Array(1));
    files.set('/d/b/c', new Uint8Array(1));
    expect(await rmPath('/d')).toBe(true);
    expect([...files.keys()]).toEqual([]);
  });

  it('is quiet about a refusal by default, and throws its reason when checked', async () => {
    bridge.reply('files.rm', { error: "refusing to remove '/opfs'" });
    expect(await rmPath('/opfs')).toBe(false);
    await expect(rmPath('/opfs', { checked: true })).rejects.toThrow(/refusing to remove/);
  });
});
