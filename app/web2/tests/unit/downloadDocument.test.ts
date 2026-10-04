import { describe, it, expect, beforeEach, vi } from 'vitest';

// The core side of a download: a heap view per chunk, and the ack.
const heap = new Map<number, Uint8Array>();
vi.mock('@/bus/emulator', () => ({
  heapBytes: (ptr: number, len: number) => heap.get(ptr)?.subarray(0, len) ?? null,
  ackTransferBuffer: vi.fn(async () => true),
}));

import { onDownloadChunk, pendingDownloads } from '@/bus/download';
import { printer, _resetPrinterForTests } from '@/state/printer.svelte';

beforeEach(() => {
  (URL as unknown as { createObjectURL: unknown }).createObjectURL = vi.fn(() => 'blob:doc');
  (URL as unknown as { revokeObjectURL: unknown }).revokeObjectURL = vi.fn();
  _resetPrinterForTests();
  heap.clear();
});

describe('a printed document from the core', () => {
  it('opens in the print viewer, named after its printer', () => {
    heap.set(100, new Uint8Array([0x25, 0x50]));
    heap.set(200, new Uint8Array([0x44, 0x46]));
    const meta = { kind: 'document', printer: 'ImageWriter II', job: 3, pages: 2, title: 'Print' };
    onDownloadChunk({
      id: 7,
      handle: 1,
      ptr: 100,
      len: 2,
      last: 0,
      name: 'imagewriter2-00003-Print.pdf',
      ...meta,
    });
    expect(printer.viewerOpen).toBe(false);
    onDownloadChunk({
      id: 7,
      handle: 1,
      ptr: 200,
      len: 2,
      last: 1,
      name: 'imagewriter2-00003-Print.pdf',
      ...meta,
    });
    expect(pendingDownloads()).toBe(0);
    expect(printer.viewerOpen).toBe(true);
    expect(printer.document?.printer).toBe('ImageWriter II');
    expect(printer.document?.pages).toBe(2);
    expect(printer.document?.name).toBe('imagewriter2-00003-Print.pdf');
  });

  it('is saved instead where the browser has no PDF viewer', () => {
    Object.defineProperty(navigator, 'pdfViewerEnabled', { value: false, configurable: true });
    heap.set(100, new Uint8Array([0x25]));
    onDownloadChunk({
      id: 8,
      handle: 1,
      ptr: 100,
      len: 1,
      last: 1,
      name: 'x.pdf',
      kind: 'document',
      printer: 'ImageWriter',
    });
    expect(printer.viewerOpen).toBe(false);
    expect(printer.document).toBeNull();
    Object.defineProperty(navigator, 'pdfViewerEnabled', { value: true, configurable: true });
  });
});
