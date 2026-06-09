import { describe, it, expect, vi, beforeEach } from 'vitest';

// Route OPFS mutations (delete, move) through the worker (storage.rm /
// storage.mv) so the worker's WasmFS inode cache stays coherent with OPFS — a
// main-thread navigator.storage mutation is invisible to the worker and breaks
// a later worker-side create at the same path (the disk-image
// copy-out-after-delete bug).
const { gsEvalMock } = vi.hoisted(() => ({ gsEvalMock: vi.fn() }));
vi.mock('@/bus/emulator', () => ({
  gsEval: (p: string, a?: unknown[]) => gsEvalMock(p, a),
  isModuleReady: () => true,
}));

import { BrowserOpfs } from '@/bus/opfs';

beforeEach(() => {
  gsEvalMock.mockReset();
});

describe('BrowserOpfs.delete', () => {
  it('routes through the worker via storage.rm', async () => {
    gsEvalMock.mockResolvedValue(true);
    await new BrowserOpfs().delete('/opfs/images/fd/System Tools.image');
    expect(gsEvalMock).toHaveBeenCalledWith('storage.rm', ['/opfs/images/fd/System Tools.image']);
  });
});

describe('BrowserOpfs.move', () => {
  it('routes through the worker via storage.mv', async () => {
    gsEvalMock.mockResolvedValue(true);
    await new BrowserOpfs().move('/opfs/a.txt', '/opfs/sub/a.txt');
    expect(gsEvalMock).toHaveBeenCalledWith('storage.mv', ['/opfs/a.txt', '/opfs/sub/a.txt']);
  });
});
