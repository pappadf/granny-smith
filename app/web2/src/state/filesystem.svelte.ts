// Reactive state for the Filesystem panel view. UI-only — not persisted.
// `expanded` is keyed by pathKey (treePath.ts joins with a space).
// `dragSourcePath` is set when a drag starts and cleared on dragend / drop.
//
// Selection supports multi-select restricted to one parent (all selected
// nodes are siblings). `selected` holds their pathKeys; `anchor` is the
// pathKey shift-range extends from. A SvelteSet keeps `.has()` reactive so
// the tree highlights update as the set is mutated in place.

import { SvelteSet } from 'svelte/reactivity';

interface FilesystemState {
  expanded: Record<string, boolean>;
  dragSourcePath: string | null;
  selected: SvelteSet<string>;
  anchor: string | null;
}

export const filesystem: FilesystemState = $state({
  expanded: { '/opfs': true },
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
