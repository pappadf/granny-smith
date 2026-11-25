// @vitest-environment node
//
// The MP4 export end to end under Node, checked by ffmpeg: Node has no
// WebCodecs, so the encoders are stand-ins that encode with ffmpeg
// (tests/helpers/ffmpegCodecs.ts) and hand back chunks shaped as a
// browser's are.  Everything else is the export's own code: a recording
// the headless emulator makes (--record of a Plus boot, beep included),
// decoded by the recording decoder module, scaled, resampled to 48 kHz,
// paced and muxed by the MP4 writer.  The file is then held to what a
// player needs: ffmpeg decodes it without a word, at a constant 1203/20
// fps with every frame, the sound as long as the picture, the beep at its
// own time, the frames close to the recording's.  A second test feeds the
// writer the packets of an ffmpeg-made MP4 (H.264 with B-frames, AAC) and
// requires the same decoded frames and samples as ffmpeg's own file.
// Skipped (loudly) without ffmpeg, the module, the headless build or the ROM.

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { Recording, NEXT_END, NEXT_VIDEO, type RecdecModule } from '@/recorder/recdec';
import { encodeRecording, type ExportCodecs } from '@/recorder/exportPipeline';
import { outputSize } from '@/recorder/scale';
import { Mp4Writer, type Track } from '@/recorder/mp4Writer';
import { AUDIO_RATE, avcCodecs, videoBitrate } from '@/recorder/probe';
import {
  fileSource,
  fixtureMissing,
  loadRecdec,
  makeRecording,
  readWav,
  scratchDir,
  type Fixture,
} from '../helpers/recordingFixture';
import {
  FakeAudioData,
  FakeVideoFrame,
  ffmpegAudioEncoder,
  ffmpegVideoEncoder,
  ffprobe,
  hasFfmpeg,
  probePackets,
  run,
} from '../helpers/ffmpegCodecs';

const missing = hasFfmpeg() ? fixtureMissing() : 'ffmpeg / ffprobe';
if (missing) console.warn(`mp4Export.ffmpeg.test: skipped, ${missing} not found`);

// A sink that keeps everything.
function memorySink() {
  const parts: Uint8Array[] = [];
  return {
    write: (b: Uint8Array) => parts.push(b.slice()),
    bytes: () => Buffer.concat(parts),
  };
}

// ffmpeg's warnings on decoding `file` completely (none is the goal).
function decodeWarnings(file: string): string {
  return run('ffmpeg', ['-v', 'warning', '-i', file, '-f', 'null', '-']).stderr.trim();
}

// The decoded sound of `file`, mono s16 at 48 kHz.
function decodedAudio(file: string): Int16Array {
  const b = run('ffmpeg', [
    '-v',
    'error',
    '-i',
    file,
    '-map',
    '0:a',
    '-f',
    's16le',
    '-ac',
    '1',
    '-',
  ]).stdout;
  return new Int16Array(b.buffer, b.byteOffset, b.length / 2);
}

// The first sample louder than a quarter of the peak, in seconds.
function onset(s: Int16Array | Float32Array, rate: number): number {
  let peak = 0;
  for (const v of s) peak = Math.max(peak, Math.abs(v));
  for (let i = 0; i < s.length; i++) if (Math.abs(s[i]) > peak / 4) return i / rate;
  return -1;
}

// The decoded frames' MD5s (framemd5's last column) of `file`'s video.
function frameHashes(file: string): string[] {
  return run('ffmpeg', ['-v', 'error', '-i', file, '-map', '0:v', '-f', 'framemd5', '-'])
    .stdout.toString()
    .split('\n')
    .filter((l) => l && !l.startsWith('#'))
    .map((l) => l.split(',').pop()!.trim());
}

describe.skipIf(!!missing)('MP4 export through ffmpeg-backed encoders', () => {
  let m: RecdecModule;
  let fx: Fixture;
  let dir: string;

  beforeAll(async () => {
    m = await loadRecdec();
    dir = scratchDir('mp4export-');
    fx = makeRecording(dir, 2_000_000, [-1]);
  }, 60_000);

  afterAll(() => {
    if (dir) rmSync(dir, { recursive: true, force: true });
  });

  // Exports the fixture with `audio` ('aac' or 'opus'); the file's path and the log.
  async function exportWith(audio: 'aac' | 'opus') {
    const rec = Recording.open(m, fileSource(fx.mkv));
    const out = outputSize(rec.info, 1);
    const fps = 1e9 / rec.info.periodNs;
    const vlog = { configs: [] as VideoEncoderConfig[], keyRequests: [] as number[] };
    const alog = { configs: [] as AudioEncoderConfig[] };
    const codecs = {
      VideoEncoder: ffmpegVideoEncoder(dir, vlog),
      AudioEncoder: ffmpegAudioEncoder(dir, alog),
      VideoFrame: FakeVideoFrame,
      AudioData: FakeAudioData,
    } as unknown as ExportCodecs;
    const sink = memorySink();
    const progress: number[] = [];
    const r = await encodeRecording(rec, {
      plan: {
        video: {
          codec: avcCodecs(out.width, out.height, fps)[0],
          width: out.width,
          height: out.height,
          framerate: fps,
          bitrate: videoBitrate(out.width, out.height, fps),
        },
        audio: {
          codec: audio,
          config: {
            codec: audio === 'aac' ? 'mp4a.40.2' : 'opus',
            sampleRate: AUDIO_RATE,
            numberOfChannels: 1,
            bitrate: 128_000,
          },
        },
        out,
      },
      sink,
      codecs,
      onProgress: (done) => progress.push(done),
    });
    rec.close();
    const file = join(dir, `export-${audio}.mp4`);
    writeFileSync(file, Buffer.concat([Buffer.from(r.head.head), sink.bytes()]));
    return { file, r, vlog, progress, out };
  }

  for (const audio of ['aac', 'opus'] as const)
    it(`makes a clean, constant-rate MP4 with ${audio === 'aac' ? 'AAC' : 'Opus'} sound in sync`, async () => {
      const { file, r, vlog, progress } = await exportWith(audio);
      expect(r.frames).toBe(fx.frames);
      expect(progress[progress.length - 1]).toBe(fx.frames);
      // A keyframe asked for every two seconds (120 frames at 60.15 fps)
      expect(vlog.keyRequests.slice(0, 3)).toEqual([0, 120, 240].filter((k) => k < fx.frames));

      expect(decodeWarnings(file)).toBe('');
      const info = ffprobe(file, '-show_streams', '-show_format', '-count_frames') as {
        streams: Record<string, string | number>[];
        format: Record<string, string>;
      };
      const [v, a] = info.streams;
      expect(v.codec_name).toBe('h264');
      expect([v.width, v.height]).toEqual([512, 342]);
      expect(v.r_frame_rate).toBe('1203/20');
      expect(v.avg_frame_rate).toBe('1203/20');
      expect(Number(v.nb_read_frames)).toBe(fx.frames);
      const videoSeconds = (fx.frames * 20) / 1203;
      expect(Number(v.duration)).toBeCloseTo(videoSeconds, 2);
      expect(a.codec_name).toBe(audio);
      expect(Number(a.sample_rate)).toBe(48000);
      expect(Number(a.channels)).toBe(1);
      // The sound is as long as the picture: the recording's own sound, resampled
      const src = readWav(fx.wav);
      expect(r.audioSamples).toBe(Math.round((src.samples.length * 48000) / 22255));
      expect(Math.abs(r.audioSamples / 48000 - videoSeconds)).toBeLessThan(1 / 1000);
      const pcm = decodedAudio(file);
      expect(Math.abs(pcm.length - r.audioSamples)).toBeLessThanOrEqual(
        audio === 'opus' ? 960 : 1024,
      );
      // The boot beep at its own time
      const want = onset(src.samples, 22255);
      expect(want).toBeGreaterThan(0);
      expect(Math.abs(onset(pcm, 48000) - want)).toBeLessThan(0.005);
      // The last frame decoded close to the recording's (a lossy codec)
      const last = run('ffmpeg', [
        '-v',
        'error',
        '-i',
        file,
        '-vf',
        `select=eq(n\\,${fx.frames - 1})`,
        '-frames:v',
        '1',
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgba',
        '-',
      ]).stdout;
      const rec = Recording.open(m, fileSource(fx.mkv));
      let ref: Uint8Array | null = null;
      for (let x = rec.next(); x !== NEXT_END; x = rec.next())
        if (x === NEXT_VIDEO) ref = rec.pixels().slice();
      rec.close();
      expect(last.length).toBe(ref!.length);
      let diff = 0;
      for (let i = 0; i < last.length; i++) if (i % 4 !== 3) diff += Math.abs(last[i] - ref![i]);
      expect(diff / ((last.length * 3) / 4)).toBeLessThan(4);
    }, 120_000);

  it('rewraps an ffmpeg-made H.264 (B-frames) + AAC stream to the same decoded frames and samples', () => {
    const src = join(dir, 'ffmpeg.mp4');
    run('ffmpeg', [
      '-v',
      'error',
      '-y',
      '-f',
      'lavfi',
      '-i',
      'testsrc2=s=322x242:r=1203/20:d=3',
      '-f',
      'lavfi',
      '-i',
      'sine=f=660:r=48000:d=3',
      '-c:v',
      'libx264',
      '-bf',
      '3',
      '-g',
      '120',
      '-pix_fmt',
      'yuv420p',
      '-c:a',
      'aac',
      '-ac',
      '1',
      '-b:a',
      '128k',
      src,
    ]);
    const { streams, packets } = probePackets(src);
    const tracks: Track[] = [
      { kind: 'video', width: 322, height: 242, timescale: 1203, avcC: streams[0].extradata },
      {
        kind: 'audio',
        codec: 'aac',
        sampleRate: 48000,
        channels: 1,
        timescale: 48000,
        bitrate: 128000,
        description: streams[1].extradata,
      },
    ];
    const sink = memorySink();
    const w = new Mp4Writer(tracks, sink);
    let firstAudio: number | null = null;
    let audioMedia = 0;
    for (const p of packets) {
      if (p.stream === 0) {
        w.addSample(0, p.data, {
          pts: Math.round((p.pts * 1203) / 20) * 20,
          duration: 20,
          key: p.key,
        });
      } else {
        firstAudio ??= p.pts;
        const d = Math.round(p.duration * 48000);
        audioMedia += d;
        w.addSample(1, p.data, { pts: 0, duration: d, key: true });
      }
    }
    const a = w.track(1) as Track & { skip?: number; presented?: number };
    a.skip = Math.round(-firstAudio! * 48000);
    a.presented = 3 * 48000;
    const r = w.finish();
    const out = join(dir, 'rewrapped.mp4');
    writeFileSync(out, Buffer.concat([Buffer.from(r.head), sink.bytes()]));
    expect(a.skip).toBe(1024);
    expect(audioMedia).toBeGreaterThan(3 * 48000);

    expect(decodeWarnings(out)).toBe('');
    // B-frames: the writer needed a composition-offset table
    expect(readFileSync(out).includes(Buffer.from('ctts'))).toBe(true);
    const info = ffprobe(out, '-show_streams', '-count_frames') as {
      streams: Record<string, string>[];
    };
    expect(info.streams[0].r_frame_rate).toBe('1203/20');
    expect(info.streams[0].avg_frame_rate).toBe('1203/20');
    expect(Number(info.streams[0].nb_read_frames)).toBe(
      packets.filter((p) => p.stream === 0).length,
    );
    expect(Number(info.streams[0].start_time)).toBe(0);
    expect(Number(info.streams[1].start_time)).toBe(0);
    // Frame for frame and sample for sample what ffmpeg's own file decodes to
    expect(frameHashes(out)).toEqual(frameHashes(src));
    const mine = decodedAudio(out);
    const theirs = decodedAudio(src);
    expect(mine.length).toBe(theirs.length);
    expect(
      Buffer.from(mine.buffer, mine.byteOffset, mine.byteLength).equals(
        Buffer.from(theirs.buffer, theirs.byteOffset, theirs.byteLength),
      ),
    ).toBe(true);
  }, 60_000);
});
