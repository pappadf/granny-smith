import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { attachHardDisk, attachCdrom, insertFloppy, ejectMedia, detectFdDriveCount } =
  await import('@/bus/media');
const { getProfile, clearProfileCache } = await import('@/bus/profile');

// A one-drive machine (the Quadra 700's shape) with its bays.
function q700(): void {
  bridge.reply('machine.id', 'q700');
  bridge.reply('machine.profile', {
    id: 'q700',
    name: 'Macintosh Quadra 700',
    floppy_slots: [{ label: 'Internal FD0', kind: 'hd' }],
    hd_bays: [{ bus: 'scsi', id: 0, label: 'SCSI HD0' }],
    cdrom: { bus: 'scsi', id: 3, label: 'CD-ROM' },
  });
}

describe('bus/media: one attach helper over the core verbs', () => {
  beforeEach(() => {
    bridge.reset();
    clearProfileCache();
  });

  it('attaches a hard disk to a bay by index and answers where it went', async () => {
    bridge.reply('machine.attach_hd', { bus: 'scsi2', id: 4, label: 'Bay 5' });
    const r = await attachHardDisk('/opfs/images/hd/a.img', 3);
    expect(bridge.calls).toEqual([
      { path: 'machine.attach_hd', args: ['/opfs/images/hd/a.img', 3] },
    ]);
    expect(r).toEqual({ ok: true, mount: { kind: 'hd', bus: 'scsi2', drive: 4 } });
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
    expect(bridge.paths()).not.toContain('machine.floppy.drive[1].present');
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
    expect(bridge.paths().filter((p) => p === 'machine.profile')).toHaveLength(1);
    expect(a?.scsi_buses).toEqual([]);
    expect(a?.hd_default).toBeNull();
    expect(a?.cdrom?.id).toBe(3);
  });

  it('does not cache a failed lookup', async () => {
    expect(await getProfile('nope')).toBeNull();
    q700();
    expect((await getProfile('nope'))?.id).toBe('q700');
  });
});
