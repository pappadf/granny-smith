import { describe, it, expect, vi, beforeEach } from 'vitest';

// Route deletes through the worker (storage.rm) so the worker's WasmFS inode
// cache stays coherent with OPFS — a main-thread navigator.storage delete is
// invisible to the worker and breaks a later worker-side create at the same
// path (the disk-image copy-out-after-delete bug).
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
