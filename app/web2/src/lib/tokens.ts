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

// Calls `cb` after the skin or the scheme changes (the data-skin and
// data-theme attributes on <html>), so code holding a measured value can
// read it again.  Returns the unsubscribe.
export function onAppearanceChange(cb: () => void): () => void {
  if (typeof MutationObserver === 'undefined' || typeof document === 'undefined') return () => {};
  const mo = new MutationObserver(() => cb());
  mo.observe(document.documentElement, {
    attributes: true,
    attributeFilter: ['data-skin', 'data-theme'],
  });
  return () => mo.disconnect();
}
