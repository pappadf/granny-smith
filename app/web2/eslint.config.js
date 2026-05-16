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
    // Svelte 5 runes can live in *.svelte.ts files; ESLint needs the TS
    // parser explicitly because typescript-eslint's flat preset only
    // captures *.ts / *.tsx by default.
    files: ['**/*.svelte.ts'],
    languageOptions: {
      parser: tseslint.parser,
    },
  },
  {
    files: ['vite.config.ts', 'vitest.config.ts', 'svelte.config.js', 'eslint.config.js'],
    languageOptions: { globals: { ...globals.node } },
  },
  {
    // Master plan §6.1 / proposal-shell-as-object-model-citizen.md §5.3:
    // only the Terminal pane may invoke `shell.run` — every other
    // caller in the bus layer must use typed object-model paths via
    // gsEval. gsEvalLine in bus/emulator.ts is the one legitimate
    // exception (the TerminalPane's call site); it carries a local
    // eslint-disable-next-line.
    files: ['src/bus/**/*.ts'],
    rules: {
      'no-restricted-syntax': [
        'error',
        {
          selector: "CallExpression[callee.name='gsEval'][arguments.0.value='shell.run']",
          message:
            "bus/* must not call gsEval('shell.run', ...). Use a typed object-model path instead. Only TerminalPane.svelte may construct shell-line strings.",
        },
      ],
    },
  },
];
