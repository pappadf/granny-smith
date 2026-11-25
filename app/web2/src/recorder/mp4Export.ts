// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The page side of the MP4 export: whether it is offered (the WebCodecs
// probe, recorder/probe.ts), the export dialog's steps, and the worker
// that does the work (recorder/export.worker.ts), started on first use.
// "Export MP4…" sits in a finished recording's toast
// (bus/recorder.ts recordingActions) and on a .mkv in the Filesystem
// panel; both open the dialog here.  Each recording -- each segment of a
// recording that spans several files -- becomes `<name>.mp4` beside it in
// /opfs/recordings/, offered for download when done.

import { exportSupport, type ExportSupport } from './probe';
import type { ExportJob, ExportOutput, WorkerIn, WorkerOut } from './exportProtocol';
import { mp4Export } from '@/state/mp4Export.svelte';
import { recorder } from '@/state/recorder.svelte';
import { showNotification } from '@/state/toasts.svelte';
import { revealInFilesystem } from '@/state/filesystem.svelte';
import { downloadFiles } from '@/bus/fsOps';
import { scratchPath } from '@/lib/opfsPaths';

const basename = (p: string): string => p.slice(p.lastIndexOf('/') + 1);

// The .mp4 written for a recording: its name with the extension replaced.
export function mp4PathFor(path: string): string {
  return /\.mkv$/i.test(path) ? path.replace(/\.mkv$/i, '.mp4') : `${path}.mp4`;
}

// Runs the probe once and keeps its answer in the store, so the toast and
// the menu can say at once whether the export is there.
export async function probeExportSupport(): Promise<ExportSupport> {
  const s = await exportSupport();
  mp4Export.support = s;
  return s;
}

// The state of the "Export MP4…" action: disabled, with the reason as its
// tooltip, when the probe said no; enabled before the probe has answered
// (the dialog then says why, if it must).
export function exportActionState(): { disabled: boolean; title: string } {
  const s = mp4Export.support;
  if (s && !s.ok) return { disabled: true, title: s.reason };
  return {
    disabled: false,
    title: 'Convert to an MP4 (H.264) that plays in browsers and on phones',
  };
}

let worker: Worker | null = null;

// The worker, started on first use.
function ensureWorker(): Worker {
  if (worker) return worker;
  worker = new Worker(new URL('./export.worker.ts', import.meta.url), {
    type: 'module',
    name: 'mp4-export',
  });
  worker.onmessage = (ev: MessageEvent<WorkerOut>) => onWorkerMessage(ev.data);
  worker.onerror = (e) => {
    console.warn('[export] worker error', e.message);
    fail(`the export worker failed: ${e.message || 'unknown error'}`);
    worker = null;
  };
  return worker;
}

// The decoder module's URLs, cache-busted the way main.mjs is.
function moduleUrls(): { moduleUrl: string; wasmUrl: string } {
  const bust = Date.now();
  return {
    moduleUrl: new URL(`recdec.js?v=${bust}`, document.baseURI).href,
    wasmUrl: new URL(`recdec.wasm?v=${bust}`, document.baseURI).href,
  };
}

function post(msg: WorkerIn): void {
  ensureWorker().postMessage(msg);
}

function fail(message: string): void {
  mp4Export.phase = 'failed';
  mp4Export.error = message;
  mp4Export.open = true;
}

// Opens the dialog for `paths` (.mkv files).  An export already running
// shows its progress instead.
export async function openExportDialog(paths: string[]): Promise<void> {
  if (mp4Export.phase === 'running') {
    mp4Export.open = true;
    return;
  }
  // The file a recording is writing now is not finished: it cannot be exported
  const writing = recorder.active && !recorder.suspended ? recorder.path : '';
  if (writing && paths.includes(writing)) {
    showNotification(
      `${basename(writing)} is still being recorded: stop the recording, then export it.`,
      'warning',
    );
    return;
  }
  Object.assign(mp4Export, {
    open: true,
    phase: 'loading',
    paths: [...paths],
    files: [],
    scale: 1,
    file: 0,
    done: 0,
    total: 0,
    error: '',
  });
  const s = await probeExportSupport();
  if (!s.ok) {
    fail(s.reason);
    return;
  }
  try {
    post({ type: 'info', ...moduleUrls(), paths: [...paths] });
  } catch (e) {
    fail(`the export worker cannot start: ${String(e)}`);
  }
}

// Starts the export at the chosen size.
export function startExport(): void {
  if (mp4Export.phase !== 'choose') return;
  const jobs: ExportJob[] = mp4Export.paths.map((src) => ({
    src,
    dst: mp4PathFor(src),
    tmp: scratchPath(`${basename(src)}.mdat`),
  }));
  Object.assign(mp4Export, { phase: 'running', file: 0, done: 0, total: 0, error: '' });
  post({ type: 'export', ...moduleUrls(), jobs, scale: mp4Export.scale });
}

// Cancels a running export (the worker deletes what it wrote).
export function cancelExport(): void {
  if (mp4Export.phase === 'running') worker?.postMessage({ type: 'cancel' } satisfies WorkerIn);
  else closeExportDialog();
}

// Closes the dialog; a running export carries on, and says when it is done.
export function closeExportDialog(): void {
  mp4Export.open = false;
  if (mp4Export.phase !== 'running') mp4Export.phase = 'idle';
}

// Offers the files an export wrote.
function finished(outputs: ExportOutput[]): void {
  mp4Export.open = false;
  mp4Export.phase = 'idle';
  if (!outputs.length) return;
  const dsts = outputs.map((o) => o.dst);
  const name =
    dsts.length > 1 ? `${basename(dsts[0])} and ${dsts.length - 1} more` : basename(dsts[0]);
  const silent = outputs.some((o) => o.hadAudio && !o.audio);
  const note = silent ? ' (video only: this browser has no AAC or Opus encoder)' : '';
  showNotification(`Exported: ${name}${note}`, 'info', [
    {
      label: dsts.length > 1 ? `Download (${dsts.length} files)` : 'Download',
      action: () =>
        void downloadFiles(dsts).then((r) => {
          if (r.failures.length)
            showNotification(`Download failed: ${r.failures.join(', ')}`, 'error');
        }),
    },
    { label: 'Show in Files', action: () => revealInFilesystem(dsts[dsts.length - 1]) },
  ]);
}

function onWorkerMessage(m: WorkerOut): void {
  switch (m.type) {
    case 'info': {
      if (mp4Export.phase !== 'loading') return;
      mp4Export.files = m.files;
      const bad = m.files.find((f) => f.error);
      if (bad) {
        fail(`${basename(bad.path)}: ${bad.error}`);
        return;
      }
      // The native size when it can be encoded, else the first size that can
      const first = m.files[0]?.sizes ?? [];
      const ok = first.find((s) => s.ok);
      if (!ok) {
        fail('This browser has no H.264 encoder that takes this recording’s size.');
        return;
      }
      mp4Export.scale = ok.scale;
      mp4Export.phase = 'choose';
      return;
    }
    case 'progress':
      mp4Export.file = m.file;
      mp4Export.done = m.done;
      mp4Export.total = m.total;
      return;
    case 'done':
      finished(m.outputs);
      return;
    case 'cancelled':
      mp4Export.open = false;
      mp4Export.phase = 'idle';
      showNotification('MP4 export cancelled', 'info');
      return;
    case 'failed':
      fail(m.message);
      return;
  }
}

// For the tests: drop the worker.
export function _resetMp4ExportWorkerForTests(): void {
  worker?.terminate();
  worker = null;
}
