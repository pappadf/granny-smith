// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The screen recorder's page side: starting and stopping a recording (the
// display toolbar's record button), the options it starts with, and what
// the page offers once one is finished.
//
// A recording is the core's `recorder` object (docs/internals/core/record/
// record.md): `recorder.start()` with no path writes
// /opfs/recordings/<model>-<YYYYmmdd-HHMMSS>.mkv, `recorder.stop()` returns
// once every file is closed.  What the page shows comes from the recorder's
// events (state/recorder.svelte), so a recording started or stopped in the
// terminal looks the same.  Hiding the page finalises the file and showing
// it again continues in the next one (em_main.c, recorder.suspend/resume).

import { gsEval, gsErrorText, isGsError } from './emulator';
import { downloadFiles } from './fsOps';
import { recorder, setRecordingFinishedHandler, type RecorderRate } from '@/state/recorder.svelte';
import { storageHealth } from '@/state/storageHealth.svelte';
import { revealInFilesystem } from '@/state/filesystem.svelte';
import { showNotification, type ToastAction } from '@/state/toasts.svelte';

// Said before a recording starts when the browser's storage is not
// persistent (files.persistent false): the file lives in memory only.
export const VOLATILE_RECORDING_MESSAGE =
  'Browser storage is not persistent: this recording is lost when the page is closed, so download it before then.';

const basename = (p: string): string => p.slice(p.lastIndexOf('/') + 1);

// Start a recording with the chosen options.  The path, or null when the
// core refused (the reason is shown).
export async function startRecording(): Promise<string | null> {
  if (recorder.active) return recorder.path;
  // The options are the core's settings, fixed once a recording runs
  const rate = await gsEval('recorder.rate', [recorder.rate]);
  const audio = await gsEval('recorder.audio', [recorder.audio]);
  const failed = [rate, audio].find(isGsError);
  if (failed) {
    showNotification(`Recording failed to start: ${gsErrorText(failed)}`, 'error');
    return null;
  }
  if (storageHealth.volatile) showNotification(VOLATILE_RECORDING_MESSAGE, 'warning');
  const r = await gsEval('recorder.start');
  if (typeof r !== 'string') {
    showNotification(`Recording failed to start: ${gsErrorText(r)}`, 'error');
    return null;
  }
  return r;
}

// Stop the recording; resolves once its files are closed.  The files are
// offered when the `stopped` event arrives (offerRecording).
export async function stopRecording(): Promise<string[]> {
  const r = await gsEval('recorder.stop');
  if (!Array.isArray(r)) {
    showNotification(`Stopping the recording failed: ${gsErrorText(r)}`, 'error');
    return [];
  }
  return r.filter((p): p is string => typeof p === 'string');
}

// Change an option for the next recording.  The core refuses while one runs,
// and so does the menu.
export function setRecorderRate(rate: RecorderRate): void {
  if (!recorder.active) recorder.rate = rate;
}
export function setRecorderAudio(on: boolean): void {
  if (!recorder.active) recorder.audio = on;
}

// The actions on a finished recording's files, in the order the toast
// shows them.  This is where Export MP4 (a WebCodecs export of the .mkv,
// read from OPFS by a worker) goes, between Download and Show in Files.
export function recordingActions(paths: string[]): ToastAction[] {
  return [
    {
      label: paths.length > 1 ? `Download (${paths.length} files)` : 'Download',
      action: () => void downloadRecording(paths),
    },
    { label: 'Show in Files', action: () => revealInFilesystem(paths[paths.length - 1]) },
  ];
}

// Save each file of a recording to the host, through the Filesystem panel's
// download path.
export async function downloadRecording(paths: string[]): Promise<void> {
  const r = await downloadFiles(paths);
  if (r.failures.length) showNotification(`Download failed: ${r.failures.join(', ')}`, 'error');
}

// A recording ended (stopped, or ended by a failure): say so, and offer its files.
export function offerRecording(paths: string[], error: string): void {
  if (error) showNotification(`Recording stopped: ${error}`, 'error');
  if (!paths.length) return;
  const name =
    paths.length > 1 ? `${basename(paths[0])} and ${paths.length - 1} more` : basename(paths[0]);
  showNotification(`Recording saved: ${name}`, 'info', recordingActions(paths));
}

setRecordingFinishedHandler(offerRecording);
