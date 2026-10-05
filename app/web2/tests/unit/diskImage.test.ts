import { describe, it, expect } from 'vitest';
import { isExpandable, isInImageSpace, listViaVfs, markExpandable } from '@/lib/diskImage';

describe('image/archive path helpers', () => {
  it('know nothing is expandable until the core says so', () => {
    // No extension guessing: an unlisted "disk.img" is just a file.
    expect(isExpandable('/opfs/images/hd/disk.img')).toBe(false);
    expect(listViaVfs('/opfs/images/hd/disk.img/partition1')).toBe(false);
  });

  it('route through the VFS at and below a file the core marked expandable', () => {
    markExpandable('/opfs/images/hd/disk.img', true);
    expect(listViaVfs('/opfs/images/hd/disk.img')).toBe(true); // the image itself → list partitions
    expect(listViaVfs('/opfs/images/hd/disk.img/partition1')).toBe(true);
    expect(listViaVfs('/opfs/images/hd/disk.img/partition1/System Folder')).toBe(true);
    expect(listViaVfs('/opfs/images/hd/notes.txt')).toBe(false);
    expect(listViaVfs('/opfs')).toBe(false);
  });

  it('isInImageSpace is true only strictly inside an expandable file', () => {
    markExpandable('/opfs/images/hd/disk.img', true);
    // The image file node is a real OPFS file — NOT in image space.
    expect(isInImageSpace('/opfs/images/hd/disk.img')).toBe(false);
    // Anything past the image boundary is read-only image space.
    expect(isInImageSpace('/opfs/images/hd/disk.img/partition1')).toBe(true);
    expect(isInImageSpace('/opfs/images/hd/disk.img/partition1/etc/motd')).toBe(true);
    // Plain OPFS paths are never image space.
    expect(isInImageSpace('/opfs/images/hd/notes.txt')).toBe(false);
    expect(isInImageSpace('/opfs')).toBe(false);
  });

  it('follow nesting: an archive inside an image is a boundary of its own', () => {
    markExpandable('/opfs/dl/tools.zip', true);
    markExpandable('/opfs/dl/tools.zip/Disk.img', true);
    expect(isExpandable('/opfs/dl/tools.zip/Disk.img')).toBe(true);
    expect(isInImageSpace('/opfs/dl/tools.zip/Disk.img')).toBe(true);
    expect(listViaVfs('/opfs/dl/tools.zip/Disk.img/partition1')).toBe(true);
  });

  it('forget a file the core no longer reports expandable', () => {
    markExpandable('/opfs/x/was.img', true);
    markExpandable('/opfs/x/was.img', false);
    expect(listViaVfs('/opfs/x/was.img/partition1')).toBe(false);
  });
});
