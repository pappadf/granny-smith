// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The MP4 export's audio resampler: a recording's sound (16-bit, mono or
// stereo, at the guest's rate -- 22,255 Hz on a Plus) to the 48 kHz an AAC
// or Opus encoder takes, as planar float.
//
// A windowed-sinc interpolator: 16 zero crossings either side, a Blackman
// window, a polyphase table of 256 phases with linear interpolation between
// neighbouring phases, each phase normalised to unit gain so a constant
// level (the chip's idle offset) passes exactly.  The cut-off is the lower
// of the two rates' Nyquist frequencies, 5 % under it.  Output sample n is
// at input position n x inRate / outRate, computed in integers, so a long
// recording does not drift; the stream's ends are extended with its first
// and last values.  The output is round(input frames x outRate / inRate)
// frames long.

const ZERO_CROSSINGS = 16;
const PHASES = 256;
const ROLLOFF = 0.95;

// The kernel at `t` input samples from the output point: fc x sinc(fc t)
// under a Blackman window `half` samples wide each side.
function kernel(t: number, fc: number, half: number): number {
  if (Math.abs(t) >= half) return 0;
  const x = Math.PI * fc * t;
  const sinc = t === 0 ? 1 : Math.sin(x) / x;
  const u = t / half;
  const w = 0.42 + 0.5 * Math.cos(Math.PI * u) + 0.08 * Math.cos(2 * Math.PI * u);
  return fc * sinc * w;
}

export class Resampler {
  readonly inRate: number;
  readonly outRate: number;
  readonly channels: number;
  private readonly taps: number; // per phase, even
  private readonly table: Float32Array; // (PHASES + 1) rows of `taps`
  private buf: Float32Array[]; // per channel: input from bufStart on
  private bufLen = 0;
  private bufStart = 0; // input index of buf[c][0] (negative: the extension before the start)
  private received = 0; // input frames pushed
  private emitted = 0; // output frames produced
  private started = false;
  private flushed = false;

  constructor(inRate: number, outRate: number, channels: number) {
    if (!(inRate > 0 && outRate > 0 && channels > 0)) throw new RangeError('bad resampler rates');
    this.inRate = inRate;
    this.outRate = outRate;
    this.channels = channels;
    const fc = Math.min(1, outRate / inRate) * ROLLOFF;
    const half = ZERO_CROSSINGS / fc;
    this.taps = 2 * Math.ceil(half);
    this.table = new Float32Array((PHASES + 1) * this.taps);
    const mid = this.taps / 2 - 1;
    for (let p = 0; p <= PHASES; p++) {
      const f = p / PHASES;
      let sum = 0;
      const row = p * this.taps;
      // Tap j weighs input idx - mid + j for an output at idx + f
      for (let j = 0; j < this.taps; j++) {
        const v = kernel(f + mid - j, fc, half);
        this.table[row + j] = v;
        sum += v;
      }
      for (let j = 0; j < this.taps; j++) this.table[row + j] /= sum;
    }
    this.buf = Array.from({ length: channels }, () => new Float32Array(1 << 14));
  }

  // The output length for `frames` input frames.
  outputLength(frames: number): number {
    return Math.round((frames * this.outRate) / this.inRate);
  }

  // Takes `frames` interleaved sample frames; returns the output frames now
  // computable, one array per channel.
  push(samples: Int16Array, frames: number = samples.length / this.channels): Float32Array[] {
    if (this.flushed) throw new Error('resampler already flushed');
    if (frames <= 0) return this.planes(0);
    if (!this.started) {
      // The extension before the start: the first value, a window's worth
      this.reserve(this.taps);
      for (let c = 0; c < this.channels; c++) this.buf[c].fill(samples[c] / 32768, 0, this.taps);
      this.bufLen = this.taps;
      this.bufStart = -this.taps;
      this.started = true;
    }
    this.reserve(this.bufLen + frames);
    for (let c = 0; c < this.channels; c++) {
      const b = this.buf[c];
      for (let i = 0; i < frames; i++) b[this.bufLen + i] = samples[i * this.channels + c] / 32768;
    }
    this.bufLen += frames;
    this.received += frames;
    return this.run(Infinity);
  }

  // Ends the stream: the rest of the output, to exactly outputLength(received).
  flush(): Float32Array[] {
    if (this.flushed) return this.planes(0);
    this.flushed = true;
    if (!this.started) return this.planes(0);
    // The extension after the end: the last value, a window's worth
    this.reserve(this.bufLen + this.taps);
    for (let c = 0; c < this.channels; c++) {
      const b = this.buf[c];
      b.fill(b[this.bufLen - 1], this.bufLen, this.bufLen + this.taps);
    }
    this.bufLen += this.taps;
    return this.run(this.outputLength(this.received));
  }

  // Makes room for `n` buffered frames per channel.
  private reserve(n: number): void {
    if (n <= this.buf[0].length) return;
    let cap = this.buf[0].length;
    while (cap < n) cap *= 2;
    this.buf = this.buf.map((b) => {
      const g = new Float32Array(cap);
      g.set(b.subarray(0, this.bufLen));
      return g;
    });
  }

  private planes(n: number): Float32Array[] {
    return Array.from({ length: this.channels }, () => new Float32Array(n));
  }

  // Produces outputs while their window lies inside the buffer (and below
  // `limit` outputs in all), then drops the input no later output needs.
  private run(limit: number): Float32Array[] {
    const mid = this.taps / 2 - 1;
    const end = this.bufStart + this.bufLen; // one past the last buffered input index
    // Outputs n with idx(n) + taps/2 < end, idx(n) = floor(n inRate / outRate)
    let last = Math.floor(((end - this.taps / 2) * this.outRate) / this.inRate);
    while (last >= 0 && Math.floor((last * this.inRate) / this.outRate) + this.taps / 2 >= end)
      last--;
    const count = Math.max(0, Math.min(last + 1, limit) - this.emitted);
    const out = this.planes(count);
    for (let i = 0; i < count; i++) {
      const n = this.emitted + i;
      const pos = n * this.inRate;
      const idx = Math.floor(pos / this.outRate);
      const fracPhase = ((pos - idx * this.outRate) / this.outRate) * PHASES;
      const p = Math.floor(fracPhase);
      const t = fracPhase - p;
      const r0 = p * this.taps;
      const r1 = r0 + this.taps;
      const at = idx - mid - this.bufStart;
      for (let c = 0; c < this.channels; c++) {
        const b = this.buf[c];
        let acc0 = 0;
        let acc1 = 0;
        for (let j = 0; j < this.taps; j++) {
          const x = b[at + j];
          acc0 += x * this.table[r0 + j];
          acc1 += x * this.table[r1 + j];
        }
        out[c][i] = acc0 + (acc1 - acc0) * t;
      }
    }
    this.emitted += count;
    // Keep from the first input the next output reads
    const keepFrom = Math.floor((this.emitted * this.inRate) / this.outRate) - mid;
    const drop = keepFrom - this.bufStart;
    if (drop > 0 && drop <= this.bufLen) {
      for (let c = 0; c < this.channels; c++) this.buf[c].copyWithin(0, drop, this.bufLen);
      this.bufLen -= drop;
      this.bufStart += drop;
    }
    return out;
  }
}
