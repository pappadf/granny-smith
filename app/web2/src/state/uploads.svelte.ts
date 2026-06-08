// In-progress activity tracker. Surfaces a one-line "<verb>: <name>" hint
// with a spinner in the status bar while a long operation is in flight — an
// upload (OPFS write + validation + persist) or a Filesystem-tab worker op
// (copy out of an image, delete, unpack, move). Cleared when the op returns,
// success or failure. Single-slot — concurrent ops are rare in this UI; if
// several kick off at once, the last to start wins the slot.

interface UploadState {
  current: string | null;
  verb: string;
}

export const uploads: UploadState = $state({ current: null, verb: 'Uploading' });

export function startUpload(name: string, verb = 'Uploading'): void {
  uploads.current = name;
  uploads.verb = verb;
}

export function finishUpload(): void {
  uploads.current = null;
}
