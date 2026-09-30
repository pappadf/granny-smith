// The console input's keymap on a real CodeMirror editor: Enter submits or
// continues a block (and keeps type-ahead), ↑ recalls history, Tab
// completes, Ctrl+C copies a selection or interrupts, and a paste is
// normalised -- a pasted block runs once, on Enter.
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import {
  applyCandidate,
  commonPrefix,
  createConsoleInput,
  type ConsoleInput,
  type ConsoleInputHandlers,
} from '@/components/panel-views/terminal/ConsoleInput';
import { ConsoleHistory } from '@/lib/consoleHistory';
import type { CompletionResult } from '@/bus/emulator';

class MemStorage {
  data = new Map<string, string>();
  getItem(k: string) {
    return this.data.get(k) ?? null;
  }
  setItem(k: string, v: string) {
    this.data.set(k, v);
  }
}

let host: HTMLDivElement;
let input: ConsoleInput;
let h: ConsoleInputHandlers & {
  submit: ReturnType<typeof vi.fn>;
  interrupt: ReturnType<typeof vi.fn>;
  clear: ReturnType<typeof vi.fn>;
};
let continues: (text: string) => boolean;
let completion: CompletionResult | null;
let outSel: string;

function key(k: string, mods: { ctrl?: boolean; shift?: boolean } = {}): void {
  input.view.contentDOM.dispatchEvent(
    new KeyboardEvent('keydown', {
      key: k,
      ctrlKey: !!mods.ctrl,
      shiftKey: !!mods.shift,
      bubbles: true,
      cancelable: true,
    }),
  );
}

function type(text: string): void {
  const v = input.view;
  v.dispatch(v.state.replaceSelection(text), { userEvent: 'input.type' });
}

// Let the async Enter / Tab handlers finish.
const settle = () => new Promise((r) => setTimeout(r, 0));

beforeEach(() => {
  host = document.createElement('div');
  document.body.appendChild(host);
  continues = () => false;
  completion = null;
  outSel = '';
  h = {
    submit: vi.fn(),
    needsContinuation: async (t: string) => continues(t),
    complete: async () => completion,
    interrupt: vi.fn(),
    clear: vi.fn(),
    find: vi.fn(),
    outputSelection: () => outSel,
    history: new ConsoleHistory(new MemStorage() as unknown as Storage),
  } as typeof h;
  input = createConsoleInput(host, h);
});

afterEach(() => {
  input.destroy();
  host.remove();
});

describe('ConsoleInput Enter', () => {
  it('submits complete input and clears it', async () => {
    type('echo hi');
    key('Enter');
    await settle();
    expect(h.submit).toHaveBeenCalledWith('echo hi');
    expect(input.text()).toBe('');
    expect(h.history.list()).toEqual(['echo hi']);
  });

  it('inserts a newline when the shell says the block continues', async () => {
    continues = (t) => t.endsWith('{');
    type('if true {');
    key('Enter');
    await settle();
    expect(h.submit).not.toHaveBeenCalled();
    expect(input.text()).toBe('if true {\n');
    type('  echo x\n}');
    key('Enter');
    await settle();
    expect(h.submit).toHaveBeenCalledWith('if true {\n  echo x\n}');
  });

  it('keeps what is typed while the shell answers (type-ahead)', async () => {
    type('echo one');
    key('Enter');
    type('echo two'); // before the answer
    await settle();
    expect(h.submit).toHaveBeenCalledWith('echo one');
    expect(input.text()).toBe('echo two');
  });

  it('Shift+Enter is a newline', () => {
    type('a');
    key('Enter', { shift: true });
    expect(input.text()).toBe('a\n');
    expect(h.submit).not.toHaveBeenCalled();
  });
});

describe('ConsoleInput keys', () => {
  it('↑ recalls history on the first line', async () => {
    type('first');
    key('Enter');
    await settle();
    type('draft');
    key('ArrowUp');
    expect(input.text()).toBe('first');
    key('ArrowDown');
    expect(input.text()).toBe('draft');
  });

  it('Tab inserts a lone candidate, with a space unless it drills in', async () => {
    type('she');
    completion = {
      candidates: [{ text: 'shell.', kind: 'object', doc: '' }],
      span: { start: 0, end: 3 },
      context: { method: null, argIndex: null, argName: null },
    };
    key('Tab');
    await settle();
    expect(input.text()).toBe('shell.');
  });

  it('Tab extends to the common prefix of several candidates', async () => {
    type('shell.co');
    completion = {
      candidates: [
        { text: 'complete', kind: 'method', doc: '' },
        { text: 'complete_x', kind: 'method', doc: '' },
      ],
      span: { start: 6, end: 8 },
      context: { method: null, argIndex: null, argName: null },
    };
    key('Tab');
    await settle();
    expect(input.text()).toBe('shell.complete');
  });

  it('Ctrl+C without a selection interrupts', () => {
    type('half typed');
    key('c', { ctrl: true });
    expect(h.interrupt).toHaveBeenCalledTimes(1);
  });

  it('Ctrl+C with a selection in the input copies instead', () => {
    type('some text');
    input.view.dispatch({ selection: { anchor: 0, head: 4 } });
    key('c', { ctrl: true });
    expect(h.interrupt).not.toHaveBeenCalled();
  });

  it('Ctrl+C with output selected copies it instead', () => {
    const write = vi.fn(async () => {});
    Object.defineProperty(navigator, 'clipboard', {
      value: { writeText: write },
      configurable: true,
    });
    outSel = 'copied output';
    key('c', { ctrl: true });
    expect(h.interrupt).not.toHaveBeenCalled();
    expect(write).toHaveBeenCalledWith('copied output');
  });

  it('Ctrl+L clears the output', () => {
    key('l', { ctrl: true });
    expect(h.clear).toHaveBeenCalled();
  });
});

describe('ConsoleInput paste', () => {
  function paste(text: string): void {
    const ev = new Event('paste', { bubbles: true, cancelable: true });
    Object.defineProperty(ev, 'clipboardData', { value: { getData: () => text } });
    input.view.contentDOM.dispatchEvent(ev);
  }

  it('normalises and inserts a single line without running it', () => {
    paste('› echo a  \r\n');
    expect(input.text()).toBe('echo a');
    expect(h.submit).not.toHaveBeenCalled();
  });

  it('a pasted block runs once, as one job, on Enter', async () => {
    paste('echo a\r\necho b\r\n');
    expect(input.text()).toBe('echo a\necho b');
    key('Enter');
    await settle();
    expect(h.submit).toHaveBeenCalledTimes(1);
    expect(h.submit).toHaveBeenCalledWith('echo a\necho b');
    expect(h.history.list()).toEqual(['echo a\necho b']);
  });
});

describe('ConsoleInput for the command browser', () => {
  it('replaces the path token at the cursor and reports the change', async () => {
    const seen: Array<[string, number]> = [];
    input.destroy();
    input = createConsoleInput(host, { ...h, onChange: (t, c) => void seen.push([t, c]) });
    type('echo mac + 1');
    input.view.dispatch({ selection: { anchor: 8 } });
    input.replaceToken('machine.cpu.pc');
    expect(input.text()).toBe('echo machine.cpu.pc + 1');
    expect(input.view.state.selection.main.head).toBe(19);
    expect(seen[seen.length - 1]).toEqual(['echo machine.cpu.pc + 1', 19]);
  });

  it('focuses at the end', () => {
    type('abc');
    input.view.dispatch({ selection: { anchor: 0 } });
    input.focusEnd();
    expect(input.view.state.selection.main.head).toBe(3);
  });
});

describe('ConsoleInput highlighting', () => {
  it('colours the input from spans, and drops spans for an older text', () => {
    type('machine.flopy');
    input.setHighlight('machine.flopy', [
      { from: 0, to: 7, cls: 'object' },
      { from: 8, to: 13, cls: 'unknown' },
    ]);
    const unknown = host.querySelector('.gs-hl-unknown');
    expect(unknown?.textContent).toBe('flopy');
    expect(host.querySelector('.gs-hl-object')?.textContent).toBe('machine');
    // An answer for text the input no longer holds changes nothing.
    input.setHighlight('something else', [{ from: 0, to: 4, cls: 'keyword' }]);
    expect(host.querySelector('.gs-hl-keyword')).toBeNull();
  });
});

describe('completion helpers', () => {
  it('commonPrefix is case-insensitive', () => {
    expect(commonPrefix(['Apple', 'apricot'])).toBe('Ap');
    expect(commonPrefix([])).toBe('');
  });

  it('applyCandidate adds a space after a lone leaf at the end only', () => {
    expect(applyCandidate('echo sh', { start: 5, end: 7 }, 'shell.', true).doc).toBe('echo shell.');
    expect(applyCandidate('x.pr', { start: 2, end: 4 }, 'prompt', true).doc).toBe('x.prompt ');
    expect(applyCandidate('x.pr y', { start: 2, end: 4 }, 'prompt', true)).toEqual({
      doc: 'x.prompt y',
      cursor: 8,
    });
  });
});
