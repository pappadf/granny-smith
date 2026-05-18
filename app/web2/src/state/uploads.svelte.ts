// Active-upload tracker. Surfaces a one-line "Uploading: <name>" hint
// in the status bar while an upload is in flight (OPFS write +
// validation + persist). Cleared when the pipeline returns success or
// failure. Single-slot — concurrent uploads are rare in this UI; if
// multiple kick off at once, the last one to start wins the slot.

interface UploadState {
  current: string | null;
}

export const uploads: UploadState = $state({ current: null });

export function startUpload(name: string): void {
  uploads.current = name;
}

export function finishUpload(): void {
  uploads.current = null;
}
