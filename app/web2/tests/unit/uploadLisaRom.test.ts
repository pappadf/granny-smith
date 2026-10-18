// A Lisa / Macintosh XL boot ROM uploaded as its two byte-wide chips (#353):
// two chips of one drop or pick are interleaved, in whichever order verifies,
// and stored as one ROM; a chip on its own is refused as the half of a ROM it
// is, never stored as a tiny "hard disk".  Over a fake core filesystem whose
// rom.identify knows one Lisa ROM and answers lisa_chip as the core does.
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
  recordRecentBoot: vi.fn(async () => {}),
}));
vi.mock('@/bus/media', () => ({
  insertFloppy: vi.fn(async () => ({ ok: true, mount: { drive: 0 } })),
  mountImage: vi.fn(async () => ({ ok: true, mount: {} })),
}));

const { acceptFiles, acceptFilesAsCategory } = await import('@/bus/upload');
const { SCRATCH_DIR } = await import('@/lib/opfsPaths');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');

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

// A 16 KB "Lisa ROM": the reset vectors every Lisa ROM opens with, then filler.
const LISA = new Uint8Array(16 * 1024);
LISA.set([0x00, 0x00, 0x04, 0x80, 0x00, 0xfe, 0x00, 0xf6]);
for (let i = 8; i < LISA.length; i++) LISA[i] = (i * 7 + 3) & 0xff;
const HIGH = LISA.filter((_, i) => i % 2 === 0); // the even bytes' chip
const LOW = LISA.filter((_, i) => i % 2 === 1); // the odd bytes' chip

const chip = (name: string, bytes: Uint8Array<ArrayBuffer>) => new File([bytes], name);
const same = (a: Uint8Array, b: Uint8Array) =>
  a.length === b.length && a.every((v, i) => v === b[i]);

let files: Map<string, Uint8Array>;

beforeEach(() => {
  bridge.reset();
  _resetForTests();
  vi.clearAllMocks();
  files = installFakeCoreFs(heap);
  // The core's answers: the ROM interleaved right verifies; each 8 KB chip
  // is told apart by its share of the reset vectors.
  bridge.reply('machine.rom.identify', (args: unknown) => {
    const f = files.get((args as [string])[0]);
    if (!f) return { error: 'cannot read' };
    if (same(f, LISA))
      return {
        recognised: true,
        supported: true,
        intact: true,
        id: '3f7b',
        name: 'Apple Lisa 2 Boot ROM (rev H)',
        compatible: ['lisa'],
        size: f.length,
        kind: 'lisa',
        lisa_chip: '',
      };
    const head = (b: number[]) => b.every((v, i) => f[i] === v);
    const lisaChip =
      f.length !== 8192 ? '' : head([0, 4, 0]) ? 'high' : head([0, 0x80, 0xfe]) ? 'low' : '';
    return { recognised: false, intact: false, size: f.length, lisa_chip: lisaChip };
  });
  bridge.reply('catalog.vroms.identify', { recognised: false });
  bridge.reply('catalog.proms.identify', { recognised: false });
  bridge.reply('machine.floppy.identify', '');
  bridge.reply('machine.scsi.identify_cdrom', false);
  // The permissive hd probe would take an 8 KB chip.
  bridge.reply('machine.scsi.identify_hd', true);
  bridge.reply('machine.boot', true);
});

const scratchLeft = () => [...files.keys()].filter((p) => p.startsWith(`${SCRATCH_DIR}/`));
const stored = () => [...files.keys()].filter((p) => p.startsWith('/opfs/images/')).sort();
const toastText = () => [...toasts.active, ...toasts.queued].map((t) => t.msg);

describe('Lisa ROM chips uploaded together', () => {
  it.each([
    ['high chip first', [chip('341-0175-H.BIN', HIGH), chip('341-0176-H.BIN', LOW)]],
    ['low chip first', [chip('341-0176-H.BIN', LOW), chip('341-0175-H.BIN', HIGH)]],
  ])('are combined into one ROM (%s) and boot it', async (_, pair) => {
    await acceptFiles(pair);
    expect(stored()).toEqual(['/opfs/images/rom/3f7b']);
    expect(same(files.get('/opfs/images/rom/3f7b')!, LISA)).toBe(true);
    expect(scratchLeft()).toEqual([]);
    expect(toastText()).toContain('1 stored (1 ROM)');
    const boots = bridge.calls.filter((c) => c.path === 'machine.boot');
    expect(boots.map((c) => c.args)).toEqual([{ model: 'lisa', rom: '/opfs/images/rom/3f7b' }]);
  });

  it('are combined beside the other files of the drop', async () => {
    const notes = new File([new TextEncoder().encode('notes\n')], 'notes.txt');
    bridge.reply('machine.scsi.identify_hd', false);
    await acceptFiles([chip('lo.bin', LOW), notes, chip('hi.bin', HIGH)]);
    expect(stored()).toEqual(['/opfs/images/rom/3f7b']);
    expect(toastText().some((m) => m.startsWith('1 stored (1 ROM), 1 rejected'))).toBe(true);
    expect(scratchLeft()).toEqual([]);
  });

  it('that do not interleave into a ROM are refused, not stored', async () => {
    const other = LOW.slice();
    other[100] ^= 0xff; // a bad dump of the low chip
    await acceptFiles([chip('hi.bin', HIGH), chip('lo.bin', other)]);
    expect(stored()).toEqual([]);
    expect(toastText().some((m) => /do not interleave into a ROM whose checksum/.test(m))).toBe(
      true,
    );
    expect(scratchLeft()).toEqual([]);
  });

  it('through the ROM category (Images > ROM > Add) are one ROM too', async () => {
    const path = await acceptFilesAsCategory([chip('lo.bin', LOW), chip('hi.bin', HIGH)], 'rom', {
      autoMount: false,
    });
    expect(path).toBe('/opfs/images/rom/3f7b');
    expect(stored()).toEqual(['/opfs/images/rom/3f7b']);
    expect(scratchLeft()).toEqual([]);
  });
});

describe('one Lisa ROM chip on its own', () => {
  it('is refused as half a ROM, not stored as a hard disk', async () => {
    await acceptFiles([chip('341-0175-H.BIN', HIGH)]);
    expect(stored()).toEqual([]);
    expect(toastText()).toContain(
      "'341-0175-H.BIN' is one of the two chips of a Lisa / Macintosh XL boot ROM (the high-byte chip): add it together with the other chip, and the two are combined into the ROM",
    );
    expect(bridge.paths()).not.toContain('machine.boot');
    expect(scratchLeft()).toEqual([]);
  });

  it('is refused through the ROM category too', async () => {
    const path = await acceptFilesAsCategory([chip('lo.bin', LOW)], 'rom');
    expect(path).toBeNull();
    expect(stored()).toEqual([]);
    expect(toastText().some((m) => /low-byte chip/.test(m))).toBe(true);
    expect(scratchLeft()).toEqual([]);
  });

  it('beside other media is refused while they are stored', async () => {
    const hd = new File([new Uint8Array(1024 * 1024).fill(0x11)], 'tiny.img');
    await acceptFiles([chip('hi.bin', HIGH), hd]);
    expect(stored()).toEqual(['/opfs/images/hd/tiny.img']);
    expect(toastText().some((m) => /1 rejected: 'hi\.bin' is one of the two chips/.test(m))).toBe(
      true,
    );
  });
});
