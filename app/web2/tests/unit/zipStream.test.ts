import { describe, it, expect } from 'vitest';
import JSZip from 'jszip';
import { crc32, zipEntries, sniffContainer, sniffDisk, ZipStreamError } from '@/lib/zipStream';
import { storedDmgName } from '@/bus/importImage';

// A stream of `bytes` in pieces of `piece` bytes (network-chunk-sized).
function streamOf(bytes: Uint8Array, piece = 777): ReadableStream<Uint8Array> {
  let at = 0;
  return new ReadableStream<Uint8Array>({
    pull(ctl) {
      if (at >= bytes.length) {
        ctl.close();
        return;
      }
      ctl.enqueue(bytes.slice(at, at + piece));
      at += piece;
    },
  });
}

async function readAll(s: ReadableStream<Uint8Array>): Promise<Uint8Array> {
  const parts: Uint8Array[] = [];
  const r = s.getReader();
  for (;;) {
    const { done, value } = await r.read();
    if (done) break;
    parts.push(value);
  }
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

function content(n: number, seed: number): Uint8Array {
  const b = new Uint8Array(n);
  let x = seed;
  for (let i = 0; i < n; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    // Mostly text-like with zero runs, so deflate has something to do.
    b[i] = i % 4096 < 1024 ? 0 : 0x41 + ((x >>> 16) % 26);
  }
  return b;
}

async function makeZip(opts: { compression: 'STORE' | 'DEFLATE'; streamFiles: boolean }) {
  const zip = new JSZip();
  zip.file('__MACOSX/._disk.img', new Uint8Array([1, 2, 3]));
  zip.file('readme.txt', 'hello');
  zip.file('dir/disk.img', content(300_000, 7));
  zip.file('second.img', content(70_000, 9));
  return zip.generateAsync({
    type: 'uint8array',
    compression: opts.compression,
    streamFiles: opts.streamFiles,
  });
}

describe('crc32', () => {
  it('matches the standard check value', () => {
    expect(crc32(0, new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
  });
  it('folds incrementally', () => {
    const a = new TextEncoder().encode('1234');
    const b = new TextEncoder().encode('56789');
    expect(crc32(crc32(0, a), b)).toBe(0xcbf43926);
  });
});

describe('zipEntries', () => {
  for (const compression of ['STORE', 'DEFLATE'] as const) {
    for (const streamFiles of [false, true]) {
      it(`reads ${compression}${streamFiles ? ' with data descriptors' : ''} members in order`, async () => {
        const bytes = await makeZip({ compression, streamFiles });
        const names: string[] = [];
        let disk: Uint8Array | null = null;
        let second: Uint8Array | null = null;
        for await (const e of zipEntries(streamOf(bytes))) {
          names.push(e.name);
          if (e.name === 'dir/disk.img') disk = await readAll(e.open());
          else if (e.name === 'second.img') second = await readAll(e.open());
          // every other member is skipped without being read
        }
        expect(names).toContain('dir/disk.img');
        expect(names.indexOf('dir/disk.img')).toBeLessThan(names.indexOf('second.img'));
        expect(disk).toEqual(content(300_000, 7));
        expect(second).toEqual(content(70_000, 9));
      });
    }
  }

  it('fails a member whose CRC does not match', async () => {
    const zip = new JSZip();
    zip.file('a.img', content(5000, 3));
    const bytes = await zip.generateAsync({ type: 'uint8array', compression: 'STORE' });
    // Flip a byte of the stored data (after the 30-byte header and the name).
    bytes[30 + 'a.img'.length + 100] ^= 0xff;
    const it = zipEntries(streamOf(bytes));
    const first = await it.next();
    expect(first.done).toBe(false);
    if (first.done) return;
    await expect(readAll(first.value.open())).rejects.toBeInstanceOf(ZipStreamError);
  });

  it('refuses what is not a zip', async () => {
    const it = zipEntries(streamOf(new TextEncoder().encode('not a zip at all, really')));
    await expect(it.next()).rejects.toBeInstanceOf(ZipStreamError);
  });
});

describe('sniffing', () => {
  it('names containers by their first bytes', () => {
    expect(sniffContainer(new Uint8Array([0x50, 0x4b, 0x03, 0x04]))).toBe('zip');
    expect(sniffContainer(new Uint8Array([0x1f, 0x8b, 8, 0]))).toBe('gzip');
    expect(sniffContainer(new TextEncoder().encode('SIT!....rLau'))).toBe('mac');
    expect(
      sniffContainer(new TextEncoder().encode('(This file must be converted with BinHex 4.0)')),
    ).toBe('mac');
    expect(sniffContainer(new Uint8Array(512))).toBe('');
  });
  it('recognises the disks a stream may carry', () => {
    const apm = new Uint8Array(1024);
    apm[0] = 0x45;
    apm[1] = 0x52;
    expect(sniffDisk(apm)).toBe('apm');
    const hfs = new Uint8Array(2048);
    hfs[1024] = 0x42;
    hfs[1025] = 0x44;
    expect(sniffDisk(hfs)).toBe('hfs');
    expect(sniffDisk(new Uint8Array(4096))).toBe('');
  });
});

describe('storedDmgName', () => {
  it('replaces a disk extension with .dmg and sanitises', () => {
    expect(storedDmgName('Disk Tools.img')).toBe('Disk_Tools.dmg');
    expect(storedDmgName('System 7.5.3.iso')).toBe('System_7.5.3.dmg');
    expect(storedDmgName('dir/quake.img.gz')).toBe('quake.dmg');
    expect(storedDmgName('Already.dmg')).toBe('Already.dmg');
    expect(storedDmgName('noext')).toBe('noext.dmg');
  });
});
