// Reading the token stylesheets for the token and contrast lints: the rules
// of a stylesheet, the declarations a skin makes, and colour
// arithmetic over resolved values.
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

export interface CssRule {
  selectors: string[];
  decls: Map<string, string>;
}

// The flat rules of a stylesheet (token files never nest).
export function parseRules(css: string): CssRule[] {
  const text = css.replace(/\/\*[\s\S]*?\*\//g, '');
  const rules: CssRule[] = [];
  for (const m of text.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    const selectors = m[1]
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean);
    const decls = new Map<string, string>();
    for (const d of m[2].matchAll(/(--[a-z0-9-]+)\s*:\s*([^;]+);/g)) decls.set(d[1], d[2].trim());
    rules.push({ selectors, decls });
  }
  return rules;
}

// The skins under src/skins: id -> its token stylesheet's rules.
export function skinRules(srcDir: string): Map<string, CssRule[]> {
  const dir = join(srcDir, 'skins');
  const out = new Map<string, CssRule[]>();
  for (const e of readdirSync(dir)) {
    const f = join(dir, e, 'tokens.css');
    if (statSync(join(dir, e)).isDirectory()) out.set(e, parseRules(readFileSync(f, 'utf8')));
  }
  return out;
}

// Whether a selector applies to skin `id`: the skin's own block (not the
// bare :root, the pre-paint fallback the default skin also gives).
export function selectorApplies(sel: string, id: string): boolean {
  const s = sel.replace(/"/g, "'");
  return s.startsWith(':root[') && s.includes(`[data-skin='${id}']`);
}

// The declarations a skin makes.
export function skinDecls(rules: CssRule[], id: string): Map<string, string> {
  const out = new Map<string, string>();
  for (const r of rules) {
    if (r.selectors.some((s) => selectorApplies(s, id)))
      for (const [k, v] of r.decls) out.set(k, v);
  }
  return out;
}

export interface Rgba {
  r: number;
  g: number;
  b: number;
  a: number;
}

// Substitute var() references from `env` until none remain.
export function resolveVars(value: string, env: Map<string, string>, depth = 0): string {
  if (depth > 20) throw new Error(`var() cycle in ${value}`);
  const next = value.replace(/var\(\s*(--[a-z0-9-]+)\s*(?:,\s*([^()]*))?\)/g, (_m, name, fb) => {
    const v = env.get(name);
    if (v === undefined) {
      if (fb !== undefined) return fb;
      throw new Error(`undefined token ${name}`);
    }
    return v;
  });
  return next === value ? next : resolveVars(next, env, depth + 1);
}

// A resolved CSS colour: hex, rgb()/rgba() (comma or space syntax),
// transparent, or color-mix(in srgb, <colour> p%, transparent).
export function parseColor(v: string): Rgba {
  const s = v.trim().toLowerCase();
  if (s === 'transparent') return { r: 0, g: 0, b: 0, a: 0 };
  const mix = s.match(/^color-mix\(in srgb,\s*(.+?)\s+([\d.]+)%,\s*transparent\)$/);
  if (mix) {
    const c = parseColor(mix[1]);
    return { ...c, a: c.a * (Number(mix[2]) / 100) };
  }
  const hex = s.match(/^#([0-9a-f]{3,8})$/);
  if (hex) {
    let h = hex[1];
    if (h.length <= 4) h = [...h].map((c) => c + c).join('');
    const n = (i: number) => parseInt(h.slice(i, i + 2), 16);
    return { r: n(0), g: n(2), b: n(4), a: h.length === 8 ? n(6) / 255 : 1 };
  }
  const fn = s.match(/^rgba?\(([^)]*)\)$/);
  if (fn) {
    const parts = fn[1]
      .split(/[\s,/]+/)
      .filter(Boolean)
      .map(Number);
    return { r: parts[0], g: parts[1], b: parts[2], a: parts.length > 3 ? parts[3] : 1 };
  }
  throw new Error(`unparsed colour: ${v}`);
}

// `top` painted over an opaque `bottom`.
export function over(top: Rgba, bottom: Rgba): Rgba {
  const a = top.a;
  return {
    r: top.r * a + bottom.r * (1 - a),
    g: top.g * a + bottom.g * (1 - a),
    b: top.b * a + bottom.b * (1 - a),
    a: 1,
  };
}

// WCAG 2.2 relative luminance and contrast ratio.
function luminance(c: Rgba): number {
  const ch = (x: number) => {
    const v = x / 255;
    return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * ch(c.r) + 0.7152 * ch(c.g) + 0.0722 * ch(c.b);
}
export function contrast(a: Rgba, b: Rgba): number {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}
