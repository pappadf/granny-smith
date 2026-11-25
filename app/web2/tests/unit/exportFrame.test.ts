// @vitest-environment node
//
// The MP4 export's picture (src/recorder/scale.ts) and sound
// (src/recorder/resample.ts): the output size at each scale, square pixels
// for a non-square recording, the black padding H.264's 4:2:0 needs for an
// odd size, nearest-neighbour scaling pixel for pixel; and the resampler's
// output length, a sine's level and frequency, a constant level passed
// exactly, and streaming in pieces giving what one push gives.

import { describe, it, expect } from 'vitest';
import { FrameScaler, outputSize, squareSize } from '@/recorder/scale';
import { Resampler } from '@/recorder/resample';

// A w x h RGBA frame whose pixel (x, y) is (x, y, x ^ y, 255).
function pattern(w: number, h: number): Uint8Array {
  const b = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      b[i] = x & 0xff;
      b[i + 1] = y & 0xff;
      b[i + 2] = (x ^ y) & 0xff;
      b[i + 3] = 255;
    }
  return b;
}

const px = (b: Uint8Array, w: number, x: number, y: number) =>
  Array.from(b.subarray((y * w + x) * 4, (y * w + x) * 4 + 4));

describe('export size', () => {
  it('is the recording’s own size at 1x and whole multiples above', () => {
    const plus = { width: 512, height: 342, displayWidth: 512, displayHeight: 342 };
    expect(outputSize(plus, 1)).toEqual({
      width: 512,
      height: 342,
      contentWidth: 512,
      contentHeight: 342,
    });
    expect(outputSize(plus, 4)).toEqual({
      width: 2048,
      height: 1368,
      contentWidth: 2048,
      contentHeight: 1368,
    });
  });

  it('pads an odd width or height by one', () => {
    const odd = { width: 513, height: 341, displayWidth: 0, displayHeight: 0 };
    expect(outputSize(odd, 1)).toEqual({
      width: 514,
      height: 342,
      contentWidth: 513,
      contentHeight: 341,
    });
    expect(outputSize(odd, 2)).toEqual({
      width: 1026,
      height: 682,
      contentWidth: 1026,
      contentHeight: 682,
    });
    expect(outputSize(odd, 3)).toEqual({
      width: 1540,
      height: 1024,
      contentWidth: 1539,
      contentHeight: 1023,
    });
  });

  it('squares non-square pixels by widening or heightening, to the nearest pixel', () => {
    // The Lisa: 720 x 364 shown at 4:3
    expect(squareSize({ width: 720, height: 364, displayWidth: 4, displayHeight: 3 })).toEqual({
      width: 720,
      height: 540,
    });
    expect(squareSize({ width: 300, height: 300, displayWidth: 401, displayHeight: 300 })).toEqual({
      width: 401,
      height: 300,
    });
  });
});

describe('frame scaler', () => {
  it('copies at 1x', () => {
    const src = pattern(6, 4);
    const out = outputSize({ width: 6, height: 4, displayWidth: 6, displayHeight: 4 }, 1);
    const dst = new Uint8Array(out.width * out.height * 4);
    new FrameScaler(6, 4, out).scale(src, dst);
    expect(dst).toEqual(src);
  });

  it('repeats each pixel scale x scale times at an integer scale', () => {
    const src = pattern(5, 3);
    const out = outputSize({ width: 5, height: 3, displayWidth: 5, displayHeight: 3 }, 3);
    const dst = new Uint8Array(out.width * out.height * 4);
    new FrameScaler(5, 3, out).scale(src, dst);
    for (let y = 0; y < out.contentHeight; y++)
      for (let x = 0; x < out.contentWidth; x++)
        expect(px(dst, out.width, x, y)).toEqual(px(src, 5, Math.floor(x / 3), Math.floor(y / 3)));
  });

  it('fills the padding column and row with opaque black', () => {
    const src = pattern(5, 3);
    const out = outputSize({ width: 5, height: 3, displayWidth: 5, displayHeight: 3 }, 1);
    expect(out).toEqual({ width: 6, height: 4, contentWidth: 5, contentHeight: 3 });
    const dst = new Uint8Array(out.width * out.height * 4).fill(0x55);
    new FrameScaler(5, 3, out).scale(src, dst);
    for (let y = 0; y < 3; y++) {
      for (let x = 0; x < 5; x++) expect(px(dst, 6, x, y)).toEqual(px(src, 5, x, y));
      expect(px(dst, 6, 5, y)).toEqual([0, 0, 0, 255]);
    }
    for (let x = 0; x < 6; x++) expect(px(dst, 6, x, 3)).toEqual([0, 0, 0, 255]);
  });

  it('widens non-square pixels by nearest neighbour', () => {
    // 4 x 2 shown at 3:1 -> 6 x 2: columns 0 1 1 2 3 3 (centres)
    const src = pattern(4, 2);
    const out = outputSize({ width: 4, height: 2, displayWidth: 3, displayHeight: 1 }, 1);
    expect(out).toEqual({ width: 6, height: 2, contentWidth: 6, contentHeight: 2 });
    const dst = new Uint8Array(out.width * out.height * 4);
    new FrameScaler(4, 2, out).scale(src, dst);
    const cols = [0, 1, 2, 3, 4, 5].map((x) => px(dst, 6, x, 0)[0]);
    expect(cols).toEqual([0, 1, 1, 2, 3, 3]);
  });
});

// RMS of a signal.
const rms = (a: Float32Array) => Math.sqrt(a.reduce((s, v) => s + v * v, 0) / a.length);

// The concatenation of resampler outputs per channel.
function joinPlanes(parts: Float32Array[][], channels: number): Float32Array[] {
  return Array.from({ length: channels }, (_, c) => {
    const n = parts.reduce((a, p) => a + p[c].length, 0);
    const out = new Float32Array(n);
    let at = 0;
    for (const p of parts) {
      out.set(p[c], at);
      at += p[c].length;
    }
    return out;
  });
}

describe('resampler', () => {
  it('makes round(n x 48000 / rate) frames, in one push or many', () => {
    for (const rate of [22255, 22257, 22050, 44100, 48000]) {
      const n = 12345;
      const src = new Int16Array(n);
      const one = new Resampler(rate, 48000, 1);
      const a = joinPlanes([one.push(src), one.flush()], 1)[0];
      expect(a.length).toBe(Math.round((n * 48000) / rate));
      const many = new Resampler(rate, 48000, 1);
      const parts: Float32Array[][] = [];
      for (let at = 0; at < n; at += 4096)
        parts.push(many.push(src.subarray(at, Math.min(n, at + 4096))));
      parts.push(many.flush());
      expect(joinPlanes(parts, 1)[0].length).toBe(a.length);
    }
  });

  it('streams in pieces to the same samples as one push', () => {
    const n = 20000;
    const src = new Int16Array(n * 2);
    for (let i = 0; i < n; i++) {
      src[i * 2] = Math.round(10000 * Math.sin((2 * Math.PI * 440 * i) / 22255));
      src[i * 2 + 1] = Math.round(8000 * Math.cos((2 * Math.PI * 1000 * i) / 22255));
    }
    const one = new Resampler(22255, 48000, 2);
    const a = joinPlanes([one.push(src), one.flush()], 2);
    const many = new Resampler(22255, 48000, 2);
    const parts: Float32Array[][] = [];
    for (let at = 0; at < n; at += 777)
      parts.push(many.push(src.subarray(at * 2, Math.min(n, at + 777) * 2)));
    parts.push(many.flush());
    const b = joinPlanes(parts, 2);
    for (let c = 0; c < 2; c++) {
      expect(b[c].length).toBe(a[c].length);
      for (let i = 0; i < a[c].length; i++) expect(b[c][i]).toBeCloseTo(a[c][i], 6);
    }
  });

  it('keeps a sine’s level and frequency', () => {
    const rate = 22255;
    const f = 1000;
    const n = rate * 2;
    const src = new Int16Array(n);
    for (let i = 0; i < n; i++) src[i] = Math.round(16000 * Math.sin((2 * Math.PI * f * i) / rate));
    const r = new Resampler(rate, 48000, 1);
    const out = joinPlanes([r.push(src), r.flush()], 1)[0];
    // Away from the ends, the level is the input's (16000 / 32768 / sqrt 2)
    const mid = out.subarray(4800, out.length - 4800);
    expect(rms(mid)).toBeCloseTo(16000 / 32768 / Math.SQRT2, 3);
    // And the sine is still at 1 kHz: its rising zero crossings a second
    let crossings = 0;
    for (let i = 1; i < mid.length; i++) if (mid[i - 1] < 0 && mid[i] >= 0) crossings++;
    expect(crossings / (mid.length / 48000)).toBeCloseTo(f, -1);
    // Against the ideal sine at the output's times
    let err = 0;
    for (let i = 4800; i < out.length - 4800; i++) {
      const ideal = (16000 / 32768) * Math.sin((2 * Math.PI * f * i) / 48000);
      err = Math.max(err, Math.abs(out[i] - ideal));
    }
    expect(err).toBeLessThan(2e-3);
  });

  it('passes a constant level exactly, to the very ends', () => {
    const src = new Int16Array(5000).fill(-8192);
    const r = new Resampler(22255, 48000, 1);
    const out = joinPlanes([r.push(src), r.flush()], 1)[0];
    for (const v of out) expect(v).toBeCloseTo(-0.25, 6);
  });
});
