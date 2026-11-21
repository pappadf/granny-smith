import { describe, it, expect } from 'vitest';
import { formatSize } from '@/lib/format';

describe('formatSize', () => {
  it('counts bytes below 1 KB', () => {
    expect(formatSize(1)).toBe('1 B');
    expect(formatSize(512)).toBe('512 B');
    expect(formatSize(1023)).toBe('1023 B');
  });
  it('uses one decimal below 10, whole units above', () => {
    expect(formatSize(2 * 1024)).toBe('2.0 KB');
    expect(formatSize(4522)).toBe('4.4 KB');
    expect(formatSize(819200)).toBe('800 KB');
    expect(formatSize(1474560)).toBe('1.4 MB');
    expect(formatSize(10439 * 1024)).toBe('10 MB');
    expect(formatSize(20971520)).toBe('20 MB');
    expect(formatSize(25 * 1024 * 1024)).toBe('25 MB');
    expect(formatSize(3 * 1024 ** 3)).toBe('3.0 GB');
    expect(formatSize(2 * 1024 ** 4)).toBe('2.0 TB');
  });
  it('stops at TB', () => {
    expect(formatSize(2048 * 1024 ** 4)).toBe('2048 TB');
  });
  it('says "0 B" for nothing, a negative or a non-number', () => {
    expect(formatSize(0)).toBe('0 B');
    expect(formatSize(-1)).toBe('0 B');
    expect(formatSize(NaN)).toBe('0 B');
  });
});
