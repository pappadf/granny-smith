// Type-safe icon registry. The string-literal union below is the full set of
// codicon ids embedded in src/icons/sprite.svg. Adding a new icon means
// adding its <symbol id="i-..."/> to the sprite AND its short name here.
//
// Sprite ids are local — they don't necessarily match the upstream codicon
// names (e.g. our `i-floppy` is upstream's `save`). See public/NOTICE for the
// upstream-name → local-id mapping.

import { spriteUrl } from '@/skins/registry';
import { resolved } from '@/state/appearance.svelte';

export type IconName =
  | 'play'
  | 'pause'
  | 'stop'
  | 'minus'
  | 'plus'
  | 'download'
  | 'layout-left'
  | 'layout-left-off'
  | 'layout-bottom'
  | 'layout-bottom-off'
  | 'layout-right'
  | 'layout-right-off'
  | 'close'
  | 'chevron'
  | 'step-into'
  | 'step-over'
  | 'restart'
  | 'trash'
  | 'chip'
  | 'hd'
  | 'floppy'
  | 'cd'
  | 'computer'
  | 'folder'
  | 'file'
  | 'speaker'
  | 'port'
  | 'clock'
  | 'empty'
  | 'upload'
  | 'mac'
  | 'sign-out'
  | 'screen-full'
  | 'screen-normal'
  | 'camera'
  | 'camera-off'
  | 'mic'
  | 'mic-off'
  | 'info'
  | 'warning'
  | 'error'
  | 'check'
  | 'arrow-up'
  | 'arrow-down'
  | 'arrow-left'
  | 'circle-filled'
  | 'debug-stackframe'
  | 'newline'
  | 'circle-outline'
  | 'case-sensitive'
  | 'ellipsis'
  | 'power'
  | 'palette';

// The symbol's URL in the active skin's sprite (tests/lint/sprite.test.ts
// checks every sprite has every id).  Reactive: it reads the resolved skin.
export function iconHref(name: IconName): string {
  return `${spriteUrl(resolved.skin)}#i-${name}`;
}
