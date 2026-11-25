// @vitest-environment node
//
// The MP4 export's container writer (src/recorder/mp4Writer.ts) from
// canned encoded chunks: the box structure (ftyp, then moov, then mdat),
// the sample tables (one stts entry per track, stsz, stsc/stco, stss),
// the sample entries (avc1 + avcC, mp4a + esds, Opus + dOps), the edit
// lists, and every sample read back through its stco offset.

import { describe, it, expect } from 'vitest';
import {
  Mp4Writer,
  aacAudioSpecificConfig,
  findBox,
  findBoxes,
  opusPreSkip,
  parseBoxes,
  type ParsedBox,
  type Track,
} from '@/recorder/mp4Writer';
import { videoTiming } from '@/recorder/exportPipeline';

// A sink that keeps everything.
function memorySink(): { write(b: Uint8Array): void; bytes(): Uint8Array } {
  const parts: Uint8Array[] = [];
  return {
    write: (b) => parts.push(b.slice()),
    bytes: () => {
      const n = parts.reduce((a, p) => a + p.length, 0);
      const out = new Uint8Array(n);
      let at = 0;
      for (const p of parts) {
        out.set(p, at);
        at += p.length;
      }
      return out;
    },
  };
}

const AVCC = new Uint8Array([
  1, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 4, 0x67, 0x64, 0, 0x1f, 1, 0, 2, 0x68, 0xee,
]);

// A recognisable sample: its track, its number, its length.
function sample(track: number, i: number, len: number): Uint8Array {
  const b = new Uint8Array(len);
  for (let j = 0; j < len; j++) b[j] = (track * 101 + i * 7 + j) & 0xff;
  return b;
}

// Reads the fields of the sample tables in a trak.
function tables(file: Uint8Array, trak: ParsedBox) {
  const dv = new DataView(file.buffer, file.byteOffset, file.byteLength);
  const u32 = (at: number) => dv.getUint32(at);
  const list = (b: ParsedBox | undefined, width: number) => {
    if (!b) return null;
    const n = u32(b.body + 4);
    return Array.from({ length: n }, (_, i) =>
      Array.from({ length: width }, (_, k) => u32(b.body + 8 + (i * width + k) * 4)),
    );
  };
  const stsz = findBox([trak], 'stsz')!;
  const common = u32(stsz.body + 4);
  const count = u32(stsz.body + 8);
  const sizes = common
    ? Array(count).fill(common)
    : Array.from({ length: count }, (_, i) => u32(stsz.body + 12 + i * 4));
  return {
    stts: list(findBox([trak], 'stts'), 2)!,
    ctts: list(findBox([trak], 'ctts'), 2),
    stss: list(findBox([trak], 'stss'), 1)?.map((r) => r[0]) ?? null,
    stsc: list(findBox([trak], 'stsc'), 3)!,
    stco: list(findBox([trak], 'stco'), 1)!.map((r) => r[0]),
    elst: list(findBox([trak], 'elst'), 3),
    mdhd: findBox([trak], 'mdhd')!,
    sizes,
  };
}

// The samples of a trak, from its tables: each chunk's offset, the
// samples per chunk, the sizes.
function samplesOf(file: Uint8Array, trak: ParsedBox): Uint8Array[] {
  const t = tables(file, trak);
  const out: Uint8Array[] = [];
  let s = 0;
  t.stco.forEach((off, ci) => {
    // The stsc run that covers chunk ci + 1
    let per = 0;
    for (const [first, n] of t.stsc) if (first <= ci + 1) per = n;
    let at = off;
    for (let k = 0; k < per; k++) {
      out.push(file.subarray(at, at + t.sizes[s]));
      at += t.sizes[s++];
    }
  });
  return out;
}

// Writes `video` frames (every `gop`-th a keyframe) and `audio` AAC or Opus
// packets, interleaved as an encoder would hand them over.
function build(tracks: Track[], video: number, audio: number, gop = 30) {
  const sink = memorySink();
  const w = new Mp4Writer(tracks, sink);
  const written: Uint8Array[][] = [[], []];
  let a = 0;
  for (let i = 0; i < video; i++) {
    const v = sample(0, i, i % gop === 0 ? 900 : 40 + (i % 5));
    w.addSample(0, v, { pts: i * 20, duration: 20, key: i % gop === 0 });
    written[0].push(v);
    // Audio catches up to the video's time: 48000 / (1203 / 20) samples a frame
    while (tracks.length > 1 && a < audio && a * 1024 * 1203 < (i + 1) * 20 * 48000) {
      const s = sample(1, a, 200 + (a % 3));
      w.addSample(1, s, { pts: 0, duration: 1024, key: true });
      written[1].push(s);
      a++;
    }
  }
  while (tracks.length > 1 && a < audio) {
    const s = sample(1, a, 201);
    w.addSample(1, s, { pts: 0, duration: 1024, key: true });
    written[1].push(s);
    a++;
  }
  const r = w.finish();
  const payload = sink.bytes();
  expect(payload.length).toBe(r.payload);
  const file = new Uint8Array(r.head.length + payload.length);
  file.set(r.head, 0);
  file.set(payload, r.head.length);
  return { file, written };
}

const video = (): Track => ({
  kind: 'video',
  width: 512,
  height: 342,
  timescale: 1203,
  avcC: AVCC,
});
const aac = (): Track => ({
  kind: 'audio',
  codec: 'aac',
  sampleRate: 48000,
  channels: 1,
  timescale: 48000,
  bitrate: 128000,
});

describe('MP4 writer', () => {
  it('lays out ftyp, moov, mdat with the index first', () => {
    const { file } = build([video(), aac()], 120, 200);
    const top = parseBoxes(file);
    expect(top.map((b) => b.type)).toEqual(['ftyp', 'moov', 'mdat']);
    expect(String.fromCharCode(...file.subarray(8, 12))).toBe('isom');
    const traks = findBoxes(top, 'trak');
    expect(traks).toHaveLength(2);
    expect(findBox([traks[0]], 'hdlr')).toBeDefined();
    expect(
      String.fromCharCode(
        ...file.subarray(
          findBox([traks[0]], 'hdlr')!.body + 8,
          findBox([traks[0]], 'hdlr')!.body + 12,
        ),
      ),
    ).toBe('vide');
  });

  it('gives each track one stts entry: a constant rate by construction', () => {
    const { file } = build([video(), aac()], 120, 200);
    const [v, a] = findBoxes(parseBoxes(file), 'trak');
    const tv = tables(file, v);
    expect(tv.stts).toEqual([[120, 20]]);
    expect(tv.ctts).toBeNull();
    expect(tv.elst).toBeNull();
    const dv = new DataView(file.buffer);
    // mdhd: the 1203 Hz time scale and 120 x 20 ticks
    expect(dv.getUint32(tv.mdhd.body + 12)).toBe(1203);
    expect(dv.getUint32(tv.mdhd.body + 16)).toBe(2400);
    const ta = tables(file, a);
    expect(ta.stts).toEqual([[200, 1024]]);
    expect(dv.getUint32(ta.mdhd.body + 12)).toBe(48000);
  });

  it('lists the keyframes in stss and omits it for audio', () => {
    const { file } = build([video(), aac()], 95, 150, 30);
    const [v, a] = findBoxes(parseBoxes(file), 'trak');
    expect(tables(file, v).stss).toEqual([1, 31, 61, 91]);
    expect(tables(file, a).stss).toBeNull();
  });

  it('reads every sample back through stsc, stsz and stco', () => {
    const { file, written } = build([video(), aac()], 120, 200);
    const [v, a] = findBoxes(parseBoxes(file), 'trak');
    const sv = samplesOf(file, v);
    const sa = samplesOf(file, a);
    expect(sv).toHaveLength(120);
    expect(sa).toHaveLength(200);
    sv.forEach((s, i) => expect(s).toEqual(written[0][i]));
    sa.forEach((s, i) => expect(s).toEqual(written[1][i]));
    // Interleaved, so both tracks have many chunks; all inside mdat
    const mdat = parseBoxes(file).find((b) => b.type === 'mdat')!;
    for (const off of [...tables(file, v).stco, ...tables(file, a).stco]) {
      expect(off).toBeGreaterThanOrEqual(mdat.body);
      expect(off).toBeLessThan(mdat.end);
    }
    expect(tables(file, v).stco.length).toBeGreaterThan(10);
  });

  it('carries the avcC in avc1 and the AudioSpecificConfig in esds', () => {
    const { file } = build([video(), aac()], 10, 10);
    const top = parseBoxes(file);
    const avc1 = findBox(top, 'avc1')!;
    const dv = new DataView(file.buffer);
    expect(dv.getUint16(avc1.body + 24)).toBe(512);
    expect(dv.getUint16(avc1.body + 26)).toBe(342);
    const avcC = findBox(top, 'avcC')!;
    expect(file.subarray(avcC.body, avcC.end)).toEqual(AVCC);
    const mp4a = findBox(top, 'mp4a')!;
    expect(dv.getUint16(mp4a.body + 16)).toBe(1); // channels
    expect(dv.getUint32(mp4a.body + 24)).toBe(48000 * 65536);
    const esds = findBox(top, 'esds')!;
    const body = Array.from(file.subarray(esds.body + 4, esds.end));
    // ES_Descriptor, DecoderConfigDescriptor (object type 0x40, audio), the ASC
    expect(body[0]).toBe(3);
    const dcd = body.indexOf(4);
    expect(body[dcd + 2]).toBe(0x40);
    expect(body[dcd + 3]).toBe(0x15);
    const asc = aacAudioSpecificConfig(48000, 1);
    expect(Array.from(asc)).toEqual([0x11, 0x88]);
    const dsi = body.indexOf(5, dcd);
    expect(body.slice(dsi + 2, dsi + 2 + body[dsi + 1])).toEqual(Array.from(asc));
    // SLConfigDescriptor, predefined 2
    expect(body.slice(-3)).toEqual([6, 1, 2]);
  });

  it('writes Opus as an Opus entry with dOps, its pre-skip in an edit list', () => {
    const head = new Uint8Array(19);
    head.set(new TextEncoder().encode('OpusHead'));
    head[8] = 1;
    head[9] = 2;
    head[10] = 0x38; // pre-skip 312
    head[11] = 0x01;
    expect(opusPreSkip(head)).toBe(312);
    const opus: Track = {
      kind: 'audio',
      codec: 'opus',
      sampleRate: 48000,
      channels: 2,
      timescale: 48000,
      bitrate: 192000,
      description: head,
      skip: 312,
      presented: 100 * 960 - 400,
    };
    const sink = memorySink();
    const w = new Mp4Writer([video(), opus], sink);
    for (let i = 0; i < 100; i++) {
      w.addSample(0, sample(0, i, 50), { pts: i * 20, duration: 20, key: i === 0 });
      w.addSample(1, sample(1, i, 80), { pts: 0, duration: 960, key: true });
    }
    const r = w.finish();
    const file = new Uint8Array(r.head.length + r.payload);
    file.set(r.head, 0);
    file.set(sink.bytes(), r.head.length);
    const top = parseBoxes(file);
    const dops = findBox(top, 'dOps')!;
    const dv = new DataView(file.buffer);
    expect(file[dops.body]).toBe(0);
    expect(file[dops.body + 1]).toBe(2);
    expect(dv.getUint16(dops.body + 2)).toBe(312);
    expect(dv.getUint32(dops.body + 4)).toBe(48000);
    const a = findBoxes(top, 'trak')[1];
    // One edit: from media time 312, for the sound's own length (ms)
    expect(tables(file, a).elst).toEqual([
      [Math.round(((100 * 960 - 400) * 1000) / 48000), 312, 0x10000],
    ]);
  });

  it('gives B-frames a ctts and an edit list that starts at the first frame', () => {
    // Decode order I P B B P B B ..., presentation 0 3 1 2 6 4 5 ...
    const order = [0, 3, 1, 2, 6, 4, 5, 9, 7, 8];
    const sink = memorySink();
    const w = new Mp4Writer([video()], sink);
    order.forEach((k, i) =>
      w.addSample(0, sample(0, i, 30), { pts: k * 20, duration: 20, key: i === 0 }),
    );
    const r = w.finish();
    const file = new Uint8Array(r.head.length + r.payload);
    file.set(r.head, 0);
    file.set(sink.bytes(), r.head.length);
    const t = tables(file, findBox(parseBoxes(file), 'trak')!);
    expect(t.stts).toEqual([[10, 20]]);
    // Offsets pts - dts, lifted by the most negative (-20)
    const offsets = t.ctts!.flatMap(([n, c]) => Array(n).fill(c));
    expect(offsets).toEqual(order.map((k, i) => k * 20 - i * 20 + 20));
    // Composition starts at 20: the edit begins there
    expect(t.elst![0][1]).toBe(20);
  });

  it('keeps an odd-sized sample table compact: a common size when all agree', () => {
    const sink = memorySink();
    const w = new Mp4Writer([aac()], sink);
    for (let i = 0; i < 7; i++)
      w.addSample(0, new Uint8Array(6), { pts: 0, duration: 1024, key: true });
    const r = w.finish();
    const file = new Uint8Array(r.head.length + r.payload);
    file.set(r.head, 0);
    file.set(sink.bytes(), r.head.length);
    const stsz = findBox(parseBoxes(file), 'stsz')!;
    const dv = new DataView(file.buffer);
    expect(dv.getUint32(stsz.body + 4)).toBe(6);
    expect(dv.getUint32(stsz.body + 8)).toBe(7);
    expect(stsz.end - stsz.body).toBe(12);
  });

  it('refuses a video track with no avcC, or a track with no samples', () => {
    const w = new Mp4Writer(
      [{ kind: 'video', width: 2, height: 2, timescale: 1203 }],
      memorySink(),
    );
    w.addSample(0, new Uint8Array(1), { pts: 0, duration: 20, key: true });
    expect(() => w.finish()).toThrow(/avcC/);
    expect(() => new Mp4Writer([video()], memorySink()).finish()).toThrow(/no samples/);
  });

  it('times the three recording rates exactly in 1203 Hz', () => {
    expect(videoTiming(16_625_103)).toEqual({ timescale: 1203, duration: 20 });
    expect(videoTiming(33_250_206)).toEqual({ timescale: 1203, duration: 40 });
    expect(videoTiming(66_500_412)).toEqual({ timescale: 1203, duration: 80 });
    expect(videoTiming(40_000_000)).toEqual({ timescale: 1000, duration: 40 });
  });
});
