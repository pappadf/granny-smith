import type { OpfsBackend } from '@/bus/opfs';
import type { CheckpointEntry, ImageCategory, OpfsEntry, RomInfo } from '@/bus/types';

// The OPFS test double: hard-coded fixtures (the prototype's mock options)
// backed by an in-memory map, so move/delete/rename mutate observable state.
// tests/setup.ts installs a fresh one before every test; it used to be the
// production default, shipped in the bundle.
export class MockOpfs implements OpfsBackend {
  private json = new Map<string, unknown>();

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
    this.files.set('/opfs/images/rom/plus-v3-4d1f8172.rom', { size: 128 * 1024 });
    this.files.set('/opfs/images/rom/iix-iicx-se30-97221136.rom', { size: 256 * 1024 });
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
      {
        name: 'Macintosh Plus v3.rom',
        path: '/opfs/images/rom/plus-v3-4d1f8172.rom',
        size: 128 * 1024,
      },
      {
        name: 'iix-iicx-se30-97221136.rom',
        path: '/opfs/images/rom/iix-iicx-se30-97221136.rom',
        size: 256 * 1024,
      },
    ];
  }

  async scanImages(cat: ImageCategory): Promise<OpfsEntry[]> {
    const fixtures: Record<ImageCategory, string[]> = {
      rom: ['plus-v3-4d1f8172.rom', 'iix-iicx-se30-97221136.rom'],
      vrom: ['ROM_97221136', 'ROM_SE30_VROM'],
      prom: ['437584e0', '8c68216e'],
      fd: ['System_7.0_Install.dsk', 'Disk_Tools.dsk'],
      hd: ['hd1.img', 'hd2.img'],
      cd: ['system7.iso'],
    };
    // ...plus whatever a test added to the category directory.
    const dir = `/opfs/images/${cat}/`;
    const added = [...this.files.keys()]
      .filter((p) => p.startsWith(dir) && !p.slice(dir.length).includes('/'))
      .map((p) => p.slice(dir.length))
      .filter((name) => !fixtures[cat].includes(name));
    return [...fixtures[cat], ...added].map((name) => ({
      name,
      path: `${dir}${name}`,
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

  // Tests: a file appears, as an upload would leave it.
  addFile(path: string, size = 0): void {
    this.files.set(path, { size });
  }

  async readFile(path: string): Promise<Blob> {
    const f = this.files.get(path);
    if (!f) throw new Error(`no such file: ${path}`);
    return new Blob([new Uint8Array(Math.min(f.size, 1024))]);
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
