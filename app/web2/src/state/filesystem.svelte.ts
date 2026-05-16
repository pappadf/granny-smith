// Reactive state for the Filesystem panel view. UI-only — not persisted.
// `expanded` is keyed by pathKey (treePath.ts joins with a space).
// `dragSourcePath` is set when a drag starts and cleared on dragend / drop.

interface FilesystemState {
  expanded: Record<string, boolean>;
  dragSourcePath: string | null;
  selectedPath: string | null;
}

export const filesystem: FilesystemState = $state({
  expanded: { '/opfs': true },
  dragSourcePath: null,
  selectedPath: null,
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

export function setFsSelected(key: string | null): void {
  filesystem.selectedPath = key;
}
