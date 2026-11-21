import { describe, it, expect } from 'vitest';
import { formatMtime } from '@/lib/fileInfo';

describe('formatMtime', () => {
  it('is blank for an unknown (0) time', () => {
    expect(formatMtime(0)).toBe('');
  });
  it('formats local time as YYYY-MM-DD HH:MM', () => {
    const secs = new Date(2026, 9, 4, 14, 32, 59).getTime() / 1000;
    expect(formatMtime(secs)).toBe('2026-10-04 14:32');
    expect(formatMtime(new Date(1999, 0, 2, 3, 4).getTime() / 1000)).toBe('1999-01-02 03:04');
  });
});
