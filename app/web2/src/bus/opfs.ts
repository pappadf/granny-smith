// OPFS bus — abstraction over the browser's persistent storage. Phase 2 ships
// a hard-coded MockOpfs backend; Phase 3 swaps in a BrowserOpfs that hits
// navigator.storage.getDirectory(). Components import `opfs` and call it like
// any object; tests can swap the backend via setOpfsBackend().

import type { CheckpointEntry, ImageCategory, OpfsEntry, RecentEntry, RomInfo } from './types';
import { CHECKPOINT_DIR, ROMS_DIR } from '@/lib/opfsPaths';
import { parseCheckpointDirName, formatCheckpointLabel } from '@/lib/checkpointMeta';

export interface OpfsBackend {
  list(dir: string): Promise<OpfsEntry[]>;
  scanRoms(): Promise<RomInfo[]>;
  scanImages(cat: ImageCategory): Promise<OpfsEntry[]>;
  scanCheckpoints(): Promise<CheckpointEntry[]>;
  readJson<T>(path: string): Promise<T | null>;
  writeJson(path: string, value: unknown): Promise<void>;
  /** Move a file or dir to a new path (recursive for dirs). */
  move(src: string, dst: string): Promise<void>;
  /** Recursive delete. */
  delete(path: string): Promise<void>;
  /** Rename within the same parent. */
  rename(path: string, newName: string): Promise<void>;
  /** Best-effort mkdir -p. */
  mkdirP(path: string): Promise<void>;
}

// Hard-coded fixtures matching the prototype's mock options. Used by Phase 2
// component tests and as a stand-in when navigator.storage isn't available.
// Backed by an in-memory map so the new move/delete/rename methods can mutate
// observable state during tests.
class MockOpfs implements OpfsBackend {
  private json = new Map<string, unknown>([
    [
      '/opfs/config/recent.json',
      [
        {
          model: 'Macintosh SE/30',
          ram: '8 MB',
          media: 'HD0: System 7.1, FD0: Install Disk',
          lastUsedAt: Date.now() - 3 * 60 * 60 * 1000,
        },
        {
          model: 'Macintosh Plus',
          ram: '4 MB',
          media: 'HD0: hd1.img',
          lastUsedAt: Date.now() - 24 * 60 * 60 * 1000,
        },
      ] satisfies RecentEntry[],
    ],
  ]);

  // Lightweight in-memory tree. Each entry maps OPFS path → kind. Files have
  // an associated size; directories have no entry but are implied by any
  // file path that walks through them.
  private files = new Map<string, { size: number }>();
  private dirs = new Set<string>();

  // Seed with the canonical category dirs + a couple of fixture files so
  // tests that just `scanImages(cat)` get predictable results.
  constructor() {
    this.dirs.add('/opfs');
    this.dirs.add('/opfs/images');
    for (const cat of ['rom', 'vrom', 'fd', 'hd', 'cd'] as const) {
      this.dirs.add(`/opfs/images/${cat}`);
    }
    this.dirs.add('/opfs/checkpoints');
    this.dirs.add('/opfs/upload');
    this.dirs.add('/opfs/config');
    this.files.set('/opfs/images/rom/Plus_v3.rom', { size: 128 * 1024 });
    this.files.set('/opfs/images/rom/SE30.rom', { size: 256 * 1024 });
    this.files.set('/opfs/images/vrom/ROM_97221136', { size: 256 * 1024 });
    this.files.set('/opfs/images/vrom/ROM_SE30_VROM', { size: 256 * 1024 });
    this.files.set('/opfs/images/fd/System_7.0_Install.dsk', { size: 1440 * 1024 });
    this.files.set('/opfs/images/fd/Disk_Tools.dsk', { size: 800 * 1024 });
    this.files.set('/opfs/images/hd/hd1.img', { size: 40 * 1024 * 1024 });
    this.files.set('/opfs/images/hd/hd2.img', { size: 80 * 1024 * 1024 });
    this.files.set('/opfs/images/cd/system7.iso', { size: 200 * 1024 * 1024 });
  }

  async list(dir: string): Promise<OpfsEntry[]> {
    const prefix = dir.replace(/\/$/, '') + '/';
    const out: OpfsEntry[] = [];
    const seen = new Set<string>();
    for (const path of this.files.keys()) {
      if (!path.startsWith(prefix)) continue;
      const rest = path.slice(prefix.length);
      const first = rest.split('/')[0];
      if (!first || seen.has(first)) continue;
      seen.add(first);
      const childPath = prefix + first;
      const isFile = this.files.has(childPath);
      out.push({ name: first, path: childPath, kind: isFile ? 'file' : 'directory' });
    }
    for (const d of this.dirs) {
      if (!d.startsWith(prefix)) continue;
      const rest = d.slice(prefix.length);
      const first = rest.split('/')[0];
      if (!first || seen.has(first)) continue;
      seen.add(first);
      out.push({ name: first, path: prefix + first, kind: 'directory' });
    }
    return out;
  }

  async scanRoms(): Promise<RomInfo[]> {
    return [
      { name: 'Macintosh Plus v3.rom', path: '/opfs/images/rom/Plus_v3.rom', size: 128 * 1024 },
      { name: 'SE30.rom', path: '/opfs/images/rom/SE30.rom', size: 256 * 1024 },
    ];
  }

  async scanImages(cat: ImageCategory): Promise<OpfsEntry[]> {
    const fixtures: Record<ImageCategory, string[]> = {
      rom: ['Plus_v3.rom', 'SE30.rom'],
      vrom: ['ROM_97221136', 'ROM_SE30_VROM'],
      fd: ['System_7.0_Install.dsk', 'Disk_Tools.dsk'],
      hd: ['hd1.img', 'hd2.img'],
      cd: ['system7.iso'],
    };
    return fixtures[cat].map((name) => ({
      name,
      path: `/opfs/images/${cat}/${name}`,
      kind: 'file' as const,
    }));
  }

  async scanCheckpoints(): Promise<CheckpointEntry[]> {
    return [];
  }

  async readJson<T>(path: string): Promise<T | null> {
    return (this.json.get(path) as T) ?? null;
  }

  async writeJson(path: string, value: unknown): Promise<void> {
    this.json.set(path, value);
  }

  async move(src: string, dst: string): Promise<void> {
    const moved = new Map<string, { size: number }>();
    for (const [p, v] of this.files.entries()) {
      if (p === src || p.startsWith(src + '/')) {
        moved.set(dst + p.slice(src.length), v);
      }
    }
    for (const [k, v] of moved) {
      this.files.delete(k.replace(dst, src));
      this.files.set(k, v);
    }
    for (const d of Array.from(this.dirs)) {
      if (d === src || d.startsWith(src + '/')) {
        this.dirs.delete(d);
        this.dirs.add(dst + d.slice(src.length));
      }
    }
  }

  async delete(path: string): Promise<void> {
    for (const p of Array.from(this.files.keys())) {
      if (p === path || p.startsWith(path + '/')) this.files.delete(p);
    }
    for (const d of Array.from(this.dirs)) {
      if (d === path || d.startsWith(path + '/')) this.dirs.delete(d);
    }
  }

  async rename(path: string, newName: string): Promise<void> {
    const parent = path.replace(/\/[^/]+$/, '');
    await this.move(path, `${parent}/${newName}`);
  }

  async mkdirP(path: string): Promise<void> {
    const parts = path.replace(/^\//, '').split('/');
    let cur = '';
    for (const p of parts) {
      cur += '/' + p;
      this.dirs.add(cur);
    }
  }
}

let backend: OpfsBackend = new MockOpfs();

// Stable export identity — components import `opfs` once at module load and
// every call dispatches through whatever backend is currently active.
export const opfs: OpfsBackend = {
  list: (dir) => backend.list(dir),
  scanRoms: () => backend.scanRoms(),
  scanImages: (cat) => backend.scanImages(cat),
  scanCheckpoints: () => backend.scanCheckpoints(),
  readJson: (path) => backend.readJson(path),
  writeJson: (path, value) => backend.writeJson(path, value),
  move: (src, dst) => backend.move(src, dst),
  delete: (path) => backend.delete(path),
  rename: (path, newName) => backend.rename(path, newName),
  mkdirP: (path) => backend.mkdirP(path),
};

export function setOpfsBackend(b: OpfsBackend): void {
  backend = b;
}

// --- BrowserOpfs ---------------------------------------------------------
//
// Real OPFS-backed implementation. Reads (list / scanRoms / scanImages /
// readJson) use navigator.storage.getDirectory() directly — safe from any
// thread, browser-internal. Writes (writeJson) also use OPFS direct;
// cross-thread *image* writes still go through gsEval('storage.cp', …) in
// bus/upload.ts because those are bigger and have to coexist with the
// emulator's own OPFS handles. JSON config files are small and short-lived
// — direct OPFS works fine.
//
// All paths use the `/opfs/...` prefix to match the C-side mount path.
// BrowserOpfs strips the prefix before walking the navigator.storage tree.

async function getDirAtPath(
  path: string,
  options: { create?: boolean } = {},
): Promise<FileSystemDirectoryHandle> {
  const rel = path.replace(/^\/opfs\/?/, '');
  let dir = await navigator.storage.getDirectory();
  if (!rel) return dir;
  for (const part of rel.split('/').filter(Boolean)) {
    dir = await dir.getDirectoryHandle(part, options);
  }
  return dir;
}

export class BrowserOpfs implements OpfsBackend {
  async list(dir: string): Promise<OpfsEntry[]> {
    try {
      const handle = await getDirAtPath(dir);
      const out: OpfsEntry[] = [];
      for await (const [name, child] of handle.entries()) {
        out.push({ name, path: `${dir}/${name}`, kind: child.kind });
      }
      return out;
    } catch {
      return [];
    }
  }

  async scanRoms(): Promise<RomInfo[]> {
    const entries = await this.list(ROMS_DIR);
    return entries
      .filter((e) => e.kind === 'file')
      .map((e) => ({ name: e.name, path: e.path, size: 0 }));
  }

  async scanImages(cat: ImageCategory): Promise<OpfsEntry[]> {
    return this.list(`/opfs/images/${cat}`);
  }

  async readJson<T>(path: string): Promise<T | null> {
    try {
      const dir = await getDirAtPath(path.replace(/\/[^/]+$/, ''));
      const fileName = path.split('/').pop() ?? '';
      const fileHandle = await dir.getFileHandle(fileName);
      const file = await fileHandle.getFile();
      const text = await file.text();
      return JSON.parse(text) as T;
    } catch {
      return null;
    }
  }

  async writeJson(path: string, value: unknown): Promise<void> {
    try {
      const dir = await getDirAtPath(path.replace(/\/[^/]+$/, ''), { create: true });
      const fileName = path.split('/').pop() ?? '';
      const fileHandle = await dir.getFileHandle(fileName, { create: true });
      const writable = await fileHandle.createWritable();
      await writable.write(JSON.stringify(value));
      await writable.close();
    } catch {
      // Quota / locked file / private mode — silent fall-through.
    }
  }

  async scanCheckpoints(): Promise<CheckpointEntry[]> {
    const entries = await this.list(CHECKPOINT_DIR);
    const out: CheckpointEntry[] = [];
    for (const e of entries) {
      if (e.kind !== 'directory') continue;
      const parsed = parseCheckpointDirName(e.name);
      if (!parsed) continue;
      const manifest = await this.readJson<{ label?: string; machine?: string }>(
        `${e.path}/manifest.json`,
      );
      let sizeBytes = 0;
      try {
        const dirEntries = await this.list(e.path);
        for (const f of dirEntries) {
          if (f.kind !== 'file') continue;
          const fileDir = await getDirAtPath(e.path);
          const fh = await fileDir.getFileHandle(f.name);
          const file = await fh.getFile();
          sizeBytes += file.size;
        }
      } catch {
        // Best effort — keep sizeBytes = 0.
      }
      out.push({
        path: e.path,
        dirName: e.name,
        id: parsed.id,
        created: parsed.created,
        label: manifest?.label ?? formatCheckpointLabel(parsed.created),
        machine: manifest?.machine ?? 'unknown',
        sizeBytes,
      });
    }
    return out;
  }

  async move(src: string, dst: string): Promise<void> {
    // OPFS has no native rename — copy then delete. For dirs, walk
    // recursively. For files, single-shot stream copy.
    const srcParent = src.replace(/\/[^/]+$/, '');
    const srcName = src.split('/').pop() ?? '';
    const parentDir = await getDirAtPath(srcParent);
    let kind: 'file' | 'directory' = 'file';
    try {
      await parentDir.getFileHandle(srcName);
    } catch {
      try {
        await parentDir.getDirectoryHandle(srcName);
        kind = 'directory';
      } catch {
        return; // src doesn't exist
      }
    }
    if (kind === 'file') {
      await this.copyFile(src, dst);
      await this.delete(src);
    } else {
      await this.copyDir(src, dst);
      await this.delete(src);
    }
  }

  private async copyFile(src: string, dst: string): Promise<void> {
    const srcParent = src.replace(/\/[^/]+$/, '');
    const srcName = src.split('/').pop() ?? '';
    const dstParent = dst.replace(/\/[^/]+$/, '');
    const dstName = dst.split('/').pop() ?? '';
    const srcDir = await getDirAtPath(srcParent);
    const dstDir = await getDirAtPath(dstParent, { create: true });
    const srcFh = await srcDir.getFileHandle(srcName);
    const file = await srcFh.getFile();
    const dstFh = await dstDir.getFileHandle(dstName, { create: true });
    const writable = await dstFh.createWritable();
    await writable.write(file);
    await writable.close();
  }

  private async copyDir(src: string, dst: string): Promise<void> {
    await this.mkdirP(dst);
    const entries = await this.list(src);
    for (const e of entries) {
      const newDst = `${dst}/${e.name}`;
      if (e.kind === 'file') await this.copyFile(e.path, newDst);
      else await this.copyDir(e.path, newDst);
    }
  }

  async delete(path: string): Promise<void> {
    try {
      const parent = await getDirAtPath(path.replace(/\/[^/]+$/, ''));
      const name = path.split('/').pop() ?? '';
      await parent.removeEntry(name, { recursive: true });
    } catch {
      // Best-effort.
    }
  }

  async rename(path: string, newName: string): Promise<void> {
    const parent = path.replace(/\/[^/]+$/, '');
    await this.move(path, `${parent}/${newName}`);
  }

  async mkdirP(path: string): Promise<void> {
    try {
      await getDirAtPath(path, { create: true });
    } catch {
      // Best-effort.
    }
  }
}

// Direct OPFS write for arbitrary File/Blob (bypasses /tmp + WASM heap).
// Used by bus/upload.ts to stage uploaded files. Mirrors fs.js::writeToOPFS.
export async function writeToOPFS(opfsPath: string, fileOrBlob: Blob): Promise<void> {
  const dir = await getDirAtPath(opfsPath.replace(/\/[^/]+$/, ''), { create: true });
  const fileName = opfsPath.split('/').pop() ?? '';
  const fileHandle = await dir.getFileHandle(fileName, { create: true });
  const writable = await fileHandle.createWritable();
  await writable.write(fileOrBlob);
  await writable.close();
}

// Best-effort directory removal.
export async function removeFromOPFS(opfsPath: string): Promise<void> {
  try {
    const dir = await getDirAtPath(opfsPath.replace(/\/[^/]+$/, ''));
    const name = opfsPath.split('/').pop() ?? '';
    await dir.removeEntry(name, { recursive: true });
  } catch {
    // Best-effort.
  }
}

export { MockOpfs };
