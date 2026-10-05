// A fake of the core's filesystem methods, on the bridge double: files live in
// a Map, and the transfer window is a fake heap.  Enough of `files.*` for the
// upload and URL-media pipelines to run for real above it — stage, probe,
// store, discard — so a test can assert what is left on disk afterwards.
//
// Usage (the heap must be the one the mocked emulator module's getModuleHeap
// answers):
//   const heap = fakeHeap();
//   vi.mock('@/bus/emulator', async () => ({
//     ...(await (await import('../helpers/bridgeMock')).emulatorModule()),
//     getModuleHeap: () => heap,
//   }));
//   beforeEach(() => { bridge.reset(); files = installFakeCoreFs(heap); });

import { bridge } from './bridgeMock';

const WINDOW_PTR = 64;
const WINDOW_SIZE = 4096;

// The fake heap the transfer window lives in.
export function fakeHeap(): { u8: Uint8Array; i16: Int16Array; i32: Int32Array } {
  return {
    u8: new Uint8Array(WINDOW_PTR + WINDOW_SIZE),
    i16: new Int16Array(1),
    i32: new Int32Array(1),
  };
}

// Route the `files.*` methods to a fresh in-memory filesystem; returns it.
// `onWrite` hears the path of every files.xfer_write.
export function installFakeCoreFs(
  heap: { u8: Uint8Array },
  onWrite?: (path: string) => void,
): Map<string, Uint8Array> {
  const files = new Map<string, Uint8Array>();
  const under = (p: string, dir: string) => p === dir || p.startsWith(`${dir}/`);
  bridge.reply('files.xfer_buffer', WINDOW_PTR);
  bridge.reply('files.xfer_size', WINDOW_SIZE);
  bridge.reply('files.xfer_write', (args: unknown) => {
    const [path, offset, len] = args as [string, number, number];
    onWrite?.(path);
    const old = offset === 0 ? new Uint8Array(0) : (files.get(path) ?? new Uint8Array(0));
    const next = new Uint8Array(Math.max(old.length, offset + len));
    next.set(old);
    next.set(heap.u8.subarray(WINDOW_PTR, WINDOW_PTR + len), offset);
    files.set(path, next);
    return true;
  });
  bridge.reply('files.xfer_read', (args: unknown) => {
    const [path, offset, len] = args as [string, number, number];
    const f = files.get(path);
    if (!f) return { error: 'no such file' };
    const part = f.subarray(offset, Math.min(offset + len, f.length));
    heap.u8.set(part, WINDOW_PTR);
    return part.length;
  });
  // Recursive, and true when the path never existed (gs_rm_tree).
  bridge.reply('files.rm', (args: unknown) => {
    const [path] = args as [string];
    for (const p of [...files.keys()]) if (under(p, path)) files.delete(p);
    return true;
  });
  bridge.reply('files.mv', (args: unknown) => {
    const [from, to] = args as [string, string];
    const f = files.get(from);
    if (!f) return false;
    files.delete(from);
    files.set(to, f);
    return true;
  });
  bridge.reply('files.cp', (args: unknown) => {
    const [from, to] = args as [string, string];
    const f = files.get(from);
    if (!f) return false;
    files.set(to, f.slice());
    return true;
  });
  bridge.reply('files.mkdir', true);
  bridge.reply('files.path_exists', (args: unknown) => files.has((args as [string])[0]));
  bridge.reply('files.path_size', (args: unknown) => {
    const f = files.get((args as [string])[0]);
    return f ? f.length : { error: 'no such file' };
  });
  // Nothing is an archive.
  bridge.reply('files.archive.identify', '');
  return files;
}
