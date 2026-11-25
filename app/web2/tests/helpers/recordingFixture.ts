// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// A real recording for the MP4 export's tests, made at test time by the
// headless emulator (`make -f Makefile.headless`: build/headless/gs-headless
// --record=...) from a Plus booting to its floppy question mark, with what
// `recorder.extract` decodes from it in tree (PNG frames, the WAV) beside
// it; and the decoder module (`make recdec-module`: build/recdec.js) loaded
// under Node.  Nothing here is committed: every file is made under tmp/.

import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  openSync,
  readFileSync,
  readSync,
  writeFileSync,
} from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { inflateSync } from 'node:zlib';
import { loadRecdecModule, type RecdecModule, type RecordingSource } from '@/recorder/recdec';

export const repoRoot = fileURLToPath(new URL('../../../../', import.meta.url));
const headless = join(repoRoot, 'build/headless/gs-headless');
const rom = join(repoRoot, 'tests/data/roms/plus-v3-4d1f8172.rom');
export const recdecJs = join(repoRoot, 'build/recdec.js');

// What is missing to make a recording here, or '' when nothing is.
export function fixtureMissing(): string {
  if (!existsSync(recdecJs)) return 'build/recdec.js (run `make recdec-module`)';
  if (!existsSync(headless)) return 'build/headless/gs-headless (run `make -f Makefile.headless`)';
  if (!existsSync(rom)) return 'tests/data/roms/plus-v3-4d1f8172.rom (scripts/fetch-test-data.sh)';
  return '';
}

// A scratch directory of the test's own under tmp/.
export function scratchDir(prefix: string): string {
  const base = join(repoRoot, 'tmp');
  mkdirSync(base, { recursive: true });
  return mkdtempSync(join(base, prefix));
}

export interface Fixture {
  dir: string;
  mkv: string;
  frames: number; // as recorder.inspect counts them
  pngs: Map<number, string>; // frame index -> recorder.extract's PNG
  wav: string; // recorder.extract's WAV of the whole sound
}

// Records `instructions` of a Plus boot at `rate` and extracts frames `at`
// (negative: from the end) and the sound.
export function makeRecording(
  dir: string,
  instructions: number,
  at: number[],
  rate = 'full',
): Fixture {
  const mkv = join(dir, 'boot.mkv');
  const wav = join(dir, 'sound.wav');
  const pngs = new Map<number, string>();
  const lines = [
    'machine.rtc.time = 3000000000',
    `scheduler.run(${instructions})`,
    'local paths = recorder.stop()',
    'local info = recorder.inspect(paths[1])',
    'echo(F"FRAMES ${info.frames}")',
  ];
  at.forEach((k, i) => {
    const png = join(dir, `frame-${i}.png`);
    lines.push(`recorder.extract(paths[1], "${png}", ${k})`);
    pngs.set(k, png);
  });
  lines.push(`recorder.extract(paths[1], "${wav}")`, 'quit()');
  const script = join(dir, 'record.lua');
  writeFileSync(script, lines.join('\n') + '\n');
  // Full rate records from the first frame (--record); another rate is set
  // first and the recording started from the script
  const args = [`rom=${rom}`, 'model=plus', 'ram=1024', `script=${script}`];
  if (rate === 'full') args.push(`--record=${mkv}`);
  else {
    writeFileSync(
      script,
      [`recorder.rate = "${rate}"`, `recorder.start("${mkv}")`, ...lines].join('\n') + '\n',
    );
  }
  const r = spawnSync(headless, args, { encoding: 'utf8' });
  const out = `${r.stdout}\n${r.stderr}`;
  const m = /FRAMES (\d+)/.exec(out);
  if (r.status !== 0 || !m) throw new Error(`gs-headless failed (${r.status}):\n${out}`);
  // Resolve indices from the end
  const frames = Number(m[1]);
  const resolved = new Map<number, string>();
  for (const [k, p] of pngs) resolved.set(k < 0 ? frames + k : k, p);
  return { dir, mkv, frames, pngs: resolved, wav };
}

// The decoder module, under Node.
export function loadRecdec(): Promise<RecdecModule> {
  return loadRecdecModule(
    pathToFileURL(recdecJs).href,
    pathToFileURL(recdecJs.replace(/\.js$/, '.wasm')).href,
  );
}

// A Node file as a RecordingSource.
export function fileSource(path: string): RecordingSource & { reads: number } {
  const fd = openSync(path, 'r');
  const size = readFileSync(path).length;
  const src = {
    size,
    reads: 0,
    read(at: number, len: number): Uint8Array {
      src.reads++;
      const b = new Uint8Array(len);
      const n = readSync(fd, b, 0, len, at);
      return b.subarray(0, n);
    },
  };
  return src;
}

// An 8-bit RGBA (colour type 6) PNG, decoded: what save_framebuffer_as_png writes.
export function readPngRgba(path: string): { width: number; height: number; data: Uint8Array } {
  const b = readFileSync(path);
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  let at = 8;
  let width = 0;
  let height = 0;
  const idat: Buffer[] = [];
  while (at < b.length) {
    const len = dv.getUint32(at);
    const type = b.toString('latin1', at + 4, at + 8);
    const body = b.subarray(at + 8, at + 8 + len);
    if (type === 'IHDR') {
      width = body.readUInt32BE(0);
      height = body.readUInt32BE(4);
      if (body[8] !== 8 || body[9] !== 6 || body[12] !== 0)
        throw new Error('not an 8-bit RGBA, non-interlaced PNG');
    } else if (type === 'IDAT') idat.push(body);
    at += 12 + len;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * 4;
  const out = new Uint8Array(height * stride);
  for (let y = 0; y < height; y++) {
    const f = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= 4 ? out[y * stride + x - 4] : 0;
      const up = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= 4 && y > 0 ? out[(y - 1) * stride + x - 4] : 0;
      let v = line[x];
      if (f === 1) v += a;
      else if (f === 2) v += up;
      else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) {
        const p = a + up - c;
        const pa = Math.abs(p - a);
        const pb = Math.abs(p - up);
        const pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c;
      }
      out[y * stride + x] = v & 0xff;
    }
  }
  return { width, height, data: out };
}

// A 16-bit PCM WAV's samples, interleaved, with its rate and channels.
export function readWav(path: string): { rate: number; channels: number; samples: Int16Array } {
  const b = readFileSync(path);
  let at = 12;
  let rate = 0;
  let channels = 0;
  while (at < b.length) {
    const id = b.toString('latin1', at, at + 4);
    const len = b.readUInt32LE(at + 4);
    if (id === 'fmt ') {
      channels = b.readUInt16LE(at + 10);
      rate = b.readUInt32LE(at + 12);
    } else if (id === 'data') {
      const s = new Int16Array(len / 2);
      for (let i = 0; i < s.length; i++) s[i] = b.readInt16LE(at + 8 + i * 2);
      return { rate, channels, samples: s };
    }
    at += 8 + len + (len & 1);
  }
  throw new Error('no data chunk');
}
