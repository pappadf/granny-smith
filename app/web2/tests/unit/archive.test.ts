import { describe, it, expect } from 'vitest';
import { isZipFile, isMacArchive, isArchiveFile, sanitizeName, isZipMagic } from '@/lib/archive';

describe('archive predicates', () => {
  it('isZipFile recognises .zip', () => {
    expect(isZipFile('foo.zip')).toBe(true);
    expect(isZipFile('FOO.ZIP')).toBe(true);
    expect(isZipFile('foo.sit')).toBe(false);
  });
  it('isMacArchive recognises Mac archive extensions', () => {
    for (const ext of ['sit', 'hqx', 'cpt', 'bin', 'sea']) {
      expect(isMacArchive(`x.${ext}`)).toBe(true);
    }
    expect(isMacArchive('x.zip')).toBe(false);
  });
  it('isArchiveFile recognises both ZIP and Mac archives', () => {
    expect(isArchiveFile('a.zip')).toBe(true);
    expect(isArchiveFile('a.sit')).toBe(true);
    expect(isArchiveFile('a.img')).toBe(false);
  });
});

describe('sanitizeName', () => {
  it('keeps alphanumerics, ._-', () => {
    expect(sanitizeName('foo.bar_baz-2.rom')).toBe('foo.bar_baz-2.rom');
  });
  it('replaces everything else with _', () => {
    expect(sanitizeName('foo bar baz.rom')).toBe('foo_bar_baz.rom');
    expect(sanitizeName('a/b\\c:d')).toBe('a_b_c_d');
  });
});

describe('isZipMagic', () => {
  it('matches PK\\x03\\x04', () => {
    expect(isZipMagic(new Uint8Array([0x50, 0x4b, 0x03, 0x04, 0x00]))).toBe(true);
  });
  it('rejects empty buffer', () => {
    expect(isZipMagic(new Uint8Array(0))).toBe(false);
  });
  it('rejects non-zip bytes', () => {
    expect(isZipMagic(new Uint8Array([0x00, 0x00, 0x00, 0x00]))).toBe(false);
  });
});
