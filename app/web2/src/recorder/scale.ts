// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The MP4 export's picture size and its nearest-neighbour scaler.
//
// The export is the recording's own size by default (a Plus at 512x342),
// or 2x, 3x or 4x that, nearest-neighbour so pixels stay square-edged.  A
// recording whose pixels are not square (the Lisa: the track's
// DisplayWidth / DisplayHeight differ in shape from the stored size) is
// widened or heightened to square pixels first, to the nearest whole pixel,
// since MP4 players honour aspect metadata unreliably.  H.264 here is
// 4:2:0, which needs even dimensions: an odd width or height gets one
// black column or row, on the right or at the bottom.

// The sizes the export dialog offers.
export const EXPORT_SCALES = [1, 2, 3, 4] as const;
export type ExportScale = (typeof EXPORT_SCALES)[number];

// The stored picture and the shape it is shown at.
export interface PictureShape {
  width: number;
  height: number;
  displayWidth: number;
  displayHeight: number;
}

// An export's frame: the encoded size (even), and the scaled picture
// inside it from the top left (the rest is the black padding).
export interface OutputSize {
  width: number;
  height: number;
  contentWidth: number;
  contentHeight: number;
}

// The picture with square pixels: the stored size, widened or heightened
// (never narrowed) to the display shape, to the nearest whole pixel.
export function squareSize(s: PictureShape): { width: number; height: number } {
  const dw = s.displayWidth || s.width;
  const dh = s.displayHeight || s.height;
  // Cross-multiplied: is the display shape wider than the stored one?
  if (dw * s.height > s.width * dh)
    return { width: Math.round((s.height * dw) / dh), height: s.height };
  if (dw * s.height < s.width * dh)
    return { width: s.width, height: Math.round((s.width * dh) / dw) };
  return { width: s.width, height: s.height };
}

// The export's frame at `scale`.
export function outputSize(s: PictureShape, scale: number): OutputSize {
  const sq = squareSize(s);
  const cw = sq.width * scale;
  const ch = sq.height * scale;
  return { width: cw + (cw & 1), height: ch + (ch & 1), contentWidth: cw, contentHeight: ch };
}

// RGBA black, opaque, as one little-endian word.
const BLACK = 0xff000000;

// Scales RGBA frames of one source size into one output size: the column
// and row maps are made once, a run of output rows that come from one
// source row is one row built and then copied.
export class FrameScaler {
  private readonly sw: number;
  private readonly sh: number;
  private readonly out: OutputSize;
  private readonly xmap: Uint32Array;
  private readonly ymap: Uint32Array;
  // Whether every output column is a whole number of copies of a source
  // column, in order (an integer scale with square pixels).
  private readonly identity: boolean;

  constructor(sourceWidth: number, sourceHeight: number, out: OutputSize) {
    this.sw = sourceWidth;
    this.sh = sourceHeight;
    this.out = out;
    this.xmap = new Uint32Array(out.contentWidth);
    // Nearest source column for each output column: the one whose span holds its centre
    for (let x = 0; x < out.contentWidth; x++)
      this.xmap[x] = Math.min(
        sourceWidth - 1,
        Math.floor(((x + 0.5) * sourceWidth) / out.contentWidth),
      );
    this.ymap = new Uint32Array(out.contentHeight);
    for (let y = 0; y < out.contentHeight; y++)
      this.ymap[y] = Math.min(
        sourceHeight - 1,
        Math.floor(((y + 0.5) * sourceHeight) / out.contentHeight),
      );
    this.identity = out.contentWidth === sourceWidth && out.contentHeight === sourceHeight;
  }

  // The output frame's byte length (RGBA).
  get byteLength(): number {
    return this.out.width * this.out.height * 4;
  }

  // Scales `src` (sourceWidth x sourceHeight RGBA) into `dst` (the output
  // size, RGBA), padding included.
  scale(src: Uint8Array, dst: Uint8Array): void {
    const { width: ow, height: oh, contentWidth: cw, contentHeight: ch } = this.out;
    if (src.length < this.sw * this.sh * 4 || dst.length < ow * oh * 4)
      throw new RangeError('frame buffer too small');
    // Word views when both buffers allow them, else a copy that does
    const s32 =
      (src.byteOffset & 3) === 0
        ? new Uint32Array(src.buffer, src.byteOffset, this.sw * this.sh)
        : new Uint32Array(src.slice(0, this.sw * this.sh * 4).buffer);
    const d32 = new Uint32Array(dst.buffer, dst.byteOffset, ow * oh);
    let prev = -1;
    for (let y = 0; y < ch; y++) {
      const row = y * ow;
      const sy = this.ymap[y];
      if (sy === prev) {
        // The same source row as the line above: copy that line
        d32.copyWithin(row, row - ow, row - ow + cw);
      } else if (this.identity) {
        d32.set(s32.subarray(sy * this.sw, sy * this.sw + cw), row);
      } else {
        const base = sy * this.sw;
        for (let x = 0; x < cw; x++) d32[row + x] = s32[base + this.xmap[x]];
      }
      if (ow > cw) d32[row + cw] = BLACK;
      prev = sy;
    }
    // The padding row at the bottom
    if (oh > ch) d32.fill(BLACK, ch * ow, oh * ow);
  }
}
