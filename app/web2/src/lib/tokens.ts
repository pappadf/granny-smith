// Design tokens read from JavaScript.  CSS is the source of every visual
// value; code that needs one (a row height to scroll by, say) reads it from
// the token instead of holding its own copy, and reads it again when the
// appearance changes.

// A token's computed value on <html>, trimmed ('' when unset).
export function readToken(name: `--gs-${string}`): string {
  if (typeof document === 'undefined') return '';
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

// A length token in px; `fallbackPx` when it is unset or not a px length
// (no stylesheet in a unit test).
export function readMetric(name: `--gs-${string}`, fallbackPx: number): number {
  const m = readToken(name).match(/^(-?[\d.]+)px$/);
  const v = m ? Number(m[1]) : NaN;
  return Number.isFinite(v) ? v : fallbackPx;
}
