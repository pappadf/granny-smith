// OPFS bus — abstraction over the browser's persistent storage: BrowserOpfs
// hits navigator.storage.getDirectory(). Components import `opfs` and call it
// like any object; tests swap the backend via setOpfsBackend().

import type { CheckpointEntry, ImageCategory, OpfsEntry, RomInfo } from './types';
import { CHECKPOINT_DIR, ROMS_DIR, FDHD_DIR } from '@/lib/opfsPaths';
import { parseCheckpointDirName, formatCheckpointLabel } from '@/lib/checkpointMeta';
import { gsEval, gsErrorText, isModuleReady } from './emulator';

export interface OpfsBackend {
  list(dir: string): Promise<OpfsEntry[]>;
  scanRoms(): Promise<RomInfo[]>;
  scanImages(cat: ImageCategory): Promise<OpfsEntry[]>;
  scanCheckpoints(): Promise<CheckpointEntry[]>;
  readJson<T>(path: string): Promise<T | null>;
  writeJson(path: string, value: unknown): Promise<void>;
  /** Read a file's bytes as a Blob. Rejects if the path isn't a readable file. */
  readFile(path: string): Promise<Blob>;
  /** Move a file or dir to a new path (recursive for dirs). */
  move(src: string, dst: string): Promise<void>;
  /** Recursive delete. */
  delete(path: string): Promise<void>;
  /** Rename within the same parent. */
  rename(path: string, newName: string): Promise<void>;
  /** Best-effort mkdir -p. */
  mkdirP(path: string): Promise<void>;
}

// No backend yet: every call throws.  main.ts installs BrowserOpfs before
// anything reads storage, and tests/setup.ts installs the MockOpfs fixture
// (tests/helpers/mockOpfs.ts), so reaching this is an ordering bug -- loud,
// where the old default served made-up fixtures.
function unset(): never {
  throw new Error('OPFS backend not installed (setOpfsBackend was never called)');
}
const unsetOpfs: OpfsBackend = {
  list: unset,
  scanRoms: unset,
  scanImages: unset,
  scanCheckpoints: unset,
  readJson: unset,
  writeJson: unset,
  readFile: unset,
  move: unset,
  delete: unset,
  rename: unset,
  mkdirP: unset,
};

let backend: OpfsBackend = unsetOpfs;

// Stable export identity — components import `opfs` once at module load and
// every call dispatches through whatever backend is currently active.
export const opfs: OpfsBackend = {
  list: (dir) => backend.list(dir),
  scanRoms: () => backend.scanRoms(),
  scanImages: (cat) => backend.scanImages(cat),
  scanCheckpoints: () => backend.scanCheckpoints(),
  readJson: (path) => backend.readJson(path),
  writeJson: (path, value) => backend.writeJson(path, value),
  readFile: (path) => backend.readFile(path),
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
      .filter((e) => e.kind === 'file' && !e.name.startsWith('._'))
      .map((e) => ({ name: e.name, path: e.path, size: 0 }));
  }

  async scanImages(cat: ImageCategory): Promise<OpfsEntry[]> {
    const entries = await this.list(`/opfs/images/${cat}`);
    // Migration: an earlier build filed HD (1.44 MB) floppies under
    // /opfs/images/fdhd/, a directory no category ever displayed — they
    // became invisible. New uploads land in fd (see media.ts), but fold any
    // stragglers already sitting in fdhd back into the fd listing so users
    // can still see, insert, and delete them. list() returns [] if the dir
    // doesn't exist, so this is a no-op on fresh profiles.
    if (cat === 'fd') entries.push(...(await this.list(FDHD_DIR)));
    // AppleDouble "._<name>" sidecars are metadata for a data file, never
    // insertable media — exclude them from every image category so they don't
    // appear in the Images tab or the New Machine media dropdowns.
    return entries.filter((e) => !e.name.startsWith('._'));
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

  async readFile(path: string): Promise<Blob> {
    const dir = await getDirAtPath(path.replace(/\/[^/]+$/, ''));
    const name = path.split('/').pop() ?? '';
    const fh = await dir.getFileHandle(name);
    return fh.getFile();
  }

  async move(src: string, dst: string): Promise<void> {
    // Route through the worker (storage.mv) so its WasmFS inode cache stays
    // coherent with OPFS — same rationale as delete(). Once the module is up
    // a worker-reported failure must NOT fall back to a main-thread mutation:
    // that would change OPFS behind the worker's cache and recreate the exact
    // stale-inode bug this routing exists to prevent — propagate it instead.
    if (isModuleReady()) {
      const res = await gsEval('storage.mv', [src, dst]);
      if (res !== true) throw new Error(gsErrorText(res));
      return;
    }
    // Module not up yet (no worker cache to desync) — direct OPFS fallback.
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
    // Route through the worker (WasmFS) so its inode cache stays coherent with
    // OPFS. A main-thread navigator.storage delete is invisible to the worker,
    // leaving a dangling inode that breaks a later worker-side create at the
    // same path (e.g. re-copying a file out of an image after deleting it).
    // As with move(): once the module is up, a worker-reported failure is
    // propagated, never retried main-thread — and propagating is also what
    // makes the UI's per-item failure accounting real instead of dead code.
    if (isModuleReady()) {
      const res = await gsEval('storage.rm', [path]);
      if (res !== true) throw new Error(gsErrorText(res));
      return;
    }
    // Module not up yet (no worker cache to desync) — direct OPFS fallback.
    const parent = await getDirAtPath(path.replace(/\/[^/]+$/, ''));
    const name = path.split('/').pop() ?? '';
    try {
      await parent.removeEntry(name, { recursive: true });
    } catch (err) {
      // Deleting something already gone is success; anything else surfaces.
      if ((err as DOMException)?.name !== 'NotFoundError') throw err;
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
