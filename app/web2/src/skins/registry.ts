// The skins this build knows.  Every skin's token stylesheet is bundled, so a
// switch never flashes the default; overrides, fonts and sprites load lazily
// (sprites are bundled with hashed names).
// A new skin adds its tokens.css import here and its manifest to
// manifests.ts.
import './workbench/tokens.css';
import './workbench-light/tokens.css';
import './midnight/tokens.css';
import './starlight/tokens.css';
import './platinum/tokens.css';
import './aqua/tokens.css';
import { midnight } from './midnight/manifest';
import { MANIFESTS } from './manifests';
import type { SkinManifest } from './types';
import defaultSprite from '../icons/sprite.svg?url';

// Each skin's icon sprite: skins/<id>/sprite.svg when it has one, else the
// default.  Bundled, so each build names them by content and a changed icon
// is never served from a stale cache.
const SPRITES = import.meta.glob<string>('./*/sprite.svg', {
  query: '?url',
  import: 'default',
  eager: true,
});

// The URL of skin `id`'s sprite.
export function spriteUrl(id: string): string {
  return SPRITES[`./${id}/sprite.svg`] ?? defaultSprite;
}

export const DEFAULT_SKIN = 'midnight';

export const skins: readonly SkinManifest[] = MANIFESTS;

// The skin with `id`, or the default when there is none.
export function getSkin(id: string | null | undefined): SkinManifest {
  return skins.find((s) => s.id === id) ?? midnight;
}
