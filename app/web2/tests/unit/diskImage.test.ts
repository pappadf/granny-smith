import { describe, it, expect } from 'vitest';
import { isDiskImage, isInImageSpace, listViaVfs } from '@/lib/diskImage';

describe('disk-image path helpers', () => {
  it('isDiskImage recognises image extensions, case-insensitive', () => {
    for (const ext of ['img', 'dsk', 'dc42', 'iso', 'toast', 'cdr', 'hda', 'image']) {
      expect(isDiskImage(`disk.${ext}`)).toBe(true);
      expect(isDiskImage(`disk.${ext.toUpperCase()}`)).toBe(true);
    }
    expect(isDiskImage('notes.txt')).toBe(false);
    expect(isDiskImage('rom.bin')).toBe(false); // .bin is a ROM/MacBinary, not a disk image
    expect(isDiskImage('')).toBe(false);
  });

  it('listViaVfs is true once a path contains an image segment', () => {
    expect(listViaVfs('/opfs/images/hd/disk.img')).toBe(true); // the image itself → list partitions
    expect(listViaVfs('/opfs/images/hd/disk.img/partition1')).toBe(true);
    expect(listViaVfs('/opfs/images/hd/disk.img/partition1/System Folder')).toBe(true);
    expect(listViaVfs('/opfs/images/hd/notes.txt')).toBe(false);
    expect(listViaVfs('/opfs')).toBe(false);
  });

  it('isInImageSpace is true only strictly inside an image (not the image file)', () => {
    // The image file node is a real OPFS file — NOT in image space.
    expect(isInImageSpace('/opfs/images/hd/disk.img')).toBe(false);
    // Anything past the image boundary is read-only image space.
    expect(isInImageSpace('/opfs/images/hd/disk.img/partition1')).toBe(true);
    expect(isInImageSpace('/opfs/images/hd/disk.img/partition1/etc/motd')).toBe(true);
    // Plain OPFS paths are never image space.
    expect(isInImageSpace('/opfs/images/hd/notes.txt')).toBe(false);
    expect(isInImageSpace('/opfs')).toBe(false);
  });
});
