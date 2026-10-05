// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Downloads from the core: a file the machine made (a checkpoint, an
// exported image, a LaserWriter job's capture) reaches the user's disk
// without either thread waiting on the other.  The core's I/O worker
// reads the file a chunk at a time into a TRANSFER BUFFER (mailbox.h) and
// the emulator thread announces each chunk as a `download_chunk` event:
// {id, handle, ptr, len, last, name}.  The page copies the bytes out of
// the shared heap into a Blob part, acknowledges the buffer (REQ_ACK_BUF;
// the worker refills it), and on the last chunk saves the Blob through a
// transient anchor.  A page that never acks makes the core's job time
// out, not the machine.
//
// A download marked `kind: "document"` is a printed document (the core's
// ImageWriter): it opens in the print viewer instead, where the browser can
// show a PDF inline, and is saved like any other download where it cannot.

import { ackTransferBuffer, heapBytes } from './emulator';
import { showNotification } from '@/state/toasts.svelte';
import { showPrintedDocument } from '@/state/printer.svelte';

interface Pending {
  name: string;
  parts: Uint8Array[];
  // A printed document's identity (kind "document"), else null
  doc: { printer: string; title: string; pages: number } | null;
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
    const doc =
      d.kind === 'document'
        ? {
            printer: typeof d.printer === 'string' ? d.printer : 'Printer',
            title: typeof d.title === 'string' ? d.title : '',
            pages: num(d.pages),
          }
        : null;
    p = { name, parts: [], doc };
    pending.set(id, p);
  }
  const view = heapBytes(ptr, len);
  // Copy out before acknowledging: the worker refills the buffer as soon as
  // the ack is served.
  if (view && len) p.parts.push(view.slice());
  void ackTransferBuffer(handle);
  if (d.last === 1 || d.last === true) {
    pending.delete(id);
    if (p.doc) {
      deliverDocument(p);
      return;
    }
    saveBlob(new Blob(p.parts as BlobPart[], { type: 'application/octet-stream' }), p.name);
  }
}

// A printed document: the viewer where the browser shows PDFs inline
// (navigator.pdfViewerEnabled; one that predates the property is taken to
// have a viewer), else a download -- as the LaserWriter's (printer/platen.ts).
function deliverDocument(p: Pending): void {
  const total = p.parts.reduce((n, part) => n + part.length, 0);
  const pdf = new Uint8Array(total);
  let off = 0;
  for (const part of p.parts) {
    pdf.set(part, off);
    off += part.length;
  }
  if (typeof navigator !== 'undefined' && navigator.pdfViewerEnabled === false) {
    saveBlob(new Blob([pdf as BlobPart], { type: 'application/pdf' }), p.name);
    return;
  }
  try {
    showPrintedDocument({
      printer: p.doc!.printer,
      name: p.name,
      title: p.doc!.title,
      pages: p.doc!.pages,
      pdf,
    });
  } catch (e) {
    console.error('[download] cannot show the document:', e);
    saveBlob(new Blob([pdf as BlobPart], { type: 'application/pdf' }), p.name);
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
