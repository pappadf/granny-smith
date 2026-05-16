// Keyboard-navigation helpers for list-shaped widgets (Tree rows,
// CommandBrowser tree, Tab strips, Disassembly rows). Returns the next
// selection index from the current one and a key. Pure — no DOM access.

export type ListKey =
  | 'ArrowUp'
  | 'ArrowDown'
  | 'Home'
  | 'End'
  | 'PageUp'
  | 'PageDown'
  | 'ArrowLeft'
  | 'ArrowRight';

export interface CycleOptions {
  /** Treat the list as cyclic so ↓ from the last lands on the first. */
  wrap?: boolean;
  /** Step size for PageUp / PageDown. Default 10. */
  pageSize?: number;
}

// Returns the new index after applying `key` to `current` in a list of
// `count` items. `current = -1` is interpreted as "before the start"
// (ArrowDown lands on 0). When `wrap` is false the result clamps;
// when true it wraps modulo count.
export function cycleListSelection(
  count: number,
  current: number,
  key: ListKey,
  opts: CycleOptions = {},
): number {
  if (count <= 0) return -1;
  const wrap = opts.wrap ?? false;
  const pageSize = opts.pageSize ?? 10;

  const clamp = (n: number) => Math.max(0, Math.min(count - 1, n));
  const wrapMod = (n: number) => ((n % count) + count) % count;

  let next: number;
  switch (key) {
    case 'ArrowDown':
    case 'ArrowRight':
      next = current < 0 ? 0 : current + 1;
      break;
    case 'ArrowUp':
    case 'ArrowLeft':
      next = current < 0 ? count - 1 : current - 1;
      break;
    case 'Home':
      next = 0;
      break;
    case 'End':
      next = count - 1;
      break;
    case 'PageDown':
      next = current < 0 ? Math.min(pageSize - 1, count - 1) : current + pageSize;
      break;
    case 'PageUp':
      next = current < 0 ? 0 : current - pageSize;
      break;
  }
  return wrap ? wrapMod(next) : clamp(next);
}

// Match a KeyboardEvent.key against the ListKey set. Returns null when
// the key isn't a list-nav key — consumers should fall through to their
// normal handling.
export function listKeyFromEvent(ev: KeyboardEvent): ListKey | null {
  switch (ev.key) {
    case 'ArrowUp':
    case 'ArrowDown':
    case 'ArrowLeft':
    case 'ArrowRight':
    case 'Home':
    case 'End':
    case 'PageUp':
    case 'PageDown':
      return ev.key;
    default:
      return null;
  }
}
