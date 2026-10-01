import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative } from 'node:path';

// Lint guard: the styling contract.  Every visual value in a component is a
// design token (a `--gs-*` custom property), every token a component reads
// exists, and no token read carries a private fallback that would silently
// render a dark-only default when the token goes missing.  A source-text scan,
// like no-model-name-matching.test.ts: auditable, no plugin.

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

// The token stylesheets: where tokens are defined (and literals belong).
const TOKEN_FILES = walk(join(SRC, 'styles'), /\.css$/);
const SOURCES = walk(SRC, /\.(svelte|ts|css)$/);

// Every `--gs-*` name some token stylesheet defines.
const DEFINED = new Set<string>();
for (const f of TOKEN_FILES) {
  for (const m of readFileSync(f, 'utf8').matchAll(/(--gs-[a-z0-9-]+)\s*:/g)) DEFINED.add(m[1]);
}

// Layout variables set inline at run time, not tokens.
const LAYOUT_VARS = new Set(['--gs-panel-size']);

// Literal colours may appear only where token values are defined, and as the
// fallbacks of the startup error page (shown before any stylesheet may apply).
const LITERAL_OK = (f: string) => TOKEN_FILES.includes(f);
const FALLBACK_OK = (f: string) => rel(f) === 'lib/webglErrorPage.ts';

const HEX = /#(?:[0-9a-fA-F]{3,4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})\b/;
const FUNC = /\b(?:rgba?|hsla?|hwb|lab|lch|oklab|oklch)\(/;
const NAMED =
  /\b(?:white|black|red|green|blue|gray|grey|silver|yellow|orange|purple|pink|brown|cyan|magenta|navy|teal|olive|maroon|lime|aqua|fuchsia)\b/;

// The CSS of a file: the <style> blocks of a component, all of a stylesheet,
// the template literals of a script (inline CSS strings).
function cssOf(f: string, text: string): string {
  if (f.endsWith('.css')) return text;
  if (f.endsWith('.svelte'))
    return [...text.matchAll(/<style[^>]*>([\s\S]*?)<\/style>/g)].map((m) => m[1]).join('\n');
  return '';
}

describe('design tokens', () => {
  it('finds the token stylesheets and the sources', () => {
    expect(TOKEN_FILES.length).toBeGreaterThan(0);
    expect(DEFINED.size).toBeGreaterThan(100);
    expect(SOURCES.length).toBeGreaterThan(60);
  });

  // L-1: a read of an undefined token renders nothing (or a fallback).
  it('L-1: every token read is defined', () => {
    const bad: string[] = [];
    for (const f of SOURCES) {
      const text = stripComments(readFileSync(f, 'utf8'));
      for (const m of text.matchAll(/var\(\s*(--gs-[a-z0-9-]+)/g)) {
        if (!DEFINED.has(m[1]) && !LAYOUT_VARS.has(m[1])) bad.push(`${rel(f)}: ${m[1]}`);
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
      const lines = scan.split('\n');
      lines.forEach((line, i) => {
        if (HEX.test(line) || FUNC.test(line)) bad.push(`${rel(f)}:${i + 1}: ${line.trim()}`);
      });
      for (const decl of cssOf(f, scan).matchAll(/([a-z-]+)\s*:\s*([^;{}]+);/g)) {
        if (decl[1].startsWith('--')) continue;
        if (NAMED.test(decl[2])) bad.push(`${rel(f)}: ${decl[1]}: ${decl[2].trim()}`);
      }
    }
    expect(bad).toEqual([]);
  });
});
