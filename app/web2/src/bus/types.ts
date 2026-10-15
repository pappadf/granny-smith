// Shared TypeScript shapes for the bus layer. Anything that crosses the
// bus seam (UI <-> emulator / OPFS) goes through one of these types.

import type { ConfigDocument } from './profile';

// One image for one storage position of the configuration.
export interface MediaImage {
  bus: string; // a storage bus id of the tree ("scsi", "ata0", "profile")
  unit: number;
  type: string; // "hd" | "cd"
  path: string;
}

export interface MachineConfig {
  /** Model id as accepted by `machine.boot` (e.g. "plus", "se30", "iici"). */
  model: string;
  /** ROM image path (under /opfs/images/rom/).  machine.boot requires it:
   *  the boot inherits nothing. */
  rom?: string;
  /** The configuration document (bus/profile.ts ConfigDocument), sent as
   *  machine.boot's config=.  Omitted: the model's default configuration.
   *  Card ROMs are not configuration: the core resolves each card's ROM
   *  from the files the platform offers, or boots its substitute. */
  config?: ConfigDocument;
  /** Floppy images by drive position id ("fd0", "fd1"). */
  floppies?: Record<string, string>;
  /** Images for the configuration's hard disks and CD-ROM drives, each by
   *  its position (machine.attach_media). */
  media?: MediaImage[];
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
  // A file the core can descend into (an image or archive), from files.list.
  expandable?: boolean;
  // A file's size in bytes, when known.
  size?: number;
  // Modification time in Unix seconds; 0 or absent when unknown.
  mtime?: number;
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
  /** The machine's model id (manifest.json `machine.model`), null if unknown */
  model: string | null;
  /** Its RAM in bytes (manifest.json `machine.ram_bytes`), 0 if unknown */
  ramBytes: number;
  /** Sum of file sizes inside the dir; 0 if unreadable */
  sizeBytes: number;
  /** A checkpoint the user created (self-contained, under saved/), not a
   *  machine's own background checkpoint */
  saved: boolean;
}
