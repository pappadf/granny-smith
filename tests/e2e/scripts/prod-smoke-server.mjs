#!/usr/bin/env node
// Minimal HTTP server for the ui-prod-smoke Playwright test.
//
// Mounts app/web2/dist/ at a SUBPATH (default /sub/path/) and
// deliberately does NOT send COOP/COEP headers. That forces the page
// to bring up cross-origin isolation via the in-page
// coi-serviceworker — the exact production environment that v0.4.0,
// v0.4.1 and v0.4.2 each broke without us noticing.
//
// `make run` serves dist/ from root WITH COI headers, masking both
// failure modes. This server intentionally does neither.
//
// Pure Node, no deps. Pass --root to point at a different bundle
// (e.g. for testing release artifacts in CI).

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { join, resolve, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const argv = Object.fromEntries(
  process.argv.slice(2).map((a) => {
    const [k, v] = a.replace(/^--/, '').split('=');
    return [k, v ?? true];
  }),
);

const here = fileURLToPath(new URL('.', import.meta.url));
const repoRoot = resolve(here, '..', '..', '..');

const PORT = parseInt(argv.port ?? '18181', 10);
const SUBPATH = (argv.subpath ?? '/sub/path/').replace(/\/?$/, '/');
const ROOT = resolve(argv.root ?? join(repoRoot, 'app', 'web2', 'dist'));

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript',
  '.mjs': 'application/javascript',
  '.wasm': 'application/wasm',
  '.css': 'text/css',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.json': 'application/json',
  '.map': 'application/json',
};

const server = createServer(async (req, res) => {
  try {
    const url = (req.url ?? '/').split('?')[0];
    if (!url.startsWith(SUBPATH)) {
      res.writeHead(404).end('Not under subpath');
      return;
    }
    let rel = url.slice(SUBPATH.length);
    if (rel === '' || rel.endsWith('/')) rel += 'index.html';
    const filePath = join(ROOT, rel);

    // Path traversal guard.
    if (!filePath.startsWith(ROOT)) {
      res.writeHead(403).end('Forbidden');
      return;
    }

    const s = await stat(filePath).catch(() => null);
    if (!s || !s.isFile()) {
      res.writeHead(404).end('Not Found: ' + rel);
      return;
    }
    const buf = await readFile(filePath);
    const type = MIME[extname(filePath)] ?? 'application/octet-stream';
    // Intentionally NO Cross-Origin-Opener-Policy and NO
    // Cross-Origin-Embedder-Policy headers — the bundle must bring up
    // COI through its registered service worker.
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': buf.byteLength });
    res.end(buf);
  } catch (err) {
    res.writeHead(500).end(String(err));
  }
});

server.listen(PORT, () => {
  console.log(`prod-smoke server: http://localhost:${PORT}${SUBPATH}`);
  console.log(`  serving: ${ROOT}`);
  console.log(`  COOP/COEP headers: NOT sent (bundle must register its own COI SW)`);
});
