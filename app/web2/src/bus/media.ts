// One attach helper for every frontend path (M5).
//
// initEmulator, the URL parameters, the drop auto-mount and the Images panel
// each attached media for themselves: a CD at SCSI id 3 on every model (id 0
// is the Network Server's CD bay, and a CD-less model got one anyway), `?hdN=`
// at SCSI id N, the Images panel's disk at the default id on the first bus
// whatever the bay's bus, an Unmount through methods that do not exist, and
// most results unchecked.  The core now derives the bays
// (machine.attach_hd / attach_cdrom / eject_media, profile.hd_bays); this
// module is the frontend's one way to use them, and every call reports
// whether it worked.

import { gsEval, gsOk, gsErrorText, isGsError } from './emulator';
import { getActiveProfile, type MediaBay } from './profile';
import { images, type MountInfo } from '@/state/images.svelte';

// How many floppy drives the active machine has: its profile's floppy_slots.
// (This used to probe drive[0..3] until one did not resolve, and the core
// exposed a phantom drive[1] on one-drive Macs, so it counted two, N-06.)
// Cached in the Images state (the badge names the drive only when there are
// several); refresh=true after a machine change.
export async function detectFdDriveCount(refresh = false): Promise<number> {
  if (images.fdDriveCount >= 0 && !refresh) return images.fdDriveCount;
  const profile = await getActiveProfile();
  images.fdDriveCount = profile?.floppy_slots.length ?? 0;
  return images.fdDriveCount;
}

export type MediaResult =
  | { ok: true; mount: MountInfo }
  // `full`: nothing was wrong with the image; every drive is occupied.
  | { ok: false; reason: string; full?: boolean };

function bayResult(kind: 'hd' | 'cd', r: unknown): MediaResult {
  if (!r || typeof r !== 'object' || isGsError(r)) return { ok: false, reason: gsErrorText(r) };
  const bay = r as Partial<MediaBay>;
  return { ok: true, mount: { kind, bus: bay.bus ?? 'scsi', drive: bay.id ?? 0 } };
}

// Attach a hard disk to the running machine's `bay`-th hard-disk bay
// (profile.hd_bays order; 0 is the boot bay), on whatever bus it is.
export async function attachHardDisk(path: string, bay = 0): Promise<MediaResult> {
  return bayResult('hd', await gsEval('machine.attach_hd', [path, bay]));
}

// Insert a CD into the running machine's CD bay (refused on a model without).
export async function attachCdrom(path: string): Promise<MediaResult> {
  return bayResult('cd', await gsEval('machine.attach_cdrom', [path]));
}

// Insert a floppy into `drive`, or, with no drive, into the first empty one
// of the machine's drives (its profile's floppy_slots) — the physical
// metaphor of dropping a disk into a Mac.
export async function insertFloppy(
  path: string,
  writable: boolean,
  drive?: number,
): Promise<MediaResult> {
  const insert = async (d: number): Promise<MediaResult> => {
    const r = await gsEval(`machine.floppy.drive[${d}].insert`, [path, writable]);
    return gsOk(r)
      ? { ok: true, mount: { kind: 'fd', bus: 'floppy', drive: d } }
      : { ok: false, reason: gsErrorText(r) };
  };
  if (drive !== undefined) return insert(drive);
  const count = await detectFdDriveCount(true);
  for (let d = 0; d < count; d++) {
    if ((await gsEval(`machine.floppy.drive[${d}].present`)) === true) continue;
    // An empty drive that refuses means the image could not be opened, not
    // that the drives are full; the next drive would refuse it too.
    return insert(d);
  }
  return {
    ok: false,
    full: count > 0,
    reason: count > 0 ? 'every floppy drive is full' : 'this machine has no floppy drive',
  };
}

// Take a medium back out, wherever attach put it.
export async function ejectMedia(mount: MountInfo): Promise<{ ok: boolean; reason?: string }> {
  const r =
    mount.kind === 'fd'
      ? await gsEval(`machine.floppy.drive[${mount.drive}].eject`)
      : await gsEval('machine.eject_media', [mount.bus ?? 'scsi', mount.drive]);
  return gsOk(r) ? { ok: true } : { ok: false, reason: gsErrorText(r) };
}
