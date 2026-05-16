// Reactive state for the Checkpoints panel view. Holds sort + selection.

export type CheckpointSortColumn = 'name' | 'machine' | 'date' | 'size';
export type SortDirection = 'asc' | 'desc';

interface CheckpointsState {
  selectedDir: string | null;
  sortColumn: CheckpointSortColumn;
  sortDir: SortDirection;
}

export const checkpoints: CheckpointsState = $state({
  selectedDir: null,
  sortColumn: 'date',
  sortDir: 'desc',
});

export function setCheckpointSort(col: CheckpointSortColumn): void {
  if (checkpoints.sortColumn === col) {
    checkpoints.sortDir = checkpoints.sortDir === 'asc' ? 'desc' : 'asc';
  } else {
    checkpoints.sortColumn = col;
    checkpoints.sortDir = col === 'date' ? 'desc' : 'asc';
  }
}

export function selectCheckpoint(dir: string | null): void {
  checkpoints.selectedDir = dir;
}
