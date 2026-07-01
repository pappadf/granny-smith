// Shared TypeScript shapes for the bus layer. Anything that crosses the
// bus seam (UI <-> emulator / OPFS) goes through one of these types.

export interface MachineConfig {
  /** Model id as accepted by `machine.boot` (e.g. "plus", "se30", "iici"). */
  model: string;
  /** Optional human-readable model name from `machine.profile(id).name`,
   *  used for status-bar display when set. Falls back to `model` if absent. */
  modelName?: string;
  /** ROM image path (under /opfs/images/rom/). Empty / unset = use the
   *  C-side default for the model (machine.boot may succeed without one
   *  during early development). */
  rom?: string;
  vrom: string;
  /** NuBus video card-kind id to install (e.g. "display_card_24ac", "mdc_8_24"),
   *  applied via `machine.nubus.video_card` before `machine.boot`. Unset = use the
   *  slot's default card. The dialog derives this from the chosen card (whose vROM
   *  it auto-resolves), so the right card boots instead of the slot default. */
  videoCard?: string;
  /** JMFB video-mode id (e.g. "13in_rgb_1bpp") seeded via `machine.nubus.video_mode`
   *  before `machine.boot`, matching web-legacy's bootFromConfig.  Without it
   *  the JMFB card never seeds its slot-PRAM / video defaults and A/UX hangs
   *  while enabling its device drivers on real hardware.  Unset on models with
   *  no configurable video (Plus / SE/30). */
  videoMode?: string;
  ram: string;
  /** Ordered list of floppy image paths, one per drive slot. Entries that
   *  are empty / '(none)' are skipped (no insertion into that slot). */
  floppies: string[];
  hd: string;
  /** How to attach `hd`: 'scsi' (default — scsi.attach_hd) or 'profile' (the
   *  Lisa/XL parallel-port ProFile — profile.attach). Sourced from the model's
   *  `machine.profile(id).hd_bus`. */
  hdBus?: 'scsi' | 'profile';
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

// One checkpoint as surfaced by opfs.scanCheckpoints(). The dir name is
// `<machine_id>-<created>` per docs/checkpointing.md; `label` and `machine`
// come from the dir's manifest.json when present (best-effort).
export interface CheckpointEntry {
  /** OPFS path of the checkpoint directory (e.g. /opfs/checkpoints/abc…-…) */
  path: string;
  /** Directory basename — `<id>-<created>` */
  dirName: string;
  /** 16 hex chars from the dir name */
  id: string;
  /** Compact ISO 8601 from the dir name */
  created: string;
  /** Human label (manifest.json `label`, else the formatted timestamp) */
  label: string;
  /** Machine model (manifest.json `machine`, else "unknown") */
  machine: string;
  /** Sum of file sizes inside the dir; 0 if unreadable */
  sizeBytes: number;
}
