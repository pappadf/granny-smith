import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('@/bus/emulator', () => ({
  heapBytes: () => null,
  ackTransferBuffer: vi.fn(async () => true),
}));

import { saveBlob as saveDownload } from '@/bus/download';
import { saveBlob } from '@/lib/saveBlob';

const createObjectURL = vi.fn<(b: Blob) => string>(() => 'blob:x');
const revokeObjectURL = vi.fn();

beforeEach(() => {
  vi.useFakeTimers();
  (URL as unknown as { createObjectURL: unknown }).createObjectURL = createObjectURL;
  (URL as unknown as { revokeObjectURL: unknown }).revokeObjectURL = revokeObjectURL;
  createObjectURL.mockClear();
  revokeObjectURL.mockClear();
  // jsdom would try to navigate on click
  vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
});

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

describe('saveBlob', () => {
  it('keeps the object URL alive while the browser reads a lazy file', () => {
    expect(saveBlob(new Blob(['x'], { type: 'text/plain' }), 'a.txt')).toBe(true);
    vi.advanceTimersByTime(1_000);
    expect(revokeObjectURL).not.toHaveBeenCalled();
    vi.advanceTimersByTime(9_000);
    expect(revokeObjectURL).toHaveBeenCalledWith('blob:x');
  });

  it('sends an untyped blob as octet-stream and keeps a typed one', () => {
    saveBlob(new Blob(['x']), 'hd0_2026');
    expect(createObjectURL.mock.calls[0][0].type).toBe('application/octet-stream');
    saveBlob(new Blob(['x'], { type: 'application/pdf' }), 'a.pdf');
    expect(createObjectURL.mock.calls[1][0].type).toBe('application/pdf');
  });

  it('reports a failure instead of throwing', () => {
    createObjectURL.mockImplementationOnce(() => {
      throw new Error('quota');
    });
    vi.spyOn(console, 'error').mockImplementation(() => {});
    expect(saveBlob(new Blob(['x']), 'a.bin')).toBe(false);
  });
});

// The checkpoint download hands a lazy OPFS File to the bus wrapper; it
// must not revoke the URL before the browser has read the file.
describe('bus/download saveBlob', () => {
  it('delays the revoke like the shared helper', () => {
    saveDownload(new File(['x'], 'cp.bin'), 'cp.bin');
    vi.advanceTimersByTime(1_000);
    expect(revokeObjectURL).not.toHaveBeenCalled();
    vi.advanceTimersByTime(9_000);
    expect(revokeObjectURL).toHaveBeenCalled();
  });
});
