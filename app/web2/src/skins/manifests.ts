// The manifests of every skin this build knows, without their stylesheets,
// so tools outside the app (the gallery screenshot spec) can list the skins
// and their schemes.  registry.ts adds the token stylesheets.
import { workbench } from './workbench/manifest';
import { platinum } from './platinum/manifest';
import type { SkinManifest } from './types';

export const MANIFESTS: readonly SkinManifest[] = [workbench, platinum];
