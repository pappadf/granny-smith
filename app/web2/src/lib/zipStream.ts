// Forward-only zip reading, for imports that must not hold an archive or its
// members whole.
//
// A zip is read front to back from its local file headers, never from the
// central directory at its tail, so it works on a network body as it
// arrives and on a local File alike.  A member's data comes out as a stream
// (stored, or inflated by the browser's DecompressionStream('deflate-raw'))
// whose CRC-32 is checked as it passes; the consumer reads it, or skips it.
// At most one member is in flight; nothing is buffered beyond a network
// chunk or two.
//
// The one case forward-only reading cannot know up front is a member whose
// sizes trail it in a data descriptor (general-purpose bit 3, what streaming
// zip writers emit).  Its end is found by the descriptor's signature followed
// by a compressed size equal to the bytes since the member began -- every
// writer that sets bit 3 in practice writes the signature.  A bit-3 member of
// a method we cannot decode fails the archive (its end cannot be found any
// other way); any other such member is skipped.

const SIG_LOCAL = 0x04034b50;
const SIG_CENTRAL = 0x02014b50;
const SIG_END = 0x06054b50;

export class ZipStreamError extends Error {}

// The browser's inflate, typed for byte streams.
export function decompressor(
  format: 'deflate-raw' | 'gzip',
): ReadableWritablePair<Uint8Array, Uint8Array> {
  return new DecompressionStream(format) as unknown as ReadableWritablePair<Uint8Array, Uint8Array>;
}

// --- CRC-32 (the zip / UDIF / gs_crc32 polynomial) ---------------------------

let crcTable: Uint32Array | null = null;
function table(): Uint32Array {
  if (crcTable) return crcTable;
  crcTable = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crcTable[n] = c >>> 0;
  }
  return crcTable;
}

// Fold `data` into a running CRC (start from 0), as gs_crc32 does.
export function crc32(crc: number, data: Uint8Array): number {
  const t = table();
  let c = ~crc >>> 0;
  for (let i = 0; i < data.length; i++) c = t[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

// --- A buffered reader over a byte stream -------------------------------------

export class ByteReader {
  private chunks: Uint8Array[] = [];
  private buffered = 0;
  private done = false;
  // Bytes taken from the stream so far (for progress).
  consumed = 0;

  constructor(
    private reader: ReadableStreamDefaultReader<Uint8Array>,
    private onRead?: (bytes: number) => void,
  ) {}

  // Buffer at least `n` bytes, or everything left.  The bytes buffered.
  async fill(n: number): Promise<number> {
    while (this.buffered < n && !this.done) {
      const { done, value } = await this.reader.read();
      if (done) {
        this.done = true;
        break;
      }
      if (value && value.length) {
        this.chunks.push(value);
        this.buffered += value.length;
        this.consumed += value.length;
        this.onRead?.(this.consumed);
      }
    }
    return Math.min(this.buffered, n);
  }

  // Take up to `n` buffered bytes (fill first for exactly n).
  private takeBuffered(n: number): Uint8Array {
    n = Math.min(n, this.buffered);
    if (this.chunks.length && this.chunks[0].length >= n) {
      const head = this.chunks[0];
      const out = head.subarray(0, n);
      if (head.length === n) this.chunks.shift();
      else this.chunks[0] = head.subarray(n);
      this.buffered -= n;
      return out;
    }
    const out = new Uint8Array(n);
    let at = 0;
    while (at < n) {
      const head = this.chunks[0];
      const k = Math.min(head.length, n - at);
      out.set(head.subarray(0, k), at);
      at += k;
      if (k === head.length) this.chunks.shift();
      else this.chunks[0] = head.subarray(k);
    }
    this.buffered -= n;
    return out;
  }

  // Exactly `n` bytes, or a ZipStreamError at the end of the stream.
  async take(n: number): Promise<Uint8Array> {
    if ((await this.fill(n)) < n) throw new ZipStreamError('the archive ends early');
    return this.takeBuffered(n);
  }

  // Up to `max` bytes: whatever one read brings (empty at the end).
  async some(max: number): Promise<Uint8Array> {
    if (!this.buffered) await this.fill(1);
    return this.takeBuffered(Math.min(max, this.buffered));
  }

  // Look at the next `n` bytes without taking them (fewer at the end).
  async peek(n: number): Promise<Uint8Array> {
    const got = await this.fill(n);
    const out = new Uint8Array(got);
    let at = 0;
    for (const c of this.chunks) {
      if (at >= got) break;
      const k = Math.min(c.length, got - at);
      out.set(c.subarray(0, k), at);
      at += k;
    }
    return out;
  }

  // Discard `n` bytes.
  async skip(n: number): Promise<void> {
    while (n > 0) {
      if (!this.buffered && (await this.fill(1)) === 0)
        throw new ZipStreamError('the archive ends early');
      const k = Math.min(n, this.buffered);
      this.takeBuffered(k);
      n -= k;
    }
  }

  cancel(): void {
    this.reader.cancel().catch(() => undefined);
  }
}

// --- Members ------------------------------------------------------------------

export interface ZipEntry {
  name: string;
  isDir: boolean;
  method: number; // 0 stored, 8 deflate, anything else unsupported
  // Decoded size when the header says (null for a bit-3 member).
  size: number | null;
  // Whether this reader can decode the member.
  supported: boolean;
  // The member's decoded bytes.  Read it to the end (or call skip): the
  // archive's next member follows.  Errors with ZipStreamError on a CRC
  // mismatch at the end.
  open(): ReadableStream<Uint8Array>;
  // Pass over the member without decoding it.
  skip(): Promise<void>;
  // The member's CRC-32 as recorded, known once it has been read or skipped.
  crc(): number | null;
}

function u16(b: Uint8Array, at: number): number {
  return b[at] | (b[at + 1] << 8);
}
function u32(b: Uint8Array, at: number): number {
  return (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
}
function u64(b: Uint8Array, at: number): number {
  return u32(b, at) + u32(b, at + 4) * 2 ** 32;
}

// Every member of the zip `body`, in archive order.  The previous member must
// be read to its end or skipped before the next is asked for.
export async function* zipEntries(
  body: ReadableStream<Uint8Array>,
  onRead?: (bytes: number) => void,
): AsyncGenerator<ZipEntry> {
  const r = new ByteReader(body.getReader(), onRead);
  try {
    for (;;) {
      const sigBytes = await r.peek(4);
      if (sigBytes.length < 4) return;
      const sig = u32(sigBytes, 0);
      if (sig === SIG_CENTRAL || sig === SIG_END) return; // the directory: no more members
      if (sig !== SIG_LOCAL) throw new ZipStreamError('not a zip archive (or a damaged one)');
      const h = await r.take(30);
      const flags = u16(h, 6);
      const method = u16(h, 8);
      const crc = u32(h, 14);
      let csize = u32(h, 18);
      let usize = u32(h, 22);
      const nameLen = u16(h, 26);
      const extraLen = u16(h, 28);
      const nameBytes = await r.take(nameLen);
      const extra = await r.take(extraLen);
      const utf8 = (flags & 0x800) !== 0;
      const name = utf8
        ? new TextDecoder().decode(nameBytes)
        : Array.from(nameBytes, (c) => String.fromCharCode(c)).join('');
      // Zip64: the extra field carries the sizes the header marks 0xFFFFFFFF.
      let zip64 = false;
      for (let at = 0; at + 4 <= extra.length; ) {
        const id = u16(extra, at);
        const len = u16(extra, at + 2);
        if (id === 0x0001) {
          zip64 = true;
          let p = at + 4;
          if (usize === 0xffffffff && p + 8 <= at + 4 + len) {
            usize = u64(extra, p);
            p += 8;
          }
          if (csize === 0xffffffff && p + 8 <= at + 4 + len) csize = u64(extra, p);
        }
        at += 4 + len;
      }
      if (flags & 0x1) throw new ZipStreamError(`${name}: encrypted zip members are not supported`);
      const deferred = (flags & 0x8) !== 0;
      const supported = method === 0 || method === 8;
      if (deferred && !supported)
        throw new ZipStreamError(
          `${name}: compression method ${method} with trailing sizes cannot be read as a stream`,
        );
      let finished = false;
      let crcKnown: number | null = deferred ? null : crc;

      // The member's compressed bytes as a stream: exactly csize of them, or,
      // for a bit-3 member, up to its data descriptor.
      const compressed = (): ReadableStream<Uint8Array> => {
        let left = csize;
        let seen = 0;
        return new ReadableStream<Uint8Array>({
          async pull(ctl) {
            if (!deferred) {
              if (left <= 0) {
                ctl.close();
                return;
              }
              const part = await r.some(Math.min(left, 1 << 20));
              if (!part.length) throw new ZipStreamError(`${name}: the archive ends early`);
              left -= part.length;
              ctl.enqueue(part);
              return;
            }
            // Bit 3: hand on everything up to a candidate descriptor; hold
            // back what could be the start of one.
            const desc = zip64 ? 24 : 16;
            const look = await r.peek(1 << 16);
            if (look.length < desc) throw new ZipStreamError(`${name}: no data descriptor`);
            for (let i = 0; i + desc <= look.length; i++) {
              if (
                look[i] === 0x50 &&
                look[i + 1] === 0x4b &&
                look[i + 2] === 0x07 &&
                look[i + 3] === 0x08 &&
                (zip64 ? u64(look, i + 8) : u32(look, i + 8)) === seen + i
              ) {
                if (i) ctl.enqueue(await r.take(i));
                const d = await r.take(desc);
                crcKnown = u32(d, 4);
                csize = seen + i;
                usize = zip64 ? u64(d, 16) : u32(d, 12);
                ctl.close();
                return;
              }
            }
            const n = look.length - desc + 1;
            seen += n;
            ctl.enqueue(await r.take(n));
          },
        });
      };

      const entry: ZipEntry = {
        name,
        isDir: name.endsWith('/'),
        method,
        size: deferred ? null : usize,
        supported,
        crc: () => crcKnown,
        open() {
          if (finished) throw new ZipStreamError(`${name}: already read`);
          finished = true;
          if (!supported) throw new ZipStreamError(`${name}: compression method ${method}`);
          let src = compressed();
          if (method === 8) src = src.pipeThrough(decompressor('deflate-raw'));
          let crcRun = 0;
          return src.pipeThrough(
            new TransformStream<Uint8Array, Uint8Array>({
              transform(chunk, ctl) {
                crcRun = crc32(crcRun, chunk);
                ctl.enqueue(chunk);
              },
              flush() {
                // A bit-3 member's CRC is read with its descriptor, which the
                // compressed stream reached before the decoder flushed.
                if (crcKnown !== null && crcRun !== crcKnown)
                  throw new ZipStreamError(`${name}: CRC mismatch (the archive is damaged)`);
              },
            }),
          );
        },
        async skip() {
          if (finished) return;
          finished = true;
          if (!deferred) {
            await r.skip(csize);
            return;
          }
          const s = compressed().getReader();
          for (;;) {
            const { done } = await s.read();
            if (done) break;
          }
        },
      };
      yield entry;
      if (!finished) await entry.skip();
    }
  } finally {
    r.cancel();
  }
}

// The kind of container the first bytes of a file show it to be, for the
// streamed import's choice of unpacker ('' when it is none of these).
export function sniffContainer(head: Uint8Array): 'zip' | 'gzip' | 'mac' | '' {
  if (head.length >= 4 && u32(head, 0) === SIG_LOCAL) return 'zip';
  if (head.length >= 4 && (u32(head, 0) === SIG_END || u32(head, 0) === 0x08074b50)) return 'zip';
  if (head.length >= 2 && head[0] === 0x1f && head[1] === 0x8b) return 'gzip';
  const text = String.fromCharCode(...head.subarray(0, Math.min(head.length, 96)));
  if (text.startsWith('SIT!') || text.startsWith('StuffIt (c)')) return 'mac';
  if (head.length >= 14 && text.substring(10, 14) === 'rLau') return 'mac';
  if (text.includes('(This file must be converted with BinHex')) return 'mac';
  // Compact Pro: magic 0x01, volume 1, a directory offset past the header.
  if (head.length >= 8 && head[0] === 0x01 && head[1] === 0x01 && u32be(head, 4) >= 8) return 'mac';
  // MacBinary: a zero version byte, a 1..63-byte name, zeros at 74 and 82.
  if (
    head.length >= 128 &&
    head[0] === 0 &&
    head[1] >= 1 &&
    head[1] <= 63 &&
    head[74] === 0 &&
    head[82] === 0 &&
    isPrintable(head.subarray(65, 73))
  )
    return 'mac';
  return '';
}

function u32be(b: Uint8Array, at: number): number {
  return ((b[at] << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3]) >>> 0;
}

function isPrintable(b: Uint8Array): boolean {
  return b.every((c) => c >= 0x20 && c < 0x7f);
}

// What the first bytes of a decoded disk show it to be: the evidence that a
// stream is a disk image worth storing as one ('' when none is visible --
// a blank disk shows nothing either, so absence is not a verdict).
export function sniffDisk(head: Uint8Array): 'apm' | 'hfs' | 'iso' | '' {
  if (head.length >= 2 && head[0] === 0x45 && head[1] === 0x52) return 'apm';
  if (
    head.length >= 1026 &&
    ((head[1024] === 0x42 && head[1025] === 0x44) || (head[1024] === 0x48 && head[1025] === 0x2b))
  )
    return 'hfs';
  if (head.length >= 32774 && String.fromCharCode(...head.subarray(32769, 32774)) === 'CD001')
    return 'iso';
  return '';
}
