// Filesystem-tab operations — the actual I/O behind the OPFS browser:
// copy-out-of-image, move, delete, download, unpack. These are pure async
// functions returning structured results; the view layer owns the tree cache,
// refresh, toasts, and progress (via the optional `onItem` callback). Keeping
// them here makes the operations unit-testable without rendering a component.

import { gsEval, gsErrorText } from './emulator';
import { opfs } from './opfs';
import { sanitizeName } from '@/lib/archive';
import { isInImageSpace } from '@/lib/diskImage';
import { UPLOAD_DIR } from '@/lib/opfsPaths';

// Per-item progress hook (e.g. to drive the status-bar indicator).
export type ProgressFn = (name: string, index: number, total: number) => void;

// Result of a bulk operation, with enough to render a toast.
export interface BulkResult {
  total: number;
  failures: string[]; // basenames that failed
  firstError: string; // reason of the first failure
}

// A source for copy-out: its in-image path and whether it's a directory.
export interface CopySource {
  path: string;
  isDir: boolean;
}

let downloadSeq = 0;

// Make a name safe for an OPFS / cross-platform destination: replace the
// characters OPFS or Windows reject (notably ':' which the HFS reader produces
// from an in-name '/') and trim trailing dots/spaces. Spaces and leading dots
// are kept.
export function opfsSafeName(name: string): string {
  return name.replace(/[\\/:*?"<>|]/g, '_').replace(/[ .]+$/g, '') || 'untitled';
}

function basename(path: string): string {
  return path.split('/').pop() ?? '';
}

// Copy items OUT of a read-only image into an OPFS folder (recursive for
// directories). Destination names are sanitised for OPFS. Existing entries
// are never overwritten: a collision — with an existing file, or between two
// batch items whose names sanitise identically — fails that item. The
// alternative silently truncates data; in the worst case the destination
// resolves to the very image being read (a file inside X.img named "X.img"
// dropped into the image's own folder) and destroys it.
export async function copyOutOfImage(
  sources: CopySource[],
  dstDir: string,
  onItem?: ProgressFn,
): Promise<BulkResult> {
  const failures: string[] = [];
  let firstError = '';
  const taken = new Set((await opfs.list(dstDir)).map((e) => e.name));
  for (let i = 0; i < sources.length; i++) {
    const src = sources[i].path;
    const name = basename(src);
    onItem?.(name, i, sources.length);
    const safe = opfsSafeName(name);
    const dst = `${dstDir}/${safe}`;
    // Hard guard independent of the listing: a destination that is a path
    // prefix of the source IS the container image — writing it would
    // truncate the image mid-read.
    if (src === dst || src.startsWith(`${dst}/`)) {
      failures.push(name);
      if (!firstError) firstError = `'${safe}' is the image being copied from`;
      continue;
    }
    if (taken.has(safe)) {
      failures.push(name);
      if (!firstError) firstError = `'${safe}' already exists in destination`;
      continue;
    }
    const args = sources[i].isDir ? ['-r', src, dst] : [src, dst];
    const res = await gsEval('storage.cp', args);
    if (res !== true) {
      failures.push(name);
      if (!firstError) firstError = gsErrorText(res);
      console.error('copy-out failed:', src, '->', dst, '|', gsErrorText(res));
    } else {
      taken.add(safe);
    }
  }
  return { total: sources.length, failures, firstError };
}

// Move items within OPFS (each into `dstDir`).
export async function moveItems(
  srcPaths: string[],
  dstDir: string,
  onItem?: ProgressFn,
): Promise<BulkResult> {
  const failures: string[] = [];
  let firstError = '';
  for (let i = 0; i < srcPaths.length; i++) {
    const src = srcPaths[i];
    const name = basename(src);
    onItem?.(name, i, srcPaths.length);
    try {
      await opfs.move(src, `${dstDir}/${name}`);
    } catch (err) {
      failures.push(name);
      if (!firstError) firstError = String(err);
      console.error('move failed:', src, '->', `${dstDir}/${name}`, '|', err);
    }
  }
  return { total: srcPaths.length, failures, firstError };
}

// Recursively delete the given paths.
export async function deleteItems(paths: string[], onItem?: ProgressFn): Promise<BulkResult> {
  const failures: string[] = [];
  for (let i = 0; i < paths.length; i++) {
    const name = basename(paths[i]);
    onItem?.(name, i, paths.length);
    try {
      await opfs.delete(paths[i]);
    } catch {
      failures.push(name);
    }
  }
  return { total: paths.length, failures, firstError: '' };
}

// Save a Blob to the user's machine via a transient object-URL anchor.
function saveBlob(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.style.display = 'none';
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

// Download one file. A plain OPFS file is read straight from OPFS; a file
// inside a (read-only) image has its data fork copied out to a scratch OPFS
// path first, then read and removed.
async function downloadOne(target: string): Promise<boolean> {
  const name = basename(target) || 'download';
  try {
    if (isInImageSpace(target)) {
      const scratch = `${UPLOAD_DIR}/.dl-${downloadSeq++}-${sanitizeName(name)}`;
      if ((await gsEval('storage.cp', [target, scratch])) !== true) return false;
      try {
        // readFile returns a lazy File backed by the OPFS entry; the browser
        // streams it AFTER the anchor click. Materialise the bytes before
        // deleting the scratch file or the download races its own backing
        // store and fails/truncates.
        const bytes = await (await opfs.readFile(scratch)).arrayBuffer();
        saveBlob(new Blob([bytes]), name);
      } finally {
        await opfs.delete(scratch);
      }
    } else {
      saveBlob(await opfs.readFile(target), name);
    }
    return true;
  } catch (err) {
    console.error('download failed', err);
    return false;
  }
}

// Download the given files to the host.
export async function downloadFiles(filePaths: string[], onItem?: ProgressFn): Promise<BulkResult> {
  const failures: string[] = [];
  for (let i = 0; i < filePaths.length; i++) {
    onItem?.(basename(filePaths[i]), i, filePaths.length);
    if (!(await downloadOne(filePaths[i]))) failures.push(basename(filePaths[i]));
  }
  return { total: filePaths.length, failures, firstError: '' };
}

// Extract a peeler-recognised archive into a sibling "<base>_unpacked" folder.
// Returns the base name (for the output dir) and whether it succeeded.
export async function unpackArchive(path: string): Promise<{ ok: boolean; base: string }> {
  const name = basename(path);
  const parentDir = path.replace(/\/[^/]+$/, '');
  const base = name.replace(/\.[^.]+$/, '') || name;
  let ok = false;
  try {
    ok = (await gsEval('archive.extract', [path, `${parentDir}/${base}_unpacked`])) === true;
  } catch {
    ok = false;
  }
  return { ok, base };
}
