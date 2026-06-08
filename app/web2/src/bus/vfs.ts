// VFS bus — structured listing of paths that descend into a guest disk image
// (partitions, then the HFS / UFS volume contents). Wraps the C-side
// `vfs.list` object-model method (see docs/target-filesystems.md), which
// returns a JSON array [{name, kind, size}]. The Filesystem tree uses this
// for image descent; plain OPFS paths keep going through opfs.ts.

import { gsEval } from './emulator';
import type { OpfsEntry } from './types';

interface VfsRawEntry {
  name: string;
  kind: 'file' | 'directory';
  size: number;
}

// List a directory that descends into a disk image. `dir` is the full
// extended path (e.g. /opfs/images/hd/disk.img or
// .../disk.img/partition1/etc). Returns entries shaped like opfs.list so the
// tree renders them uniformly. Returns [] on any failure (unreadable or
// non-mountable image, a partition with no filesystem, a read error) — the
// node simply shows no children.
export async function vfsList(dir: string): Promise<OpfsEntry[]> {
  const raw = await gsEval('vfs.list', [dir]);
  if (typeof raw !== 'string') return [];
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return [];
  }
  if (!Array.isArray(parsed)) return [];
  return (parsed as VfsRawEntry[]).map((e) => ({
    name: e.name,
    path: `${dir}/${e.name}`,
    kind: e.kind === 'directory' ? 'directory' : 'file',
  }));
}
