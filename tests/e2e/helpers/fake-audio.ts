// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Build a WAV for Chromium's --use-file-for-fake-audio-capture.
//
// The flag replaces the fake device's 440 Hz beep with the contents of a
// file, which is what lets a browser spec drive real speech through the
// whole live capture path — getUserMedia → AudioWorklet → shared-heap ring
// → em_audio_in.c → the Singer's DMA → the DSP → Casper. Nothing else
// reaches those layers; the headless suite injects below the seam.
//
// Two things have to be done to the asset first.
//
// 1. RESAMPLE TO 48 kHz. The speech assets are prepared at the codec's
//    24 kHz (singer.c's WAV loader ignores the file's rate outright — see
//    its header). A real microphone hands the browser 44.1/48 kHz, and the
//    AudioContext runs at the browser's native rate, so feeding 24 kHz here
//    would skip the C-side resampler entirely — the one part of the live
//    path the headless rows cannot exercise. Upsampling with a proper
//    windowed sinc rather than linear interpolation matters for the same
//    reason the product's own resampler does: linear interpolation is 6 dB
//    down at 11 kHz and leaves an image 9 dB down at 13 kHz, and a fixture
//    that mangles the signal cannot tell you the product mangled it.
//
// 2. PAD THE TAIL. Chromium loops the file seamlessly. Casper's EndPoint
//    closes an utterance after 40 negative frames (400 ms), so without a
//    gap the loop point runs one utterance into the next and neither is
//    matchable. The padding is the asset's OWN leading room tone, tiled —
//    not digital silence, which is not what a microphone delivers and not
//    what the recognizer's noise estimator is expecting.

import * as fs from "node:fs";

interface Wav {
  rate: number;
  channels: number;
  samples: Int16Array; // interleaved
}

export function readPcm16Wav(file: string): Wav {
  const b = fs.readFileSync(file);
  if (b.toString("ascii", 0, 4) !== "RIFF" || b.toString("ascii", 8, 12) !== "WAVE")
    throw new Error(`${file}: not a RIFF/WAVE file`);
  let pos = 12;
  let rate = 0;
  let channels = 0;
  let bits = 0;
  let data: Buffer | null = null;
  while (pos + 8 <= b.length) {
    const id = b.toString("ascii", pos, pos + 4);
    const len = b.readUInt32LE(pos + 4);
    const body = pos + 8;
    if (id === "fmt ") {
      channels = b.readUInt16LE(body + 2);
      rate = b.readUInt32LE(body + 4);
      bits = b.readUInt16LE(body + 14);
    } else if (id === "data") {
      data = b.subarray(body, Math.min(body + len, b.length));
    }
    pos = body + len + (len & 1);
  }
  if (!data || bits !== 16 || !rate) throw new Error(`${file}: expected PCM16`);
  const samples = new Int16Array(data.length >> 1);
  for (let i = 0; i < samples.length; i++) samples[i] = data.readInt16LE(i * 2);
  return { rate, channels, samples };
}

export function writePcm16Wav(file: string, w: Wav): void {
  const bytes = w.samples.length * 2;
  const b = Buffer.alloc(44 + bytes);
  b.write("RIFF", 0, "ascii");
  b.writeUInt32LE(36 + bytes, 4);
  b.write("WAVE", 8, "ascii");
  b.write("fmt ", 12, "ascii");
  b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); // PCM
  b.writeUInt16LE(w.channels, 22);
  b.writeUInt32LE(w.rate, 24);
  b.writeUInt32LE(w.rate * w.channels * 2, 28); // byte rate
  b.writeUInt16LE(w.channels * 2, 32); // block align
  b.writeUInt16LE(16, 34);
  b.write("data", 36, "ascii");
  b.writeUInt32LE(bytes, 40);
  for (let i = 0; i < w.samples.length; i++) b.writeInt16LE(w.samples[i], 44 + i * 2);
  fs.writeFileSync(file, b);
}

// Kaiser-windowed sinc, zero-stuffed 2x upsample. Same shape as the
// product's own resampler (em_audio_in.c), at a fixed integer ratio.
function upsample2x(x: Int16Array): Int16Array {
  const TAPS = 65; // odd: a centre tap, so the even phase is a pure delay
  const beta = 7.0;
  const mid = (TAPS - 1) / 2;
  const i0 = (v: number) => {
    let s = 1.0;
    let t = 1.0;
    for (let k = 1; k < 40; k++) {
      const q = v / (2 * k);
      t *= q * q;
      s += t;
      if (t < 1e-13 * s) break;
    }
    return s;
  };
  const i0b = i0(beta);
  const h = new Float64Array(TAPS);
  for (let k = 0; k < TAPS; k++) {
    const d = k - mid;
    const a = 0.5 * d; // cutoff at 0.25 of the OUTPUT rate = the input Nyquist
    const sinc = Math.abs(a) < 1e-9 ? 1 : Math.sin(Math.PI * a) / (Math.PI * a);
    const r = d / (mid + 0.5);
    h[k] = sinc * (i0(beta * Math.sqrt(1 - r * r)) / i0b);
  }
  // Normalise EACH POLYPHASE BRANCH to unity DC gain. Only the taps of one
  // parity contribute to a given output sample, so scaling the filter as a
  // whole is not the same thing — and getting this wrong is not cosmetic
  // here: an early version carried a stray factor of two, which put the
  // fixture 6 dB hot, and 6-12 dB hot is precisely the level error that
  // makes Apple's recognizer reject an utterance (its AGC winds the codec
  // gain down; see dsp3210-plaintalk/sr-test-audio-assets.md §2). A fixture
  // that mis-levels the signal cannot tell you the product mis-levelled it.
  for (const parity of [0, 1]) {
    let sum = 0;
    for (let k = parity; k < TAPS; k += 2) sum += h[k];
    if (sum !== 0) for (let k = parity; k < TAPS; k += 2) h[k] /= sum;
  }
  const n = x.length * 2;
  const y = new Int16Array(n);
  for (let m = 0; m < n; m++) {
    let acc = 0;
    // Only even-indexed zero-stuffed positions carry a sample.
    for (let k = m % 2 === 0 ? 0 : 1; k < TAPS; k += 2) {
      const src = (m - k) >> 1;
      if (src >= 0 && src < x.length) acc += h[k] * x[src];
    }
    y[m] = acc < -32768 ? -32768 : acc > 32767 ? 32767 : Math.round(acc);
  }
  return y;
}

// Read a 24 kHz mono speech asset and write the 48 kHz, tail-padded file
// Chromium should loop into the fake microphone.
export function buildFakeCaptureWav(src: string, dst: string, padSeconds = 1.6): void {
  const w = readPcm16Wav(src);
  const mono =
    w.channels === 1
      ? w.samples
      : Int16Array.from({ length: w.samples.length / w.channels }, (_, i) => w.samples[i * w.channels]);

  const up = upsample2x(mono);
  const rate = w.rate * 2;

  // The asset's own leading room tone, tiled to `padSeconds`.
  const toneLen = Math.min(up.length, Math.round(rate * 0.4));
  const padLen = Math.round(rate * padSeconds);
  const out = new Int16Array(up.length + padLen);
  out.set(up, 0);
  for (let i = 0; i < padLen; i++) out[up.length + i] = up[i % toneLen];

  writePcm16Wav(dst, { rate, channels: 1, samples: out });
}
