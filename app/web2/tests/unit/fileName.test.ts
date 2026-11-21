import { describe, it, expect } from 'vitest';
import { safeFileName, sanitizeName, slug } from '@/lib/fileName';
import { blankDiskName } from '@/lib/blankMedia';

// Stored names come out of these (blank disks, checkpoint downloads, the
// AFP cache): the outputs are pinned to the rules they replaced.
describe('lib/fileName', () => {
  it('keeps the user name OPFS-safe', () => {
    expect(safeFileName('My Disk: v2.img')).toBe('My Disk_ v2.img');
    expect(safeFileName('a/b\\c*?"<>|')).toBe('a_b_c______');
    expect(safeFileName('trailing. . ')).toBe('trailing');
    expect(safeFileName('.hidden')).toBe('.hidden');
    expect(safeFileName('...')).toBe('untitled');
    expect(safeFileName('...', '')).toBe('');
  });

  it('collapses a run of other characters into one _ by default', () => {
    expect(sanitizeName('My  Disk (1).img')).toBe('My_Disk_1_.img');
    expect(sanitizeName('a__b')).toBe('a__b');
  });

  it('replaces one _ per character without collapse, as blank disk names do', () => {
    expect(sanitizeName('My  Disk', { collapse: false })).toBe('My__Disk');
    expect(blankDiskName('hd0', '20 MB', 'abcd1234', '.img')).toBe('blank_20_MB_hd0_abcd1234.img');
    expect(blankDiskName('fd0', '', 'abcd1234', '.dsk')).toBe('blank_disk_fd0_abcd1234.dsk');
  });

  it('slugs a folder name', () => {
    expect(slug('My Server (2)', 'volume')).toBe('My-Server-2');
    expect(slug('***', 'volume')).toBe('volume');
  });
});
