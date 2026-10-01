// A skin: a named visual design, made of token values per colour scheme and
// optionally an icon sprite, webfonts and an override stylesheet.  See
// src/skins/README.md.

export type Scheme = 'dark' | 'light';

// A webfont a skin ships under public/skins/<id>/fonts/.
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
  schemes: readonly Scheme[]; // at least one; the first is the fallback
  sprite?: string; // relative URL under public/, default 'icons/sprite.svg'
  fonts?: readonly SkinFont[];
  overrides?: () => Promise<unknown>; // lazy import of the skin's overrides.css
  metaThemeColor?: Partial<Record<Scheme, string>>; // <meta name="theme-color">
}
