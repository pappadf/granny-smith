// Per-category collapsed state for the Images panel view, plus the mount-state
// mirror that drives each row's badge. The view's own insert/eject actions set
// it; a guest-initiated floppy eject clears it via C's Module.onFloppyChange
// event (onFloppyDriveChange below), so no polling is needed.

import type { ImageCategory } from '@/bus/types';

// What a mounted image is sitting in: its media category, the bus it is on
// ('floppy', 'scsi', 'scsi2', 'profile' — as machine.attach_hd answers it)
// and the unit on that bus (floppy drive index, or SCSI id).  bus/media.ts
// ejects it from exactly there.
export interface MountInfo {
  kind: ImageCategory;
  bus?: string;
  drive: number;
}

interface ImagesState {
  collapsed: Record<ImageCategory, boolean>;
  /** OPFS path → where it is mounted. Set by the Images-section insert/eject
   *  (and the upload auto-insert); onFloppyDriveChange() removes floppy entries
   *  the guest has ejected on its own. */
  mounted: Record<string, MountInfo>;
  /** Number of floppy drives the active machine exposes (-1 = not yet probed).
   *  Decides whether a floppy badge names its drive. */
  fdDriveCount: number;
  /** Bump whenever the OPFS image catalog changes (file added / renamed
   *  / deleted under /opfs/images/). Components that cache an inventory
   *  (e.g. WelcomeConfigSlide's ROM / VROM / FD / HD / CD dropdowns)
   *  watch this counter via $effect and re-scan when it changes. */
  revision: number;
}

export const images: ImagesState = $state({
  collapsed: { rom: false, vrom: true, prom: true, fd: false, hd: true, cd: true },
  mounted: {},
  fdDriveCount: -1,
  revision: 0,
});

export function toggleCategory(cat: ImageCategory): void {
  images.collapsed[cat] = !images.collapsed[cat];
}

// Record (info) or clear (null) where an image is mounted.
export function setMounted(path: string, info: MountInfo | null): void {
  if (info) images.mounted[path] = info;
  else delete images.mounted[path];
}

export function isMounted(path: string): boolean {
  return !!images.mounted[path];
}

// Badge text for a mounted image, or null when not mounted. Floppies and CDs
// are removable media → "Inserted"; hard disks → "Mounted". On a machine with
// more than one floppy drive the floppy badge names the drive it went into.
export function mountBadge(path: string): string | null {
  const info = images.mounted[path];
  if (!info) return null;
  if (info.kind === 'hd') return 'Mounted';
  if (info.kind === 'fd' && images.fdDriveCount > 1) return `Inserted · Drive ${info.drive + 1}`;
  return 'Inserted';
}

// Handle a floppy drive present-state change pushed from C (Module.onFloppyChange,
// installed in bus/emulator.ts). When a drive goes empty, drop the "Inserted"
// badge of whatever we had recorded there — so a guest-initiated eject (the
// MacWorks loader eject, or an eject from inside the OS) clears it with no poll.
export function onFloppyDriveChange(drive: number, present: boolean): void {
  if (present) return; // insertions set their own badge via the mount action
  for (const [path, info] of Object.entries(images.mounted)) {
    if (info.kind === 'fd' && info.drive === drive) delete images.mounted[path];
  }
}

// Call after any successful upload / rename / delete touching an image
// under /opfs/images/. Triggers re-scans in inventory consumers.
export function bumpImagesRevision(): void {
  images.revision++;
}
