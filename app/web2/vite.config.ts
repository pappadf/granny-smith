import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { fileURLToPath } from 'node:url';

// Cross-origin isolation is required for SharedArrayBuffer, which the
// Emscripten emulator depends on once bus/emulator.ts arrives in Phase 3.
// Setting the headers here from Phase 0 means the dev server is ready when
// that integration lands - one less footgun. See plan-doc §10 and risk #1.
const coiHeaders = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
};

export default defineConfig({
  plugins: [svelte()],
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
