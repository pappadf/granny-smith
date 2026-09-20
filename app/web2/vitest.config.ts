import { defineConfig, mergeConfig } from 'vitest/config';
import viteConfig from './vite.config';
import { fileURLToPath } from 'node:url';

export default mergeConfig(
  viteConfig,
  defineConfig({
    resolve: {
      // Tell vite (and vitest) to pick the client-side build of Svelte.
      // Without this, vitest pulls in svelte/internal/server which makes
      // mount() throw "not available on the server" inside jsdom tests.
      conditions: ['browser'],
    },
    server: {
      // The printer test loads the interpreter module the wasm build
      // produces (build/platen-<version>.js), which lies outside this
      // package: let Vite serve the repository root to the test runner.
      fs: { allow: [fileURLToPath(new URL('../../', import.meta.url))] },
    },
    test: {
      environment: 'jsdom',
      globals: true,
      include: ['tests/**/*.{test,spec}.{ts,svelte}'],
      setupFiles: ['./tests/setup.ts'],
      passWithNoTests: true,
    },
  }),
);
