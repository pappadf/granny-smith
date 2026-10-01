# Skins

A skin is a named visual design for the web UI: one folder of token values,
plus, optionally, an icon sprite, webfonts and an override stylesheet. A
skin changes how the UI looks, never what it does or where things are. The
default skin is `workbench`.

## What a skin can change

- **Tokens.** Every visual value is a `--gs-*` custom property. A skin sets
  the semantic tokens (surfaces, text, borders, intents, machine states, the
  syntax and code palettes) for each scheme it has. It may also override any
  scale token (type, space, radius, metrics, motion) or component token
  (`--gs-button-*`, `--gs-tab-*`, …). [TOKENS.md](TOKENS.md) lists them all,
  with kinds and defaults.
- **Icons.** Its own sprite under `public/skins/<id>/`, which must define
  every id in `src/lib/icons.ts`.
- **Fonts.** Webfonts under `public/skins/<id>/fonts/`.
- **Overrides.** `overrides.css`, for shapes tokens cannot express (a
  bevelled button with several shadows, say), written against the `gs-*`
  part classes listed below.

## What a skin cannot change

- **Layout and behaviour.** What is where, what a control does, and which
  controls exist.
- **The emulated screen.** The `#screen` and `#screen3d` canvases are the
  machine's picture (lint L-9). A skin may frame it with
  `--gs-screen-frame-bg`, `-shadow`, `-padding` (a bezel) and `-radius`.
- **Browser-drawn chrome:**
  - the built-in PDF viewer of the print dialog (its toolbar, pages and
    zoom belong to the browser);
  - the open list of a native `<select>` (it follows only `color-scheme`);
  - `title=` tooltips;
  - the browser's own fullscreen UI.
- **Scrollbar shape.** Scrollbars take the skin's colours
  (`--gs-scrollbar-thumb`, `-thumb-hover`, `-track`). Chromium and Firefox
  draw them through `scrollbar-color`, so their shape is the platform's thin
  scrollbar. Safari uses the `::-webkit-scrollbar` rules in
  `styles/base.css`.

## Quick start

1. Copy `skins/workbench/` to `skins/<id>/` (`<id>` in kebab-case).
2. In `tokens.css`, change the selectors to `:root[data-skin='<id>'][data-theme='dark']`
   and `…[data-theme='light']`. Keep only the schemes the skin has, and
   leave out the bare `:root` (only the default skin provides that
   pre-paint fallback). Then set the values.
3. In `manifest.ts`, set `id`, `name` and `schemes` (the first scheme is
   the fallback).
4. Register the skin in `registry.ts`: import its `tokens.css` and add its
   manifest to `skins`.
5. Look at it in the gallery: `npm run dev`, then open `?gallery&skin=<id>`
   (add `&story=<name>&theme=light` for one story). In the app, use
   `?skin=<id>`.

Every skin's `tokens.css` is bundled, because a switch must never flash the
default. Overrides, fonts and sprites load when the skin is first used.

## The manifest

```ts
export const paper: SkinManifest = {
  id: 'paper',
  name: 'Paper',
  schemes: ['light'], // one scheme: the toolbar's scheme toggle is disabled
  sprite: 'skins/paper/sprite.svg', // relative to public/; omit for the default
  fonts: [
    {
      family: 'Paper Sans',
      src: 'skins/paper/fonts/paper-sans.woff2',
      weight: '400 700',
      license: 'OFL-1.1',
    },
  ],
  overrides: () => import('./overrides.css'),
  metaThemeColor: { light: '#f4f1ea' }, // default: --gs-surface-raised
};
```

**Single-scheme skins.** If the user's preference (or the OS, under
`system`) asks for a scheme the skin lacks, the skin's first scheme is
used, and the scheme toggle is disabled.

## Token values

- Colours go only in `tokens.css`. Components never hold literals (lint
  L-4).
- A skin may keep a private reference palette as `--gs-ref-*` tokens in
  its own `tokens.css`. Nothing outside the skin may read them (L-6).
- Every semantic token must be defined in every scheme the skin has (L-2).
  Derived semantic tokens (those with a default in TOKENS.md) may be left
  out.
- Text must meet the contrast pairs in `styles/contract.ts`
  (`CONTRAST_PAIRS`), checked by `tests/lint/contrast.test.ts`.

## Part hooks and overrides

`overrides.css` is for what tokens cannot express. Its rules go in the
`gs.skin` cascade layer, which wins over every component style whatever the
specificity, and are scoped to the skin:

```css
@layer gs.skin {
  :root[data-skin='paper'] .gs-button[data-variant='primary'] {
    box-shadow:
      inset 0 1px 0 rgb(255 255 255 / 0.6),
      0 1px 2px rgb(0 0 0 / 0.3);
  }
}
```

Target only the `gs-*` part classes and their `data-*` states. Other class
names in the DOM are test hooks and can change. The parts are:

- **Primitives (`components/ui/`):**
  - `gs-button`, `gs-icon-button`, `gs-segmented`, `gs-chip`, `gs-link`
    and `gs-action-row`;
  - `gs-input`, `gs-inline-input`, `gs-select`, `gs-check`, `gs-radio-group`,
    `gs-field` and `gs-form`;
  - `gs-tabs`, `gs-toolbar`, `gs-separator`, `gs-disclosure`,
    `gs-tree-item`, `gs-list-row`, `gs-heading`, `gs-hint`, `gs-sash` and
    `gs-switch`;
  - `gs-badge`, `gs-progress`, `gs-spinner`, `gs-activity-dot`,
    `gs-status-dot`, `gs-drive`, `gs-card`, `gs-hero__*` and `gs-callout`.
- **Composites:** `gs-section`, `gs-table`, `gs-modal` /
  `gs-modal-backdrop`, `gs-menu`, `gs-toast` and `gs-statusbar` (with
  `gs-statusbar__item`).
- **Elements:** a part's elements are `<part>__<element>`, for example
  `gs-tree-item__label` and `gs-table__row`.
- **States:** a part shows its state through ARIA (`aria-selected`,
  `aria-pressed`, `aria-checked`) or `data-state`, `data-variant`,
  `data-intent` and `data-size`, never through a bare class.

## Checks to run

```sh
npm run lint && npx svelte-check && npx vitest run tests/lint   # in app/web2
make ui2-gallery                                                 # the screenshots
```

The token lints (`tests/lint/tokens.test.ts`) cover:

- L-1: every token read is in the contract.
- L-2: every semantic token is in every scheme.
- L-3: no fallbacks.
- L-4: no literal colours.
- L-5: no literal scale values.
- L-6: no reference-token reads outside the skin.
- L-7: one writer of the appearance attributes.
- L-8: focus stays visible.
- L-9: the screen canvases are left alone.
- L-10: every stylesheet is in its cascade layer.
- L-11: no native dialogs.

`contrast.test.ts` checks the contrast pairs, and `sprite.test.ts` checks
that each sprite covers the icon registry. The gallery screenshots run per
skin and scheme; their baselines are recorded in the CI image
(`tests/e2e/README.md`).

## Assets

- **URLs are relative** (`skins/<id>/…`, no leading `/`), because the app
  is built with `base: './'` and served from sub-paths.
- **Fonts** need an SPDX licence id in the manifest and an entry in
  `THIRD_PARTY_NOTICES.md`. List the family first in the skin's
  `--gs-font-*` tokens, with a system fallback after it, so text renders
  before the font arrives.
- **A sprite** must define every id in `src/lib/icons.ts` (as `i-<id>`
  symbols with `fill="currentColor"`). External `<use>` references cannot
  fall back one symbol at a time.

## How the mechanism works

- **Selection.**
  - `<html data-skin data-theme>` selects the token blocks.
  - `state/appearance.svelte.ts` is their only writer. `index.html`'s
    pre-paint script sets them once before any stylesheet, from `?skin=`,
    `gs-skin` and `gs-theme`, so the first frame is already right.
- **Cascade layers.** `styles/layers.css` fixes the order `gs.reset`,
  `gs.base`, `gs.tokens`, `gs.components`, `gs.skin`, `gs.overrides`.
  - Component styles are put in `gs.components` by a preprocess step in
    `svelte.config.js`. Svelte scopes selectors inside `@layer` as it does
    at the top level.
  - `styles/preferences.css` (reduced motion, forced colours) comes after
    every skin's tokens, so a user's settings win over any skin.
- **JavaScript.**
  - Colours reach CodeMirror (the console input) as `var(--gs-…)` strings,
    so a switch restyles it without reconfiguring the editor.
  - Metrics a switch can change (fonts, sizes) are re-measured:
    `onAppearanceChange` in `lib/tokens.ts` calls the editor's
    `requestMeasure()`.
  - Code that reads a token value in JavaScript (`readToken`,
    `readMetric`) reads it again then too.
