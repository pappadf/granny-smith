// Path helpers for the Filesystem tree's "descend into an image or archive"
// feature. A guest disk image (HFS / UFS / APM), an archive (zip, StuffIt,
// Compact Pro, BinHex, MacBinary, gzip) and any nesting of them can be
// browsed read-only as an extended path —
// /opfs/images/hd/disk.img/partition3/etc/motd, /opfs/dl/tools.zip/Disk.img —
// handled by the C-side `files.list`, which marks each file the core's
// format registry recognises as `expandable`. The tree records those paths
// here, so it knows where OPFS ends and the VFS begins without guessing
// from file extensions.

// Paths of files the core reported expandable. Grows as listings arrive; a
// path is forgotten when the core says it is no longer expandable.
const roots = new Set<string>();

// Record what a listing said about the file at `path`.
export function markExpandable(path: string, expandable: boolean): void {
  if (expandable) roots.add(path);
  else roots.delete(path);
}

// True when the core has reported the file at `path` as expandable.
export function isExpandable(path: string): boolean {
  return roots.has(path);
}

// Index of the first path segment that ends an expandable file, or -1: the
// boundary between the host filesystem and image/archive space.
function rootSegmentIndex(fullPath: string): number {
  const segs = fullPath.split('/');
  let prefix = '';
  for (let i = 0; i < segs.length; i++) {
    prefix = i === 0 ? segs[0] : `${prefix}/${segs[i]}`;
    if (roots.has(prefix)) return i;
  }
  return -1;
}

// True when listing this path's children must go through `files.list` rather
// than OPFS: either the path IS an expandable file (list its partitions or
// members) or it lives inside one.
export function listViaVfs(fullPath: string): boolean {
  return rootSegmentIndex(fullPath) >= 0;
}

// True when the path points *inside* an image or archive (a partition,
// directory, or file within it) — an expandable segment exists with at least
// one more segment after it. These nodes are read-only: the image backend
// rejects writes, so the UI must not offer rename / delete / drop / unpack on
// them. The image file itself (last segment) is a real OPFS file and is NOT
// in image space.
export function isInImageSpace(fullPath: string): boolean {
  const i = rootSegmentIndex(fullPath);
  if (i < 0) return false;
  return i < fullPath.split('/').length - 1;
}
