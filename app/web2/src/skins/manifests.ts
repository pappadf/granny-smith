// The manifests of every skin this build knows, without their stylesheets,
// so tools outside the app (the gallery screenshot spec) can list the skins.
// registry.ts adds the token stylesheets.  The order is the appearance
// menu's.
import { workbench } from './workbench/manifest';
import { workbenchLight } from './workbench-light/manifest';
import { midnight } from './midnight/manifest';
import { starlight } from './starlight/manifest';
import { platinum } from './platinum/manifest';
import { aqua } from './aqua/manifest';
import type { SkinManifest } from './types';

export const MANIFESTS: readonly SkinManifest[] = [
  workbench,
  workbenchLight,
  midnight,
  starlight,
  platinum,
  aqua,
];
