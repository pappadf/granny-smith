#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// literal-to-token.mjs - replace literal scale values in the web2 components'
// <style> blocks with the scale tokens of app/web2/src/styles/scale.css.
//
// Only exact matches are converted (font-size: 11px -> var(--gs-font-size-xs)),
// so the result renders identically; a value off the scale is left alone and
// reported.  Covers font size and weight, radius, z-index, durations and
// easings, unitless line heights, the caps letter-spacing and transform,
// tabular numerals, border and focus widths, and padding / margin / gap.
//
// Usage: node scripts/literal-to-token.mjs [--dry-run] [file-or-dir ...]
// (default: app/web2/src).  Kept for one release, then deleted.

import { readFileSync, writeFileSync, readdirSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const FONT_SIZE = {
  '9px': '3xs', '10px': '2xs', '11px': 'xs', '12px': 'sm', '13px': 'base', '14px': 'md',
  '15px': 'lg', '16px': 'xl', '18px': '2xl', '22px': '3xl', '28px': '4xl',
};
const FONT_WEIGHT = { 200: 'light', 400: 'regular', 500: 'medium', 600: 'semibold', 700: 'bold' };
const RADIUS = { '2px': 'xs', '3px': 'sm', '4px': 'md', '6px': 'lg', '9999px': 'pill', '50%': 'round' };
const Z = { 1: 'raised', 5: 'sash', 10: 'layer', 100: 'drop', 2545: 'toast', 2600: 'modal', 2700: 'popover', 2800: 'menu' };
const DURATION = {
  '80ms': 'instant', '100ms': 'fast', '0.1s': 'fast', '.1s': 'fast', '150ms': 'quick',
  '0.15s': 'quick', '.15s': 'quick', '200ms': 'base', '0.2s': 'base', '250ms': 'slow',
  '300ms': 'slower', '0.3s': 'slower', '0.9s': 'spin', '.9s': 'spin', '900ms': 'spin',
  '1s': 'pulse', '1.3s': 'indeterminate',
};
const EASE = { 'ease-out': 'out', 'ease-in-out': 'in-out', linear: 'linear' };
const LINE_HEIGHT = { '1.4': 'base', '1.5': 'relaxed', '1.6': 'code' };
const SPACE = {
  '1px': 'px', '2px': '0-5', '4px': '1', '6px': '1-5', '8px': '2', '10px': '2-5', '12px': '3',
  '14px': '3-5', '16px': '4', '20px': '5', '24px': '6', '28px': '7', '32px': '8',
};
const SPACE_PROPS = /^(padding|margin|gap|row-gap|column-gap)(-(top|right|bottom|left|inline|block)(-(start|end))?)?$/;

const offScale = [];

// Each whitespace-separated px word of `value` through `map`.
function mapWords(value, map, prefix) {
  return value.replace(/(^|[\s(,])(-?[\d.]+(?:px|%)?)(?=$|[\s),])/g, (m, pre, word) => {
    if (word === '0') return m;
    if (map[word] !== undefined) return `${pre}var(--gs-${prefix}-${map[word]})`;
    return m;
  });
}

function convertDecl(prop, value, where) {
  const v = value.trim();
  if (v.includes('var(--gs-') && !/\d/.test(v.replace(/var\([^)]*\)/g, ''))) return value;
  let out = value;
  if (prop === 'font-size' && FONT_SIZE[v]) out = `var(--gs-font-size-${FONT_SIZE[v]})`;
  else if (prop === 'font-weight' && FONT_WEIGHT[v]) out = `var(--gs-font-weight-${FONT_WEIGHT[v]})`;
  else if (/^border(-[a-z]+)*-radius$/.test(prop)) out = mapWords(value, RADIUS, 'radius');
  else if (prop === 'z-index' && Z[v]) out = `var(--gs-z-${Z[v]})`;
  else if (prop === 'line-height' && LINE_HEIGHT[v]) out = `var(--gs-line-height-${LINE_HEIGHT[v]})`;
  else if (prop === 'letter-spacing' && v === '0.04em') out = 'var(--gs-caps-tracking)';
  else if (prop === 'text-transform' && v === 'uppercase') out = 'var(--gs-caps-transform)';
  else if (prop === 'font-variant-numeric' && v === 'tabular-nums') out = 'var(--gs-numeric)';
  else if (/^(transition|animation)(-duration)?$/.test(prop)) {
    out = value.replace(/(^|[\s,])([\d.]+m?s)(?=$|[\s,])/g, (m, pre, d) =>
      DURATION[d] ? `${pre}var(--gs-duration-${DURATION[d]})` : m,
    );
    out = out.replace(/(^|[\s,])(ease-in-out|ease-out|linear)(?=$|[\s,])/g, (m, pre, e) =>
      `${pre}var(--gs-ease-${EASE[e]})`,
    );
  } else if (SPACE_PROPS.test(prop)) out = mapWords(value, SPACE, 'space');
  else if (/^border(-(top|right|bottom|left))?$/.test(prop))
    out = value.replace(/^(\s*)1px(\s+solid)/, '$1var(--gs-border-width)$2').replace(/^(\s*)2px(\s+solid)/, '$1var(--gs-border-width-strong)$2');
  else if (prop === 'outline')
    out = value.replace(/^(\s*)1px(\s+solid)/, '$1var(--gs-focus-width)$2');
  else if (prop === 'outline-offset' && v === '-1px') out = 'var(--gs-focus-offset)';
  if (out === value && /[1-9]/.test(v) &&
      /^(font-size|font-weight|border-radius|z-index|transition|animation|padding|margin|gap)/.test(prop))
    offScale.push(`${where}: ${prop}: ${v}`);
  return out;
}

// Rewrite the declarations of one stylesheet text.
function convertCss(css, where) {
  return css.replace(/(^|[{;\s])([a-z-]+)(\s*:\s*)([^;{}]+)(;)/g, (m, pre, prop, colon, value, semi) => {
    if (prop.startsWith('--')) return m;
    return `${pre}${prop}${colon}${convertDecl(prop, value, where)}${semi}`;
  });
}

function walk(p, out) {
  if (statSync(p).isDirectory()) for (const e of readdirSync(p)) walk(join(p, e), out);
  else if (p.endsWith('.svelte')) out.push(p);
  return out;
}

const args = process.argv.slice(2);
const dry = args.includes('--dry-run');
const targets = args.filter((a) => a !== '--dry-run');
const files = (targets.length ? targets : ['app/web2/src']).flatMap((t) => walk(resolve(root, t), []));
let changed = 0;
for (const f of files) {
  const text = readFileSync(f, 'utf8');
  const where = relative(root, f);
  const next = text.replace(/(<style[^>]*>)([\s\S]*?)(<\/style>)/g, (m, open, css, close) =>
    open + convertCss(css, where) + close,
  );
  if (next !== text) {
    changed++;
    if (!dry) writeFileSync(f, next);
  }
}
console.log(`literal-to-token: ${changed} files${dry ? ' would change' : ' changed'}`);
if (offScale.length) {
  console.log(`off the scale (left as they are, ${offScale.length}):`);
  for (const o of offScale) console.log(`  ${o}`);
}
