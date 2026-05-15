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

export { MockOpfs };
