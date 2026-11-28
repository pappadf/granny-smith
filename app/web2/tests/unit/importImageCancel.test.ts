// Cancelling a streamed import while it waited for the next bytes stored the
// part that had arrived as if it were the whole disk: a cancelled stream reads
// as ended, and the writers took the end for the end of the image.  A
// cancelled reader throws instead.
import { describe, it, expect } from 'vitest';
import { ImportCancelled, windowsOf } from '@/bus/importImage';

// A stream that hands out `chunks` and then stays open, as a slow download does.
function openStream(chunks: Uint8Array[]): ReadableStream<Uint8Array> {
  return new ReadableStream<Uint8Array>({
    start(ctl) {
      for (const c of chunks) ctl.enqueue(c);
    },
  });
}

describe('windowsOf (stream)', () => {
  it('throws when cancelled while waiting at a window boundary', async () => {
    const r = windowsOf(
      { kind: 'stream', stream: openStream([new Uint8Array(8)]), total: null },
      8,
    );
    expect((await r.next())?.length).toBe(8);
    const waiting = r.next(); // nothing buffered: waits on the stream
    r.cancel();
    await expect(waiting).rejects.toBeInstanceOf(ImportCancelled);
    await expect(r.next()).rejects.toBeInstanceOf(ImportCancelled);
  });

  it('throws when cancelled inside a window', async () => {
    const r = windowsOf(
      { kind: 'stream', stream: openStream([new Uint8Array(3)]), total: null },
      8,
    );
    const waiting = r.next();
    await new Promise((res) => setTimeout(res, 0));
    r.cancel();
    await expect(waiting).rejects.toBeInstanceOf(ImportCancelled);
  });

  it('still ends with null when the stream ends by itself', async () => {
    const stream = new ReadableStream<Uint8Array>({
      start(ctl) {
        ctl.enqueue(new Uint8Array(11));
        ctl.close();
      },
    });
    const r = windowsOf({ kind: 'stream', stream, total: null }, 8);
    expect((await r.next())?.length).toBe(8);
    expect((await r.next())?.length).toBe(3);
    expect(await r.next()).toBeNull();
  });
});
