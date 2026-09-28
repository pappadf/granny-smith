// @vitest-environment node
// The page moves file bytes through the core's transfer window (bus/xfer.ts):
// a copy into wasm memory plus a storage.xfer_write / xfer_read request, never
// a Module.FS call on the page's thread (which deadlocked Safari).  The fake
// core below keeps "files" in a Map and serves the window from a fake heap.
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

const WINDOW_PTR = 64;
const WINDOW_SIZE = 16;
const heap = { u8: new Uint8Array(256), i16: new Int16Array(1), i32: new Int32Array(1) };

vi.mock('@/bus/emulator', async () => ({
  ...(await (await import('../helpers/bridgeMock')).emulatorModule()),
  getModuleHeap: () => heap,
}));

const { streamToOpfs } = await import('@/bus/upload');
const { xferReadAll, xferRead } = await import('@/bus/xfer');

const files = new Map<string, Uint8Array>();

beforeEach(() => {
  bridge.reset();
  files.clear();
  bridge.reply('storage.xfer_buffer', WINDOW_PTR);
  bridge.reply('storage.xfer_size', WINDOW_SIZE);
  bridge.reply('storage.xfer_write', (args: unknown) => {
    const [path, offset, len] = args as [string, number, number];
    const old = offset === 0 ? new Uint8Array(0) : (files.get(path) ?? new Uint8Array(0));
    const next = new Uint8Array(Math.max(old.length, offset + len));
    next.set(old);
    next.set(heap.u8.subarray(WINDOW_PTR, WINDOW_PTR + len), offset);
    files.set(path, next);
    return true;
  });
  bridge.reply('storage.xfer_read', (args: unknown) => {
    const [path, offset, len] = args as [string, number, number];
    const f = files.get(path);
    if (!f) return { error: 'no such file' };
    const part = f.subarray(offset, Math.min(offset + len, f.length));
    heap.u8.set(part, WINDOW_PTR);
    return part.length;
  });
});

const bytes = (n: number, seed = 1) =>
  Uint8Array.from({ length: n }, (_, i) => (i * 7 + seed) & 0xff);

describe('the transfer window', () => {
  it('a Blob larger than the window lands whole, a window per request', async () => {
    const data = bytes(40);
    expect(await streamToOpfs('/opfs/upload/a.rom', new Blob([data]))).toBe(true);
    expect(files.get('/opfs/upload/a.rom')).toEqual(data);
    expect(bridge.paths().filter((p) => p === 'storage.xfer_write')).toHaveLength(3); // 16+16+8
  });

  it("a stream's small chunks are gathered into full windows", async () => {
    const data = bytes(37, 3);
    const stream = new ReadableStream<Uint8Array>({
      start(c) {
        for (let at = 0; at < data.length; at += 5) c.enqueue(data.subarray(at, at + 5));
        c.close();
      },
    });
    expect(await streamToOpfs('/opfs/upload/b.img', stream)).toBe(true);
    expect(files.get('/opfs/upload/b.img')).toEqual(data);
    expect(bridge.paths().filter((p) => p === 'storage.xfer_write')).toHaveLength(3); // not 8
  });

  it('an empty source still creates the file', async () => {
    expect(await streamToOpfs('/opfs/upload/empty', new Uint8Array(0))).toBe(true);
    expect(files.get('/opfs/upload/empty')).toEqual(new Uint8Array(0));
  });

  it('reads back what was written, and a head', async () => {
    const data = bytes(33, 9);
    files.set('/f', data);
    expect(await xferReadAll('/f')).toEqual(data);
    expect(await xferRead('/f', 0, 4)).toEqual(data.subarray(0, 4));
  });

  it('two transfers at once do not share the window', async () => {
    const a = bytes(40, 1);
    const b = bytes(40, 100);
    const [ra, rb] = await Promise.all([
      streamToOpfs('/a', new Blob([a])),
      streamToOpfs('/b', new Blob([b])),
    ]);
    expect(ra && rb).toBe(true);
    expect(files.get('/a')).toEqual(a);
    expect(files.get('/b')).toEqual(b);
  });

  it('a refused write reports failure', async () => {
    bridge.reply('storage.xfer_write', { error: 'disk full' });
    expect(await streamToOpfs('/c', new Blob([bytes(4)]))).toBe(false);
  });
});
