// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Stand-ins for WebCodecs' VideoEncoder and AudioEncoder that encode with
// ffmpeg (libx264, its AAC encoder, libopus), for the MP4 export's tests
// under Node, which has no WebCodecs.  They take what the export gives a
// real encoder -- RGBX VideoFrames at k x the frame period, f32-planar
// AudioData at 48 kHz -- and hand back what a real one does: chunks in
// decoding order with the input's timestamps, length-prefixed (AVCC)
// H.264 with the avcC as the first chunk's decoderConfig.description, raw
// AAC frames with the AudioSpecificConfig, Opus packets with the OpusHead.
// ffmpeg runs once, at flush(): the frames wait in a raw file under tmp/.
// Also: reading an ffmpeg-made file's packets back (ffprobe -show_data),
// for feeding the writer encoded samples directly.

import { closeSync, openSync, rmSync, writeSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

// Whether ffmpeg and ffprobe run here.
export function hasFfmpeg(): boolean {
  const a = spawnSync('ffmpeg', ['-version'], { encoding: 'utf8' });
  const b = spawnSync('ffprobe', ['-version'], { encoding: 'utf8' });
  return a.status === 0 && b.status === 0;
}

// Runs a tool; its stdout, or throws with its stderr.
export function run(
  tool: string,
  args: string[],
  maxBuffer = 1 << 30,
): { stdout: Buffer; stderr: string } {
  const r = spawnSync(tool, args, { maxBuffer });
  if (r.status !== 0) throw new Error(`${tool} ${args.join(' ')} failed:\n${r.stderr?.toString()}`);
  return { stdout: r.stdout as Buffer, stderr: r.stderr?.toString() ?? '' };
}

// ffprobe's JSON for `file`.
export function ffprobe(file: string, ...args: string[]): Record<string, unknown> {
  return JSON.parse(
    run('ffprobe', ['-v', 'error', '-of', 'json', ...args, file]).stdout.toString(),
  );
}

// ffprobe's hex dump (-show_data) as bytes.
function hexDump(s: string | undefined): Uint8Array {
  if (!s) return new Uint8Array(0);
  const hex = s
    .split('\n')
    .map((l) => l.slice(10, 50).replace(/ /g, ''))
    .join('');
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

export interface ProbedPacket {
  stream: number;
  pts: number; // seconds
  duration: number; // seconds
  key: boolean;
  data: Uint8Array;
}

export interface ProbedStream {
  codec: string;
  extradata: Uint8Array;
}

// Every packet of `file`, in file (decoding) order, and each stream's extradata.
export function probePackets(file: string): { streams: ProbedStream[]; packets: ProbedPacket[] } {
  const s = ffprobe(file, '-show_streams', '-show_data') as {
    streams: { codec_name: string; extradata?: string }[];
  };
  const p = ffprobe(file, '-show_packets', '-show_data') as {
    packets: {
      stream_index: number;
      pts_time: string;
      duration_time: string;
      flags: string;
      data: string;
    }[];
  };
  return {
    streams: s.streams.map((x) => ({ codec: x.codec_name, extradata: hexDump(x.extradata) })),
    packets: p.packets.map((x) => ({
      stream: x.stream_index,
      pts: Number(x.pts_time),
      duration: Number(x.duration_time),
      key: x.flags.startsWith('K'),
      data: hexDump(x.data),
    })),
  };
}

// An encoded chunk as WebCodecs hands it over.
function chunk(type: 'key' | 'delta', timestamp: number, duration: number, data: Uint8Array) {
  return {
    type,
    timestamp,
    duration,
    byteLength: data.length,
    copyTo: (dst: Uint8Array) => dst.set(data),
  };
}

let seq = 0;

// A VideoFrame from a buffer: the bytes copied, as the real one does.
export class FakeVideoFrame {
  readonly data: Uint8Array;
  readonly timestamp: number;
  readonly codedWidth: number;
  readonly codedHeight: number;
  constructor(
    data: Uint8Array,
    init: { timestamp: number; codedWidth: number; codedHeight: number; format: string },
  ) {
    if (init.format !== 'RGBX') throw new Error(`unexpected frame format ${init.format}`);
    this.data = data.slice(0, init.codedWidth * init.codedHeight * 4);
    this.timestamp = init.timestamp;
    this.codedWidth = init.codedWidth;
    this.codedHeight = init.codedHeight;
  }
  close(): void {}
}

// AudioData: f32-planar samples, copied.
export class FakeAudioData {
  readonly data: Float32Array;
  readonly numberOfFrames: number;
  readonly numberOfChannels: number;
  readonly sampleRate: number;
  readonly timestamp: number;
  constructor(init: {
    format: string;
    sampleRate: number;
    numberOfFrames: number;
    numberOfChannels: number;
    timestamp: number;
    data: Float32Array;
  }) {
    if (init.format !== 'f32-planar') throw new Error(`unexpected audio format ${init.format}`);
    this.data = init.data.slice();
    this.numberOfFrames = init.numberOfFrames;
    this.numberOfChannels = init.numberOfChannels;
    this.sampleRate = init.sampleRate;
    this.timestamp = init.timestamp;
  }
  close(): void {}
}

type Output = (
  c: ReturnType<typeof chunk>,
  meta?: { decoderConfig?: { description?: Uint8Array } },
) => void;

// The stand-in VideoEncoder: frames to a raw file, then libx264 (with
// B-frames, as a browser's encoder may use) into an MP4 whose packets are
// handed back.
export function ffmpegVideoEncoder(
  dir: string,
  log: { configs: VideoEncoderConfig[]; keyRequests: number[] },
) {
  return class {
    state = 'unconfigured';
    encodeQueueSize = 0;
    private readonly output: Output;
    private cfg!: VideoEncoderConfig;
    private raw = '';
    private fd = -1;
    private stamps: number[] = [];
    constructor(init: { output: Output; error: (e: unknown) => void }) {
      this.output = init.output;
    }
    configure(c: VideoEncoderConfig): void {
      this.cfg = c;
      log.configs.push(c);
      this.raw = join(dir, `video-${seq++}.rgba`);
      this.fd = openSync(this.raw, 'w');
      this.state = 'configured';
    }
    encode(f: FakeVideoFrame, opts?: { keyFrame?: boolean }): void {
      if (f.codedWidth !== this.cfg.width || f.codedHeight !== this.cfg.height)
        throw new Error('frame size');
      if (opts?.keyFrame) log.keyRequests.push(this.stamps.length);
      writeSync(this.fd, f.data);
      this.stamps.push(f.timestamp);
    }
    async flush(): Promise<void> {
      closeSync(this.fd);
      const out = `${this.raw}.mp4`;
      const gop = log.keyRequests.length > 1 ? log.keyRequests[1] - log.keyRequests[0] : 250;
      run('ffmpeg', [
        '-v',
        'error',
        '-y',
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgba',
        '-s',
        `${this.cfg.width}x${this.cfg.height}`,
        '-r',
        `${this.cfg.framerate}`,
        '-i',
        this.raw,
        '-c:v',
        'libx264',
        '-preset',
        'veryfast',
        '-crf',
        '18',
        '-g',
        `${gop}`,
        '-pix_fmt',
        'yuv420p',
        out,
      ]);
      rmSync(this.raw);
      const { streams, packets } = probePackets(out);
      const period = 1 / this.cfg.framerate!;
      packets.forEach((p, i) => {
        // The input frame this packet is: its timestamp is that frame's
        const k = Math.round(p.pts / period);
        this.output(
          chunk(p.key ? 'key' : 'delta', this.stamps[k], Math.round(period * 1e6), p.data),
          i === 0 ? { decoderConfig: { description: streams[0].extradata } } : undefined,
        );
      });
    }
    close(): void {
      this.state = 'closed';
    }
  };
}

// The stand-in AudioEncoder: samples to a raw f32 file, then ffmpeg's AAC
// encoder or libopus, packets handed back with their timestamps (an AAC
// encoder's priming shows as a negative first one).
export function ffmpegAudioEncoder(dir: string, log: { configs: AudioEncoderConfig[] }) {
  return class {
    state = 'unconfigured';
    encodeQueueSize = 0;
    private readonly output: Output;
    private cfg!: AudioEncoderConfig;
    private raw = '';
    private fd = -1;
    constructor(init: { output: Output; error: (e: unknown) => void }) {
      this.output = init.output;
    }
    configure(c: AudioEncoderConfig): void {
      this.cfg = c;
      log.configs.push(c);
      this.raw = join(dir, `audio-${seq++}.f32`);
      this.fd = openSync(this.raw, 'w');
      this.state = 'configured';
    }
    encode(a: FakeAudioData): void {
      const n = a.numberOfFrames;
      const ch = a.numberOfChannels;
      const inter = new Float32Array(n * ch);
      for (let c = 0; c < ch; c++)
        for (let i = 0; i < n; i++) inter[i * ch + c] = a.data[c * n + i];
      writeSync(this.fd, new Uint8Array(inter.buffer));
    }
    async flush(): Promise<void> {
      closeSync(this.fd);
      const out = `${this.raw}.mp4`;
      const opus = this.cfg.codec === 'opus';
      run('ffmpeg', [
        '-v',
        'error',
        '-y',
        '-f',
        'f32le',
        '-ar',
        `${this.cfg.sampleRate}`,
        '-ac',
        `${this.cfg.numberOfChannels}`,
        '-i',
        this.raw,
        '-c:a',
        opus ? 'libopus' : 'aac',
        '-b:a',
        `${this.cfg.bitrate}`,
        out,
      ]);
      rmSync(this.raw);
      const { streams, packets } = probePackets(out);
      packets.forEach((p, i) =>
        this.output(
          chunk('key', Math.round(p.pts * 1e6), Math.round(p.duration * 1e6), p.data),
          i === 0 ? { decoderConfig: { description: streams[0].extradata } } : undefined,
        ),
      );
    }
    close(): void {
      this.state = 'closed';
    }
  };
}
