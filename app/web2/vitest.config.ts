import { defineConfig, mergeConfig } from 'vitest/config';
import viteConfig from './vite.config';

export default mergeConfig(
  viteConfig,
  defineConfig({
    resolve: {
      // Tell vite (and vitest) to pick the client-side build of Svelte.
      // Without this, vitest pulls in svelte/internal/server which makes
      // mount() throw "not available on the server" inside jsdom tests.
      conditions: ['browser'],
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
