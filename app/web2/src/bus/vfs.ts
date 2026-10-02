// VFS bus — structured listing of paths that descend into a guest disk image
// or an archive (partitions, then the HFS / UFS volume contents; an
// archive's members; any nesting of them). Wraps the C-side `files.list`
// object-model method, which returns [{name, kind, size, expandable}]. The
// Filesystem tree uses this for descent; plain OPFS paths keep going through
// opfs.ts, with the core asked which of their files are expandable.

import { gsEval, gsErrorText } from './emulator';
import { opfs } from './opfs';
import { listViaVfs, isExpandable, markExpandable } from '@/lib/diskImage';
import type { OpfsEntry } from './types';

interface VfsRawEntry {
  name: string;
  kind: 'file' | 'directory';
  size: number;
  expandable?: boolean;
}

// List a directory through the core. `dir` is the full extended path (e.g.
// /opfs/images/hd/disk.img or .../disk.img/partition1/etc). Returns entries
// shaped like opfs.list so the tree renders them uniformly, and records which
// are expandable. THROWS on failure (module not up, unreadable or busy image,
// oversized/corrupt listing) — failures must be distinguishable from a
// genuinely empty directory so the tree doesn't cache them as permanent
// emptiness.
export async function vfsList(dir: string): Promise<OpfsEntry[]> {
  // files.list returns a native array of {name, kind, size, expandable}
  // objects (V_LIST of V_MAP through the gsEval bridge) — no inner JSON.parse.
  const parsed = await gsEval('files.list', [dir]);
  if (!Array.isArray(parsed)) throw new Error(gsErrorText(parsed));
  return (parsed as VfsRawEntry[]).map((e) => {
    const path = `${dir}/${e.name}`;
    const expandable = e.kind === 'file' && e.expandable === true;
    markExpandable(path, expandable);
    return { name: e.name, path, kind: e.kind === 'directory' ? 'directory' : 'file', expandable };
  });
}

// An OPFS listing, with each file's expandable flag from the core.  When the
// core cannot answer (the module is not up yet), the entries go without.
async function opfsListAnnotated(dir: string): Promise<OpfsEntry[]> {
  const entries = await opfs.list(dir);
  if (!entries.some((e) => e.kind === 'file')) return entries;
  let flags: Map<string, boolean> | null = null;
  try {
    const parsed = await gsEval('files.list', [dir]);
    if (Array.isArray(parsed))
      flags = new Map((parsed as VfsRawEntry[]).map((e) => [e.name, e.expandable === true]));
  } catch {
    flags = null;
  }
  return entries.map((e) => {
    if (e.kind !== 'file' || !flags) return e;
    const expandable = flags.get(e.name) === true;
    markExpandable(e.path, expandable);
    return { ...e, expandable };
  });
}

// List a directory's children for the Filesystem tree, auto-routing between
// OPFS and descent and collapsing a lone synthetic partition (floppies and
// raw single-volume images have no partition map — the VFS reports a single
// "partition1" — so descend straight into the volume).
export async function listDir(dir: string): Promise<OpfsEntry[]> {
  if (!listViaVfs(dir)) return opfsListAnnotated(dir);
  const entries = await vfsList(dir);
  if (
    isExpandable(dir) &&
    entries.length === 1 &&
    entries[0].kind === 'directory' &&
    /^partition\d+$/i.test(entries[0].name)
  ) {
    return vfsList(entries[0].path);
  }
  return entries;
}
