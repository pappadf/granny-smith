// Storage used before a backend is installed is an ordering bug, and says so:
// the old default answered with made-up fixture files.
import { describe, it, expect, vi } from 'vitest';

describe('opfs without a backend', () => {
  it('throws instead of serving fixtures', async () => {
    vi.resetModules(); // a fresh module, untouched by tests/setup.ts
    const { opfs } = await import('@/bus/opfs');
    expect(() => opfs.scanRoms()).toThrow(/not installed/);
    expect(() => opfs.list('/opfs')).toThrow(/not installed/);
  });
});
