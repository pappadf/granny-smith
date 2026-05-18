// Mock watchpoint store. Phase 6 ships the UI structure against this
// in-memory $state list; Phase 7 swaps in a real implementation that
// wires to `debug.logpoints --read/--write` (or a new
// `debug.watchpoints` subtree) and actually fires on memory access.
//
// The TypeScript surface here is the eventual real surface — only the
// underlying storage changes.

export interface Watchpoint {
  id: number;
  lo: number;
  hi: number;
  mode: 'r' | 'w' | 'rw';
  enabled: boolean;
}

interface State {
  entries: Watchpoint[];
}

export const watchpoints: State = $state({ entries: [] });

let nextId = 1;

export function addWatchpoint(lo: number, hi: number, mode: Watchpoint['mode']): number {
  const id = nextId++;
  watchpoints.entries.push({ id, lo: lo >>> 0, hi: hi >>> 0, mode, enabled: true });
  return id;
}

export function removeWatchpoint(id: number): void {
  const ix = watchpoints.entries.findIndex((w) => w.id === id);
  if (ix >= 0) watchpoints.entries.splice(ix, 1);
}

export function toggleWatchpoint(id: number): void {
  const w = watchpoints.entries.find((e) => e.id === id);
  if (w) w.enabled = !w.enabled;
}

// Test-only helper: drop all watchpoints + reset id counter so test
// runs don't leak state into each other.
export function _resetWatchpointsForTests(): void {
  watchpoints.entries.length = 0;
  nextId = 1;
}
