// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// What this browser can encode for the MP4 export, asked of WebCodecs at
// run time (VideoEncoder.isConfigSupported / AudioEncoder.isConfigSupported
// with the exact configurations the export uses), never inferred from a
// version string.  Used on the page (whether "Export MP4…" is offered) and
// in the export worker (the configurations it encodes with).
//
// Video is H.264, High profile, the level the frame size and rate need,
// variable bitrate with a ceiling of 0.2 bits per pixel per frame; when the
// default (often a hardware encoder) refuses, the probe is repeated with
// `hardwareAcceleration: "prefer-software"`, and a browser without High
// profile is tried at Main and then Constrained Baseline.  Audio is AAC-LC
// at 48 kHz (128 kb/s mono, 192 kb/s stereo); without an AAC encoder, Opus
// at the same rate and bitrate; without either the export is video only.

// The ffmpeg command for when the browser cannot export (as the user guide gives it).
export const FFMPEG_COMMAND =
  'ffmpeg -i recording.mkv -vf scale=iw*4:ih*4:flags=neighbor -c:v libx264 -crf 16 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart recording.mp4';

export const AUDIO_RATE = 48000;

// H.264 levels: level_idc, the largest frame in macroblocks, macroblocks a
// second (ITU-T H.264 Table A-1).
const LEVELS: [number, number, number][] = [
  [10, 99, 1485],
  [11, 396, 3000],
  [12, 396, 6000],
  [13, 396, 11880],
  [20, 396, 11880],
  [21, 792, 19800],
  [22, 1620, 20250],
  [30, 1620, 40500],
  [31, 3600, 108000],
  [32, 5120, 216000],
  [40, 8192, 245760],
  [41, 8192, 245760],
  [42, 8704, 522240],
  [50, 22080, 589824],
  [51, 36864, 983040],
  [52, 36864, 2073600],
  [60, 139264, 4177920],
  [61, 139264, 8355840],
  [62, 139264, 16711680],
];

// The lowest level that holds `width` x `height` at `fps`.
export function h264Level(width: number, height: number, fps: number): number {
  const mbs = Math.ceil(width / 16) * Math.ceil(height / 16);
  for (const [level, maxFs, maxMbps] of LEVELS)
    if (mbs <= maxFs && mbs * fps <= maxMbps) return level;
  return 62;
}

// The codec strings tried, best first: High, Main, Constrained Baseline.
export function avcCodecs(width: number, height: number, fps: number): string[] {
  const lvl = h264Level(width, height, fps).toString(16).padStart(2, '0');
  return [`avc1.6400${lvl}`, `avc1.4d00${lvl}`, `avc1.42e0${lvl}`];
}

// The bitrate ceiling: 0.2 bits per pixel per frame (a static screen uses a
// small fraction of it).
export function videoBitrate(width: number, height: number, fps: number): number {
  return Math.round(0.2 * width * height * fps);
}

// The video configuration the export encodes with, for a frame of
// `width` x `height` at `fps`; null when this browser has no H.264 encoder
// that takes it.
export async function probeVideo(
  width: number,
  height: number,
  fps: number,
): Promise<VideoEncoderConfig | null> {
  const VE = (globalThis as { VideoEncoder?: typeof VideoEncoder }).VideoEncoder;
  if (!VE?.isConfigSupported) return null;
  for (const codec of avcCodecs(width, height, fps)) {
    for (const hardwareAcceleration of ['no-preference', 'prefer-software'] as const) {
      const config: VideoEncoderConfig = {
        codec,
        width,
        height,
        framerate: fps,
        bitrate: videoBitrate(width, height, fps),
        bitrateMode: 'variable',
        latencyMode: 'quality',
        hardwareAcceleration,
        avc: { format: 'avc' },
      };
      try {
        const r = await VE.isConfigSupported(config);
        if (r.supported) return config;
      } catch {
        // A configuration the browser cannot parse: the next one
      }
    }
  }
  return null;
}

export type AudioCodec = 'aac' | 'opus';

export interface AudioPlan {
  codec: AudioCodec;
  config: AudioEncoderConfig;
}

// The audio configuration for `channels`, AAC first, then Opus; null when
// neither encoder is there.
export async function probeAudio(channels: number): Promise<AudioPlan | null> {
  const AE = (globalThis as { AudioEncoder?: typeof AudioEncoder }).AudioEncoder;
  if (!AE?.isConfigSupported) return null;
  const bitrate = channels > 1 ? 192_000 : 128_000;
  const tries: AudioPlan[] = [
    {
      codec: 'aac',
      config: {
        codec: 'mp4a.40.2',
        sampleRate: AUDIO_RATE,
        numberOfChannels: channels,
        bitrate,
        aac: { format: 'aac' },
      } as AudioEncoderConfig,
    },
    {
      codec: 'opus',
      config: {
        codec: 'opus',
        sampleRate: AUDIO_RATE,
        numberOfChannels: channels,
        bitrate,
        opus: { format: 'opus' },
      } as AudioEncoderConfig,
    },
  ];
  for (const t of tries) {
    try {
      const r = await AE.isConfigSupported(t.config);
      if (r.supported) return t;
    } catch {
      // Not this one
    }
  }
  return null;
}

// Whether the export is offered at all, and what it will make: the probe
// at a typical size (a 640x480 screen at 60.15 fps).
export interface ExportSupport {
  ok: boolean;
  // Why not, when not: said by the disabled menu item.
  reason: string;
  // The audio it would encode, null for video only.
  audio: AudioCodec | null;
}

export const NO_ENCODER_REASON =
  'This browser cannot encode H.264 video (no WebCodecs VideoEncoder for it), so it cannot export MP4. ' +
  'Download the .mkv and convert it with ffmpeg instead: ' +
  FFMPEG_COMMAND;

let cached: Promise<ExportSupport> | null = null;

// The probe, made once per page (or worker).
export function exportSupport(): Promise<ExportSupport> {
  if (!cached)
    cached = (async () => {
      const video = await probeVideo(640, 480, 1203 / 20);
      if (!video) return { ok: false, reason: NO_ENCODER_REASON, audio: null };
      const audio = await probeAudio(1);
      return { ok: true, reason: '', audio: audio?.codec ?? null };
    })();
  return cached;
}

// For the tests: probe again.
export function _resetExportSupportForTests(): void {
  cached = null;
}
