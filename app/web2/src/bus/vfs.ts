// VFS bus — structured listing of paths that descend into a guest disk image
// (partitions, then the HFS / UFS volume contents). Wraps the C-side
// `vfs.list` object-model method (see docs/target-filesystems.md), which
// returns a JSON array [{name, kind, size}]. The Filesystem tree uses this
// for image descent; plain OPFS paths keep going through opfs.ts.

import { gsEval, gsErrorText } from './emulator';
import { opfs } from './opfs';
import { listViaVfs, isInImageSpace } from '@/lib/diskImage';
import type { OpfsEntry } from './types';

interface VfsRawEntry {
  name: string;
  kind: 'file' | 'directory';
  size: number;
}

// List a directory that descends into a disk image. `dir` is the full
// extended path (e.g. /opfs/images/hd/disk.img or
// .../disk.img/partition1/etc). Returns entries shaped like opfs.list so the
// tree renders them uniformly. THROWS on failure (module not up, unreadable
// or busy image, oversized/corrupt listing) — failures must be
// distinguishable from a genuinely empty directory so the tree doesn't cache
// them as permanent emptiness.
export async function vfsList(dir: string): Promise<OpfsEntry[]> {
  // vfs.list returns a native array of {name, kind, size} objects (V_LIST
  // of V_MAP through the gsEval bridge) — no inner JSON.parse.
  const parsed = await gsEval('vfs.list', [dir]);
  if (!Array.isArray(parsed)) throw new Error(gsErrorText(parsed));
  return (parsed as VfsRawEntry[]).map((e) => ({
    name: e.name,
    path: `${dir}/${e.name}`,
    kind: e.kind === 'directory' ? 'directory' : 'file',
  }));
}

// List a directory's children for the Filesystem tree, auto-routing between
// OPFS and image-descent and collapsing a lone synthetic partition (floppies
// and raw single-volume images have no partition map — the VFS reports a single
// "partition1" — so descend straight into the volume).
export async function listDir(dir: string): Promise<OpfsEntry[]> {
  if (!listViaVfs(dir)) return opfs.list(dir);
  const entries = await vfsList(dir);
  if (
    !isInImageSpace(dir) &&
    entries.length === 1 &&
    entries[0].kind === 'directory' &&
    /^partition\d+$/i.test(entries[0].name)
  ) {
    return vfsList(entries[0].path);
  }
  return entries;
}
