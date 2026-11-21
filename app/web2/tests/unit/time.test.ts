import { describe, it, expect } from 'vitest';
import { compactLocalStamp, localStamp, pad2, utcCompactStamp } from '@/lib/time';
import { formatLocalStamp } from '@/lib/checkpointMeta';

// File names are built from these: the formats are fixed.
describe('lib/time stamps', () => {
  const d = new Date(2026, 0, 2, 3, 4, 5); // local time

  it('pads to two digits', () => {
    expect(pad2(7)).toBe('07');
    expect(pad2(42)).toBe('42');
  });

  it('writes the compact local stamp downloads are named with', () => {
    expect(compactLocalStamp(d)).toBe('20260102-030405');
  });

  it('writes the panels stamp, seconds optional', () => {
    expect(localStamp(d)).toBe('2026-01-02 03:04');
    expect(localStamp(d, true)).toBe('2026-01-02 03:04:05');
    expect(formatLocalStamp(d, true)).toBe('2026-01-02 03:04:05');
  });

  it('writes compact ISO 8601 UTC', () => {
    expect(utcCompactStamp(new Date(Date.UTC(2026, 3, 30, 15, 30, 45)))).toBe('20260430T153045Z');
  });
});
