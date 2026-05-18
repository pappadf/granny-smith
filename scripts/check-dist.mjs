#!/usr/bin/env node
// Static validation of app/web2/dist/ produced by `make ui2` / Vite.
//
// Catches the deploy-blocker class of bug that shipped in v0.4.0, v0.4.1
// and v0.4.2: origin-rooted URLs that 404 once the bundle is deployed
// to a subpath like /gs-pages/latest/. Also confirms the COI service
// worker is wired up — without it, static hosts that can't set
// COOP/COEP headers have no SharedArrayBuffer and the WASM thread
// bridge can't start.
//
// Pure Node, no deps. Designed to fail fast in CI immediately after
// the Vite build step.

import { readFileSync, existsSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..');
const distDir = join(repoRoot, 'app', 'web2', 'dist');
const indexPath = join(distDir, 'index.html');
const swPath = join(distDir, 'coi-serviceworker.js');

if (!existsSync(indexPath)) {
  console.error(`check-dist: ${indexPath} does not exist — run 'make ui2' first`);
  process.exit(2);
}

const html = readFileSync(indexPath, 'utf8');
const failures = [];

// Walk every src= / href= attribute value.
const refs = [];
const re = /\s(?:src|href)\s*=\s*['"]([^'"]+)['"]/g;
let m;
while ((m = re.exec(html))) refs.push(m[1]);

if (refs.length === 0) {
  failures.push(`no src= / href= attributes found in ${indexPath} (parser miss?)`);
}

// Any path starting with '/' (and not '//' protocol-relative) is
// origin-rooted and will 404 on a subpath deploy. External absolute
// URLs (http://, https://) are fine — flagged separately if we later
// decide we don't want any.
for (const r of refs) {
  if (r.startsWith('//')) continue;
  if (r.startsWith('/')) failures.push(`origin-rooted reference in index.html: ${r}`);
}

// COI service worker must be registered AND the file must exist.
if (!html.includes('coi-serviceworker.js')) {
  failures.push(
    'index.html does not register coi-serviceworker.js — SharedArrayBuffer will be unavailable on static hosts',
  );
}
if (!existsSync(swPath)) {
  failures.push(`coi-serviceworker.js missing from dist/ (expected ${swPath})`);
}

if (failures.length) {
  console.error('check-dist: validation FAILED');
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}

console.log(`check-dist: OK (${refs.length} references in index.html, all relative; COI SW present)`);
