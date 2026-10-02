// A skin: a named visual design, made of token values (a light or a dark
// one: its --gs-color-scheme token says which) and optionally an icon
// sprite (skins/<id>/sprite.svg, found by registry.ts), webfonts and an
// override stylesheet.  See src/skins/README.md.

// A webfont a skin ships under public/skins/<folder>/fonts/.
export interface SkinFont {
  family: string; // the CSS family name the skin's --gs-font-* tokens use
  src: string; // relative URL (the build's base is './'), woff2
  weight?: string; // '400' | '200 700'
  style?: 'normal' | 'italic';
  license: string; // SPDX id; must be listed in THIRD_PARTY_NOTICES.md
}

export interface SkinManifest {
  id: string; // kebab-case; the folder name and the data-skin value
  name: string; // human label
  fonts?: readonly SkinFont[];
  overrides?: () => Promise<unknown>; // lazy import of the skin's overrides.css
  metaThemeColor?: string; // <meta name="theme-color">, else --gs-surface-raised
}
