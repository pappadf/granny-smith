// Type-safe icon registry. The string-literal union below is the full set of
// codicon ids embedded in /public/icons/sprite.svg. Adding a new icon means
// adding its <symbol id="i-..."/> to the sprite AND its short name here.
//
// Sprite ids are local — they don't necessarily match the upstream codicon
// names (e.g. our `i-floppy` is upstream's `save`). See public/NOTICE for the
// upstream-name → local-id mapping.

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
  | 'color-mode'
  | 'sign-out'
  | 'screen-full'
  | 'screen-normal';

export function iconHref(name: IconName): string {
  return `/icons/sprite.svg#i-${name}`;
}
