// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The MP4 export's container writer: ISO base media (ISO/IEC 14496-12)
// with H.264 video (an `avc1` sample entry carrying the encoder's `avcC`,
// ISO/IEC 14496-15) and AAC (`mp4a` with an `esds`, ISO/IEC 14496-14) or
// Opus (`Opus` with a `dOps`, the Opus-in-ISOBMFF mapping) audio.
//
// Samples go to an MdatSink as they are encoded (the worker's temporary
// OPFS file); the writer keeps only their sizes, durations, offsets and
// flags.  finish() then builds what goes in front of them -- `ftyp`, the
// whole `moov`, the `mdat` header -- so the final file is that header
// followed by the sink's bytes, index first, and streams.
//
// Every track's samples have one duration (the video's is the recording's
// frame period in a 1203 Hz time scale: 20, 40 or 80), so each `stts` is a
// single entry and the file is constant-rate by construction; the writer
// still run-length codes whatever it is given.  Video samples whose
// presentation order differs from their decoding order (B-frames) get a
// `ctts` and an edit list that starts the presentation at the first frame.
// Consecutive samples of one track form a chunk (`stsc`, `stco`, or
// `co64` past 4 GB).

export interface VideoTrack {
  kind: 'video';
  width: number;
  height: number;
  timescale: number;
  // The AVCDecoderConfigurationRecord (WebCodecs' decoderConfig.description).
  avcC?: Uint8Array;
}

export interface AudioTrack {
  kind: 'audio';
  codec: 'aac' | 'opus';
  sampleRate: number;
  channels: number;
  timescale: number; // the sample rate
  bitrate: number;
  // AAC: the AudioSpecificConfig; Opus: the OpusHead (pre-skip read from it).
  description?: Uint8Array;
  // Samples at the start of the decoded stream that are not part of the
  // sound (an encoder's priming, Opus' pre-skip): the edit list skips them.
  skip?: number;
  // Samples of sound the track presents (the source's length at this
  // rate); the rest of the last frame is padding.  Unset: all of it.
  presented?: number;
}

export type Track = VideoTrack | AudioTrack;

// Where sample bytes go, in order.
export interface MdatSink {
  write(bytes: Uint8Array): void;
}

export interface SampleInfo {
  // Presentation time in the track's time scale (video), or the decode
  // time (audio, which is written in order).
  pts: number;
  duration: number;
  key: boolean;
}

// What finish() gives: the bytes in front of the samples.
export interface Mp4Head {
  head: Uint8Array;
  // Bytes of sample data the sink took, after the head.
  payload: number;
}

// Per-track bookkeeping.
interface TrackState {
  t: Track;
  sizes: number[];
  durations: number[];
  pts: number[];
  keys: number[]; // 1-based sample numbers of sync samples
  chunks: { offset: number; count: number }[];
}

const MOVIE_TIMESCALE = 1000;
const U32_MAX = 0xffffffff;

// ---------------------------------------------------------------------------
// Box assembly
// ---------------------------------------------------------------------------

// A growing byte buffer with big-endian writers.
class Bytes {
  buf = new Uint8Array(256);
  len = 0;
  private reserve(n: number): void {
    if (this.len + n <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + n) cap *= 2;
    const g = new Uint8Array(cap);
    g.set(this.buf.subarray(0, this.len));
    this.buf = g;
  }
  u8(v: number): this {
    this.reserve(1);
    this.buf[this.len++] = v & 0xff;
    return this;
  }
  u16(v: number): this {
    return this.u8(v >>> 8).u8(v);
  }
  u24(v: number): this {
    return this.u8(v >>> 16)
      .u8(v >>> 8)
      .u8(v);
  }
  u32(v: number): this {
    return this.u8(v >>> 24)
      .u8(v >>> 16)
      .u8(v >>> 8)
      .u8(v);
  }
  u64(v: number): this {
    return this.u32(Math.floor(v / 0x100000000)).u32(v >>> 0);
  }
  i32(v: number): this {
    return this.u32(v >>> 0);
  }
  str(s: string): this {
    for (let i = 0; i < s.length; i++) this.u8(s.charCodeAt(i));
    return this;
  }
  zeros(n: number): this {
    this.reserve(n);
    this.buf.fill(0, this.len, this.len + n);
    this.len += n;
    return this;
  }
  bytes(b: Uint8Array): this {
    this.reserve(b.length);
    this.buf.set(b, this.len);
    this.len += b.length;
    return this;
  }
  done(): Uint8Array {
    return this.buf.slice(0, this.len);
  }
}

// A box: size, type, body.
function box(type: string, ...parts: Uint8Array[]): Uint8Array {
  const n = parts.reduce((a, p) => a + p.length, 8);
  const b = new Bytes().u32(n).str(type);
  for (const p of parts) b.bytes(p);
  return b.done();
}

// A full box: version and flags before the body.
function fullBox(type: string, version: number, flags: number, ...parts: Uint8Array[]): Uint8Array {
  return box(type, new Bytes().u8(version).u24(flags).done(), ...parts);
}

// The unity transformation matrix of mvhd and tkhd.
function matrix(b: Bytes): Bytes {
  return b.u32(0x10000).u32(0).u32(0).u32(0).u32(0x10000).u32(0).u32(0).u32(0).u32(0x40000000);
}

// An MPEG-4 descriptor: tag, length (one byte when it fits, else the
// four-byte form), body.
function descriptor(tag: number, body: Uint8Array): Uint8Array {
  const b = new Bytes().u8(tag);
  const n = body.length;
  if (n < 0x80) b.u8(n);
  else
    b.u8(0x80 | ((n >>> 21) & 0x7f))
      .u8(0x80 | ((n >>> 14) & 0x7f))
      .u8(0x80 | ((n >>> 7) & 0x7f))
      .u8(n & 0x7f);
  return b.bytes(body).done();
}

// AudioSpecificConfig for AAC-LC at `rate` with `channels`, when the
// encoder gave none.
export function aacAudioSpecificConfig(rate: number, channels: number): Uint8Array {
  const rates = [
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
  ];
  const fi = rates.indexOf(rate);
  if (fi < 0) throw new RangeError(`no AAC frequency index for ${rate} Hz`);
  // object type 2 (5 bits), frequency index (4), channel configuration (4), three zero bits
  const v = (2 << 11) | (fi << 7) | (channels << 3);
  return new Uint8Array([v >>> 8, v & 0xff]);
}

// The Opus pre-skip an OpusHead states (bytes 10-11, little-endian), or
// libopus's usual 312 at 48 kHz.
export function opusPreSkip(head: Uint8Array | undefined): number {
  if (head && head.length >= 12 && String.fromCharCode(...head.subarray(0, 8)) === 'OpusHead')
    return head[10] | (head[11] << 8);
  return 312;
}

// ---------------------------------------------------------------------------
// The writer
// ---------------------------------------------------------------------------

export class Mp4Writer {
  private readonly tracks: TrackState[];
  private readonly sink: MdatSink;
  private payload = 0; // bytes written to the sink
  private lastTrack = -1; // the track of the chunk being filled

  constructor(tracks: Track[], sink: MdatSink) {
    if (!tracks.length) throw new Error('an MP4 needs a track');
    this.tracks = tracks.map((t) => ({
      t,
      sizes: [],
      durations: [],
      pts: [],
      keys: [],
      chunks: [],
    }));
    this.sink = sink;
  }

  // The track's configuration, to fill in what the encoder reports late
  // (the avcC, the AudioSpecificConfig).
  track(i: number): Track {
    return this.tracks[i].t;
  }

  // Bytes of samples so far.
  get payloadBytes(): number {
    return this.payload;
  }

  // Samples of track `i` so far.
  samples(i: number): number {
    return this.tracks[i].sizes.length;
  }

  // Appends one encoded sample of track `i`.
  addSample(i: number, data: Uint8Array, info: SampleInfo): void {
    const s = this.tracks[i];
    if (i !== this.lastTrack || !s.chunks.length) {
      s.chunks.push({ offset: this.payload, count: 0 });
      this.lastTrack = i;
    }
    s.chunks[s.chunks.length - 1].count++;
    s.sizes.push(data.length);
    s.durations.push(info.duration);
    s.pts.push(info.pts);
    if (info.key) s.keys.push(s.sizes.length);
    this.sink.write(data);
    this.payload += data.length;
  }

  // Builds the header: ftyp, moov (offsets pointing past it), the mdat header.
  finish(): Mp4Head {
    for (const s of this.tracks) {
      if (!s.sizes.length) throw new Error(`the ${s.t.kind} track has no samples`);
      if (s.t.kind === 'video' && !s.t.avcC) throw new Error('the video track has no avcC');
    }
    const ftyp = this.ftyp();
    // The mdat header is 8 bytes, or 16 with a 64-bit size
    const big = this.payload + 8 > U32_MAX;
    const mdatHead = big
      ? new Bytes()
          .u32(1)
          .str('mdat')
          .u64(this.payload + 16)
          .done()
      : new Bytes()
          .u32(this.payload + 8)
          .str('mdat')
          .done();
    // Offsets past 4 GB need co64: the last chunk's, counted from the file's
    // start, so the head in front of it counts too (an hour's sample tables
    // are megabytes).  The moov's size does not depend on the base.
    let co64 = big;
    let sizeProbe = this.moov(0, co64).length;
    if (!co64 && ftyp.length + sizeProbe + mdatHead.length + this.payload > U32_MAX) {
      co64 = true;
      sizeProbe = this.moov(0, co64).length;
    }
    const base = ftyp.length + sizeProbe + mdatHead.length;
    const moov = this.moov(base, co64);
    const head = new Uint8Array(ftyp.length + moov.length + mdatHead.length);
    head.set(ftyp, 0);
    head.set(moov, ftyp.length);
    head.set(mdatHead, ftyp.length + moov.length);
    return { head, payload: this.payload };
  }

  private ftyp(): Uint8Array {
    const b = new Bytes().str('isom').u32(0x200);
    for (const brand of ['isom', 'iso2', 'avc1', 'mp41']) b.str(brand);
    return box('ftyp', b.done());
  }

  // A track's media duration, and the part of it presented (after the
  // edit list's skip), in its own time scale.
  private spans(s: TrackState): { media: number; shown: number; skip: number } {
    const media = s.durations.reduce((a, d) => a + d, 0);
    const skip = this.editSkip(s);
    // Video: every frame, shifted by the lift; audio: what follows the skip,
    // cut to the sound's own length (the encoder pads its last frame)
    if (s.t.kind === 'video') return { media, shown: media, skip };
    const rest = Math.max(0, media - skip);
    return { media, shown: s.t.presented ? Math.min(rest, s.t.presented) : rest, skip };
  }

  // Composition offsets for a video track whose presentation order is not
  // its decoding order: decode times are the running sum of durations, each
  // offset lifted by the most negative one so all are >= 0.  null when
  // every sample is presented as it is decoded.
  private compositionOffsets(s: TrackState): { offsets: number[]; lift: number } | null {
    if (s.t.kind !== 'video') return null;
    let dts = 0;
    const raw: number[] = [];
    let min = 0;
    let reordered = false;
    // A loop, not Math.min(...pts): an hour at 60 fps is past the engine's argument limit
    let first = Infinity;
    for (const p of s.pts) if (p < first) first = p;
    for (let i = 0; i < s.pts.length; i++) {
      const c = s.pts[i] - first - dts;
      raw.push(c);
      if (c !== 0) reordered = true;
      if (c < min) min = c;
      dts += s.durations[i];
    }
    if (!reordered) return null;
    return { offsets: raw.map((c) => c - min), lift: -min };
  }

  // Media time the edit list starts at.
  private editSkip(s: TrackState): number {
    if (s.t.kind === 'audio') return s.t.skip ?? 0;
    return this.compositionOffsets(s)?.lift ?? 0;
  }

  private moov(base: number, co64: boolean): Uint8Array {
    const durations = this.tracks.map(
      (s) => (this.spans(s).shown * MOVIE_TIMESCALE) / s.t.timescale,
    );
    const movieDuration = Math.round(Math.max(...durations));
    const mvhd = fullBox(
      'mvhd',
      0,
      0,
      matrix(
        new Bytes()
          .u32(0) // creation time
          .u32(0) // modification time
          .u32(MOVIE_TIMESCALE)
          .u32(movieDuration)
          .u32(0x10000) // rate 1.0
          .u16(0x100) // volume 1.0
          .zeros(10),
      )
        .zeros(24) // pre_defined
        .u32(this.tracks.length + 1) // next track ID
        .done(),
    );
    const traks = this.tracks.map((s, i) => this.trak(s, i + 1, base, co64));
    return box('moov', mvhd, ...traks);
  }

  private trak(s: TrackState, id: number, base: number, co64: boolean): Uint8Array {
    const video = s.t.kind === 'video';
    const { media, shown, skip } = this.spans(s);
    const shownMovie = Math.round((shown * MOVIE_TIMESCALE) / s.t.timescale);
    const tkhd = fullBox(
      'tkhd',
      0,
      3, // enabled, in movie
      matrix(
        new Bytes()
          .u32(0)
          .u32(0)
          .u32(id)
          .u32(0) // reserved
          .u32(shownMovie)
          .zeros(8)
          .u16(0) // layer
          .u16(video ? 0 : 1) // alternate group
          .u16(video ? 0 : 0x100) // volume
          .u16(0),
      )
        .u32(video ? (s.t as VideoTrack).width * 0x10000 : 0)
        .u32(video ? (s.t as VideoTrack).height * 0x10000 : 0)
        .done(),
    );
    const parts = [tkhd];
    if (skip > 0 || shown !== media) {
      // One edit: the media from `skip`, at rate 1, for the shown duration
      const elst = fullBox(
        'elst',
        0,
        0,
        new Bytes().u32(1).u32(shownMovie).i32(skip).u16(1).u16(0).done(),
      );
      parts.push(box('edts', elst));
    }
    const mdhd = fullBox(
      'mdhd',
      0,
      0,
      new Bytes().u32(0).u32(0).u32(s.t.timescale).u32(media).u16(0x55c4 /* und */).u16(0).done(),
    );
    const name = video ? 'VideoHandler' : 'SoundHandler';
    const hdlr = fullBox(
      'hdlr',
      0,
      0,
      new Bytes()
        .u32(0)
        .str(video ? 'vide' : 'soun')
        .zeros(12)
        .str(name)
        .u8(0)
        .done(),
    );
    const mhd = video
      ? fullBox('vmhd', 0, 1, new Bytes().zeros(8).done())
      : fullBox('smhd', 0, 0, new Bytes().zeros(4).done());
    const dinf = box(
      'dinf',
      fullBox('dref', 0, 0, new Bytes().u32(1).done(), fullBox('url ', 0, 1)),
    );
    const minf = box('minf', mhd, dinf, this.stbl(s, base, co64));
    parts.push(box('mdia', mdhd, hdlr, minf));
    return box('trak', ...parts);
  }

  private stbl(s: TrackState, base: number, co64: boolean): Uint8Array {
    const parts = [this.stsd(s)];
    // stts: run-length of the durations
    const runs: [number, number][] = [];
    for (const d of s.durations) {
      const last = runs[runs.length - 1];
      if (last && last[1] === d) last[0]++;
      else runs.push([1, d]);
    }
    const stts = new Bytes().u32(runs.length);
    for (const [n, d] of runs) stts.u32(n).u32(d);
    parts.push(fullBox('stts', 0, 0, stts.done()));
    // ctts when presentation order is not decoding order
    const co = this.compositionOffsets(s);
    if (co) {
      const cr: [number, number][] = [];
      for (const c of co.offsets) {
        const last = cr[cr.length - 1];
        if (last && last[1] === c) last[0]++;
        else cr.push([1, c]);
      }
      const ctts = new Bytes().u32(cr.length);
      for (const [n, c] of cr) ctts.u32(n).u32(c);
      parts.push(fullBox('ctts', 0, 0, ctts.done()));
    }
    // stss when not every sample is a sync sample
    if (s.keys.length !== s.sizes.length) {
      const stss = new Bytes().u32(s.keys.length);
      for (const k of s.keys) stss.u32(k);
      parts.push(fullBox('stss', 0, 0, stss.done()));
    }
    // stsc: run-length of samples per chunk
    const sc: [number, number][] = [];
    s.chunks.forEach((c, i) => {
      const last = sc[sc.length - 1];
      if (!last || last[1] !== c.count) sc.push([i + 1, c.count]);
    });
    const stsc = new Bytes().u32(sc.length);
    for (const [first, n] of sc) stsc.u32(first).u32(n).u32(1);
    parts.push(fullBox('stsc', 0, 0, stsc.done()));
    // stsz: one size each (a common size only when all agree)
    const same = s.sizes.every((n) => n === s.sizes[0]);
    const stsz = new Bytes().u32(same ? s.sizes[0] : 0).u32(s.sizes.length);
    if (!same) for (const n of s.sizes) stsz.u32(n);
    parts.push(fullBox('stsz', 0, 0, stsz.done()));
    // stco / co64: the chunks' file offsets
    const co_ = new Bytes().u32(s.chunks.length);
    for (const c of s.chunks) {
      if (co64) co_.u64(base + c.offset);
      else co_.u32(base + c.offset);
    }
    parts.push(fullBox(co64 ? 'co64' : 'stco', 0, 0, co_.done()));
    return box('stbl', ...parts);
  }

  private stsd(s: TrackState): Uint8Array {
    let entry: Uint8Array;
    if (s.t.kind === 'video') {
      const v = s.t;
      const name = new Uint8Array(32); // compressorname: empty
      entry = box(
        'avc1',
        new Bytes()
          .zeros(6)
          .u16(1) // data reference index
          .zeros(16) // pre_defined, reserved
          .u16(v.width)
          .u16(v.height)
          .u32(0x00480000) // 72 dpi
          .u32(0x00480000)
          .u32(0) // reserved
          .u16(1) // frame count
          .bytes(name)
          .u16(0x18) // depth
          .u16(0xffff) // pre_defined -1
          .done(),
        box('avcC', v.avcC!),
        box('pasp', new Bytes().u32(1).u32(1).done()),
      );
    } else {
      const a = s.t;
      const audioEntry = new Bytes()
        .zeros(6)
        .u16(1)
        .zeros(8) // reserved (version 0)
        .u16(a.channels)
        .u16(16) // sample size
        .u16(0)
        .u16(0)
        .u32(a.sampleRate * 0x10000)
        .done();
      if (a.codec === 'aac') {
        const asc = a.description ?? aacAudioSpecificConfig(a.sampleRate, a.channels);
        const dcd = descriptor(
          4,
          new Bytes()
            .u8(0x40) // Audio ISO/IEC 14496-3
            .u8(0x15) // audio stream
            .u24(0) // buffer size
            .u32(a.bitrate) // max bitrate
            .u32(a.bitrate) // average bitrate
            .bytes(descriptor(5, asc))
            .done(),
        );
        const es = descriptor(
          3,
          new Bytes()
            .u16(0)
            .u8(0)
            .bytes(dcd)
            .bytes(descriptor(6, new Uint8Array([2])))
            .done(),
        );
        entry = box('mp4a', audioEntry, fullBox('esds', 0, 0, es));
      } else {
        const dops = new Bytes()
          .u8(0) // version
          .u8(a.channels)
          .u16(opusPreSkip(a.description))
          .u32(a.sampleRate) // input sample rate
          .u16(0) // output gain
          .u8(0) // channel mapping family
          .done();
        entry = box('Opus', audioEntry, box('dOps', dops));
      }
    }
    return fullBox('stsd', 0, 0, new Bytes().u32(1).done(), entry);
  }
}

// ---------------------------------------------------------------------------
// Reading back (the tests, and a check of what was written)
// ---------------------------------------------------------------------------

// A box found by parseBoxes: type, where its body is, its children when it
// is a container.
export interface ParsedBox {
  type: string;
  start: number; // the box's first byte
  body: number; // its body's first byte
  end: number;
  children?: ParsedBox[];
}

const CONTAINERS = new Set(['moov', 'trak', 'mdia', 'minf', 'stbl', 'edts', 'dinf']);
// Containers whose body starts with fixed fields before the child boxes.
const ENTRY_SKIP: Record<string, number> = { stsd: 8, avc1: 78, mp4a: 28, Opus: 28, dref: 8 };

// Walks the boxes of `b[from, to)`.
export function parseBoxes(b: Uint8Array, from = 0, to = b.length): ParsedBox[] {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const out: ParsedBox[] = [];
  let at = from;
  while (at + 8 <= to) {
    let size = dv.getUint32(at);
    const type = String.fromCharCode(...b.subarray(at + 4, at + 8));
    let body = at + 8;
    if (size === 1) {
      size = dv.getUint32(at + 8) * 0x100000000 + dv.getUint32(at + 12);
      body = at + 16;
    } else if (size === 0) size = to - at;
    if (size < 8 || at + size > to) throw new Error(`box ${type} at ${at} runs past its parent`);
    const p: ParsedBox = { type, start: at, body, end: at + size };
    if (CONTAINERS.has(type)) p.children = parseBoxes(b, body, at + size);
    else if (type in ENTRY_SKIP) p.children = parseBoxes(b, body + ENTRY_SKIP[type], at + size);
    out.push(p);
    at += size;
  }
  if (at !== to) throw new Error(`stray bytes at ${at}`);
  return out;
}

// The first box of `type` under `boxes`, depth first.
export function findBox(boxes: ParsedBox[], type: string): ParsedBox | undefined {
  for (const x of boxes) {
    if (x.type === type) return x;
    const c = x.children && findBox(x.children, type);
    if (c) return c;
  }
  return undefined;
}

// Every box of `type` under `boxes`, depth first.
export function findBoxes(boxes: ParsedBox[], type: string): ParsedBox[] {
  const out: ParsedBox[] = [];
  for (const x of boxes) {
    if (x.type === type) out.push(x);
    if (x.children) out.push(...findBoxes(x.children, type));
  }
  return out;
}
