// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The screen recorder, as the page shows it: whether a recording runs, what
// it has written so far (the status bar's indicator), the options the next
// one starts with (the record button's menu, persisted), and the files the
// last one wrote.
//
// Fed by the core's `recorder` state events (src/core/record/record.c,
// docs/internals/core/record/record.md §4): {state, path, segment, frames,
// seconds, bytes, dropped, external[, message]} with `state` one of
// started, recording (at most once a second while frames are recorded),
// segment (a new file), suspended (the page was hidden: the file is
// closed), error and stopped.  The events are the only source, so a
// recording started or stopped from the terminal shows the same way.

export type RecorderRate = 'full' | 'half' | 'quarter';

export interface RecorderState {
  active: boolean;
  // The page was hidden: the last file is closed until it is shown again.
  suspended: boolean;
  // The file being written (or the last one written).
  path: string;
  // Every file of the recording running now, or of the last one, in order.
  paths: string[];
  frames: number;
  seconds: number; // emulated seconds the frames cover
  bytes: number; // file bytes, every segment
  dropped: number; // frames recorded as a repeat because the recorder was behind
  // The screen is presented by the GPU: frames are taken at half rate at most.
  external: boolean;
  // Why the last recording failed, "" when it did not.
  error: string;
  // The options the next recording starts with.
  rate: RecorderRate;
  audio: boolean;
}

export const recorder: RecorderState = $state({
  active: false,
  suspended: false,
  path: '',
  paths: [],
  frames: 0,
  seconds: 0,
  bytes: 0,
  dropped: 0,
  external: false,
  error: '',
  rate: 'full',
  audio: true,
});

// Who hears about a finished recording (bus/recorder.ts: the toast that
// offers the files).  Set from there, so this store imports no bus code.
let onFinished: ((paths: string[], error: string) => void) | null = null;
export function setRecordingFinishedHandler(
  fn: ((paths: string[], error: string) => void) | null,
): void {
  onFinished = fn;
}

const num = (v: unknown): number => (typeof v === 'number' && Number.isFinite(v) ? v : 0);

// One `recorder` state event.
export function applyRecorderEvent(d: Record<string, unknown>): void {
  const state = typeof d.state === 'string' ? d.state : '';
  const path = typeof d.path === 'string' ? d.path : '';
  recorder.frames = num(d.frames);
  recorder.seconds = num(d.seconds);
  recorder.bytes = num(d.bytes);
  recorder.dropped = num(d.dropped);
  recorder.external = d.external === true;
  if (path) recorder.path = path;
  switch (state) {
    case 'started':
      recorder.active = true;
      recorder.suspended = false;
      recorder.error = '';
      recorder.paths = path ? [path] : [];
      break;
    case 'segment':
      recorder.active = true;
      recorder.suspended = false;
      if (path && !recorder.paths.includes(path)) recorder.paths.push(path);
      break;
    case 'suspended':
      recorder.suspended = true;
      break;
    case 'recording':
      recorder.active = true;
      break;
    case 'error':
      recorder.error =
        typeof d.message === 'string' && d.message ? d.message : 'the recorder failed';
      break;
    case 'stopped': {
      const was = recorder.active;
      recorder.active = false;
      recorder.suspended = false;
      if (was) onFinished?.([...recorder.paths], recorder.error);
      break;
    }
    default:
      break;
  }
}

// "m:ss", or "h:mm:ss" from an hour: the recorded (emulated) time.
export function formatRecordedTime(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const ss = String(s % 60).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`;
}

// Back to the state of a page that has recorded nothing (tests).
export function _resetRecorderForTests(): void {
  Object.assign(recorder, {
    active: false,
    suspended: false,
    path: '',
    paths: [],
    frames: 0,
    seconds: 0,
    bytes: 0,
    dropped: 0,
    external: false,
    error: '',
    rate: 'full',
    audio: true,
  });
  onFinished = null;
}
