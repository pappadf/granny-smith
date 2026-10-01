// The skins this build knows.  Every skin's token stylesheet is bundled, so a
// switch never flashes the default; overrides, fonts and sprites load lazily.
// A new skin adds its tokens.css import here and its manifest to
// manifests.ts.
import './workbench/tokens.css';
import './workbench-light/tokens.css';
import './midnight/tokens.css';
import './starlight/tokens.css';
import './platinum/tokens.css';
import './aqua/tokens.css';
import { workbench } from './workbench/manifest';
import { MANIFESTS } from './manifests';
import type { SkinManifest } from './types';

export const DEFAULT_SKIN = 'workbench';
// The default for a first visit when the operating system prefers light.
export const LIGHT_DEFAULT_SKIN = 'workbench-light';

export const skins: readonly SkinManifest[] = MANIFESTS;

// The skin with `id`, or the default when there is none.
export function getSkin(id: string | null | undefined): SkinManifest {
  return skins.find((s) => s.id === id) ?? workbench;
}
