// shell.highlight spans in UTF-16, and text cut into coloured runs.
import { describe, it, expect } from 'vitest';
import { toUtf16Spans, highlightParts } from '@/lib/highlight';

describe('highlight', () => {
  it('converts UTF-8 byte spans to UTF-16 offsets', () => {
    const text = 'echo "über" 5';
    // "über" is 7 bytes from byte 5; 5 starts at byte 13 (UTF-16 12).
    const spans = toUtf16Spans(text, [
      { start: 0, end: 4, class: 'method' },
      { start: 5, end: 12, class: 'string' },
      { start: 13, end: 14, class: 'number' },
    ]);
    expect(spans).toEqual([
      { from: 0, to: 4, cls: 'method' },
      { from: 5, to: 11, cls: 'string' },
      { from: 12, to: 13, cls: 'number' },
    ]);
    expect(text.slice(5, 11)).toBe('"über"');
  });

  it('drops malformed entries', () => {
    expect(toUtf16Spans('abc', null)).toEqual([]);
    expect(toUtf16Spans('abc', [{ start: 1 }, 'x', { start: 2, end: 2, class: 'x' }])).toEqual([]);
  });

  it('cuts text into runs with and without a class', () => {
    expect(
      highlightParts('let x = 1', [
        { from: 0, to: 3, cls: 'decl' },
        { from: 8, to: 9, cls: 'number' },
      ]),
    ).toEqual([
      { text: 'let', cls: 'decl' },
      { text: ' x = ', cls: null },
      { text: '1', cls: 'number' },
    ]);
    expect(highlightParts('abc', [])).toEqual([{ text: 'abc', cls: null }]);
  });
});
