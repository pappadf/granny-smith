// The one reader of `catalog.profile(id)` and `catalog.default_config(id)`.
//
// The core publishes each model's configuration space as one tree -- options,
// floppy positions, storage buses, expansion slots, the cards that fit them,
// the display devices and the monitors they take -- with every label
// already written, and the default configuration as a document that
// mirrors the tree node by node (src/machines/machine_config.c).  This file
// owns the shape and memoises it per model for the session.  A card's
// availability follows the ROMs uploaded, so the cache is dropped when the
// stored ROM set changes (clearProfileCache).

import { gsEval, isGsError } from './emulator';

// A place a medium goes, as the attach verbs answer it: the bus it is on
// and the unit there.
export interface MediaBay {
  bus: string;
  id: number;
  label: string;
}

// One choice: an id the document carries and the label the core wrote.
export interface Choice {
  id: string;
  label: string;
  detail?: string;
}

// A scalar option (memory, AppleTalk, power supplies, a card's video memory).
export interface ConfigOption {
  id: string;
  label: string;
  detail?: string;
  kind: 'choice';
  values: Choice[];
  default: string;
  requires?: { value: string; option: string; not: string };
}

// A floppy drive position: the drive types it takes ("none" where the
// position can be empty).
export interface FloppyPosition {
  id: string; // "fd0"
  label: string;
  types: Choice[];
  default: string;
}

// A unit a storage device may use, with the position text the core composed
// ("ID 0 · Internal hard disk bay", "Master") and its short form without the
// bay ("ID 0"), for a narrow unit menu.
export interface StorageUnit {
  unit: number;
  label: string;
  short?: string;
}

export interface StorageBus {
  id: string;
  label: string;
  detail?: string;
  kind: string;
  width?: string;
  units: StorageUnit[];
  reserved: number[];
  shares_units_with: string[];
  external_connector: boolean;
  bays: StorageUnit[];
  accepts: Choice[]; // device types: hd, cd
  startup: boolean; // the startup record can name a device here
  // The blank hard disks its drives take: the files.* method that creates
  // one (with `arg`), and the new file's name stem and extension.
  blank_disks: BlankDisk[];
}

export interface BlankDisk {
  label: string;
  method: string;
  arg: string;
  name: string;
  ext: string;
}

export interface Slot {
  id: string;
  label: string;
  detail?: string;
  bus: string;
  kind: string;
  excludes: string[];
  fill_order: number;
}

export interface Card {
  id: string;
  label: string;
  detail?: string;
  class: string;
  fits: string[];
  rom: { kind: string; substitute: boolean } | null;
  status: 'ok' | 'substitute' | 'unavailable';
  reason?: string;
  monitors: string[];
  // Startup video modes per monitor ("WxHxD" ids, labels from the core).
  modes: Record<string, Choice[]>;
  options: ConfigOption[];
}

export interface BuiltinDisplay {
  id: 'builtin';
  label: string;
  detail?: string;
  monitors: string[];
  default_monitor: string;
  modes: Record<string, Choice[]>;
  options: ConfigOption[];
}

export interface Monitor {
  id: string;
  label: string;
  width: number;
  height: number;
}

// The configuration document machine.boot's config= takes.
export interface StorageDevice {
  bus: string;
  unit: number;
  type: string; // "hd" | "cd"
}

export interface CardEntry {
  slot: string;
  card: string;
  options: Record<string, string>;
}

export interface DisplayEntry {
  monitor: string;
  mode?: string;
}

export interface ConfigDocument {
  model: string;
  options: Record<string, string>;
  floppies: Record<string, string>;
  storage: StorageDevice[];
  startup: { bus: string; unit: number } | null;
  cards: CardEntry[];
  displays: Record<string, DisplayEntry>;
}

export interface MachineProfile {
  id: string;
  name: string;
  freq: number;
  // Derived capability probe: the typed facts the UI reads instead of
  // guessing from the model name.
  capabilities: {
    cpu?: { model?: number; address_bits?: number; fpu?: boolean };
    mmu?: { present?: boolean; kind?: string };
    nubus?: boolean;
    pci?: boolean;
    video_in?: boolean;
    audio_in?: boolean;
    aux_cpus?: Array<{ name?: string; arch?: string; freq?: number }>;
  };
  options: ConfigOption[];
  floppies: FloppyPosition[];
  storage: StorageBus[];
  slots: Slot[];
  cards: Card[];
  displays: { builtin: BuiltinDisplay | null; max_connected: number };
  monitors: Monitor[];
  defaults: ConfigDocument;
}

const cache = new Map<string, Promise<MachineProfile | null>>();

// The profile of `model`, or null when the core does not know it.  Memoised
// per session; a failed lookup is not cached, so a retry can succeed.
export function getProfile(model: string): Promise<MachineProfile | null> {
  const hit = cache.get(model);
  if (hit) return hit;
  const p = (async () => {
    const r = await gsEval('catalog.profile', [model]);
    if (!r || typeof r !== 'object' || isGsError(r)) return null;
    return normalise(r as Partial<MachineProfile>);
  })();
  cache.set(model, p);
  void p.then((v) => {
    if (!v) cache.delete(model);
  });
  return p;
}

// The running machine's profile, or null with no machine.
export async function getActiveProfile(): Promise<MachineProfile | null> {
  const id = await gsEval('machine.id');
  return typeof id === 'string' && id ? getProfile(id) : null;
}

// Forget every cached profile: tests, and an upload that changes which card
// ROMs are present (a card's status is the core's answer to that).
export function clearProfileCache(): void {
  cache.clear();
}

// An empty document for a model (what normalise fills a missing one with).
function emptyDocument(model: string): ConfigDocument {
  return {
    model,
    options: {},
    floppies: {},
    storage: [],
    startup: null,
    cards: [],
    displays: {},
  };
}

// Fill the arrays and maps a consumer iterates, so no caller needs `?? []`.
function normalise(r: Partial<MachineProfile>): MachineProfile {
  const id = r.id ?? '';
  const d = (r.defaults ?? {}) as Partial<ConfigDocument>;
  return {
    id,
    name: r.name ?? '',
    freq: r.freq ?? 0,
    capabilities: r.capabilities ?? {},
    options: r.options ?? [],
    floppies: r.floppies ?? [],
    storage: (r.storage ?? []).map((b) => ({
      ...b,
      units: b.units ?? [],
      reserved: b.reserved ?? [],
      shares_units_with: b.shares_units_with ?? [],
      bays: b.bays ?? [],
      accepts: b.accepts ?? [],
      blank_disks: b.blank_disks ?? [],
      startup: b.startup === true,
      external_connector: b.external_connector === true,
    })),
    slots: (r.slots ?? []).map((s) => ({ ...s, excludes: s.excludes ?? [] })),
    cards: (r.cards ?? []).map((c) => ({
      ...c,
      fits: c.fits ?? [],
      monitors: c.monitors ?? [],
      modes: c.modes ?? {},
      options: c.options ?? [],
      rom: c.rom ?? null,
      status: c.status ?? 'ok',
    })),
    displays: {
      builtin: r.displays?.builtin
        ? {
            ...r.displays.builtin,
            monitors: r.displays.builtin.monitors ?? [],
            modes: r.displays.builtin.modes ?? {},
            options: r.displays.builtin.options ?? [],
          }
        : null,
      max_connected: r.displays?.max_connected ?? 1,
    },
    monitors: r.monitors ?? [],
    defaults: {
      ...emptyDocument(id),
      ...d,
      storage: d.storage ?? [],
      cards: (d.cards ?? []).map((c) => ({ ...c, options: c.options ?? {} })),
      displays: d.displays ?? {},
      options: d.options ?? {},
      floppies: d.floppies ?? {},
      startup: d.startup ?? null,
    },
  };
}
