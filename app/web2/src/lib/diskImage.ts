// Disk-image path helpers for the Filesystem tree's "descend into an image"
// feature. A guest disk image (HFS / UFS / APM) can be browsed read-only as
// an extended path — /opfs/images/hd/disk.img/partition3/etc/motd — handled
// by the C-side `vfs.list` (see docs/target-filesystems.md). These pure
// helpers classify a path segment so the tree knows when to expand an image
// node and route its children through the VFS instead of OPFS.

// Extensions the host treats as a mountable disk image. Matches the media
// categories the app already knows about (floppy / HD / CD), plus the
// generic ".image" some Mac tools emit.
const IMAGE_EXT = /\.(img|dsk|dc42|iso|toast|cdr|hda|image)$/i;

// True when a filename looks like a browsable disk image (by extension).
export function isDiskImage(name: string): boolean {
  return IMAGE_EXT.test(name || '');
}

// Index of the first path segment that is a disk image, or -1. A path can
// only descend through one image (the VFS resolver stops at the first file
// segment), so the first match is the boundary.
function imageSegmentIndex(fullPath: string): number {
  const segs = fullPath.split('/');
  for (let i = 0; i < segs.length; i++) {
    if (isDiskImage(segs[i])) return i;
  }
  return -1;
}

// True when listing this path's children must go through `vfs.list` rather
// than OPFS: either the path IS a disk image (list its partitions) or it
// lives inside one (list the volume contents).
export function listViaVfs(fullPath: string): boolean {
  return imageSegmentIndex(fullPath) >= 0;
}

// True when the path points *inside* an image (a partition, directory, or
// file within the guest volume) — i.e. an image segment exists with at least
// one more segment after it. These nodes are read-only: the image backend
// rejects writes, so the UI must not offer rename / delete / drop / unpack on
// them. The image file itself (last segment) is a real OPFS file and is NOT
// in image space.
export function isInImageSpace(fullPath: string): boolean {
  const i = imageSegmentIndex(fullPath);
  if (i < 0) return false;
  return i < fullPath.split('/').length - 1;
}

// The disk-image file portion of a path — everything up to and including the
// first image-extension segment — or null when the path doesn't cross an
// image boundary. Used to address the image for a force-unmount.
export function imageRootOf(fullPath: string): string | null {
  const segs = fullPath.split('/');
  const i = segs.findIndex((s) => isDiskImage(s));
  return i < 0 ? null : segs.slice(0, i + 1).join('/');
}
