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
    // Project-wide no-restricted-syntax guards. ESLint flat configs
    // merge by replacing rule values for overlapping file globs, so we
    // keep all `no-restricted-syntax` selectors in this single block
    // and route them to the right files via the source-pattern.
    //
    //   - `shell.run`: master plan §6.1 / proposal-shell-as-object-
    //     model-citizen.md §5.3. Only TerminalPane.svelte may construct
    //     shell-line strings; bus/* uses typed object-model paths.
    //     gsEvalLine in bus/emulator.ts is the one legitimate exception
    //     and carries a local eslint-disable-next-line.
    //   - `toast()`: master plan §12 Phase 7. The legacy alias was
    //     retired in favour of `showNotification(msg, severity)`.
    files: ['src/**/*.{ts,svelte,svelte.ts}'],
    rules: {
      'no-restricted-syntax': [
        'error',
        {
          selector: "CallExpression[callee.name='toast']",
          message:
            'Use showNotification(msg, severity) — the toast() alias was removed in Phase 7.',
        },
      ],
    },
  },
  {
    files: ['src/bus/**/*.ts'],
    rules: {
      // Same merged-into-one rule pattern (see comment above).
      'no-restricted-syntax': [
        'error',
        {
          selector: "CallExpression[callee.name='toast']",
          message:
            'Use showNotification(msg, severity) — the toast() alias was removed in Phase 7.',
        },
        {
          selector: "CallExpression[callee.name='gsEval'][arguments.0.value='shell.run']",
          message:
            "bus/* must not call gsEval('shell.run', ...). Use a typed object-model path instead. Only TerminalPane.svelte may construct shell-line strings.",
        },
      ],
    },
  },
];
