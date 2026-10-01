import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { CONTRAST_PAIRS } from '@/styles/contract';
import { skins, DEFAULT_SKIN } from '@/skins/registry';
import {
  parseRules,
  skinRules,
  skinDecls,
  resolveVars,
  parseColor,
  over,
  contrast,
} from '../helpers/tokenCss';

// Lint guard: legible colour pairs.  For every skin and scheme, resolve the
// token values (the defaults in styles/, then the skin's own) and compute the
// WCAG 2.2 contrast of every pair in contract.ts's CONTRAST_PAIRS, without a
// browser.  A background with alpha is painted over the app surface first.

const here = dirname(fileURLToPath(import.meta.url));
const SRC = join(here, '..', '..', 'src');

// Pairs below their minimum on purpose, with the ratio measured when they were
// accepted ("skin scheme fg on bg": ratio).  Every entry needs a reason.
const KNOWN: Record<string, number> = {
  // The toast badges carry VS Code's notification colours; the glyph on them
  // is an icon, and the badge keeps its colour as the signal.
  'workbench dark --gs-info-on-solid on --gs-info-solid': 2.59,
  'workbench dark --gs-success-on-solid on --gs-success-solid': 3.33,
  'workbench dark --gs-danger-on-solid on --gs-danger-solid': 3.57,
  // The paused bar is the status bar's orange in both schemes.
  'workbench dark --gs-state-active-fg on --gs-state-paused-bg': 3.81,
  'workbench light --gs-state-active-fg on --gs-state-paused-bg': 3.81,
};

const defaults = [
  ...parseRules(readFileSync(join(SRC, 'styles/scale.css'), 'utf8')),
  ...parseRules(readFileSync(join(SRC, 'styles/components.css'), 'utf8')),
];
const rules = skinRules(SRC);

// The resolved token values of one skin in one scheme, with the status bar's
// rules for `scope` applied.
function env(skin: string, scheme: string, scope?: string): Map<string, string> {
  const e = new Map<string, string>();
  for (const r of defaults) {
    const root = r.selectors.includes(':root');
    const scoped = scope && r.selectors.some((s) => s === `.gs-statusbar${scope}`);
    if (root || scoped) for (const [k, v] of r.decls) e.set(k, v);
  }
  const own = skinDecls(rules.get(skin) ?? [], skin, scheme);
  for (const [k, v] of own) e.set(k, v);
  return e;
}

describe('colour contrast', () => {
  const results: string[] = [];
  const failures: string[] = [];
  for (const skin of skins) {
    for (const scheme of skin.schemes) {
      for (const p of CONTRAST_PAIRS) {
        const e = env(skin.id, scheme, p.scope);
        const base = parseColor(resolveVars(`var(${p.over ?? '--gs-surface-app'})`, e));
        const bg = over(
          parseColor(resolveVars(`var(${p.bg})`, e)),
          over(base, { r: 0, g: 0, b: 0, a: 1 }),
        );
        const fg = over(parseColor(resolveVars(`var(${p.fg})`, e)), bg);
        const ratio = Math.round(contrast(fg, bg) * 100) / 100;
        const key = `${skin.id} ${scheme} ${p.fg} on ${p.bg}${p.scope ? ` ${p.scope}` : ''}`;
        results.push(`${key}: ${ratio}`);
        if (ratio < p.min && KNOWN[key] === undefined) failures.push(`${key}: ${ratio} < ${p.min}`);
      }
    }
  }

  it('checks every pair of every skin', () => {
    expect(skins.some((s) => s.id === DEFAULT_SKIN)).toBe(true);
    expect(results.length).toBeGreaterThan(50);
  });

  it('every pair meets its minimum (or is a known, explained exception)', () => {
    expect(failures).toEqual([]);
  });

  // An exception whose pair now passes, or whose ratio moved, is stale.
  it('every known exception is still measured as recorded', () => {
    const measured = new Map(
      results.map((r) => [r.slice(0, r.lastIndexOf(':')), Number(r.slice(r.lastIndexOf(':') + 1))]),
    );
    const stale = Object.entries(KNOWN).filter(([k, v]) => measured.get(k) !== v);
    expect(stale).toEqual([]);
  });
});
