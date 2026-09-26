import { describe, it, expect } from 'vitest';
import { parseTerminalData } from '@/components/panel-views/terminal/lineDiscipline';

const kinds = (s: string) =>
  parseTerminalData(s).map((a) => (a.kind === 'insert' ? `+${a.text}` : a.kind));

describe('parseTerminalData', () => {
  it('a printable run is one insert', () => {
    expect(kinds('abc')).toEqual(['+abc']);
    expect(kinds('é漢')).toEqual(['+é漢']); // IME / non-ASCII text arrives here too
  });

  it('a multi-line paste is inserts and submits, in order', () => {
    expect(kinds('a\rb\r')).toEqual(['+a', 'submit', '+b', 'submit']);
    expect(kinds('one\r\ntwo\nthree')).toEqual(['+one', 'submit', '+two', 'submit', '+three']);
  });

  it('control characters edit the line', () => {
    expect(kinds('ab\x7f')).toEqual(['+ab', 'backspace']);
    expect(kinds('\b\t\x03\x0c')).toEqual(['backspace', 'tab', 'interrupt', 'clear']);
    expect(kinds('\x01')).toEqual([]);
  });

  it('cursor and editing sequences, CSI and SS3', () => {
    expect(kinds('\x1b[A\x1b[B\x1b[C\x1b[D')).toEqual(['up', 'down', 'right', 'left']);
    expect(kinds('\x1bOA\x1bOH\x1bOF')).toEqual(['up', 'home', 'end']);
    expect(kinds('\x1b[H\x1b[F\x1b[1~\x1b[4~\x1b[3~')).toEqual([
      'home',
      'end',
      'home',
      'end',
      'delete',
    ]);
  });

  it('an unknown sequence is swallowed, not inserted', () => {
    expect(kinds('a\x1b[1;5Pb')).toEqual(['+a', '+b']); // Ctrl-F1
    expect(kinds('\x1b[1;5C')).toEqual(['right']); // a modified arrow is still an arrow
    expect(kinds('a\x1b[15~b')).toEqual(['+a', '+b']); // F5
    expect(kinds('\x1bx')).toEqual([]); // Alt+x
    expect(kinds('\x1b')).toEqual([]);
  });
});
