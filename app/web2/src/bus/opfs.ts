// OPFS bus — abstraction over the browser's persistent storage. Phase 2 ships
// a hard-coded MockOpfs backend; Phase 3 swaps in a BrowserOpfs that hits
// navigator.storage.getDirectory(). Components import `opfs` and call it like
// any object; tests can swap the backend via setOpfsBackend().

import type { ImageCategory, OpfsEntry, RecentEntry, RomInfo } from './types';

export interface OpfsBackend {
  list(dir: string): Promise<OpfsEntry[]>;
  scanRoms(): Promise<RomInfo[]>;
  scanImages(cat: ImageCategory): Promise<OpfsEntry[]>;
  readJson<T>(path: string): Promise<T | null>;
  writeJson(path: string, value: unknown): Promise<void>;
}

// Hard-coded fixtures matching the prototype's mock options. Phase 3 replaces
// this whole class with one that walks navigator.storage.getDirectory().
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

  async list(): Promise<OpfsEntry[]> {
    return [];
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

  async readJson<T>(path: string): Promise<T | null> {
    return (this.json.get(path) as T) ?? null;
  }

  async writeJson(path: string, value: unknown): Promise<void> {
    this.json.set(path, value);
  }
}

let backend: OpfsBackend = new MockOpfs();

// Stable export identity — components import `opfs` once at module load and
// every call dispatches through whatever backend is currently active.
export const opfs: OpfsBackend = {
  list: (dir) => backend.list(dir),
  scanRoms: () => backend.scanRoms(),
  scanImages: (cat) => backend.scanImages(cat),
  readJson: (path) => backend.readJson(path),
  writeJson: (path, value) => backend.writeJson(path, value),
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

import { ROMS_DIR } from '@/lib/opfsPaths';

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
