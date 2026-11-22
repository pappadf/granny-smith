// VFS bus — structured listing of paths that descend into a guest disk image
// or an archive (partitions, then the HFS / UFS volume contents; an
// archive's members; any nesting of them). Wraps the C-side `files.list`
// object-model method, which returns [{name, kind, size, mtime, expandable}].
// The Filesystem tree uses this for descent; plain OPFS paths keep going
// through opfs.ts, with the core asked which of their files are expandable.
// Also the path helpers every store route shares: uniquePath (the one
// de-duplicating name scheme), walkFiles (the one recursive file list) and
// rmPath (files.rm, quiet or checked).

import { gsEval, gsErrorText, gsOk } from './emulator';
import { opfs } from './opfs';
import { listViaVfs, isExpandable, markExpandable } from '@/lib/diskImage';
import { identifyFile } from '@/lib/media';
import type { OpfsEntry } from './types';

interface VfsRawEntry {
  name: string;
  kind: 'file' | 'directory';
  size: number;
  mtime?: number;
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
  // files.list returns a native array of {name, kind, size, mtime, expandable}
  // objects (VK_LIST of VK_MAP through the gsEval bridge) — no inner JSON.parse.
  const parsed = await gsEval('files.list', [dir]);
  if (!Array.isArray(parsed)) throw new Error(gsErrorText(parsed));
  return (parsed as VfsRawEntry[]).map((e) => {
    const path = `${dir}/${e.name}`;
    const expandable = e.kind === 'file' && e.expandable === true;
    markExpandable(path, expandable);
    return {
      name: e.name,
      path,
      kind: e.kind === 'directory' ? 'directory' : 'file',
      expandable,
      size: e.size,
      mtime: e.mtime ?? 0,
    };
  });
}

// An OPFS listing, with each file's expandable flag from the core (and its
// size, where OPFS could not say).  When the core cannot answer (the module
// is not up yet), the entries go without.
async function opfsListAnnotated(dir: string): Promise<OpfsEntry[]> {
  const entries = await opfs.list(dir);
  if (!entries.some((e) => e.kind === 'file')) return entries;
  let core: Map<string, VfsRawEntry> | null = null;
  try {
    const parsed = await gsEval('files.list', [dir]);
    if (Array.isArray(parsed)) core = new Map((parsed as VfsRawEntry[]).map((e) => [e.name, e]));
  } catch {
    core = null;
  }
  return entries.map((e) => {
    if (e.kind !== 'file' || !core) return e;
    const c = core.get(e.name);
    const expandable = c?.expandable === true;
    markExpandable(e.path, expandable);
    return { ...e, expandable, size: e.size ?? c?.size };
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

// One directory's raw `files.list` entries, unrouted and unannotated (the
// path field's browser), or the core's reason when it cannot list it.
export async function listRaw(dir: string): Promise<unknown[] | string> {
  const r = await gsEval('files.list', [dir]);
  return Array.isArray(r) ? r : gsErrorText(r);
}

// The Mac OS volumes of a disk image (files.identify's volumes, numbered as
// the attached disk's partitions); none for an image the core cannot read.
export async function diskPartitions(path: string): Promise<unknown[]> {
  const id = await identifyFile(gsEval, path).catch(() => null);
  return id?.volumes ?? [];
}

// A file's size in bytes, or null when the core cannot say.
export async function pathSize(path: string): Promise<number | null> {
  const r = await gsEval('files.path_size', [path]);
  return typeof r === 'number' ? r : null;
}

// Whether anything exists at `path`.
export async function pathExists(path: string): Promise<boolean> {
  return (await gsEval('files.path_exists', [path])) === true;
}

// Remove a file or directory tree through the core (on the worker, so its
// WasmFS view stays coherent).  By default quiet: true when it is gone,
// false when the core refused, never throwing -- for best-effort cleanup.
// `checked`: a refusal throws the core's reason instead.
export async function rmPath(path: string, opts: { checked?: boolean } = {}): Promise<boolean> {
  let res: unknown;
  try {
    res = await gsEval('files.rm', [path]);
  } catch (e) {
    if (opts.checked) throw e;
    return false;
  }
  if (gsOk(res)) return true;
  if (opts.checked) throw new Error(gsErrorText(res));
  return false;
}

// A free path for `name` in `dir`: the name itself when nothing is there,
// else the first "name 2.ext", "name 3.ext", ... (the Finder's scheme).
// With `sameAs`, a file already there that is byte-identical to that one is
// the answer (`same`: it is stored already) rather than a reason to move on.
export async function uniquePath(
  dir: string,
  name: string,
  opts: { sameAs?: string } = {},
): Promise<{ path: string; same: boolean }> {
  const dot = name.lastIndexOf('.');
  const stem = dot > 0 ? name.slice(0, dot) : name;
  const ext = dot > 0 ? name.slice(dot) : '';
  for (let n = 1; ; n++) {
    const path = n === 1 ? `${dir}/${name}` : `${dir}/${stem} ${n}${ext}`;
    if (!(await pathExists(path))) return { path, same: false };
    // files.path_compare answers -1 for identical files, else an offset.
    if (
      opts.sameAs !== undefined &&
      (await gsEval('files.path_compare', [opts.sameAs, path])) === -1
    )
      return { path, same: true };
  }
}

// Every file under `dir` (depth first, in listing order), as full paths or,
// with `relative`, as paths relative to `dir`.  Each entry says what it is
// (files.list), so a subdirectory is descended, never taken for a file.
// THROWS when a directory cannot be listed (an unreadable or oversized
// listing is the core's error, not an empty directory); callers decide
// whether to say so.
export async function walkFiles(dir: string, opts: { relative?: boolean } = {}): Promise<string[]> {
  const out: string[] = [];
  // Descend one directory: `rel` is its path relative to `dir`.
  const walk = async (rel: string): Promise<void> => {
    const at = rel ? `${dir}/${rel}` : dir;
    const entries = await gsEval('files.list', [at]);
    if (!Array.isArray(entries)) throw new Error(`cannot list ${at}: ${gsErrorText(entries)}`);
    for (const e of entries as { name?: unknown; kind?: unknown }[]) {
      if (typeof e?.name !== 'string' || e.name === '.' || e.name === '..') continue;
      const child = rel ? `${rel}/${e.name}` : e.name;
      if (e.kind === 'directory') await walk(child);
      else out.push(opts.relative ? child : `${dir}/${child}`);
    }
  };
  await walk('');
  return out;
}
