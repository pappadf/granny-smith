// The one reader of `machine.profile(id)` (M4, F-11, F-49).
//
// Five call sites used to fetch the profile each for itself — the config
// dialog, capabilities, the URL path, the ROM-drop boot, the default HD bay —
// each with its own partial type, none cached, and a sixth inferred the
// floppy count by probing drives.  This file owns the full shape, mirroring
// build_profile (src/machines/machine.c), and memoises it per model for the
// session: a profile is static data.

import { gsEval, isGsError } from './emulator';

// A place a medium goes, as the core derives it (profile_hd_bays,
// profile_cdrom_bay): the bus it is on and the unit on that bus.
// machine.attach_hd / attach_cdrom answer the same shape.
export interface MediaBay {
  bus: 'floppy' | 'scsi' | 'scsi2' | 'profile';
  id: number;
  label: string;
}

export interface VideoMonitor {
  id: string;
  name?: string;
  width?: number;
  height?: number;
  depths?: number[];
}

export interface MachineProfile {
  id: string;
  name: string;
  freq: number;
  ram_options: number[]; // KB
  ram_default: number; // KB
  ram_max: number; // KB
  floppy_slots: Array<{ label: string; kind: string }>;
  scsi_buses: Array<{
    object: string;
    label: string;
    slots: Array<{ label: string; id: number; boot: boolean }>;
  }>;
  // 'scsi' or 'profile' (the Lisa/XL parallel-port ProFile).
  hd_bus: string;
  has_cdrom: boolean;
  cdrom_id: number;
  // The derived bays: every hard-disk bay in attach order (the boot bay
  // first), the default one, and the CD bay (null on a model without one).
  hd_bays: MediaBay[];
  hd_default: MediaBay | null;
  cdrom: MediaBay | null;
  // Derived capability probe (proposal §4.4): the typed facts the UI reads
  // instead of guessing from the model name.
  capabilities: {
    cpu?: { model?: number; address_bits?: number; fpu?: boolean };
    mmu?: { present?: boolean; kind?: string };
    nubus?: boolean;
    pci?: boolean;
    video_in?: boolean;
    audio_in?: boolean;
    aux_cpus?: Array<{ name?: string; arch?: string; freq?: number }>;
  };
  // Per-card video slot shape — the single source for the VROM requirement
  // (per-card requires_vrom) and the video-mode list (each card's monitors ×
  // depths).  Every declared socket is emitted; the dialog configures the
  // first.
  video_slots: Array<{
    slot: string;
    fixed: boolean;
    default_card: string;
    cards: Array<{
      id: string;
      display_name?: string;
      requires_vrom: boolean;
      monitors?: VideoMonitor[];
    }>;
  }>;
  // PCI sockets: each declared socket with the cards that fit it; each card
  // with its UI class ('display', 'other', ...), requires_prom and the
  // options it accepts (machine.boot's pci_option="key=value").  A fixed
  // `fallback` slot is emulator scaffolding standing in until a socket
  // supplies a card of its class (the 9500's Control/Chaos).
  pci_slots: Array<{
    slot: number;
    label?: string;
    fixed: boolean;
    fallback?: boolean;
    default_card?: string;
    cards: Array<{
      id: string;
      display_name?: string;
      class?: string;
      requires_prom?: boolean;
      options?: Array<{
        key: string;
        label?: string;
        default_value?: string;
        values?: Array<{ id: string; label?: string }>;
      }>;
      monitors?: VideoMonitor[];
    }>;
  }>;
  // The machine's own built-in video when it is not a NuBus pseudo-card (the
  // PDM family's Ariel scanout); an empty map on every other machine.
  builtin_video: { id?: string; display_name?: string; monitors?: VideoMonitor[] };
}

const cache = new Map<string, Promise<MachineProfile | null>>();

// The profile of `model`, or null when the core does not know it.  Memoised
// per session; a failed lookup is not cached, so a retry can succeed.
export function getProfile(model: string): Promise<MachineProfile | null> {
  const hit = cache.get(model);
  if (hit) return hit;
  const p = (async () => {
    const r = await gsEval('machine.profile', [model]);
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

// Forget every cached profile (tests).
export function clearProfileCache(): void {
  cache.clear();
}

// Fill the arrays and maps a consumer iterates, so no caller needs `?? []`.
function normalise(r: Partial<MachineProfile>): MachineProfile {
  return {
    id: r.id ?? '',
    name: r.name ?? '',
    freq: r.freq ?? 0,
    ram_options: r.ram_options ?? [],
    ram_default: r.ram_default ?? 0,
    ram_max: r.ram_max ?? 0,
    floppy_slots: r.floppy_slots ?? [],
    scsi_buses: r.scsi_buses ?? [],
    hd_bus: r.hd_bus ?? 'scsi',
    has_cdrom: r.has_cdrom === true,
    cdrom_id: r.cdrom_id ?? 0,
    hd_bays: r.hd_bays ?? [],
    hd_default: r.hd_default ?? null,
    cdrom: r.cdrom ?? null,
    capabilities: r.capabilities ?? {},
    video_slots: r.video_slots ?? [],
    pci_slots: r.pci_slots ?? [],
    builtin_video: r.builtin_video ?? {},
  };
}
