import js from '@eslint/js';
import tseslint from 'typescript-eslint';
import svelte from 'eslint-plugin-svelte';
import svelteParser from 'svelte-eslint-parser';
import globals from 'globals';

export default [
  { ignores: ['dist/**', 'node_modules/**', '.svelte-kit/**', 'public/icons/sprite.svg'] },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  ...svelte.configs['flat/recommended'],
  {
    languageOptions: {
      globals: {
        ...globals.browser,
        ...globals.es2022,
      },
    },
  },
  {
    files: ['**/*.svelte'],
    languageOptions: {
      parser: svelteParser,
      parserOptions: {
        parser: tseslint.parser,
        extraFileExtensions: ['.svelte'],
        svelteFeatures: { runes: true },
      },
    },
  },
  {
    files: ['vite.config.ts', 'vitest.config.ts', 'svelte.config.js', 'eslint.config.js'],
    languageOptions: { globals: { ...globals.node } },
  },
];
