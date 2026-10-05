import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const {
  attachHardDisk,
  attachMedia,
  attachCdrom,
  insertFloppy,
  ejectMedia,
  detectFdDriveCount,
  mountImage,
  deviceLabel,
} = await import('@/bus/media');
const { getProfile, clearProfileCache } = await import('@/bus/profile');

// A running one-drive machine: machine.floppy.drive[] holds the drives it
// was built with, so drive[1] does not resolve.
function q700(): void {
  bridge.reply('machine.id', 'q700');
  bridge.reply('machine.floppy.drive[1].present', { error: "path 'drive[1]' did not resolve" });
  bridge.reply('catalog.profile', {
    id: 'q700',
    name: 'Macintosh Quadra 700',
    floppies: [{ id: 'fd0', label: 'Internal floppy drive', types: [], default: 'hd' }],
    storage: [{ id: 'scsi', label: 'SCSI', kind: 'scsi', units: [{ unit: 0, label: 'ID 0' }] }],
  });
}

describe('bus/media: one attach helper over the core verbs', () => {
  beforeEach(() => {
    bridge.reset();
    clearProfileCache();
  });

  it('attaches an image to a device by its position', async () => {
    bridge.reply('machine.attach_media', { bus: 'ata0', id: 1, label: 'Slave' });
    const r = await attachMedia('ata0', 1, 'hd', '/opfs/images/hd/a.img');
    expect(bridge.calls).toEqual([
      { path: 'machine.attach_media', args: ['ata0', 1, 'hd', '/opfs/images/hd/a.img'] },
    ]);
    expect(r).toEqual({ ok: true, mount: { kind: 'hd', bus: 'ata0', drive: 1 } });
  });

  it("attaches a hard disk to the default configuration's Nth disk and answers where it went", async () => {
    bridge.reply('machine.attach_hd', { bus: 'scsi2', id: 4, label: 'Bay 5' });
    const r = await attachHardDisk('/opfs/images/hd/a.img', 3);
    expect(bridge.calls).toEqual([
      { path: 'machine.attach_hd', args: ['/opfs/images/hd/a.img', 3] },
    ]);
    expect(r).toEqual({ ok: true, mount: { kind: 'hd', bus: 'scsi2', drive: 4 } });
  });

  it("mounts into the running machine's first empty device that takes the image", async () => {
    bridge.reply('machine.storage', [
      {
        bus: 'scsi',
        bus_label: 'SCSI',
        unit: 0,
        position: 'ID 0 · Bay',
        type: 'hd',
        present: true,
      },
      { bus: 'scsi', bus_label: 'SCSI', unit: 3, position: 'ID 3', type: 'cd', present: false },
      {
        bus: 'scsi2',
        bus_label: 'External SCSI',
        unit: 5,
        position: 'ID 5',
        type: 'hd',
        present: false,
      },
    ]);
    bridge.reply('machine.attach_media', { bus: 'scsi2', id: 5, label: 'External SCSI' });
    const r = await mountImage('hd', '/opfs/images/hd/b.img');
    expect(bridge.calls.at(-1)).toEqual({
      path: 'machine.attach_media',
      args: ['scsi2', 5, 'hd', '/opfs/images/hd/b.img'],
    });
    expect(r.ok).toBe(true);
  });

  it('says every device is in use rather than trying an occupied one', async () => {
    bridge.reply('machine.storage', [
      { bus: 'scsi', bus_label: 'SCSI', unit: 0, position: 'ID 0', type: 'hd', present: true },
    ]);
    const r = await mountImage('hd', '/opfs/images/hd/b.img');
    expect(r).toEqual({ ok: false, full: true, reason: 'every hard disk is in use' });
    expect(bridge.calls.some((c) => c.path === 'machine.attach_media')).toBe(false);
  });

  it('labels a device by its bus and position', () => {
    expect(
      deviceLabel({
        bus: 'scsi',
        busLabel: 'Internal SCSI',
        unit: 0,
        position: 'ID 0 · Internal hard disk bay',
        type: 'hd',
        present: false,
      }),
    ).toBe('Internal SCSI · ID 0 · Internal hard disk bay');
  });

  it("reports the core's refusal instead of a success", async () => {
    bridge.reply('machine.attach_cdrom', {
      error: 'machine.attach_cdrom: Macintosh Plus has no CD-ROM bay',
    });
    const r = await attachCdrom('/opfs/images/cd/x.iso');
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toContain('no CD-ROM bay');
    // The CD goes to the model's bay, never a hardcoded id 3.
    expect(bridge.calls[0]).toEqual({
      path: 'machine.attach_cdrom',
      args: ['/opfs/images/cd/x.iso'],
    });
  });

  it('inserts a floppy into the first empty drive the machine has, and no phantom one', async () => {
    q700();
    bridge.reply('machine.floppy.drive[0].present', true);
    const r = await insertFloppy('/opfs/images/fd/a.dsk', true);
    expect(r).toEqual({ ok: false, full: true, reason: 'every floppy drive is full' });
    expect(bridge.paths()).not.toContain('machine.floppy.drive[1].insert');
    expect(await detectFdDriveCount()).toBe(1);
  });

  it('an empty drive that refuses means the image could not be opened', async () => {
    q700();
    bridge.reply('machine.floppy.drive[0].present', false);
    bridge.reply('machine.floppy.drive[0].insert', false);
    const r = await insertFloppy('/opfs/images/fd/bad.dsk', true);
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.full).toBeUndefined();
  });

  it('ejects from where the mount put it', async () => {
    bridge.reply('machine.eject_media', null);
    bridge.reply('machine.floppy.drive[0].eject', true);
    expect(await ejectMedia({ kind: 'hd', bus: 'scsi2', drive: 4 })).toEqual({ ok: true });
    expect(await ejectMedia({ kind: 'fd', bus: 'floppy', drive: 0 })).toEqual({ ok: true });
    expect(bridge.calls).toEqual([
      { path: 'machine.eject_media', args: ['scsi2', 4] },
      { path: 'machine.floppy.drive[0].eject', args: undefined },
    ]);
  });
});

describe('bus/profile: one reader, memoised', () => {
  beforeEach(() => {
    bridge.reset();
    clearProfileCache();
  });

  it('asks the core once per model and fills what a consumer iterates', async () => {
    q700();
    const a = await getProfile('q700');
    const b = await getProfile('q700');
    expect(a).toBe(b);
    expect(bridge.paths().filter((p) => p === 'catalog.profile')).toHaveLength(1);
    // What the core left out, a consumer can still iterate.
    expect(a?.slots).toEqual([]);
    expect(a?.cards).toEqual([]);
    expect(a?.storage[0].reserved).toEqual([]);
    expect(a?.storage[0].accepts).toEqual([]);
    expect(a?.displays).toEqual({ builtin: null, max_connected: 1 });
    expect(a?.defaults.storage).toEqual([]);
    expect(a?.defaults.startup).toBeNull();
  });

  it('does not cache a failed lookup', async () => {
    expect(await getProfile('nope')).toBeNull();
    q700();
    expect((await getProfile('nope'))?.id).toBe('q700');
  });
});
