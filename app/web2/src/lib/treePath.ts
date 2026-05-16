// Path-array helpers shared by Tree and FilesystemView. A "tree path" here is
// an array of string ids identifying one node from the root downward, e.g.
//   ['/opfs', '/opfs/images', '/opfs/images/rom', '/opfs/images/rom/Plus.rom']
// for a Filesystem node, or
//   ['cpu', 'cpu.pc'] for a Machine node.
//
// Keeping the helpers pure (no Svelte state) makes them unit-testable and
// reusable from the Filesystem drag-drop validator.

export type TreePath = readonly string[];

// String-form of a path, used as a Map/Record key (e.g. for `expanded`).
// Separator is a literal space — Filesystem segments are slash-prefixed
// absolute paths, so spaces don't collide with anything inside an id.
const SEP = ' ';

export function pathKey(path: TreePath): string {
  return path.join(SEP);
}

export function pathKeyToArray(key: string): string[] {
  return key.length === 0 ? [] : key.split(SEP);
}

// True iff `descendant` is the same path as `ancestor` or a strict descendant.
// Used to reject drop targets inside the dragged subtree.
export function pathIsAncestorOrSelf(ancestor: TreePath, descendant: TreePath): boolean {
  if (descendant.length < ancestor.length) return false;
  for (let i = 0; i < ancestor.length; i++) {
    if (ancestor[i] !== descendant[i]) return false;
  }
  return true;
}

// True iff `a` and `b` share the same parent path (siblings or self).
export function pathSiblingsOrSelf(a: TreePath, b: TreePath): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length - 1; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

// Parent path. Empty path returns empty.
export function parentPath(path: TreePath): TreePath {
  return path.slice(0, Math.max(0, path.length - 1));
}

// The terminal id of the path (last segment) — typically the filename or
// the last gsEval path segment. Returns '' for an empty path.
export function lastSegment(path: TreePath): string {
  return path[path.length - 1] ?? '';
}

// Joins a path into a slash-form human label (Filesystem paths happen to
// already use slashes as ids, so this is mostly a passthrough; included
// here so tests cover the edge case of empty paths).
export function pathToLabel(path: TreePath): string {
  if (!path.length) return '/';
  return lastSegment(path);
}
