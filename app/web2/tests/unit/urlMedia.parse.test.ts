import { describe, it, expect } from 'vitest';
import { parseUrlMediaParams, hasUrlMedia } from '@/bus/urlMedia';

function make(qs: string) {
  return new URLSearchParams(qs);
}

describe('parseUrlMediaParams', () => {
  it('returns nulls when no params present', () => {
    const p = parseUrlMediaParams(make(''));
    expect(p.rom).toBeNull();
    expect(p.vrom).toBeNull();
    expect(p.model).toBeNull();
    expect(p.cd).toBeNull();
    expect(p.floppies).toEqual([]);
    expect(p.hardDisks).toEqual([]);
  });

  it('extracts rom, vrom, model, speed', () => {
    const p = parseUrlMediaParams(make('rom=/r&vrom=/v&model=Macintosh+Plus&speed=max'));
    expect(p.rom).toBe('/r');
    expect(p.vrom).toBe('/v');
    expect(p.model).toBe('Macintosh Plus');
    expect(p.speed).toBe('max');
  });

  it('collects floppies fd0..fdN', () => {
    const p = parseUrlMediaParams(make('fd0=/a&fd1=/b&fd2=/c'));
    expect(p.floppies.map((f) => f.slot)).toEqual(['fd0', 'fd1', 'fd2']);
    expect(p.floppies.map((f) => f.url)).toEqual(['/a', '/b', '/c']);
  });

  it('collects hard disks hd0..hdN', () => {
    const p = parseUrlMediaParams(make('hd0=/a&hd2=/c&hd1=/b'));
    expect(p.hardDisks.map((h) => h.slot).sort()).toEqual(['hd0', 'hd1', 'hd2']);
  });

  it('ignores unrelated keys', () => {
    const p = parseUrlMediaParams(make('foo=bar&fd0=/a&model=X'));
    expect(p.floppies).toHaveLength(1);
    expect(p.model).toBe('X');
  });
});

describe('hasUrlMedia', () => {
  it('false when params are empty', () => {
    expect(hasUrlMedia(parseUrlMediaParams(make('')))).toBe(false);
  });
  it('true when rom is set', () => {
    expect(hasUrlMedia(parseUrlMediaParams(make('rom=/a')))).toBe(true);
  });
  it('true when at least one floppy is set', () => {
    expect(hasUrlMedia(parseUrlMediaParams(make('fd0=/a')))).toBe(true);
  });
  it('true when a hard disk is set', () => {
    expect(hasUrlMedia(parseUrlMediaParams(make('hd0=/a')))).toBe(true);
  });
  it('false when only an unrelated key is set', () => {
    expect(hasUrlMedia(parseUrlMediaParams(make('foo=bar')))).toBe(false);
  });
});
