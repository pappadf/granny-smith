// Find in text: whether a text matches, and the text split around the
// matches (for <mark>).  An empty query matches nothing.

export interface FindPart {
  t: string;
  // A match.
  m: boolean;
}

export function findMatches(text: string, q: string, caseSensitive: boolean): boolean {
  if (!q) return false;
  return caseSensitive ? text.includes(q) : text.toLowerCase().includes(q.toLowerCase());
}

export function findParts(text: string, q: string, caseSensitive: boolean): FindPart[] {
  if (!q) return [{ t: text, m: false }];
  const hay = caseSensitive ? text : text.toLowerCase();
  const needle = caseSensitive ? q : q.toLowerCase();
  const out: FindPart[] = [];
  let i = 0;
  for (;;) {
    const j = hay.indexOf(needle, i);
    if (j < 0) break;
    if (j > i) out.push({ t: text.slice(i, j), m: false });
    out.push({ t: text.slice(j, j + needle.length), m: true });
    i = j + needle.length;
  }
  if (i < text.length) out.push({ t: text.slice(i), m: false });
  return out;
}
