// The terminal's line discipline: what xterm's `onData` delivers, as editing
// actions.  `onData` is the one input path (F-40) -- keys, pastes, IME
// composition and virtual keyboards all arrive through it, where the old
// `onKey` handler saw only single keystrokes and dropped the rest.  Pure, so
// tests/unit/lineDiscipline.test.ts drives it without an xterm.

export type TermAction =
  | { kind: 'insert'; text: string }
  | { kind: 'submit' }
  | { kind: 'backspace' }
  | { kind: 'delete' }
  | { kind: 'left' }
  | { kind: 'right' }
  | { kind: 'up' }
  | { kind: 'down' }
  | { kind: 'home' }
  | { kind: 'end' }
  | { kind: 'tab' }
  | { kind: 'interrupt' } // Ctrl-C
  | { kind: 'clear' }; // Ctrl-L

// CSI/SS3 final bytes, and `ESC [ n ~` parameters, that the line editor
// understands.  Anything else is swallowed whole rather than inserted.
const FINAL: Record<string, TermAction['kind']> = {
  A: 'up',
  B: 'down',
  C: 'right',
  D: 'left',
  H: 'home',
  F: 'end',
};
const TILDE: Record<string, TermAction['kind']> = {
  '1': 'home',
  '7': 'home',
  '3': 'delete',
  '4': 'end',
  '8': 'end',
};

export function parseTerminalData(data: string): TermAction[] {
  const out: TermAction[] = [];
  let run = '';
  const flush = () => {
    if (run) out.push({ kind: 'insert', text: run });
    run = '';
  };
  const push = (kind: Exclude<TermAction['kind'], 'insert'>) => {
    flush();
    out.push({ kind } as TermAction);
  };
  for (let i = 0; i < data.length; i++) {
    const c = data[i];
    if (c === '\x1b') {
      const intro = data[i + 1];
      if (intro === '[' || intro === 'O') {
        // Parameters, then one final byte in @..~.
        let j = i + 2;
        while (j < data.length && /[0-9;]/.test(data[j])) j++;
        const final = data[j];
        const params = data.slice(i + 2, j);
        const kind = final === '~' ? TILDE[params] : final ? FINAL[final] : undefined;
        if (kind) push(kind as Exclude<TermAction['kind'], 'insert'>);
        else flush();
        i = j; // swallow the sequence (or the truncated remnant)
      } else {
        flush(); // a lone ESC, or Alt+key: neither edits the line
        if (intro !== undefined) i++;
      }
      continue;
    }
    if (c === '\r') push('submit');
    else if (c === '\n') {
      if (data[i - 1] !== '\r') push('submit'); // "\r\n" is one line end
    } else if (c === '\x7f' || c === '\b') push('backspace');
    else if (c === '\t') push('tab');
    else if (c === '\x03') push('interrupt');
    else if (c === '\x0c') push('clear');
    else if (c >= ' ') run += c;
    // other C0 controls: ignored
  }
  flush();
  return out;
}
