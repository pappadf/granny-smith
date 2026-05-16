// Shared TypeScript shapes for the bus layer. Anything that crosses the
// bus seam (UI <-> emulator / OPFS) goes through one of these types.

export interface MachineConfig {
  model: string;
  /** ROM image path (under /opfs/images/rom/). Empty / unset = use the
   *  C-side default for the model (machine.boot may succeed without one
   *  during early development). */
  rom?: string;
  vrom: string;
  ram: string;
  fd: string;
  hd: string;
  cd: string;
}

export interface RomInfo {
  name: string;
  path: string;
  size: number;
}

export interface OpfsEntry {
  name: string;
  path: string;
  kind: 'file' | 'directory';
}

export interface RecentEntry {
  model: string;
  ram: string;
  media: string;
  lastUsedAt: number;
}

export type ImageCategory = 'rom' | 'vrom' | 'fd' | 'hd' | 'cd';
