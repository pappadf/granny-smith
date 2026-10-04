// A drop of several files: each runs the single-file flow on its own (stage,
// probe, store or reject, discard) over a fake core filesystem, and the drop
// ends with one truthful summary.  A checkpoint among them is refused; a ROM
// boots only when it is the drop's only ROM; only the first file of a
// category is mounted.
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';
import { fakeHeap, installFakeCoreFs } from '../helpers/fakeCoreFs';

const heap = fakeHeap();

vi.mock('@/bus/emulator', async () => ({
  ...(await (await import('../helpers/bridgeMock')).emulatorModule()),
  getModuleHeap: () => heap,
}));
vi.mock('@/bus/boot', () => ({
  reconcileUiWithMachine: vi.fn(async () => {}),
  prepareFreshMachine: vi.fn(async () => {}),
}));
vi.mock('@/bus/media', () => ({
  insertFloppy: vi.fn(async () => ({ ok: true, mount: { drive: 0 } })),
  attachCdrom: vi.fn(async () => ({ ok: true, mount: {} })),
}));

const { acceptFiles, dropSummary } = await import('@/bus/upload');
const { SCRATCH_DIR } = await import('@/lib/opfsPaths');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');
const media = await import('@/bus/media');

// jsdom's Blob has no arrayBuffer(); the browser's does.
if (!Blob.prototype.arrayBuffer) {
  Blob.prototype.arrayBuffer = function (this: Blob) {
    return new Promise<ArrayBuffer>((resolve) => {
      const r = new FileReader();
      r.onload = () => resolve(r.result as ArrayBuffer);
      r.readAsArrayBuffer(this);
    });
  };
}

// A ROM the fake core recognises (first byte 'R'), with id `id`.
const rom = (name: string, id: number) => {
  const b = new Uint8Array(64 * 1024).fill(0x52);
  b[1] = id;
  return new File([b], name);
};
// An 800 KB floppy image.
const floppy = (name: string) => new File([new Uint8Array(819200).fill(0xa5)], name);
const notes = () => new File([new TextEncoder().encode('just some notes\n')], 'notes.txt');
const checkpoint = () =>
  new File([new TextEncoder().encode('GSCHKPT3'), new Uint8Array(100)], 'state.checkpoint');

let files: Map<string, Uint8Array>;

beforeEach(() => {
  bridge.reset();
  _resetForTests();
  vi.clearAllMocks();
  files = installFakeCoreFs(heap);
  bridge.reply('machine.rom.identify', (args: unknown) => {
    const f = files.get((args as [string])[0]);
    if (!f || f[0] !== 0x52) return { recognised: false };
    return {
      recognised: true,
      supported: true,
      intact: true,
      id: `ROM${f[1]}`,
      name: 'Macintosh Plus',
      compatible: ['plus'],
      size: f.length,
    };
  });
  bridge.reply('catalog.vroms.identify', { recognised: false });
  bridge.reply('catalog.proms.identify', { recognised: false });
  bridge.reply('machine.scsi.identify_cdrom', false);
  // Too small to be a disk.
  bridge.reply('machine.scsi.identify_hd', false);
  bridge.reply('machine.boot', true);
  bridge.reply('checkpoint.load', true);
  bridge.reply('catalog.vroms.offer', true);
});

const scratchLeft = () => [...files.keys()].filter((p) => p.startsWith(`${SCRATCH_DIR}/`));
const toastText = () => [...toasts.active, ...toasts.queued].map((t) => t.msg);

describe('a drop of several files', () => {
  it('stores each valid file, rejects the other with its reason, and leaves no scratch file', async () => {
    await acceptFiles([rom('plus.rom', 1), floppy('a.dsk'), floppy('b.dsk'), notes()]);
    expect([...files.keys()].sort()).toEqual([
      '/opfs/images/fd/a.dsk',
      '/opfs/images/fd/b.dsk',
      '/opfs/images/rom/ROM1',
    ]);
    expect(scratchLeft()).toEqual([]);
    expect(toastText()).toContain(
      "3 stored (1 ROM, 2 floppies), 1 rejected: 'notes.txt' doesn't look like a ROM, floppy, HD, CD, or archive",
    );
    // No per-file "uploaded" messages: the summary is the account.
    expect(toastText().some((m) => / uploaded$/.test(m))).toBe(false);
  });

  it("boots the drop's only ROM and mounts only the first floppy, into an empty drive", async () => {
    await acceptFiles([floppy('a.dsk'), rom('plus.rom', 1), floppy('b.dsk')]);
    const boots = bridge.calls.filter((c) => c.path === 'machine.boot');
    expect(boots.map((c) => c.args)).toEqual([{ model: 'plus', rom: '/opfs/images/rom/ROM1' }]);
    expect(media.insertFloppy).toHaveBeenCalledTimes(1);
    expect(media.insertFloppy).toHaveBeenCalledWith('/opfs/images/fd/a.dsk', true);
  });

  it('boots nothing when the drop holds two ROMs', async () => {
    await acceptFiles([rom('one.rom', 1), rom('two.rom', 2)]);
    expect(bridge.paths()).not.toContain('machine.boot');
    expect([...files.keys()].filter((p) => p.startsWith('/opfs/images/rom/')).sort()).toEqual([
      '/opfs/images/rom/ROM1',
      '/opfs/images/rom/ROM2',
    ]);
  });

  it('refuses a checkpoint among other files, and still stores the rest', async () => {
    await acceptFiles([checkpoint(), floppy('a.dsk')]);
    expect(bridge.paths()).not.toContain('checkpoint.load');
    expect(files.has('/opfs/images/fd/a.dsk')).toBe(true);
    expect(toastText()).toContain(
      "1 stored (1 floppy), 1 rejected: 'state.checkpoint' is a checkpoint: drop a checkpoint on its own",
    );
    expect(scratchLeft()).toEqual([]);
  });
});

describe('a single file', () => {
  it('keeps its own messages and auto-actions', async () => {
    await acceptFiles([floppy('a.dsk')]);
    expect(toastText()).toContain('a.dsk uploaded');
    expect(media.insertFloppy).toHaveBeenCalledWith('/opfs/images/fd/a.dsk', true);
    expect(scratchLeft()).toEqual([]);
  });

  it('says why it was not stored', async () => {
    await acceptFiles([notes()]);
    expect(toastText()).toContain(
      "'notes.txt' doesn't look like a ROM, floppy, HD, CD, or archive",
    );
    expect(scratchLeft()).toEqual([]);
  });

  it('a checkpoint dropped alone is loaded', async () => {
    await acceptFiles([checkpoint()]);
    expect(bridge.paths()).toContain('checkpoint.load');
    expect(scratchLeft()).toEqual([]);
  });
});

describe('dropSummary', () => {
  it('names nothing stored and every rejection', () => {
    expect(
      dropSummary(
        ['x', 'y'],
        [
          { stored: false, reason: 'is not a valid ROM image', severity: 'error' },
          { stored: false, reason: 'could not be uploaded', severity: 'error' },
        ],
      ),
    ).toEqual({
      msg: "0 stored, 2 rejected: 'x' is not a valid ROM image; 'y' could not be uploaded",
      severity: 'error',
    });
  });

  it('is plain information when everything was stored', () => {
    expect(
      dropSummary(
        ['a', 'b'],
        [
          { stored: true, category: 'hd', path: '/opfs/images/hd/a.dmg' },
          { stored: true, category: 'cdrom', path: '/opfs/images/cd/b.dmg' },
        ],
      ),
    ).toEqual({ msg: '2 stored (1 hard disk, 1 CD-ROM)', severity: 'info' });
  });
});
