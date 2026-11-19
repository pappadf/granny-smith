// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The one transient-anchor save: every download the page starts itself (a
// file out of OPFS, a checkpoint, a printed document, the log buffer) goes
// through here.

// How long an object URL outlives its click.  The download resolves the
// URL after the click (a lazy OPFS-backed File is read only then), so an
// immediate revoke can fail it; a few seconds of held memory is the cost.
const REVOKE_MS = 10_000;

// Save a Blob to the user's machine via a transient object-URL anchor.  An
// untyped Blob is sent as application/octet-stream: given none, Safari takes
// a file with no extension (hd0_2026-10-05_11-42-50) for text and saves it
// as .txt.  Returns false when the save could not start (the caller toasts).
export function saveBlob(blob: Blob, filename: string): boolean {
  try {
    if (!blob.type) blob = new Blob([blob], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), REVOKE_MS);
    return true;
  } catch (e) {
    console.error(`[saveBlob] cannot save ${filename}:`, e);
    return false;
  }
}
