// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Walking an `/opfs/...` path to its directory handle in the browser's own
// OPFS tree (navigator.storage.getDirectory()).  Shared by the page's OPFS
// backend (bus/opfs.ts) and the workers that read and write OPFS files
// themselves (the MP4 export, recorder/export.worker.ts), so it imports
// nothing that needs the page.  All paths use the `/opfs/...` prefix of the
// C-side mount; the prefix is stripped before walking.

// The directory at `path` (`/opfs` is the root); `create` makes the
// missing directories on the way.
export async function getDirAtPath(
  path: string,
  options: { create?: boolean } = {},
): Promise<FileSystemDirectoryHandle> {
  const rel = path.replace(/^\/opfs\/?/, '');
  let dir = await navigator.storage.getDirectory();
  if (!rel) return dir;
  for (const part of rel.split('/').filter(Boolean)) {
    dir = await dir.getDirectoryHandle(part, options);
  }
  return dir;
}

// A file path's directory and name: `/opfs/a/b.mkv` -> [`/opfs/a`, `b.mkv`].
export function splitOpfsPath(path: string): [string, string] {
  const i = path.lastIndexOf('/');
  return [path.slice(0, i), path.slice(i + 1)];
}
