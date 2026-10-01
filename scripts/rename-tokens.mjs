#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// rename-tokens.mjs - rename the web2 design tokens to the semantic names.
//
// The token rename is one clean break: this rewrites every `--gs-*` read and
// definition under the given paths (default: app/web2/src, app/web2/tests,
// tests/e2e, docs/guide/web.md) from the old names to the new.  A branch
// that predates the rename can re-run it after merging main.  It matches
// whole names only (--gs-bg never matches --gs-bg-alt) and reports a token
// that was retired without a successor, which must be rewritten by hand.
//
// Usage: node scripts/rename-tokens.mjs [--dry-run] [path ...]
//
// Kept for one release after the rename, then deleted.

import { readFileSync, writeFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

// Old name -> new name; null: retired, no successor (a reader is an error).
export const RENAMES = {
  'bg': 'surface-app',
  'bg-alt': 'surface-raised',
  'bg-input': null,
  'fg': 'text',
  'fg-bright': 'text-strong',
  'fg-muted': 'text-muted',
  'fg-dim': 'text-subtle',
  'border-soft': 'border-subtle',
  'btn-hover': 'control-hover',
  'btn-active': 'control-active',
  'focus': 'focus-ring',
  'tab-active-fg': 'tab-fg-selected',
  'tab-inactive-fg': 'tab-fg',
  'list-hover': 'row-hover',
  'list-sel': null,
  'list-sel-inactive': 'row-selected-inactive',
  'list-sel-fg': 'row-selected-fg',
  'odd-row': 'row-alt',
  'link': 'text-link',
  'card-border': 'border-card',
  'primary-bg': 'accent',
  'primary-hover': 'accent-hover',
  'primary-active': 'accent-active',
  'primary-fg': 'text-on-accent',
  'sb-idle-bg': 'state-idle-bg',
  'sb-idle-fg': 'state-idle-fg',
  'sb-running': 'state-running-bg',
  'sb-paused': 'state-paused-bg',
  'sb-stopped': 'state-stopped-bg',
  'sb-fg-running': 'state-active-fg',
  'sb-hover': 'state-hover',
  'disasm-addr': 'code-address',
  'disasm-mnemonic': 'code-mnemonic',
  'disasm-operands': 'code-operand',
  'disasm-comment': 'code-comment',
  'disasm-pc': 'code-pc-marker',
  'disasm-pc-row-bg': 'code-pc-row-bg',
  'reg-name': 'code-reg-name',
  'reg-val': 'code-reg-value',
  'reg-changed': 'code-changed-fg',
  'section-heading': 'heading-fg',
  'section-head-bg': null,
  'logs-fg': 'log-fg',
  'screen-bg': 'surface-screen',
  'toast-info': 'info-solid',
  'toast-warning': 'warning-solid',
  'toast-error': 'danger-solid',
  'toast-shadow': 'shadow-toast',
  'terminal-bg': 'console-bg',
  'terminal-fg': 'console-fg',
  'terminal-cursor': 'console-cursor',
  'terminal-selection': 'console-selection',
  'changed-bg': 'code-changed-bg',
  'apple-green': 'success-solid',
};

const TEXT = /\.(svelte|ts|js|mjs|css|md|html)$/;

function walk(p, out) {
  const st = statSync(p);
  if (st.isDirectory()) {
    for (const e of readdirSync(p)) {
      if (e === 'node_modules' || e === 'dist' || e.endsWith('-snapshots')) continue;
      walk(join(p, e), out);
    }
  } else if (TEXT.test(p)) out.push(p);
  return out;
}

const args = process.argv.slice(2);
const dry = args.includes('--dry-run');
const targets = args.filter((a) => a !== '--dry-run');
const paths = (targets.length
  ? targets
  : ['app/web2/src', 'app/web2/tests', 'tests/e2e', 'docs/guide/web.md']
).map((p) => resolve(root, p));

// One pass over the whole name: `--gs-` followed by a maximal name.
const NAME = /--gs-([a-z0-9]+(?:-[a-z0-9]+)*)(?![a-z0-9-])/g;
let files = 0;
let edits = 0;
const retired = [];
for (const p of paths) {
  if (!existsSync(p)) continue;
  for (const f of walk(p, [])) {
    if (f.endsWith('rename-tokens.mjs')) continue;
    const text = readFileSync(f, 'utf8');
    let n = 0;
    const next = text.replace(NAME, (m, name) => {
      if (!(name in RENAMES)) return m;
      const to = RENAMES[name];
      if (to === null) {
        retired.push(`${f}: --gs-${name}`);
        return m;
      }
      n++;
      return `--gs-${to}`;
    });
    if (n) {
      files++;
      edits += n;
      if (dry) console.log(`${f}: ${n}`);
      else writeFileSync(f, next);
    }
  }
}
console.log(`rename-tokens: ${edits} names in ${files} files${dry ? ' (dry run)' : ''}`);
if (retired.length) {
  console.error('rename-tokens: retired tokens still referenced (rewrite by hand):');
  for (const r of retired) console.error(`  ${r}`);
  process.exit(1);
}
