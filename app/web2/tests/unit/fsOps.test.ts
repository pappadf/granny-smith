import { describe, it, expect, vi, beforeEach } from 'vitest';

// copyOutOfImage must never overwrite an existing destination. The worst case
// is dst resolving to the very image being read (a file inside X.img named
// "X.img" dropped into the image's own folder) — fopen("wb") would truncate
// the mounted image mid-read.
const { gsEvalMock } = vi.hoisted(() => ({ gsEvalMock: vi.fn() }));
vi.mock('@/bus/emulator', () => ({
  gsEval: (p: string, a?: unknown[]) => gsEvalMock(p, a),
  gsErrorText: (res: unknown) => String(res),
  isModuleReady: () => true,
}));

import { copyOutOfImage, opfsSafeName } from '@/bus/fsOps';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import type { OpfsEntry } from '@/bus/types';

// Backend whose list() returns a configurable set of existing names.
class ListOpfs extends MockOpfs {
  existing: string[] = [];
  async list(dir: string): Promise<OpfsEntry[]> {
    return this.existing.map((n) => ({ name: n, path: `${dir}/${n}`, kind: 'file' as const }));
  }
}

let backend: ListOpfs;
beforeEach(() => {
  backend = new ListOpfs();
  setOpfsBackend(backend);
  gsEvalMock.mockReset();
  gsEvalMock.mockResolvedValue(true);
});

describe('copyOutOfImage collision guards', () => {
  it('copies when the destination is free', async () => {
    const res = await copyOutOfImage(
      [{ path: '/opfs/images/fd/X.img/partition1/Read Me', isDir: false }],
      '/opfs/upload',
    );
    expect(res.failures).toEqual([]);
    expect(gsEvalMock).toHaveBeenCalledWith('storage.cp', [
      '/opfs/images/fd/X.img/partition1/Read Me',
      '/opfs/upload/Read Me',
    ]);
  });

  it('fails instead of overwriting an existing destination file', async () => {
    backend.existing = ['Read Me'];
    const res = await copyOutOfImage(
      [{ path: '/opfs/images/fd/X.img/partition1/Read Me', isDir: false }],
      '/opfs/upload',
    );
    expect(res.failures).toEqual(['Read Me']);
    expect(res.firstError).toMatch(/already exists/);
    expect(gsEvalMock).not.toHaveBeenCalled();
  });

  it('refuses a destination that is the source image itself', async () => {
    // File named like its containing image, dropped into the image's folder:
    // dst === the image path. The existing-name listing is empty here so only
    // the prefix guard stands between the drop and a truncated image.
    const res = await copyOutOfImage(
      [{ path: '/opfs/images/fd/X.img/partition1/X.img', isDir: false }],
      '/opfs/images/fd',
    );
    expect(res.failures).toEqual(['X.img']);
    expect(res.firstError).toMatch(/image being copied from/);
    expect(gsEvalMock).not.toHaveBeenCalled();
  });

  it('copies a disk image out via storage.cp (fork preservation is handled C-side)', async () => {
    // storage.cp now writes an AppleDouble "._<name>" sidecar whenever the
    // source carries a resource fork (e.g. an NDIF image, whose block map lives
    // there), so copy-out stays a single verb — no format sniffing here and no
    // implicit export_raw decode.
    const src = '/opfs/images/cd/Drivers.toast/partition2/GC for System 7.x.img';
    const res = await copyOutOfImage([{ path: src, isDir: false }], '/opfs/images/fd');
    expect(res.failures).toEqual([]);
    expect(gsEvalMock).toHaveBeenCalledWith('storage.cp', [
      src,
      '/opfs/images/fd/GC for System 7.x.img',
    ]);
    expect(gsEvalMock).not.toHaveBeenCalledWith('storage.export_raw', expect.anything());
  });

  it('fails the second of two batch items whose names sanitise identically', async () => {
    // HFS surfaces in-name '/' as ':'; opfsSafeName maps both to '_'.
    expect(opfsSafeName('Install 1:2')).toBe(opfsSafeName('Install 1_2'));
    const res = await copyOutOfImage(
      [
        { path: '/opfs/images/fd/X.img/partition1/Install 1:2', isDir: false },
        { path: '/opfs/images/fd/X.img/partition1/Install 1_2', isDir: false },
      ],
      '/opfs/upload',
    );
    expect(res.total).toBe(2);
    expect(res.failures).toEqual(['Install 1_2']);
    expect(gsEvalMock).toHaveBeenCalledTimes(1);
  });
});
