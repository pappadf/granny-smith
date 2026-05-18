// Per-category collapsed state for the Images panel view. Mount state for
// individual images is derived from the existing `machine` state (Phase 3
// already updates machine.fd / machine.hd / machine.cd from device events).

import type { ImageCategory } from '@/bus/types';

interface ImagesState {
  collapsed: Record<ImageCategory, boolean>;
  /** Set of OPFS paths currently mounted in any drive. Updated by the
   *  Images-section mount/unmount actions; the legacy Welcome flow
   *  doesn't write here, so the badge only reflects this view's actions
   *  until the C side starts pushing mount events directly. */
  mounted: Record<string, boolean>;
}

export const images: ImagesState = $state({
  collapsed: { rom: false, vrom: true, fd: false, hd: true, cd: true },
  mounted: {},
});

export function toggleCategory(cat: ImageCategory): void {
  images.collapsed[cat] = !images.collapsed[cat];
}

export function setMounted(path: string, on: boolean): void {
  if (on) images.mounted[path] = true;
  else delete images.mounted[path];
}
