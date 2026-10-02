// In-progress activity tracker. Surfaces a one-line "<verb>: <name>" hint
// with a spinner in the status bar while a long operation is in flight — an
// upload (OPFS write + validation + persist) or a Filesystem-tab worker op
// (copy out of an image, delete, unpack, move, download). Cleared when the op
// returns, success or failure. Single-slot — concurrent ops are rare in this
// UI; if several kick off at once, the last to start wins the slot.

interface ActivityState {
  current: string | null;
  verb: string;
  // A progress line ("read 120 MB of 2.0 GB, stored 4.1 MB"), or ''.
  detail: string;
  // Set while the activity can be cancelled (a streamed image import).
  cancel: (() => void) | null;
}

export const activity: ActivityState = $state({
  current: null,
  verb: 'Uploading',
  detail: '',
  cancel: null,
});

export function startActivity(name: string, verb = 'Uploading'): void {
  activity.current = name;
  activity.verb = verb;
  activity.detail = '';
  activity.cancel = null;
}

// Progress and cancellation for the activity in the slot.
export function setActivityDetail(detail: string): void {
  activity.detail = detail;
}

export function setActivityCancel(cancel: (() => void) | null): void {
  activity.cancel = cancel;
}

export function endActivity(): void {
  activity.current = null;
  activity.detail = '';
  activity.cancel = null;
}

// A bridge request that has been running unusually long (bus/emulator.ts).
// Informational only: the request is not stuck until it is — the notice just
// says what the emulator is busy with.  `path` is null when nothing is slow.
interface BridgeBusyState {
  path: string | null;
  seconds: number;
}

export const bridgeBusy: BridgeBusyState = $state({ path: null, seconds: 0 });
