// Map a Filesystem-view entry to a codicon id. Keeps icon decisions out of
// the row renderer so the lookup is unit-testable and easy to extend.
//
// Codicon ids resolve to <use href="/icons/sprite.svg#i-…"> inside Icon.svelte;
// the sprite already ships chip / floppy / hd / cd / file / folder / json.

import type { OpfsEntry } from '@/bus/types';
import type { IconName } from './icons';

const EXT_MAP: Record<string, IconName> = {
  rom: 'chip',
  bin: 'chip',
  img: 'hd',
  dsk: 'floppy',
  dc42: 'floppy',
  iso: 'cd',
  toast: 'cd',
  cdr: 'cd',
  hda: 'hd',
  json: 'file',
  txt: 'file',
  log: 'file',
};

export function iconForFsEntry(e: OpfsEntry): IconName {
  if (e.kind === 'directory') return 'folder';
  const ext = e.name.split('.').pop()?.toLowerCase() ?? '';
  return EXT_MAP[ext] ?? 'file';
}

export function iconForCategory(cat: 'rom' | 'vrom' | 'fd' | 'hd' | 'cd'): IconName {
  switch (cat) {
    case 'rom':
    case 'vrom':
      return 'chip';
    case 'fd':
      return 'floppy';
    case 'hd':
      return 'hd';
    case 'cd':
      return 'cd';
  }
}

export const CATEGORY_LABELS: Record<'rom' | 'vrom' | 'fd' | 'hd' | 'cd', string> = {
  rom: 'ROM',
  vrom: 'VROM',
  fd: 'Floppy Disk',
  hd: 'Hard Disk',
  cd: 'CD-ROM',
};

export const CATEGORY_ACCEPT: Record<'rom' | 'vrom' | 'fd' | 'hd' | 'cd', string> = {
  rom: '.rom,.bin',
  vrom: '.rom,.bin',
  fd: '.dsk,.img,.dc42',
  hd: '.img,.hda',
  cd: '.iso,.toast,.cdr',
};
