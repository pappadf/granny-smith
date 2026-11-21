// A minimal zip writer for tests (PKWARE APPNOTE 4.3): stored or deflated
// members, in the order given, optionally with data descriptors (flag bit 3:
// the local header's CRC and sizes are zero and follow the data, as a writer
// that cannot seek back leaves them).  Independent of the readers under test:
// its own CRC-32, and the platform's CompressionStream for DEFLATE.

export interface ZipMember {
  name: string;
  data: Uint8Array | string;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(b: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

async function deflateRaw(b: Uint8Array): Promise<Uint8Array> {
  const s = new ReadableStream<BufferSource>({
    start(c) {
      c.enqueue(b as Uint8Array<ArrayBuffer>);
      c.close();
    },
  }).pipeThrough(new CompressionStream('deflate-raw'));
  const parts: Uint8Array[] = [];
  const r = s.getReader();
  for (let p = await r.read(); !p.done; p = await r.read()) parts.push(p.value);
  return concat(parts);
}

function concat(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

export async function makeZip(
  members: ZipMember[],
  opts: { method?: 'store' | 'deflate'; descriptors?: boolean } = {},
): Promise<Uint8Array> {
  const deflate = opts.method === 'deflate';
  const descriptors = opts.descriptors ?? false;
  const flags = 0x0800 | (descriptors ? 0x0008 : 0); // UTF-8 names
  const method = deflate ? 8 : 0;
  const locals: Uint8Array[] = [];
  const centrals: Uint8Array[] = [];
  let offset = 0;
  for (const m of members) {
    const name = new TextEncoder().encode(m.name);
    const data = typeof m.data === 'string' ? new TextEncoder().encode(m.data) : m.data;
    const packed = deflate ? await deflateRaw(data) : data;
    const crc = crc32(data);

    const lh = new Uint8Array(30);
    const l = new DataView(lh.buffer);
    l.setUint32(0, 0x04034b50, true);
    l.setUint16(4, 20, true); // version needed
    l.setUint16(6, flags, true);
    l.setUint16(8, method, true);
    if (!descriptors) {
      l.setUint32(14, crc, true);
      l.setUint32(18, packed.length, true);
      l.setUint32(22, data.length, true);
    }
    l.setUint16(26, name.length, true);
    locals.push(lh, name, packed);
    let size = lh.length + name.length + packed.length;
    if (descriptors) {
      const dd = new Uint8Array(16);
      const d = new DataView(dd.buffer);
      d.setUint32(0, 0x08074b50, true);
      d.setUint32(4, crc, true);
      d.setUint32(8, packed.length, true);
      d.setUint32(12, data.length, true);
      locals.push(dd);
      size += dd.length;
    }

    const ch = new Uint8Array(46);
    const c = new DataView(ch.buffer);
    c.setUint32(0, 0x02014b50, true);
    c.setUint16(4, 20, true); // version made by
    c.setUint16(6, 20, true); // version needed
    c.setUint16(8, flags, true);
    c.setUint16(10, method, true);
    c.setUint32(16, crc, true);
    c.setUint32(20, packed.length, true);
    c.setUint32(24, data.length, true);
    c.setUint16(28, name.length, true);
    c.setUint32(42, offset, true);
    centrals.push(ch, name);
    offset += size;
  }
  const cd = concat(centrals);
  const end = new Uint8Array(22);
  const e = new DataView(end.buffer);
  e.setUint32(0, 0x06054b50, true);
  e.setUint16(8, members.length, true);
  e.setUint16(10, members.length, true);
  e.setUint32(12, cd.length, true);
  e.setUint32(16, offset, true);
  return concat([...locals, cd, end]);
}
