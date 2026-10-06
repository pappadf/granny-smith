import { describe, it, expect } from 'vitest';
import { formatMtime, formatSize } from '@/lib/fileInfo';

describe('formatSize', () => {
  it('counts bytes below 1 KB', () => {
    expect(formatSize(0)).toBe('0 B');
    expect(formatSize(1023)).toBe('1023 B');
  });
  it('uses one decimal below 10, whole units above', () => {
    expect(formatSize(4522)).toBe('4.4 KB');
    expect(formatSize(819200)).toBe('800 KB');
    expect(formatSize(1474560)).toBe('1.4 MB');
    expect(formatSize(10439 * 1024)).toBe('10 MB');
    expect(formatSize(20971520)).toBe('20 MB');
    expect(formatSize(3 * 1024 ** 3)).toBe('3.0 GB');
  });
});

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
