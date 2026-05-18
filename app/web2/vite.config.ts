import { defineConfig, type Plugin, type ViteDevServer, type PreviewServer } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { fileURLToPath } from 'node:url';
import { readFile, stat } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const buildDir = resolve(fileURLToPath(new URL('../../build', import.meta.url)));

// Phase 3: serve build/main.{mjs,wasm}, build/wasm/*, build/coi-serviceworker.js
// from /<repo>/build/ directly via dev + preview middleware. No copy step is
// needed during dev; `make ui2` copies these into dist/ for production.
function serveBuildArtifacts(): Plugin {
  const matchers: RegExp[] = [
    /^\/main\.mjs(\?.*)?$/,
    /^\/main\.wasm(\?.*)?$/,
    /^\/coi-serviceworker\.js(\?.*)?$/,
    /^\/wasm\//,
  ];

  const handle = (
    req: { url?: string },
    res: {
      writeHead: (code: number, headers?: Record<string, string>) => void;
      end: (body?: Buffer | string) => void;
    },
    next: () => void,
  ) => {
    const url = req.url;
    if (!url) return next();
    if (!matchers.some((re) => re.test(url))) return next();
    const cleanUrl = url.split('?')[0];
    const filePath = join(buildDir, cleanUrl.replace(/^\//, ''));
    void (async () => {
      try {
        const st = await stat(filePath);
        if (!st.isFile()) return next();
        const data = await readFile(filePath);
        const ext = filePath.split('.').pop();
        const ct =
          ext === 'mjs' || ext === 'js'
            ? 'text/javascript'
            : ext === 'wasm'
              ? 'application/wasm'
              : 'application/octet-stream';
        res.writeHead(200, {
          'Content-Type': ct,
          'Content-Length': String(data.byteLength),
          'Cross-Origin-Embedder-Policy': 'require-corp',
        });
        res.end(data);
      } catch {
        next();
      }
    })();
  };

  return {
    name: 'gs-serve-build-artifacts',
    configureServer(server: ViteDevServer) {
      server.middlewares.use(handle);
    },
    configurePreviewServer(server: PreviewServer) {
      server.middlewares.use(handle);
    },
  };
}

const coiHeaders = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
};

export default defineConfig({
  plugins: [svelte(), serveBuildArtifacts()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  build: {
    target: 'es2022',
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    strictPort: false,
    headers: coiHeaders,
  },
  preview: {
    port: 4173,
    headers: coiHeaders,
  },
});
