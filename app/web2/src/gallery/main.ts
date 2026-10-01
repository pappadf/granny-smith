// Dev-only UI gallery entry (`?gallery` on the dev server). main.ts imports
// this behind `import.meta.env.DEV`, so production builds never contain it
// (scripts/check-dist.mjs asserts that no gallery chunk reaches dist/).
import { mount } from 'svelte';
import Gallery from './Gallery.svelte';
import { setOpfsBackend } from '@/bus/opfs';
// The component tests' in-memory OPFS: its fixture ROMs, disks and folders
// fill the Images, Filesystem and Checkpoints stories.
import { MockOpfs } from '../../tests/helpers/mockOpfs';

// Mount the gallery in place of the app.
export function startGallery(target: HTMLElement): void {
  setOpfsBackend(new MockOpfs());
  mount(Gallery, { target });
}
