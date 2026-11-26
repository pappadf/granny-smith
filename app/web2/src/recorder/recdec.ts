// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The MP4 export's decoder, as the worker sees it: the standalone,
// non-threaded Emscripten module `make recdec-module` links
// (build/recdec.js + recdec.wasm, from src/platform/wasm/recdec_main.c:
// the emulator's own Matroska reader and its ZMBV and FLAC decoders), and a
// thin wrapper over its C API.  The module has no file of its own: each
// read it makes calls back into JS (Module.recdecRead), which a
// RecordingSource answers -- the worker's from the OPFS file, the tests'
// from a Node file.  Every call that can allocate may grow the module's
// memory, so the heap views are re-read from the module after each call.

// The module surface the wrapper uses (RECDEC_MODULE_EXPORTS and
// EXPORTED_RUNTIME_METHODS in the Makefile's recdec-module target).
export interface RecdecModule {
  HEAPU8: Uint8Array;
  HEAP16: Int16Array;
  UTF8ToString(ptr: number): string;
  recdecRead?: (source: number, at: number, len: number) => Uint8Array;
  _recdec_open(source: number, size: number): number;
  _recdec_last_error(): number;
  _recdec_width(d: number): number;
  _recdec_height(d: number): number;
  _recdec_display_width(d: number): number;
  _recdec_display_height(d: number): number;
  _recdec_period_ns(d: number): number;
  _recdec_frames_total(d: number): number;
  _recdec_truncated(d: number): number;
  _recdec_audio_rate(d: number): number;
  _recdec_audio_channels(d: number): number;
  _recdec_audio_packets(d: number): number;
  _recdec_next(d: number): number;
  _recdec_frame_changed(d: number): number;
  _recdec_rgba(d: number): number;
  _recdec_pcm(d: number): number;
  _recdec_pcm_frames(d: number): number;
  _recdec_message(d: number): number;
  _recdec_close(d: number): void;
}

interface RecdecModuleConfig {
  locateFile?(path: string): string;
  print?(s: string): void;
  printErr?(s: string): void;
}

type RecdecModuleFactory = (config: RecdecModuleConfig) => Promise<RecdecModule>;

// A file the module reads: its size, and a synchronous read of up to `len`
// bytes at `at` (fewer only at the end).
export interface RecordingSource {
  size: number;
  read(at: number, len: number): Uint8Array;
}

// What a recording holds, from its header and a scan of its packets.
export interface RecordingInfo {
  width: number; // the stored picture
  height: number;
  displayWidth: number; // the shape to show it at (the stored size when square)
  displayHeight: number;
  periodNs: number; // the frame period (DefaultDuration)
  frames: number; // video frames in the file
  audioRate: number; // 0 with no audio track
  audioChannels: number;
  truncated: boolean; // the file ends inside a cluster (never closed)
}

// What recdec_next decoded.
export const NEXT_END = 0;
export const NEXT_VIDEO = 1;
export const NEXT_AUDIO = 2;

// Fetches and instantiates the module.  `moduleUrl` is the ES6 glue
// (cache-busted by the page like main.mjs), `wasmUrl` where its binary is.
export async function loadRecdecModule(moduleUrl: string, wasmUrl: string): Promise<RecdecModule> {
  const mod = (await import(/* @vite-ignore */ moduleUrl)) as { default: RecdecModuleFactory };
  const m = await mod.default({
    locateFile: (p: string) => (p.endsWith('.wasm') ? wasmUrl : p),
    print: (s: string) => console.log('[recdec]', s),
    printErr: (s: string) => console.warn('[recdec]', s),
  });
  installReader(m);
  return m;
}

// The sources the module may read, by number.
const sources = new Map<number, RecordingSource>();
let nextSource = 1;

// Points the module's read callback at the source table.
function installReader(m: RecdecModule): void {
  m.recdecRead = (source: number, at: number, len: number): Uint8Array => {
    const s = sources.get(source);
    if (!s) return new Uint8Array(0);
    return s.read(at, Math.min(len, Math.max(0, s.size - at)));
  };
}

// An open recording: its facts, then frames and audio blocks in file order.
export class Recording {
  readonly info: RecordingInfo;
  private readonly m: RecdecModule;
  private d: number;
  private readonly source: number;

  private constructor(m: RecdecModule, d: number, source: number) {
    this.m = m;
    this.d = d;
    this.source = source;
    this.info = {
      width: m._recdec_width(d),
      height: m._recdec_height(d),
      displayWidth: m._recdec_display_width(d),
      displayHeight: m._recdec_display_height(d),
      periodNs: m._recdec_period_ns(d),
      frames: m._recdec_frames_total(d),
      audioRate: m._recdec_audio_rate(d),
      audioChannels: m._recdec_audio_channels(d),
      truncated: m._recdec_truncated(d) !== 0,
    };
  }

  // Opens `src`; throws with the module's reason when it is not a recording.
  static open(m: RecdecModule, src: RecordingSource): Recording {
    if (!m.recdecRead) installReader(m);
    const source = nextSource++;
    sources.set(source, src);
    const d = m._recdec_open(source, src.size);
    if (!d) {
      sources.delete(source);
      throw new Error(m.UTF8ToString(m._recdec_last_error()) || 'cannot open the recording');
    }
    return new Recording(m, d, source);
  }

  // Decodes the next packet: NEXT_VIDEO, NEXT_AUDIO or NEXT_END; throws on a
  // decoding failure.
  next(): number {
    const r = this.m._recdec_next(this.d);
    if (r < 0) throw new Error(this.m.UTF8ToString(this.m._recdec_message(this.d)));
    return r;
  }

  // Whether the frame just decoded differs from the one before it.
  frameChanged(): boolean {
    return this.m._recdec_frame_changed(this.d) !== 0;
  }

  // The frame just decoded, RGBA (alpha 255): a view of the module's
  // memory, valid until the next call.
  pixels(): Uint8Array {
    const p = this.m._recdec_rgba(this.d);
    if (!p) throw new Error('no frame decoded');
    return this.m.HEAPU8.subarray(p, p + this.info.width * this.info.height * 4);
  }

  // The audio block just decoded, interleaved: a view of the module's
  // memory, valid until the next call.
  pcm(): Int16Array {
    const p = this.m._recdec_pcm(this.d);
    const n = this.m._recdec_pcm_frames(this.d) * this.info.audioChannels;
    return this.m.HEAP16.subarray(p >> 1, (p >> 1) + n);
  }

  close(): void {
    if (!this.d) return;
    this.m._recdec_close(this.d);
    this.d = 0;
    sources.delete(this.source);
  }
}
