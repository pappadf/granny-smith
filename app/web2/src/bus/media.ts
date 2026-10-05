// One attach helper for every frontend path.
//
// initEmulator, the URL parameters, the drop auto-mount and the Images panel
// all attach media through here, and every call reports whether it worked.
// The core owns the positions: machine.attach_media names a device of the
// configuration by its bus and unit (the storage tree's ids), and
// machine.attach_hd / attach_cdrom are its shorthand for "the Nth hard disk /
// the CD-ROM drive of the model's default configuration".  Nothing here
// knows a bus.

import { gsEval, gsOk, gsErrorText, isGsError } from './emulator';
import type { MediaBay } from './profile';
import { images, type MountInfo } from '@/state/images.svelte';

// The most floppy drives any machine has.
const MAX_FLOPPY_DRIVES = 4;

// How many floppy drives the running machine has: the drives it was built
// with, which is what its configuration asked for (a position left empty has
// no drive).  machine.floppy.drive[] holds exactly those, so the first index
// that does not resolve is the count.  Cached in the Images state (the badge
// names the drive only when there are several); refresh=true after a
// machine change.
export async function detectFdDriveCount(refresh = false): Promise<number> {
  if (images.fdDriveCount >= 0 && !refresh) return images.fdDriveCount;
  let n = 0;
  while (n < MAX_FLOPPY_DRIVES && !isGsError(await gsEval(`machine.floppy.drive[${n}].present`)))
    n++;
  images.fdDriveCount = n;
  return n;
}

export type MediaResult =
  | { ok: true; mount: MountInfo }
  // `full`: nothing was wrong with the image; every drive is occupied.
  | { ok: false; reason: string; full?: boolean };

function bayResult(kind: 'hd' | 'cd', r: unknown): MediaResult {
  if (!r || typeof r !== 'object' || isGsError(r)) return { ok: false, reason: gsErrorText(r) };
  const bay = r as Partial<MediaBay>;
  return { ok: true, mount: { kind, bus: bay.bus ?? '', drive: bay.id ?? 0 } };
}

// Attach a hard disk to the default configuration's `n`-th hard disk (0 is
// the startup disk), on whatever bus it is.
export async function attachHardDisk(path: string, n = 0): Promise<MediaResult> {
  return bayResult('hd', await gsEval('machine.attach_hd', [path, n]));
}

// An image into the configuration's device at (bus, unit): a hard disk's
// image, or a disc in a CD-ROM drive.
export async function attachMedia(
  bus: string,
  unit: number,
  type: string,
  path: string,
): Promise<MediaResult> {
  const kind = type === 'cd' ? 'cd' : 'hd';
  return bayResult(kind, await gsEval('machine.attach_media', [bus, unit, type, path]));
}

// One storage device of the running machine (machine.storage): a position
// an image attaches to, as its configuration built it.
export interface MachineDevice {
  bus: string;
  busLabel: string;
  unit: number;
  position: string;
  type: 'hd' | 'cd';
  present: boolean;
}

// The running machine's devices that take `type`.
export async function machineDevices(type: 'hd' | 'cd'): Promise<MachineDevice[]> {
  const r = await gsEval('machine.storage');
  if (!Array.isArray(r)) return [];
  return (r as Record<string, unknown>[])
    .filter((d) => d.type === type)
    .map((d) => ({
      bus: String(d.bus ?? ''),
      busLabel: String(d.bus_label ?? d.bus ?? ''),
      unit: Number(d.unit ?? 0),
      position: String(d.position ?? ''),
      type,
      present: d.present === true,
    }));
}

// "Internal SCSI · ID 0 · Internal hard disk bay".
export function deviceLabel(d: MachineDevice): string {
  return d.position ? `${d.busLabel} · ${d.position}` : d.busLabel;
}

// An image into one of the running machine's devices that take `type`:
// `device`, or the first with nothing in it.  A hard disk only ever goes to a
// hard-disk device the machine was built with (drives are construction).
export async function mountImage(
  type: 'hd' | 'cd',
  path: string,
  device?: MachineDevice,
): Promise<MediaResult> {
  if (device) return attachMedia(device.bus, device.unit, device.type, path);
  const devices = await machineDevices(type);
  if (!devices.length)
    return {
      ok: false,
      reason: type === 'hd' ? 'this machine has no hard disk' : 'this machine has no CD-ROM drive',
    };
  const free = devices.find((d) => !d.present);
  if (!free)
    return {
      ok: false,
      full: true,
      reason: type === 'hd' ? 'every hard disk is in use' : 'every CD-ROM drive is full',
    };
  return attachMedia(free.bus, free.unit, free.type, path);
}

// Insert a CD into the default configuration's CD-ROM drive (refused when the
// running machine has none there).
export async function attachCdrom(path: string): Promise<MediaResult> {
  return bayResult('cd', await gsEval('machine.attach_cdrom', [path]));
}

// Insert a floppy into `drive`, or, with no drive, into the first empty one
// of the machine's drives — the physical
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
      : await gsEval('machine.eject_media', [mount.bus ?? '', mount.drive]);
  return gsOk(r) ? { ok: true } : { ok: false, reason: gsErrorText(r) };
}
