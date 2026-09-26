// Shared TypeScript shapes for the bus layer. Anything that crosses the
// bus seam (UI <-> emulator / OPFS) goes through one of these types.

export interface MachineConfig {
  /** Model id as accepted by `machine.boot` (e.g. "plus", "se30", "iici"). */
  model: string;
  /** ROM image path (under /opfs/images/rom/). Becomes the boot document's
   *  `rom` field, which machine.boot requires — the document is the whole
   *  specification and inherits nothing (proposal-boot-vs-reset §3.1). */
  rom?: string;
  vrom: string;
  /** NuBus video card-kind id to install (e.g. "display_card_24ac", "mdc_8_24") —
   *  the boot document's `video_card` field. Unset = use the slot's default
   *  card. The dialog derives this from the chosen card (whose vROM it
   *  auto-resolves), so the right card boots instead of the slot default. */
  videoCard?: string;
  // A display-class PCI card and the expansion ROM that drives it.
  // Separate from videoCard because they are different buses and the
  // boot document has a field for each.
  pciCard?: string;
  prom?: string;
  // "key=value[,key=value]" for the staged PCI card (e.g. "vram=4m").
  pciOption?: string;
  /** JMFB video-mode id (e.g. "13in_rgb_1bpp") — the boot document's
   *  `video_mode` field; the card factory consumes it during boot (sense
   *  lines + slot-PRAM/video defaults).  Without it the JMFB card never
   *  seeds its slot-PRAM / video defaults and A/UX hangs while enabling
   *  its device drivers on real hardware.  Unset on models with no
   *  configurable video (Plus / SE/30). */
  videoMode?: string;
  /** Which port the monitor is plugged into on a machine that has BOTH
   *  built-in video and NuBus slots (the PDM family).  'none' leaves the
   *  built-in port unconnected, which makes the ROM turn built-in video off
   *  and hands the screen to the NuBus card.  Unset = the machine's own
   *  default monitor. */
  monitor?: string;
  /** RAM in KB — one of the profile's ram_options.  Omitted: the core
   *  boots the model's ram_default (there is no frontend fallback). */
  ramKb?: number;
  /** Ordered list of floppy image paths, one per drive slot. Entries that
   *  are empty / '(none)' are skipped (no insertion into that slot). */
  floppies: string[];
  hd: string;
  /** Which hard-disk bay `hd` goes into: an index into the model's
   *  profile.hd_bays (0, the default, is the boot bay).  The core knows
   *  which bus each bay is on — SCSI, the Network Servers' second channel,
   *  the Lisa's ProFile — so nothing here does. */
  hdBay?: number;
  /** CD image, into the model's CD bay; only sent for a model that has one. */
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

export type ImageCategory = 'rom' | 'vrom' | 'prom' | 'fd' | 'hd' | 'cd';

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
