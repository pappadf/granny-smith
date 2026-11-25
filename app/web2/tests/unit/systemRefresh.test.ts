import { describe, it, expect } from 'vitest';
import { REFRESH_INTERVAL_MS } from '@/lib/systemRows';

// The SYSTEM tab re-reads its open levels this often while the machine
// runs.  A value that changes once a second (the Lisa's MMU context with two
// processes switching) must not alias: sampling needs to be at least twice
// as fast as the change.
describe('the SYSTEM tab refresh while running', () => {
  it('samples at least twice a second', () => {
    expect(REFRESH_INTERVAL_MS).toBeLessThanOrEqual(500);
  });
});
