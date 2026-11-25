// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The MP4 export itself, apart from where its files live: a recording
// decoded frame by frame (recdec.ts), each frame scaled (scale.ts) and
// given to a WebCodecs VideoEncoder at frame k x the frame period, the
// sound resampled to 48 kHz (resample.ts) and given to an AudioEncoder,
// and every encoded chunk handed to the MP4 writer (mp4Writer.ts) as it
// comes.  The worker (export.worker.ts) supplies the OPFS files; the tests
// supply Node files and stand-in encoders.  The WebCodecs classes are
// injected (ExportCodecs) so the same code runs under both.
//
// The encoders' queues pace the decoding: past a few frames waiting, the
// loop waits for the encoder to take one.  A keyframe every two seconds.

import type { Recording } from './recdec';
import { NEXT_AUDIO, NEXT_END, NEXT_VIDEO } from './recdec';
import { FrameScaler, type OutputSize } from './scale';
import { Resampler } from './resample';
import {
  Mp4Writer,
  opusPreSkip,
  type AudioTrack,
  type MdatSink,
  type Mp4Head,
  type Track,
} from './mp4Writer';
import { AUDIO_RATE, type AudioPlan } from './probe';

// The WebCodecs surface the export uses.
export interface ExportCodecs {
  VideoEncoder: new (init: VideoEncoderInit) => VideoEncoder;
  AudioEncoder?: new (init: AudioEncoderInit) => AudioEncoder;
  VideoFrame: new (data: AllowSharedBufferSource, init: VideoFrameBufferInit) => VideoFrame;
  AudioData: new (init: AudioDataInit) => AudioData;
}

// The browser's own (in a window or a worker).
export function webCodecs(): ExportCodecs {
  const g = globalThis as unknown as ExportCodecs;
  return {
    VideoEncoder: g.VideoEncoder,
    AudioEncoder: g.AudioEncoder,
    VideoFrame: g.VideoFrame,
    AudioData: g.AudioData,
  };
}

// What to make: the video configuration (probe.ts), the audio one or null
// for video only, the frame size.
export interface ExportPlan {
  video: VideoEncoderConfig;
  audio: AudioPlan | null;
  out: OutputSize;
}

export interface ExportOptions {
  plan: ExportPlan;
  sink: MdatSink;
  codecs: ExportCodecs;
  // Frames done of frames total, after each frame.
  onProgress?: (done: number, total: number) => void;
  signal?: AbortSignal;
}

export interface ExportResult {
  head: Mp4Head;
  frames: number;
  audioSamples: number; // at 48 kHz, presented
}

// Thrown when the signal aborts the export.
export class ExportCancelled extends Error {
  constructor() {
    super('cancelled');
    this.name = 'ExportCancelled';
  }
}

// The frames waiting in an encoder before the decoding waits for it.
const QUEUE_MAX = 8;

// The MP4 time scale and sample duration for a frame period: 1203 Hz and
// 20, 40 or 80 for the recorder's three rates (16,625,103 ns x 1, 2, 4 is
// 20/1203 s x 1, 2, 4 to within a nanosecond); else 90 kHz, rounded.
export function videoTiming(periodNs: number): { timescale: number; duration: number } {
  for (const timescale of [1203, 1000, 90000]) {
    const d = (periodNs * timescale) / 1e9;
    if (Math.abs(d - Math.round(d)) < 1e-3 && Math.round(d) > 0)
      return { timescale, duration: Math.round(d) };
  }
  return { timescale: 90000, duration: Math.max(1, Math.round((periodNs * 90000) / 1e9)) };
}

// Waits until `enc` holds at most `max` items.
async function drain(
  enc: { encodeQueueSize: number; addEventListener?: unknown },
  max: number,
): Promise<void> {
  while (enc.encodeQueueSize > max) {
    await new Promise<void>((resolve) => {
      const t = setTimeout(resolve, 5);
      const target = enc as unknown as EventTarget;
      if (typeof target.addEventListener === 'function')
        target.addEventListener(
          'dequeue',
          () => {
            clearTimeout(t);
            resolve();
          },
          { once: true },
        );
    });
  }
}

// Encodes `rec` to MP4 samples in `opts.sink`; the header to put in front
// of them.  Rejects with ExportCancelled on an abort and with the
// encoder's or decoder's error on a failure; the encoders are closed
// either way.
export async function encodeRecording(rec: Recording, opts: ExportOptions): Promise<ExportResult> {
  const { plan, codecs } = opts;
  const info = rec.info;
  if (!info.frames) throw new Error('the recording has no frames');
  if (!info.periodNs) throw new Error('the recording states no frame rate');
  const { timescale, duration } = videoTiming(info.periodNs);
  const periodUs = info.periodNs / 1000;
  const fps = 1e9 / info.periodNs;
  const gop = Math.max(1, Math.round(2 * fps));

  const withAudio = !!(plan.audio && info.audioRate && codecs.AudioEncoder);
  const tracks: Track[] = [
    { kind: 'video', width: plan.out.width, height: plan.out.height, timescale },
  ];
  if (withAudio)
    tracks.push({
      kind: 'audio',
      codec: plan.audio!.codec,
      sampleRate: AUDIO_RATE,
      channels: info.audioChannels,
      timescale: AUDIO_RATE,
      bitrate: plan.audio!.config.bitrate ?? 128_000,
    });
  const writer = new Mp4Writer(tracks, opts.sink);

  let failure: unknown = null;
  const fail = (e: unknown) => {
    failure ??= e;
  };

  // Video: chunks to the writer at their frame's time
  const venc = new codecs.VideoEncoder({
    output: (chunk, meta) => {
      try {
        const desc = meta?.decoderConfig?.description;
        const v = writer.track(0);
        if (desc && v.kind === 'video' && !v.avcC) v.avcC = toBytes(desc);
        const data = new Uint8Array(chunk.byteLength);
        chunk.copyTo(data);
        const k = Math.round(chunk.timestamp / periodUs);
        writer.addSample(0, data, { pts: k * duration, duration, key: chunk.type === 'key' });
      } catch (e) {
        fail(e);
      }
    },
    error: fail,
  });

  // Audio: chunks in order, each its own duration at 48 kHz
  let aenc: AudioEncoder | null = null;
  let firstAudioTs: number | null = null;
  if (withAudio) {
    const fallbackFrame = plan.audio!.codec === 'aac' ? 1024 : 960;
    aenc = new codecs.AudioEncoder!({
      output: (chunk, meta) => {
        try {
          const a = writer.track(1) as AudioTrack;
          const desc = meta?.decoderConfig?.description;
          if (desc && !a.description) a.description = toBytes(desc);
          firstAudioTs ??= chunk.timestamp;
          const data = new Uint8Array(chunk.byteLength);
          chunk.copyTo(data);
          const d = chunk.duration
            ? Math.round((chunk.duration * AUDIO_RATE) / 1e6)
            : fallbackFrame;
          writer.addSample(1, data, { pts: 0, duration: d, key: true });
        } catch (e) {
          fail(e);
        }
      },
      error: fail,
    });
  }

  const closeAll = () => {
    for (const e of [venc, aenc])
      try {
        if (e && e.state !== 'closed') e.close();
      } catch {
        // Closing a failed encoder may throw; it is closed regardless
      }
  };

  try {
    venc.configure(plan.video);
    if (aenc) aenc.configure(plan.audio!.config);
    const scaler = new FrameScaler(info.width, info.height, plan.out);
    const scaled = new Uint8Array(scaler.byteLength);
    const resampler = withAudio
      ? new Resampler(info.audioRate, AUDIO_RATE, info.audioChannels)
      : null;
    let audioOut = 0; // 48 kHz frames given to the encoder
    let audioIn = 0; // source frames taken

    // Gives the encoder planar output of the resampler.
    const feedAudio = (planes: Float32Array[]) => {
      const n = planes[0]?.length ?? 0;
      if (!n || !aenc) return;
      const data = new Float32Array(n * planes.length);
      planes.forEach((p, c) => data.set(p, c * n));
      const ad = new codecs.AudioData({
        format: 'f32-planar',
        sampleRate: AUDIO_RATE,
        numberOfFrames: n,
        numberOfChannels: planes.length,
        timestamp: Math.round((audioOut * 1e6) / AUDIO_RATE),
        data,
      });
      aenc.encode(ad);
      ad.close();
      audioOut += n;
    };

    let k = 0;
    for (;;) {
      if (opts.signal?.aborted) throw new ExportCancelled();
      if (failure) throw failure;
      const r = rec.next();
      if (r === NEXT_END) break;
      if (r === NEXT_VIDEO) {
        if (rec.frameChanged() || k === 0) scaler.scale(rec.pixels(), scaled);
        const frame = new codecs.VideoFrame(scaled, {
          format: 'RGBX',
          codedWidth: plan.out.width,
          codedHeight: plan.out.height,
          timestamp: Math.round(k * periodUs),
          duration: Math.round(periodUs),
        });
        venc.encode(frame, { keyFrame: k % gop === 0 });
        frame.close();
        k++;
        opts.onProgress?.(k, info.frames);
        await drain(venc, QUEUE_MAX);
      } else if (r === NEXT_AUDIO && resampler) {
        const pcm = rec.pcm();
        audioIn += pcm.length / info.audioChannels;
        feedAudio(resampler.push(pcm));
        if (aenc) await drain(aenc, QUEUE_MAX);
      }
    }
    if (resampler) feedAudio(resampler.flush());
    await venc.flush();
    if (aenc) await aenc.flush();
    if (failure) throw failure;
    if (opts.signal?.aborted) throw new ExportCancelled();

    let presented = 0;
    if (withAudio && writer.samples(1)) {
      const a = writer.track(1) as AudioTrack;
      presented = resampler!.outputLength(audioIn);
      a.presented = presented;
      // What precedes the sound: Opus' pre-skip, or an encoder's priming
      // it announced with a negative first timestamp
      a.skip =
        a.codec === 'opus'
          ? opusPreSkip(a.description)
          : Math.max(0, Math.round((-(firstAudioTs ?? 0) * AUDIO_RATE) / 1e6));
    } else if (withAudio) {
      // The encoder gave nothing (a silent zero-length stream): no track
      throw new Error('the audio encoder produced no output');
    }
    return { head: writer.finish(), frames: k, audioSamples: presented };
  } finally {
    closeAll();
  }
}

// A BufferSource as bytes (a copy).
function toBytes(b: AllowSharedBufferSource): Uint8Array {
  if (
    b instanceof ArrayBuffer ||
    (typeof SharedArrayBuffer !== 'undefined' && b instanceof SharedArrayBuffer)
  )
    return new Uint8Array(b.slice(0));
  const v = b as ArrayBufferView;
  return new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice();
}
