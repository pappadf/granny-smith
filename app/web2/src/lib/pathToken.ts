// The path token under the console's cursor, and what a completion says
// about it -- the pure half of the command browser ↔ console sync.
//
// A path token is a run of path characters: segment names, `.`, `[`, `]`,
// `"` (keyed entries, `log.category["scsi"]`) and `$` (aliases).

const PATH_CHAR = /[A-Za-z0-9_.$[\]"]/;

// The token around `cursor` (half-open [start, end)); empty at whitespace.
export function pathTokenAt(text: string, cursor: number): { start: number; end: number } {
  let start = cursor;
  while (start > 0 && PATH_CHAR.test(text[start - 1])) start--;
  let end = cursor;
  while (end < text.length && PATH_CHAR.test(text[end])) end++;
  return { start, end };
}

// `text` with the token at `cursor` replaced by `insert`; the cursor lands
// after the insert.
export function replaceTokenAt(
  text: string,
  cursor: number,
  insert: string,
): { text: string; cursor: number } {
  const { start, end } = pathTokenAt(text, cursor);
  return { text: text.slice(0, start) + insert + text.slice(end), cursor: start + insert.length };
}

// What the browser follows while the user types: the node the token has
// resolved to so far (`parent`, '' for the root), the partial segment being
// typed, the candidate names that match it, and -- for a `$…` token -- the
// alias name.
export interface CompletionFocus {
  parent: string;
  partial: string;
  names: string[];
  alias: string | null;
}

// A candidate as a segment name: without the drill-in suffix the completer
// adds (`shell.`, `drive[`, `name=`, `dir/`) or the closing of a key.
export function candidateName(c: string): string {
  return c.replace(/(\["?|"\]|\]|[./=(])$/, '');
}

// Where the last segment of a token starts: after its last `.` or `[`
// outside a quoted key; -1 when the token is one segment.
function lastSeparator(token: string): number {
  let quoted = false;
  let at = -1;
  for (let i = 0; i < token.length; i++) {
    const c = token[i];
    if (c === '"') quoted = !quoted;
    else if (!quoted && (c === '.' || c === '[')) at = i;
  }
  return at;
}

// The completer answers with candidates that replace [span.start,
// span.end): whole paths when the span covers the token, bare segments
// when it covers only the last one.  Either way the browser needs the
// node the token has resolved to and the segment names that match.
export function completionFocus(
  line: string,
  span: { start: number; end: number },
  candidates: readonly string[],
): CompletionFocus {
  let tokStart = span.start;
  while (tokStart > 0 && PATH_CHAR.test(line[tokStart - 1])) tokStart--;
  const token = line.slice(tokStart, span.end);
  const lead = line.slice(tokStart, span.start); // the token before the span
  if (token.startsWith('$')) {
    const names = candidates.map((c) => candidateName(lead + c).replace(/^\$/, ''));
    return { parent: '', partial: token.slice(1), names, alias: token.slice(1) };
  }
  const k = lastSeparator(token);
  // `machine.floppy.drive[0].ins` → machine.floppy.drive[0] + `ins`;
  // `machine.floppy.drive[` / `log.category["sc` → the collection + key.
  const parent = k < 0 ? '' : token.slice(0, k);
  let head = k < 0 ? '' : token.slice(0, k + 1);
  let partial = token.slice(k + 1);
  if (token[k] === '[' && partial.startsWith('"')) {
    head += '"';
    partial = partial.slice(1);
  }
  partial = partial.replace(/"?\]$/, '');
  const names = candidates.map((c) => {
    const full = lead + c;
    return candidateName(full.startsWith(head) ? full.slice(head.length) : c);
  });
  return { parent, partial, names, alias: null };
}
