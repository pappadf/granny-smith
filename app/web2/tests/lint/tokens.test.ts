import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative } from 'node:path';
import { TOKENS, TOKEN_BY_NAME, LAYOUT_VARS } from '@/styles/contract';
import { skins } from '@/skins/registry';
import { parseRules, skinRules, skinDecls } from '../helpers/tokenCss';

// Lint guard: the styling contract (src/styles/contract.ts).  Every visual
// value in a component is a design token (a `--gs-*` custom property), every
// token a component reads is in the contract, every skin defines every
// semantic token in every scheme it supports, and no token read carries a
// private fallback that would silently render a dark-only default when the
// token goes missing.  A source-text scan, like no-model-name-matching.test.ts:
// auditable, no plugin.

const here = dirname(fileURLToPath(import.meta.url));
const SRC = join(here, '..', '..', 'src');

// Files under src/ with one of the extensions, test files excluded.
function walk(dir: string, ext: RegExp): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    if (statSync(full).isDirectory()) out.push(...walk(full, ext));
    else if (ext.test(entry) && !/\.(test|spec)\./.test(entry)) out.push(full);
  }
  return out;
}

const rel = (f: string) => relative(SRC, f).replace(/\\/g, '/');

// Comments out of CSS, JS and markup, so prose ("#180", "rgba") never counts.
function stripComments(text: string): string {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/(^|[^:'"`\\])\/\/[^\n]*/g, '$1');
}

const SOURCES = walk(SRC, /\.(svelte|ts|css)$/);

// Where token values live: the default stylesheets and the skins' tokens.
const DEFAULTS = ['styles/scale.css', 'styles/components.css'].map((f) => join(SRC, f));
const SKIN_TOKEN_FILES = SOURCES.filter((f) => /^skins\/[^/]+\/tokens\.css$/.test(rel(f)));
const VALUE_FILES = [...DEFAULTS, ...SKIN_TOKEN_FILES];

// Every token any value file defines, by file.
const DEFINED = new Map<string, string[]>();
for (const f of VALUE_FILES) {
  for (const m of stripComments(readFileSync(f, 'utf8')).matchAll(/(--gs-[a-z0-9-]+)\s*:/g)) {
    DEFINED.set(m[1], [...(DEFINED.get(m[1]) ?? []), rel(f)]);
  }
}
// The default stylesheets' :root values.
const DEFAULT_ROOT = new Map<string, string>();
for (const f of DEFAULTS)
  for (const r of parseRules(readFileSync(f, 'utf8')))
    if (r.selectors.includes(':root')) for (const [k, v] of r.decls) DEFAULT_ROOT.set(k, v);

// Literal colours belong where token values are defined, and as the
// fallbacks of the startup error page (shown before any stylesheet applies).
const LITERAL_OK = (f: string) => VALUE_FILES.includes(f);
const FALLBACK_OK = (f: string) => rel(f) === 'lib/webglErrorPage.ts';

const HEX = /#(?:[0-9a-fA-F]{3,4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})\b/;
const FUNC = /\b(?:rgba?|hsla?|hwb|lab|lch|oklab|oklch)\(/;
const NAMED =
  /\b(?:white|black|red|green|blue|gray|grey|silver|yellow|orange|purple|pink|brown|cyan|magenta|navy|teal|olive|maroon|lime|aqua|fuchsia)\b/;

// The CSS of a file: the <style> blocks of a component, all of a stylesheet.
function cssOf(f: string, text: string): string {
  if (f.endsWith('.css')) return text;
  if (f.endsWith('.svelte'))
    return [...text.matchAll(/<style[^>]*>([\s\S]*?)<\/style>/g)].map((m) => m[1]).join('\n');
  return '';
}

describe('design tokens', () => {
  it('finds the contract, the skins and the sources', () => {
    expect(TOKENS.length).toBeGreaterThan(100);
    expect(SKIN_TOKEN_FILES.length).toBe(skins.length);
    expect(SOURCES.length).toBeGreaterThan(60);
  });

  it('the contract names each token once', () => {
    expect(TOKEN_BY_NAME.size).toBe(TOKENS.length);
  });

  // L-1: a read of a token outside the contract is a typo or a leftover.
  it('L-1: every token read is in the contract', () => {
    const bad: string[] = [];
    for (const f of SOURCES) {
      const text = stripComments(readFileSync(f, 'utf8'));
      for (const m of text.matchAll(/var\(\s*(--gs-[a-z0-9-]+)/g)) {
        if (m[1].startsWith('--gs-ref-')) continue; // L-6
        if (!TOKEN_BY_NAME.has(m[1]) && !LAYOUT_VARS.includes(m[1])) bad.push(`${rel(f)}: ${m[1]}`);
      }
    }
    expect(bad).toEqual([]);
  });

  it('every token defined is in the contract', () => {
    const bad = [...DEFINED.keys()].filter(
      (n) => !TOKEN_BY_NAME.has(n) && !n.startsWith('--gs-ref-'),
    );
    expect(bad).toEqual([]);
  });

  // Scale, component and derived tokens are defaulted in styles/, derived ones
  // to the token the contract names.
  it('every non-semantic or derived token has its default', () => {
    const bad: string[] = [];
    for (const t of TOKENS) {
      const required = t.layer === 'semantic' && !t.derived;
      if (required) continue;
      if (!DEFINED.get(t.name)?.some((f) => f.startsWith('styles/')))
        bad.push(`${t.name}: no default`);
      if (t.derived && DEFAULT_ROOT.get(t.name) !== `var(${t.derived})`)
        bad.push(`${t.name}: default is not var(${t.derived})`);
    }
    expect(bad).toEqual([]);
  });

  // L-2: a skin missing a semantic token in a scheme renders that role with
  // nothing (or another scheme's value).
  it('L-2: every skin defines every semantic token in every scheme', () => {
    const rules = skinRules(SRC);
    const bad: string[] = [];
    for (const skin of skins) {
      const r = rules.get(skin.id);
      if (!r) {
        bad.push(`${skin.id}: no tokens.css`);
        continue;
      }
      for (const scheme of skin.schemes) {
        const decls = skinDecls(r, skin.id, scheme);
        for (const t of TOKENS)
          if (t.layer === 'semantic' && !t.derived && !decls.has(t.name))
            bad.push(`${skin.id} ${scheme}: ${t.name}`);
      }
    }
    expect(bad).toEqual([]);
  });

  // L-3: a fallback hides a missing token behind a value of one scheme.
  it('L-3: no token read carries a fallback', () => {
    const bad: string[] = [];
    for (const f of SOURCES) {
      if (FALLBACK_OK(f)) continue;
      const text = stripComments(readFileSync(f, 'utf8'));
      for (const m of text.matchAll(/var\(\s*(--gs-[a-z0-9-]+)\s*,/g))
        bad.push(`${rel(f)}: ${m[1]}`);
    }
    expect(bad).toEqual([]);
  });

  // L-4: a literal colour cannot follow the scheme or a skin.
  it('L-4: no literal colours outside the token stylesheets', () => {
    const bad: string[] = [];
    for (const f of SOURCES) {
      if (LITERAL_OK(f)) continue;
      const text = stripComments(readFileSync(f, 'utf8'));
      // The startup error page: literals only as the fallbacks of var().
      const scan = FALLBACK_OK(f) ? text.replace(/var\(--gs-[a-z0-9-]+,[^;`]*\)/g, '') : text;
      scan.split('\n').forEach((line, i) => {
        if (HEX.test(line) || FUNC.test(line)) bad.push(`${rel(f)}:${i + 1}: ${line.trim()}`);
      });
      for (const decl of cssOf(f, scan).matchAll(/([a-z-]+)\s*:\s*([^;{}]+);/g)) {
        if (decl[1].startsWith('--')) continue;
        if (NAMED.test(decl[2])) bad.push(`${rel(f)}: ${decl[1]}: ${decl[2].trim()}`);
      }
    }
    expect(bad).toEqual([]);
  });

  // L-5: a literal type size, weight, radius, z-order, shadow or duration
  // cannot follow a skin, nor can literal spacing (padding, margin, gap).
  it('L-5: no literal scale values in component styles', () => {
    const bad: string[] = [];
    const spacing: string[] = [];
    const ok = (v: string) => /^(0|inherit|none|initial|unset)$/.test(v.trim());
    // A value made of tokens, keywords and zero lengths only.
    const tokenised = (v: string) =>
      !/(^|[\s(,])-?\d*\.?\d+(px|em|rem|ms|s|%)(?=$|[\s),])/.test(
        v.replace(/var\([^()]*\)/g, '').replace(/(^|[\s(,])0(?=$|[\s),])/g, ''),
      );
    for (const f of SOURCES) {
      if (!f.endsWith('.svelte') || rel(f).startsWith('gallery/')) continue;
      const css = cssOf(f, stripComments(readFileSync(f, 'utf8')));
      for (const d of css.matchAll(/([a-z-]+)\s*:\s*([^;{}]+);/g)) {
        const [prop, value] = [d[1], d[2].trim()];
        if (ok(value)) continue;
        const scaled =
          /^(font-size|font-weight|border(-[a-z]+)*-radius|z-index|box-shadow)$/.test(prop) ||
          /^(transition|animation)(-duration)?$/.test(prop);
        if (scaled) {
          // A duration inside a transition or animation shorthand.
          const v = /^(transition|animation)$/.test(prop)
            ? value.replace(/var\([^()]*\)/g, '').match(/\b\d*\.?\d+m?s\b/)
              ? 'x1'
              : ''
            : value;
          if (/^(transition|animation)$/.test(prop) ? v : !tokenised(value))
            bad.push(`${rel(f)}: ${prop}: ${value}`);
        } else if (/^(padding|margin|gap|row-gap|column-gap)(-[a-z-]+)?$/.test(prop)) {
          // Lengths relative to the text (em, ch) are typographic, allowed.
          if (!tokenised(value.replace(/-?[\d.]+(em|ch)\b/g, '')))
            spacing.push(`${rel(f)}: ${prop}: ${value}`);
        }
      }
    }
    expect(bad).toEqual([]);
    expect(spacing).toEqual([]);
  });

  // L-8: focus must stay visible.  An outline removed outside a
  // :focus-visible rule (where a component shows focus its own way) hides the
  // keyboard focus base.css draws.
  it('L-8: no outline removed outside a :focus-visible rule', () => {
    const bad: string[] = [];
    for (const f of SOURCES) {
      if (!f.endsWith('.svelte')) continue;
      const css = cssOf(f, stripComments(readFileSync(f, 'utf8')));
      for (const r of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
        if (/outline\s*:\s*(none|0)\s*;/.test(r[2]) && !r[1].includes(':focus-visible'))
          bad.push(`${rel(f)}: ${r[1].trim()}`);
      }
    }
    expect(bad).toEqual([]);
  });

  // L-6: a skin's reference palette is private to its own token file.
  it('L-6: nothing outside a skin reads a reference token', () => {
    const bad: string[] = [];
    for (const f of SOURCES) {
      if (SKIN_TOKEN_FILES.includes(f)) continue;
      const text = stripComments(readFileSync(f, 'utf8'));
      if (/--gs-ref-/.test(text)) bad.push(rel(f));
    }
    expect(bad).toEqual([]);
  });

  // L-11: the browser's prompt, confirm and alert boxes cannot be skinned
  // (nor reached by the app's focus handling); state/dialogs asks instead.
  it('L-11: no native prompt, confirm or alert', () => {
    const bad: string[] = [];
    for (const f of SOURCES) {
      if (f.endsWith('.css')) continue;
      stripComments(readFileSync(f, 'utf8'))
        .split('\n')
        .forEach((line, i) => {
          if (/(?<![\w.$])(?:window\.)?(?:prompt|confirm|alert)\s*\(/.test(line))
            bad.push(`${rel(f)}:${i + 1}: ${line.trim()}`);
        });
    }
    expect(bad).toEqual([]);
  });

  // Report (not fail) contract tokens nothing reads yet.
  it('reports unread tokens', () => {
    const read = new Set<string>();
    for (const f of SOURCES) {
      if (VALUE_FILES.includes(f)) continue;
      for (const m of readFileSync(f, 'utf8').matchAll(/var\(\s*(--gs-[a-z0-9-]+)/g))
        read.add(m[1]);
    }
    // Token defaults read each other too.
    for (const f of VALUE_FILES)
      for (const m of readFileSync(f, 'utf8').matchAll(/var\(\s*(--gs-[a-z0-9-]+)/g))
        read.add(m[1]);
    const unread = TOKENS.filter((t) => !read.has(t.name)).map((t) => t.name);
    if (unread.length) console.warn(`tokens nothing reads yet: ${unread.join(', ')}`);
    expect(Array.isArray(unread)).toBe(true);
  });
});
