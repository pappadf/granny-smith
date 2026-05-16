// Pure state-transition table for the Display/Filesystem drag-and-drop
// state machine specced in ui-design-spec.md §8.5. Lives in lib/ (not
// the component) so unit tests can drive it without a DOM.
//
//   Idle ──dragenter(Files)──▶ Active
//                              ├─ over-display ──▶ Display
//                              ├─ over-fs-tree ──▶ FsTree
//                              └─ over-other   ──▶ Active (no overlay)
//   Display / FsTree ──leave (dragDepth=0) / drop / end / viewport-exit──▶ Idle

export type DragState = 'idle' | 'active' | 'display' | 'fs-tree';

export type DragEvt =
  | { kind: 'enter'; hasFiles: boolean }
  | { kind: 'leave-all' }
  | { kind: 'over-display' }
  | { kind: 'over-fs-tree' }
  | { kind: 'over-other' }
  | { kind: 'drop' }
  | { kind: 'end' }
  | { kind: 'viewport-exit' };

export function nextDragState(s: DragState, e: DragEvt): DragState {
  switch (e.kind) {
    case 'enter':
      // Only react to file drags; ignore internal drag events (those
      // don't carry the Files type — they carry a typed MIME like
      // application/x-gs-tree-path).
      if (!e.hasFiles) return s;
      return s === 'idle' ? 'active' : s;
    case 'leave-all':
    case 'drop':
    case 'end':
    case 'viewport-exit':
      return 'idle';
    case 'over-display':
      return s === 'idle' ? s : 'display';
    case 'over-fs-tree':
      return s === 'idle' ? s : 'fs-tree';
    case 'over-other':
      // Stay in the "drag is happening somewhere" state but drop any
      // overlay highlight that the previous over-* event set.
      return s === 'idle' ? s : 'active';
  }
}

// Respect the OS-level reduced-motion preference. Used by the overlay
// component to set transition: 0ms.
export function isReducedMotion(): boolean {
  if (typeof window === 'undefined' || !window.matchMedia) return false;
  return window.matchMedia('(prefers-reduced-motion: reduce)').matches === true;
}

// Helper: is the (x, y) point inside the viewport? Used to detect
// drags leaving via the chrome edge.
export function isOutsideViewport(x: number, y: number): boolean {
  if (typeof window === 'undefined') return false;
  return x < 0 || y < 0 || x >= window.innerWidth || y >= window.innerHeight;
}
