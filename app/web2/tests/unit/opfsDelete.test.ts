import { describe, it, expect, vi, beforeEach } from 'vitest';

// Route OPFS mutations (delete, move) through the worker (storage.rm /
// storage.mv) so the worker's WasmFS inode cache stays coherent with OPFS — a
// main-thread navigator.storage mutation is invisible to the worker and breaks
// a later worker-side create at the same path (the disk-image
// copy-out-after-delete bug). Once the module is up, a worker-reported
// failure must PROPAGATE, never silently retry main-thread — that retry would
// recreate the exact stale-inode incoherence the routing exists to prevent.
const { gsEvalMock } = vi.hoisted(() => ({ gsEvalMock: vi.fn() }));
vi.mock('@/bus/emulator', () => ({
  gsEval: (p: string, a?: unknown[]) => gsEvalMock(p, a),
  gsErrorText: (res: unknown) =>
    res && typeof res === 'object' && 'error' in res
      ? String((res as { error: unknown }).error)
      : String(res),
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

  it('propagates a worker-reported failure instead of falling back main-thread', async () => {
    gsEvalMock.mockResolvedValue({ error: "storage.rm: refusing to remove '/opfs'" });
    await expect(new BrowserOpfs().delete('/opfs')).rejects.toThrow(/refusing to remove/);
    // Exactly one call — the worker route. No navigator.storage fallback ran
    // (jsdom has no navigator.storage; reaching it would throw a TypeError,
    // not our error message).
    expect(gsEvalMock).toHaveBeenCalledTimes(1);
  });
});

describe('BrowserOpfs.move', () => {
  it('routes through the worker via storage.mv', async () => {
    gsEvalMock.mockResolvedValue(true);
    await new BrowserOpfs().move('/opfs/a.txt', '/opfs/sub/a.txt');
    expect(gsEvalMock).toHaveBeenCalledWith('storage.mv', ['/opfs/a.txt', '/opfs/sub/a.txt']);
  });

  it('propagates a worker-reported failure instead of falling back main-thread', async () => {
    gsEvalMock.mockResolvedValue({ error: "storage.mv: destination '/opfs/b' already exists" });
    await expect(new BrowserOpfs().move('/opfs/a', '/opfs/b')).rejects.toThrow(/already exists/);
    expect(gsEvalMock).toHaveBeenCalledTimes(1);
  });
});
