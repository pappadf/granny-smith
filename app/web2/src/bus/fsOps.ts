// Filesystem-tab operations — the actual I/O behind the OPFS browser:
// copy-out-of-image, move, delete, download, unpack. These are pure async
// functions returning structured results; the view layer owns the tree cache,
// refresh, toasts, and progress (via the optional `onItem` callback). Keeping
// them here makes the operations unit-testable without rendering a component.

import { gsEval } from './emulator';
import { opfs } from './opfs';
import { sanitizeName } from '@/lib/archive';
import { isInImageSpace, imageRootOf } from '@/lib/diskImage';
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

// Extract a human-readable reason from a gsEval result. The bridge encodes a
// C-side V_ERROR as {"error": "..."}; anything else stringifies.
function errText(res: unknown): string {
  if (res && typeof res === 'object' && 'error' in res) {
    return String((res as { error: unknown }).error);
  }
  return String(res);
}

function basename(path: string): string {
  return path.split('/').pop() ?? '';
}

// Copy items OUT of a read-only image into an OPFS folder (recursive for
// directories). Destination names are sanitised for OPFS. On a copy failure a
// stale image auto-mount is dropped and the copy retried once.
export async function copyOutOfImage(
  sources: CopySource[],
  dstDir: string,
  onItem?: ProgressFn,
): Promise<BulkResult> {
  const failures: string[] = [];
  let firstError = '';
  for (let i = 0; i < sources.length; i++) {
    const src = sources[i].path;
    const name = basename(src);
    onItem?.(name, i, sources.length);
    const dst = `${dstDir}/${opfsSafeName(name)}`;
    const args = sources[i].isDir ? ['-r', src, dst] : [src, dst];
    let res = await gsEval('storage.cp', args);
    if (res !== true) {
      // A cached image auto-mount can wedge after intervening OPFS changes;
      // drop it and retry once.
      const root = imageRootOf(src);
      if (root) {
        await gsEval('storage.unmount', [root]);
        res = await gsEval('storage.cp', args);
      }
    }
    if (res !== true) {
      failures.push(name);
      if (!firstError) firstError = errText(res);
      console.error('copy-out failed:', src, '->', dst, '|', errText(res));
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
        saveBlob(await opfs.readFile(scratch), name);
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
