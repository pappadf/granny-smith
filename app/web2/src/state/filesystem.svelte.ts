// Reactive state for the Filesystem panel view. `expanded` is keyed by
// pathKey (treePath.ts joins with U+001F) and is persisted to localStorage by
// persist.svelte.ts; the rest is UI-only session state.
// `dragSourcePath` is set when a drag starts and cleared on dragend / drop.
//
// Selection supports multi-select restricted to one parent (all selected
// nodes are siblings). `selected` holds their pathKeys; `anchor` is the
// pathKey shift-range extends from. A SvelteSet keeps `.has()` reactive so
// the tree highlights update as the set is mutated in place.

import { SvelteSet } from 'svelte/reactivity';
import { setActiveTab } from './layout.svelte';
import { pathKey } from '@/lib/treePath';

interface FilesystemState {
  expanded: Record<string, boolean>;
  // A file to show (revealInFilesystem): the view re-reads its folders and
  // clears this once it has.
  reveal: string | null;
  dragSourcePath: string | null;
  selected: SvelteSet<string>;
  anchor: string | null;
}

export const filesystem: FilesystemState = $state({
  expanded: { '/opfs': true },
  reveal: null,
  dragSourcePath: null,
  selected: new SvelteSet<string>(),
  anchor: null,
});

export function toggleFsExpanded(key: string): void {
  filesystem.expanded[key] = !filesystem.expanded[key];
}

export function setFsExpanded(key: string, open: boolean): void {
  filesystem.expanded[key] = open;
}

export function setFsDragSource(key: string | null): void {
  filesystem.dragSourcePath = key;
}

// Replace the selection with a single key (plain click).
export function selectOnly(key: string): void {
  filesystem.selected.clear();
  filesystem.selected.add(key);
  filesystem.anchor = key;
}

// Toggle one key in/out of the selection (Cmd/Ctrl+click).
export function toggleSelected(key: string): void {
  if (filesystem.selected.has(key)) filesystem.selected.delete(key);
  else filesystem.selected.add(key);
  filesystem.anchor = key;
}

// Replace the selection with an explicit set, keeping/setting the anchor
// (Shift+click range).
export function setFsSelection(keys: string[], anchor: string | null): void {
  filesystem.selected.clear();
  for (const k of keys) filesystem.selected.add(k);
  filesystem.anchor = anchor;
}

export function clearFsSelection(): void {
  filesystem.selected.clear();
  filesystem.anchor = null;
}

// The tree path of an /opfs file: one id per folder from /opfs down
// ('/opfs/recordings/a.mkv' -> ['/opfs', '/opfs/recordings', '/opfs/recordings/a.mkv']).
export function opfsTreePath(path: string): string[] {
  const parts = path.split('/').filter(Boolean);
  const out: string[] = [];
  for (let i = 0; i < parts.length; i++) out.push(`/${parts.slice(0, i + 1).join('/')}`);
  return out;
}

// Show an /opfs file in the Filesystem tab: its folders open, it selected.
export function revealInFilesystem(path: string): void {
  const tp = opfsTreePath(path);
  for (let i = 1; i < tp.length; i++) filesystem.expanded[pathKey(tp.slice(0, i))] = true;
  selectOnly(pathKey(tp));
  filesystem.reveal = path;
  setActiveTab('filesystem');
}
