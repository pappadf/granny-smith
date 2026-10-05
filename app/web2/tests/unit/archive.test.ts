import { describe, it, expect } from 'vitest';
import { sanitizeName } from '@/lib/archive';

describe('sanitizeName', () => {
  it('keeps alphanumerics, ._-', () => {
    expect(sanitizeName('foo.bar_baz-2.rom')).toBe('foo.bar_baz-2.rom');
  });
  it('replaces everything else with _', () => {
    expect(sanitizeName('foo bar baz.rom')).toBe('foo_bar_baz.rom');
    expect(sanitizeName('a/b\\c:d')).toBe('a_b_c_d');
  });
});
