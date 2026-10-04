// URL media through the real pipeline (fetch -> scratch -> validate -> store
// -> boot), over a fake core filesystem: a download that does not validate as
// its slot's category is rejected with the validator's reason and never
// attached, every exit leaves the scratch area empty, each download writes a
// scratch file of its own, and the URL's vROM goes in the boot document.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
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
  setStartupDisk: vi.fn(async () => {}),
}));
vi.mock('@/bus/profile', () => ({
  getProfile: vi.fn(async () => ({ floppy_slots: [0, 1] })),
}));
vi.mock('@/bus/media', () => ({
  insertFloppy: vi.fn(async (_p: string, _w: boolean, n = 0) => ({
    ok: true,
    mount: { drive: n },
  })),
  attachHardDisk: vi.fn(async (_p: string, n: number) => ({ ok: true, mount: { bay: n } })),
  attachCdrom: vi.fn(async () => ({ ok: true, mount: {} })),
}));

const { processUrlMedia } = await import('@/bus/urlMedia');
const { SCRATCH_DIR } = await import('@/lib/opfsPaths');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');
const media = await import('@/bus/media');

// A ROM the fake core recognises: 64 KB whose first byte is 'R'.
const PLUS_ROM = new Uint8Array(64 * 1024).fill(0x52);
// An 800 KB floppy image (floppy-sized: a valid fd, an invalid hd).
const FLOPPY = new Uint8Array(819200).fill(0xa5);
// Neither a ROM nor any floppy size.
const JUNK = new Uint8Array(1000).fill(0x33);
// A declaration ROM the fake core recognises: 32 KB whose first byte is 'V'.
const VROM = new Uint8Array(32 * 1024).fill(0x56);

let files: Map<string, Uint8Array>;
let served: Record<string, Uint8Array<ArrayBuffer>>;
const written: string[] = [];

beforeEach(() => {
  bridge.reset();
  _resetForTests();
  vi.clearAllMocks();
  written.length = 0;
  files = installFakeCoreFs(heap, (p) => written.push(p));
  bridge.reply('machine.rom.identify', (args: unknown) => {
    const f = files.get((args as [string])[0]);
    if (!f || f[0] !== 0x52) return { recognised: false };
    return {
      recognised: true,
      supported: true,
      intact: true,
      id: '4D1F8172',
      name: 'Macintosh Plus',
      compatible: ['plus'],
      size: f.length,
    };
  });
  bridge.reply('machine.scsi.identify_hd', (args: unknown) => {
    const f = files.get((args as [string])[0]);
    return !!f && f.length !== 819200;
  });
  bridge.reply('machine.boot', true);
  bridge.reply('machine.id', 'plus');
  bridge.reply('catalog.vroms.identify', (args: unknown) => {
    const f = files.get((args as [string])[0]);
    if (!f || f[0] !== 0x56) return { recognised: false };
    return { recognised: true, card_id: 'mdc_8_24', compatible: ['mdc_8_24'], crc: '0xd1629664' };
  });
  bridge.reply('catalog.vroms.offer', true);
  served = {};
  vi.stubGlobal(
    'fetch',
    vi.fn(async (url: string) => {
      const name = String(url).split('/').pop() ?? '';
      const body = served[name];
      return body ? new Response(body) : new Response('', { status: 404 });
    }),
  );
});
afterEach(() => vi.unstubAllGlobals());

// What is left in the scratch area.
const scratchLeft = () => [...files.keys()].filter((p) => p.startsWith(`${SCRATCH_DIR}/`));

const toastText = () => [...toasts.active, ...toasts.queued].map((t) => t.msg).join('\n');

describe('URL media: an unvalidated download is rejected, never attached from scratch', () => {
  it('a ROM that is not a ROM does not boot, and leaves no scratch file', async () => {
    served['bad.rom'] = JUNK;
    expect(await processUrlMedia(new URLSearchParams('rom=bad.rom'))).toBe(false);
    expect(bridge.paths()).not.toContain('machine.boot');
    expect(scratchLeft()).toEqual([]);
    expect(written.every((p) => p.startsWith(`${SCRATCH_DIR}/`))).toBe(true);
  });

  it('a floppy that is not a floppy is rejected with its reason; the boot goes ahead without it', async () => {
    served['plus.rom'] = PLUS_ROM;
    served['bad.dsk'] = JUNK;
    expect(await processUrlMedia(new URLSearchParams('rom=plus.rom&fd0=bad.dsk'))).toBe(true);
    const boot = bridge.calls.find((c) => c.path === 'machine.boot');
    expect(boot?.args).toEqual({ model: 'plus', rom: '/opfs/images/rom/4D1F8172' });
    expect(media.insertFloppy).not.toHaveBeenCalled();
    expect(toastText()).toMatch(/FD0: 'fd0_[\d_-]+' is not a valid Floppy Disk image/);
    expect(scratchLeft()).toEqual([]);
  });

  it('a hard disk that fails validation is rejected; the boot goes ahead without it', async () => {
    served['plus.rom'] = PLUS_ROM;
    served['floppy.img'] = FLOPPY;
    expect(await processUrlMedia(new URLSearchParams('rom=plus.rom&hd0=floppy.img'))).toBe(true);
    expect(media.attachHardDisk).not.toHaveBeenCalled();
    expect(toastText()).toMatch(/HD0: 'hd0_[\d_-]+' is not a valid Hard Disk image/);
    expect(scratchLeft()).toEqual([]);
    expect([...files.keys()].some((p) => p.startsWith('/opfs/images/hd/'))).toBe(false);
  });

  it('a valid floppy is stored and inserted from its store, not from scratch', async () => {
    served['plus.rom'] = PLUS_ROM;
    served['sys.dsk'] = FLOPPY;
    await processUrlMedia(new URLSearchParams('rom=plus.rom&fd0=sys.dsk'));
    // Stored under the slot and the time, not the URL's name.
    const stored = [...files.keys()].find((p) => p.startsWith('/opfs/images/fd/fd0_'));
    expect(stored).toBeDefined();
    expect(media.insertFloppy).toHaveBeenCalledWith(stored, true, 0);
    expect(scratchLeft()).toEqual([]);
  });

  it('two ROM halves of different sizes are both discarded', async () => {
    served['a.bin'] = new Uint8Array(32 * 1024).fill(0x52);
    served['b.bin'] = new Uint8Array(16 * 1024).fill(0x52);
    expect(await processUrlMedia(new URLSearchParams('rom=a.bin&rom=b.bin'))).toBe(false);
    expect(toastText()).toMatch(/the two halves differ in size/);
    expect(scratchLeft()).toEqual([]);
  });

  it('a failed download leaves nothing behind', async () => {
    served['plus.rom'] = PLUS_ROM;
    expect(await processUrlMedia(new URLSearchParams('rom=plus.rom&fd0=missing.dsk'))).toBe(true);
    expect(toastText()).toMatch(/missing\.dsk: not found/);
    expect(scratchLeft()).toEqual([]);
  });

  it("a second URL boot of the same slot writes its own scratch file, never the first machine's media", async () => {
    served['plus.rom'] = PLUS_ROM;
    served['one.dsk'] = FLOPPY;
    await processUrlMedia(new URLSearchParams('rom=plus.rom&fd0=one.dsk'));
    const first = written.filter((p) => p.includes('url_fd0'));
    const firstPath = [...files.keys()].find((p) => p.startsWith('/opfs/images/fd/fd0_')) ?? '';
    const mounted = files.get(firstPath);
    expect(mounted).toBeDefined();
    written.length = 0;

    served['two.dsk'] = new Uint8Array(819200).fill(0x5a);
    await processUrlMedia(new URLSearchParams('rom=plus.rom&fd0=two.dsk'));
    const second = written.filter((p) => p.includes('url_fd0'));
    expect(first.length).toBeGreaterThan(0);
    expect(second.length).toBeGreaterThan(0);
    expect(new Set(second).has(first[0])).toBe(false);
    expect(written).not.toContain(firstPath);
    expect(files.get(firstPath)).toBe(mounted);
    expect(scratchLeft()).toEqual([]);
  });
});

describe("the URL's vROM is the boot's", () => {
  it('goes in the machine.boot document', async () => {
    served['plus.rom'] = PLUS_ROM;
    served['card.vrom'] = VROM;
    expect(await processUrlMedia(new URLSearchParams('rom=plus.rom&vrom=card.vrom'))).toBe(true);
    const boot = bridge.calls.find((c) => c.path === 'machine.boot');
    expect(boot?.args).toEqual({
      model: 'plus',
      rom: '/opfs/images/rom/4D1F8172',
      vrom: '/opfs/images/vrom/d1629664',
    });
    expect(scratchLeft()).toEqual([]);
  });

  it('one that is not a vROM is rejected, and the boot goes ahead without it, saying so', async () => {
    served['plus.rom'] = PLUS_ROM;
    served['junk.vrom'] = JUNK;
    expect(await processUrlMedia(new URLSearchParams('rom=plus.rom&vrom=junk.vrom'))).toBe(true);
    const boot = bridge.calls.find((c) => c.path === 'machine.boot');
    expect(boot?.args).toEqual({ model: 'plus', rom: '/opfs/images/rom/4D1F8172' });
    expect(toastText()).toMatch(/VROM: 'vrom_[\d_-]+' is not a valid Video ROM image/);
    expect(toastText()).toMatch(/Booting plus without the URL's video ROM/);
    expect(scratchLeft()).toEqual([]);
  });
});
