// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The MP4 export's worker: it reads a finished recording from OPFS itself
// (navigator.storage, not the emulator), decodes it with the recording
// decoder module (build/recdec.js, loaded on first use), encodes it with
// WebCodecs, and writes the MP4 back to OPFS -- the samples to a scratch
// file as they are encoded, then the final file index first.  It runs
// apart from the emulator, so an export never holds the guest up, and the
// emulator may keep running or record again meanwhile.  The page side is
// recorder/mp4Export.ts.
//
// Messages in: info {moduleUrl, wasmUrl, paths}; export {moduleUrl,
// wasmUrl, jobs: [{src, dst, tmp}], scale}; cancel.
// Messages out: info {files}; progress {file, files, done, total}; done
// {outputs}; failed {message}; cancelled.

import { loadRecdecModule, Recording, type RecdecModule, type RecordingSource } from './recdec';
import { EXPORT_SCALES, outputSize } from './scale';
import { probeAudio, probeVideo } from './probe';
import { encodeRecording, ExportCancelled, webCodecs } from './exportPipeline';
import type { ExportFileInfo, ExportJob, ExportOutput, WorkerIn } from './exportProtocol';
import { getDirAtPath, splitOpfsPath } from '@/lib/opfsDir';

// Worker-only APIs the page's DOM typings lack: OPFS's synchronous access
// handle and FileReaderSync.
interface FileSystemSyncAccessHandle {
  read(buffer: Uint8Array, options?: { at: number }): number;
  write(buffer: Uint8Array, options?: { at: number }): number;
  truncate(size: number): void;
  getSize(): number;
  flush(): void;
  close(): void;
}
declare const FileReaderSync: { new (): { readAsArrayBuffer(blob: Blob): ArrayBuffer } };

let modulePromise: Promise<RecdecModule> | null = null;
let abort: AbortController | null = null;

// The decoder module, fetched once.
function recdec(moduleUrl: string, wasmUrl: string): Promise<RecdecModule> {
  if (!modulePromise) modulePromise = loadRecdecModule(moduleUrl, wasmUrl);
  modulePromise.catch(() => (modulePromise = null));
  return modulePromise;
}

// A recording in OPFS as the module reads it: synchronous slices of the
// File (FileReaderSync), so the emulator's own handles on the file never
// conflict with this one.
async function openSource(path: string): Promise<RecordingSource> {
  const [dir, name] = splitOpfsPath(path);
  const file = await (await (await getDirAtPath(dir)).getFileHandle(name)).getFile();
  const reader = new FileReaderSync();
  return {
    size: file.size,
    read: (at, len) => new Uint8Array(reader.readAsArrayBuffer(file.slice(at, at + len))),
  };
}

// A new file at `path` opened for synchronous writing (emptied), and its directory.
async function createFile(
  path: string,
): Promise<{ dir: FileSystemDirectoryHandle; name: string; h: FileSystemSyncAccessHandle }> {
  const [d, name] = splitOpfsPath(path);
  const dir = await getDirAtPath(d, { create: true });
  const fh = await dir.getFileHandle(name, { create: true });
  const h = await (
    fh as unknown as { createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle> }
  ).createSyncAccessHandle();
  h.truncate(0);
  return { dir, name, h };
}

// Removes `name` from `dir`, quietly.
async function removeQuietly(
  dir: FileSystemDirectoryHandle | undefined,
  name: string,
): Promise<void> {
  try {
    await dir?.removeEntry(name);
  } catch {
    // Already gone
  }
}

// A sink that collects small writes into 1 MB blocks for the sync handle.
class HandleSink {
  private readonly h: FileSystemSyncAccessHandle;
  private readonly block = new Uint8Array(1 << 20);
  private fill = 0;
  private at = 0;
  constructor(h: FileSystemSyncAccessHandle) {
    this.h = h;
  }
  write(bytes: Uint8Array): void {
    if (this.fill + bytes.length > this.block.length) this.flush();
    if (bytes.length > this.block.length) {
      this.put(bytes);
      return;
    }
    this.block.set(bytes, this.fill);
    this.fill += bytes.length;
  }
  flush(): void {
    if (this.fill) this.put(this.block.subarray(0, this.fill));
    this.fill = 0;
  }
  private put(b: Uint8Array): void {
    let done = 0;
    while (done < b.length) done += this.h.write(b.subarray(done), { at: this.at + done });
    this.at += b.length;
  }
}

// What the dialog needs: each file's facts, which sizes this browser can
// encode, and the audio it would get.
async function info(
  moduleUrl: string,
  wasmUrl: string,
  paths: string[],
): Promise<ExportFileInfo[]> {
  const m = await recdec(moduleUrl, wasmUrl);
  const out: ExportFileInfo[] = [];
  for (const path of paths) {
    let rec: Recording | null = null;
    try {
      rec = Recording.open(m, await openSource(path));
      const i = rec.info;
      const fps = 1e9 / (i.periodNs || 16_625_103);
      const sizes = [];
      for (const scale of EXPORT_SCALES) {
        const o = outputSize(i, scale);
        sizes.push({
          scale,
          width: o.width,
          height: o.height,
          ok: !!(await probeVideo(o.width, o.height, fps)),
        });
      }
      const audio = i.audioRate ? ((await probeAudio(i.audioChannels))?.codec ?? null) : null;
      out.push({ path, info: i, sizes, audio });
    } catch (e) {
      out.push({ path, error: e instanceof Error ? e.message : String(e), sizes: [], audio: null });
    } finally {
      rec?.close();
    }
  }
  return out;
}

// Exports one recording; the output's facts.
async function exportOne(
  m: RecdecModule,
  job: ExportJob,
  scale: number,
  signal: AbortSignal,
  progress: (done: number, total: number) => void,
): Promise<ExportOutput> {
  const rec = Recording.open(m, await openSource(job.src));
  let tmp: Awaited<ReturnType<typeof createFile>> | null = null;
  let dst: Awaited<ReturnType<typeof createFile>> | null = null;
  try {
    const i = rec.info;
    const out = outputSize(i, scale);
    const fps = 1e9 / i.periodNs;
    const video = await probeVideo(out.width, out.height, fps);
    if (!video) throw new Error(`this browser cannot encode H.264 at ${out.width}x${out.height}`);
    const audio = i.audioRate ? await probeAudio(i.audioChannels) : null;
    tmp = await createFile(job.tmp);
    const sink = new HandleSink(tmp.h);
    const r = await encodeRecording(rec, {
      plan: { video, audio, out },
      sink,
      codecs: webCodecs(),
      onProgress: progress,
      signal,
    });
    sink.flush();
    // The final file: the header, then the samples copied over
    dst = await createFile(job.dst);
    let at = 0;
    while (at < r.head.head.length) at += dst.h.write(r.head.head.subarray(at), { at });
    const buf = new Uint8Array(4 << 20);
    for (let off = 0; off < r.head.payload;) {
      if (signal.aborted) throw new ExportCancelled();
      const n = tmp.h.read(buf.subarray(0, Math.min(buf.length, r.head.payload - off)), {
        at: off,
      });
      if (n <= 0) throw new Error('the scratch file is shorter than what was written to it');
      let w = 0;
      while (w < n) w += dst.h.write(buf.subarray(w, n), { at: at + w });
      at += n;
      off += n;
    }
    dst.h.flush();
    const bytes = dst.h.getSize();
    dst.h.close();
    dst = null; // complete: no longer removed on a failure
    return {
      src: job.src,
      dst: job.dst,
      bytes,
      frames: r.frames,
      width: out.width,
      height: out.height,
      audio: audio && i.audioRate ? audio.codec : null,
      hadAudio: i.audioRate > 0,
    };
  } catch (e) {
    // A partial result goes: the final file, if begun
    if (dst) {
      try {
        dst.h.close();
      } catch {
        // Closed already
      }
      await removeQuietly(dst.dir, dst.name);
    }
    throw e;
  } finally {
    rec.close();
    if (tmp) {
      try {
        tmp.h.close();
      } catch {
        // Closed already
      }
      await removeQuietly(tmp.dir, tmp.name);
    }
  }
}

// Runs the jobs one after the other.
async function runExport(msg: Extract<WorkerIn, { type: 'export' }>): Promise<void> {
  abort = new AbortController();
  const signal = abort.signal;
  const outputs: ExportOutput[] = [];
  try {
    const m = await recdec(msg.moduleUrl, msg.wasmUrl);
    for (let f = 0; f < msg.jobs.length; f++) {
      let last = 0;
      outputs.push(
        await exportOne(m, msg.jobs[f], msg.scale, signal, (done, total) => {
          // At most ten progress messages a second, and the last frame
          const now = Date.now();
          if (done === total || now - last >= 100) {
            last = now;
            postMessage({ type: 'progress', file: f, files: msg.jobs.length, done, total });
          }
        }),
      );
    }
    postMessage({ type: 'done', outputs });
  } catch (e) {
    // Files of earlier jobs stay only when the whole export finished
    for (const o of outputs) {
      const [d, n] = splitOpfsPath(o.dst);
      await removeQuietly(await getDirAtPath(d).catch(() => undefined), n);
    }
    if (e instanceof ExportCancelled || signal.aborted) postMessage({ type: 'cancelled' });
    else {
      console.error('[export] failed:', e);
      postMessage({ type: 'failed', message: e instanceof Error ? e.message : String(e) });
    }
  } finally {
    abort = null;
  }
}

self.onmessage = (ev: MessageEvent<WorkerIn>) => {
  const msg = ev.data;
  if (msg.type === 'info') {
    info(msg.moduleUrl, msg.wasmUrl, msg.paths).then(
      (files) => postMessage({ type: 'info', files }),
      (e: unknown) =>
        postMessage({ type: 'failed', message: `the decoder failed to load: ${String(e)}` }),
    );
  } else if (msg.type === 'export') {
    void runExport(msg);
  } else if (msg.type === 'cancel') {
    abort?.abort();
  }
};
