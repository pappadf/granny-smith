// @vitest-environment node
//
// The MP4 export's decoder module (`make recdec-module` -> build/recdec.js,
// src/platform/wasm/recdec_main.c) under Node, against a recording the
// headless emulator makes at test time (--record of a Plus boot): its
// facts, every frame it decodes against the PNGs `recorder.extract`
// writes from the same file in tree, its sound against the extracted WAV,
// a file cut short, and a file that is not a recording.  Skipped (loudly)
// when the module, the headless build or the ROM is missing.

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { NEXT_AUDIO, NEXT_END, NEXT_VIDEO, Recording, type RecdecModule } from '@/recorder/recdec';
import {
  fileSource,
  fixtureMissing,
  loadRecdec,
  makeRecording,
  readPngRgba,
  readWav,
  scratchDir,
  type Fixture,
} from '../helpers/recordingFixture';

const missing = fixtureMissing();
if (missing) console.warn(`recdec.test: skipped, ${missing} not found`);

describe.skipIf(!!missing)('recording decoder module', () => {
  let m: RecdecModule;
  let fx: Fixture;

  beforeAll(async () => {
    m = await loadRecdec();
    fx = makeRecording(scratchDir('recdec-'), 6_000_000, [0, 1, 50, 200, -2, -1]);
  }, 60_000);

  afterAll(() => {
    if (fx) rmSync(fx.dir, { recursive: true, force: true });
  });

  it('reads the recording’s facts', () => {
    const rec = Recording.open(m, fileSource(fx.mkv));
    try {
      expect(rec.info).toEqual({
        width: 512,
        height: 342,
        displayWidth: 512,
        displayHeight: 342,
        periodNs: 16_625_103,
        frames: fx.frames,
        audioRate: 22255,
        audioChannels: 1,
        truncated: false,
      });
    } finally {
      rec.close();
    }
  });

  it('decodes every frame and the sound exactly as recorder.extract does', () => {
    const src = fileSource(fx.mkv);
    const rec = Recording.open(m, src);
    const want = readWav(fx.wav);
    expect(want.rate).toBe(22255);
    const pcm = new Int16Array(want.samples.length + (1 << 14));
    let pcmLen = 0;
    let k = 0;
    let compared = 0;
    try {
      for (;;) {
        const r = rec.next();
        if (r === NEXT_END) break;
        if (r === NEXT_VIDEO) {
          const png = fx.pngs.get(k);
          if (png) {
            const ref = readPngRgba(png);
            expect([ref.width, ref.height]).toEqual([512, 342]);
            // Byte for byte, alpha included
            expect(Buffer.from(rec.pixels()).equals(Buffer.from(ref.data))).toBe(true);
            compared++;
          }
          k++;
        } else if (r === NEXT_AUDIO) {
          const block = rec.pcm();
          pcm.set(block, pcmLen);
          pcmLen += block.length;
        }
      }
    } finally {
      rec.close();
    }
    expect(k).toBe(fx.frames);
    expect(compared).toBe(fx.pngs.size);
    expect(pcmLen).toBe(want.samples.length);
    expect(Buffer.from(pcm.buffer, 0, pcmLen * 2).equals(Buffer.from(want.samples.buffer))).toBe(
      true,
    );
    // Read a cluster at a time, not a byte at a time
    expect(src.reads).toBeLessThan(fx.frames);
  });

  it('reads a file cut short to its last complete cluster', () => {
    const whole = readFileSync(fx.mkv);
    const cut = join(fx.dir, 'cut.mkv');
    writeFileSync(cut, whole.subarray(0, Math.floor(whole.length * 0.6)));
    const rec = Recording.open(m, fileSource(cut));
    try {
      expect(rec.info.truncated).toBe(true);
      expect(rec.info.frames).toBeGreaterThan(0);
      expect(rec.info.frames).toBeLessThan(fx.frames);
      let n = 0;
      for (let r = rec.next(); r !== NEXT_END; r = rec.next()) if (r === NEXT_VIDEO) n++;
      expect(n).toBe(rec.info.frames);
    } finally {
      rec.close();
    }
  });

  // The page reads a block as pcm_frames x the track's channels, so a track
  // whose Channels the frames contradict must fail, not read past the block
  it('refuses audio whose frames contradict the track’s channel count', () => {
    const whole = Buffer.from(readFileSync(fx.mkv));
    // Channels (ID 9F, one-byte size, value 1): the only one in the header
    const at = whole.indexOf(Buffer.from([0x9f, 0x81, 0x01]));
    expect(at).toBeGreaterThan(0);
    const stereo = join(fx.dir, 'stereo-track.mkv');
    whole[at + 2] = 2;
    writeFileSync(stereo, whole);
    const rec = Recording.open(m, fileSource(stereo));
    try {
      expect(rec.info.audioChannels).toBe(2);
      expect(() => {
        for (let r = rec.next(); r !== NEXT_END; r = rec.next());
      }).toThrow(/a frame has 1 channels, the track 2/);
    } finally {
      rec.close();
    }
    // More channels than a FLAC frame here has: the audio is left out
    const many = join(fx.dir, 'five-track.mkv');
    whole[at + 2] = 5;
    writeFileSync(many, whole);
    const rec5 = Recording.open(m, fileSource(many));
    try {
      expect(rec5.info.audioChannels).toBe(0);
      let audio = 0;
      for (let r = rec5.next(); r !== NEXT_END; r = rec5.next()) if (r === NEXT_AUDIO) audio++;
      expect(audio).toBe(0);
    } finally {
      rec5.close();
    }
  });

  it('refuses a file that is not a recording, saying why', () => {
    const junk = join(fx.dir, 'junk.mkv');
    writeFileSync(junk, Buffer.from('not a matroska file at all'));
    expect(() => Recording.open(m, fileSource(junk))).toThrow(/not a recording/);
  });
});
