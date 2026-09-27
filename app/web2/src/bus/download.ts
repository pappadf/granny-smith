// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Downloads from the core: a file the machine made (a checkpoint, an
// exported image, a LaserWriter job's capture) reaches the user's disk
// without either thread waiting on the other.  The core's I/O worker
// reads the file a chunk at a time into a STAGED BUFFER (mailbox.h) and
// the emulator thread announces each chunk as a `download_chunk` event:
// {id, handle, ptr, len, last, name}.  The page copies the bytes out of
// the shared heap into a Blob part, acknowledges the buffer (REQ_ACK_BUF;
// the worker refills it), and on the last chunk saves the Blob through a
// transient anchor.  A page that never acks makes the core's job time
// out, not the machine.

import { ackStagedBuffer, heapBytes } from './emulator';
import { showNotification } from '@/state/toasts.svelte';

interface Pending {
  name: string;
  parts: Uint8Array[];
}

const pending = new Map<number, Pending>();

const num = (v: unknown): number => (typeof v === 'number' ? v : 0);

// One `download_chunk` event (bus/emulator.ts routes them here).
export function onDownloadChunk(d: Record<string, unknown>): void {
  const id = num(d.id);
  const handle = num(d.handle);
  const ptr = num(d.ptr);
  const len = num(d.len);
  const name = typeof d.name === 'string' && d.name ? d.name : 'download.bin';
  let p = pending.get(id);
  if (!p) {
    p = { name, parts: [] };
    pending.set(id, p);
  }
  const view = heapBytes(ptr, len);
  // Copy out before acknowledging: the worker refills the buffer as soon as
  // the ack is served.
  if (view && len) p.parts.push(view.slice());
  void ackStagedBuffer(handle);
  if (d.last === 1 || d.last === true) {
    pending.delete(id);
    saveBlob(new Blob(p.parts as BlobPart[], { type: 'application/octet-stream' }), p.name);
  }
}

// The transient-anchor save (the same shape fsOps and the platen use).
export function saveBlob(blob: Blob, filename: string): void {
  try {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  } catch (e) {
    console.error('[download] failed:', e);
    showNotification(`Could not download ${filename}`, 'error');
  }
}

// For tests: what is in flight.
export function pendingDownloads(): number {
  return pending.size;
}
