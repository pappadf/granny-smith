// Find in text: matching and the parts around each match.
import { describe, it, expect } from 'vitest';
import { findMatches, findParts } from '@/lib/find';

const marks = (text: string, q: string, cs = false) =>
  findParts(text, q, cs)
    .map((p) => (p.m ? `[${p.t}]` : p.t))
    .join('');

describe('findParts', () => {
  it('splits around every match, ignoring case unless asked', () => {
    expect(marks('Beta beta', 'beta')).toBe('[Beta] [beta]');
    expect(marks('Beta beta', 'beta', true)).toBe('Beta [beta]');
    expect(marks('aaa', 'aa')).toBe('[aa]a');
    expect(findParts('abc', 'abc', false)).toEqual([{ t: 'abc', m: true }]);
  });

  it('no query, or no match: the text as one part', () => {
    expect(findParts('abc', '', false)).toEqual([{ t: 'abc', m: false }]);
    expect(findParts('abc', 'x', false)).toEqual([{ t: 'abc', m: false }]);
  });

  it('findMatches agrees', () => {
    expect(findMatches('Alpha', 'alp', false)).toBe(true);
    expect(findMatches('Alpha', 'alp', true)).toBe(false);
    expect(findMatches('Alpha', '', false)).toBe(false);
  });
});
