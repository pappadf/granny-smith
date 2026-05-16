import { describe, it, expect } from 'vitest';
import { iconForFsEntry, iconForCategory, CATEGORY_LABELS } from '@/lib/iconForFsEntry';

describe('iconForFsEntry', () => {
  it('returns folder for directories regardless of name', () => {
    expect(iconForFsEntry({ name: 'rom', path: '/opfs/rom', kind: 'directory' })).toBe('folder');
  });

  it('maps known extensions to typed codicons', () => {
    expect(iconForFsEntry({ name: 'Plus.rom', path: '/opfs/x/Plus.rom', kind: 'file' })).toBe(
      'chip',
    );
    expect(iconForFsEntry({ name: 'hd1.img', path: '/opfs/x/hd1.img', kind: 'file' })).toBe('hd');
    expect(iconForFsEntry({ name: 'sys.dsk', path: '/opfs/x/sys.dsk', kind: 'file' })).toBe(
      'floppy',
    );
    expect(iconForFsEntry({ name: 'sys.iso', path: '/opfs/x/sys.iso', kind: 'file' })).toBe('cd');
  });

  it('falls back to file for unknown extensions', () => {
    expect(iconForFsEntry({ name: 'README', path: '/opfs/x/README', kind: 'file' })).toBe('file');
    expect(iconForFsEntry({ name: 'a.xyz', path: '/opfs/x/a.xyz', kind: 'file' })).toBe('file');
  });

  it('is case-insensitive on the extension', () => {
    expect(iconForFsEntry({ name: 'Plus.ROM', path: '/opfs/x/Plus.ROM', kind: 'file' })).toBe(
      'chip',
    );
  });
});

describe('iconForCategory', () => {
  it('returns the right icon per ImageCategory', () => {
    expect(iconForCategory('rom')).toBe('chip');
    expect(iconForCategory('vrom')).toBe('chip');
    expect(iconForCategory('fd')).toBe('floppy');
    expect(iconForCategory('hd')).toBe('hd');
    expect(iconForCategory('cd')).toBe('cd');
  });
});

describe('CATEGORY_LABELS', () => {
  it('has a label for each category', () => {
    expect(CATEGORY_LABELS.rom).toBe('ROM');
    expect(CATEGORY_LABELS.vrom).toBe('VROM');
    expect(CATEGORY_LABELS.fd).toBe('Floppy Disk');
    expect(CATEGORY_LABELS.hd).toBe('Hard Disk');
    expect(CATEGORY_LABELS.cd).toBe('CD-ROM');
  });
});
