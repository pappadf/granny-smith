// A drop of several files: each runs the single-file flow on its own (stage,
// probe, store or reject, discard) over a fake core filesystem, and the drop
// ends with one truthful summary.  A checkpoint among them is refused; a ROM
// boots only when it is the drop's only ROM; only the first file of a
// category is mounted.
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';
import { fakeHeap, installFakeCoreFs } from '../helpers/fakeCoreFs';
import { gzipSync } from 'node:zlib';

const heap = fakeHeap();

vi.mock('@/bus/emulator', async () => ({
  ...(await (await import('../helpers/bridgeMock')).emulatorModule()),
  getModuleHeap: () => heap,
}));
vi.mock('@/bus/boot', () => ({
  reconcileUiWithMachine: vi.fn(async () => {}),
  prepareFreshMachine: vi.fn(async () => {}),
  recordRecentBoot: vi.fn(async () => {}),
}));
// The terminal: a drop's rejections are printed there too (S1018-66).
const printed = vi.hoisted(() => [] as string[]);
vi.mock('@/bus/logSink', () => ({
  routePrintLine: (line: string) => printed.push(line),
}));
vi.mock('@/bus/media', () => ({
  insertFloppy: vi.fn(async () => ({ ok: true, mount: { drive: 0 } })),
  mountImage: vi.fn(async () => ({ ok: true, mount: {} })),
}));

const { acceptFiles, acceptFilesRaw, acceptFilesIntoCategory, dropSummary } =
  await import('@/bus/upload');
const { SCRATCH_DIR } = await import('@/lib/opfsPaths');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');
const media = await import('@/bus/media');
const boot = await import('@/bus/boot');
const { machine } = await import('@/state/machine.svelte');

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
// Nor stream().
if (!Blob.prototype.stream) {
  Blob.prototype.stream = function (this: Blob) {
    const read = () => this.arrayBuffer();
    return new ReadableStream<Uint8Array>({
      async start(c) {
        c.enqueue(new Uint8Array(await read()));
        c.close();
      },
    });
  } as typeof Blob.prototype.stream;
}

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
    // No per-file "added" messages: the summary is the account.
    expect(toastText().some((m) => / added$/.test(m))).toBe(false);
    // The toast is one line; each rejection is in the terminal in full.
    expect(printed).toContain(
      "upload: 'notes.txt' doesn't look like a ROM, floppy, HD, CD, or archive",
    );
  });

  it("boots the drop's only ROM and mounts only the first floppy, into an empty drive", async () => {
    await acceptFiles([floppy('a.dsk'), rom('plus.rom', 1), floppy('b.dsk')]);
    const boots = bridge.calls.filter((c) => c.path === 'machine.boot');
    expect(boots.map((c) => c.args)).toEqual([{ model: 'plus', rom: '/opfs/images/rom/ROM1' }]);
    // A dropped ROM's boot goes on the Welcome page's Recent list.
    expect(boot.recordRecentBoot).toHaveBeenCalledWith({
      model: 'plus',
      rom: '/opfs/images/rom/ROM1',
    });
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
      "1 stored (1 floppy), 1 rejected: 'state.checkpoint' is a checkpoint: load a checkpoint on its own",
    );
    expect(scratchLeft()).toEqual([]);
  });
});

describe('a single file', () => {
  it('keeps its own messages and auto-actions', async () => {
    machine.status = 'running';
    try {
      await acceptFiles([floppy('a.dsk')]);
    } finally {
      machine.status = 'no-machine';
    }
    expect(toastText()).toContain('a.dsk added');
    expect(media.insertFloppy).toHaveBeenCalledWith('/opfs/images/fd/a.dsk', true);
    expect(scratchLeft()).toEqual([]);
  });

  it('after Shut down a floppy is stored, not inserted into the stopped machine', async () => {
    machine.status = 'stopped';
    try {
      await acceptFiles([floppy('a.dsk')]);
    } finally {
      machine.status = 'no-machine';
    }
    expect(toastText()).toContain('a.dsk added');
    expect(media.insertFloppy).not.toHaveBeenCalled();
    expect(toastText().some((m) => /Inserted/.test(m))).toBe(false);
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
          { stored: false, reason: 'could not be loaded', severity: 'error' },
        ],
      ),
    ).toEqual({
      msg: "0 stored, 2 rejected: 'x' is not a valid ROM image; 'y' could not be loaded",
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

// S1018-25: a Filesystem-tab drop keeps the file's name; only what OPFS
// cannot hold is replaced, and then the toast says what it was saved as.
describe('a drop into the Filesystem tab', () => {
  it('keeps spaces and accents, and names a changed name', async () => {
    const text = (name: string) => new File([new TextEncoder().encode('hi')], name);
    await acceptFilesRaw([text('Read Me café.txt'), text('a:b.txt')], '/opfs/shared');
    expect(files.has('/opfs/shared/Read Me café.txt')).toBe(true);
    expect(files.has('/opfs/shared/a_b.txt')).toBe(true);
    expect(toastText()).toContain("'Read Me café.txt' saved to /opfs/shared");
    expect(toastText()).toContain("'a:b.txt' saved to /opfs/shared as 'a_b.txt'");
  });
});

// S1018-73 (IM-09): a drop of several files on one Images section stored the
// first and ignored the rest.
describe('a drop of several files on an Images section', () => {
  it('stores every file, and inserts only the first', async () => {
    machine.status = 'running'; // with no machine the files are only stored
    expect(await acceptFilesIntoCategory([floppy('a.dsk'), floppy('b.dsk')], 'fd')).toBe(true);
    expect([...files.keys()].filter((p) => p.startsWith('/opfs/images/')).sort()).toEqual([
      '/opfs/images/fd/a.dsk',
      '/opfs/images/fd/b.dsk',
    ]);
    expect(media.insertFloppy).toHaveBeenCalledTimes(1);
    expect(media.insertFloppy).toHaveBeenCalledWith('/opfs/images/fd/a.dsk', true);
  });
});

// S1018-31: a gzipped floppy was staged as it is and found to hold "no
// mountable media"; it is unpacked, and what it holds stored.
describe('a gzipped floppy', () => {
  it('is unpacked and stored as the floppy it holds', async () => {
    const gz = new File([gzipSync(new Uint8Array(819200).fill(0xa5))], 'sys608.dsk.gz');
    await acceptFiles([gz]);
    expect([...files.keys()].filter((p) => p.startsWith('/opfs/images/'))).toEqual([
      '/opfs/images/fd/sys608.dsk',
    ]);
    expect(scratchLeft()).toEqual([]);
  });
});

// S1018-28: adding a file stored already copied it over itself, which failed
// ("could not be stored") once a drive had held it.  It is left as it is; a
// different file of that name is stored beside it.
describe('adding a file of a name stored already', () => {
  it('leaves the same file as it is, and stores a different one beside it', async () => {
    await acceptFilesIntoCategory([floppy('sys.dsk')], 'fd');
    bridge.calls.length = 0;
    await acceptFilesIntoCategory([floppy('sys.dsk')], 'fd');
    const writes = bridge.calls.filter(
      (c) =>
        (c.path === 'files.cp' || c.path === 'files.mv') &&
        (c.args as string[])[1] === '/opfs/images/fd/sys.dsk',
    );
    expect(writes).toEqual([]);
    expect(toastText().some((m) => /Failed|could not/.test(m))).toBe(false);
    expect(scratchLeft()).toEqual([]);

    const other = new File([new Uint8Array(819200).fill(0x5a)], 'sys.dsk');
    await acceptFilesIntoCategory([other], 'fd');
    expect([...files.keys()].filter((p) => p.startsWith('/opfs/images/')).sort()).toEqual([
      '/opfs/images/fd/sys 2.dsk',
      '/opfs/images/fd/sys.dsk',
    ]);
  });
});

// S1018-06: a dropped HFS or partitioned hard disk was filed as a CD-ROM, as
// the CD probe took any HFS/APM signature.  A drop files an image as a CD only
// when it holds ISO 9660; the CD-ROM section's own Add still takes the disk.
describe('a dropped disk image', () => {
  // A 2 MB image the fake core finds a disk (HFS: 'H', ISO 9660: 'I').
  const disk = (name: string, kind: 'H' | 'I') =>
    new File([new Uint8Array(2 * 1024 * 1024).fill(kind.charCodeAt(0))], name);
  const kindOf = (args: unknown) => files.get((args as [string])[0])?.[0];
  beforeEach(() => {
    bridge.reply('machine.scsi.identify_hd', (a: unknown) => kindOf(a) !== undefined);
    bridge.reply('machine.scsi.identify_cdrom', (a: unknown) => kindOf(a) !== undefined);
    bridge.reply('machine.scsi.identify_iso', (a: unknown) => kindOf(a) === 0x49);
  });
  const stored = () => [...files.keys()].filter((p) => p.startsWith('/opfs/images/')).sort();

  it('is a hard disk when it holds no ISO 9660', async () => {
    await acceptFiles([disk('hfs.img', 'H')]);
    expect(stored()).toEqual(['/opfs/images/hd/hfs.img']);
  });

  it('is a CD-ROM when it holds ISO 9660', async () => {
    await acceptFiles([disk('disc.iso', 'I')]);
    expect(stored()).toEqual(['/opfs/images/cd/disc.iso']);
  });

  it('is still a disc when added to the CD-ROM section', async () => {
    await acceptFilesIntoCategory([disk('hfs.img', 'H')], 'cdrom');
    expect(stored()).toEqual(['/opfs/images/cd/hfs.img']);
  });
});
