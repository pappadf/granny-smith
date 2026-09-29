// The console's input: a CodeMirror 6 editor, one line by default, growing
// for blocks and pastes.  It owns the keymap -- Enter
// (submit unless the shell says the input continues), Shift+Enter, history
// on ↑/↓ at the first/last line, Tab completion (common prefix, then a
// popup of `shell.complete` candidates with their kind and doc), Ctrl+C
// (copy with a selection, else interrupt), Ctrl+L, Mod+F -- and the paste
// normalisation.  What those keys *do* to the console is the caller's
// (ConsoleView.svelte), through `ConsoleInputHandlers`.

import { EditorState, Prec } from '@codemirror/state';
import {
  EditorView,
  keymap,
  placeholder as cmPlaceholder,
  drawSelection,
  type Command,
} from '@codemirror/view';
import { defaultKeymap, history, historyKeymap, insertNewline } from '@codemirror/commands';
import {
  autocompletion,
  closeCompletion,
  completionStatus,
  startCompletion,
  type Completion,
  type CompletionContext,
  type CompletionResult as CmCompletionResult,
} from '@codemirror/autocomplete';
import type { CompletionResult } from '@/bus/emulator';
import { normalisePaste } from '@/lib/consoleModel';
import { copyText, type ConsoleHistory } from '@/lib/consoleHistory';

export interface ConsoleInputHandlers {
  // Enter on complete input: run it.
  submit(text: string): void;
  // Whether Enter should insert a newline instead (shell.needs_continuation).
  needsContinuation(text: string): Promise<boolean>;
  complete(line: string, cursor: number): Promise<CompletionResult | null>;
  // Ctrl+C with nothing selected.
  interrupt(): void;
  // Ctrl+L.
  clear(): void;
  // Mod+F.
  find(): void;
  // Text selected in the output, if any (Ctrl+C copies it).
  outputSelection(): string;
  history: ConsoleHistory;
}

export interface ConsoleInput {
  readonly view: EditorView;
  text(): string;
  setText(text: string): void;
  // Replace the whole input with `text` (the command browser's insert).
  replaceAll(text: string): void;
  focus(): void;
  destroy(): void;
}

// --- Completion ------------------------------------------------------------

// Candidates' common prefix (case-insensitive, as the shell matches).
export function commonPrefix(items: readonly string[]): string {
  if (!items.length) return '';
  let common = items[0];
  for (let i = 1; i < items.length; i++) {
    let j = 0;
    const s = items[i];
    while (j < common.length && j < s.length && common[j].toLowerCase() === s[j].toLowerCase()) j++;
    common = common.slice(0, j);
  }
  return common;
}

// The palette class of a candidate kind (--gs-syntax-*).
const KIND_CLASS: Record<string, string> = {
  method: 'method',
  attr: 'attribute',
  alias: 'alias',
  keyword: 'keyword',
  value: 'enum',
  object: 'object',
  collection: 'object',
};

// The whole input as a Tab sees it: one replacement, if the candidates
// agree on one; the text a lone candidate completes to (trailing space
// unless it drills in: "name.", "dir/", "name=").
export function applyCandidate(
  doc: string,
  span: { start: number; end: number },
  text: string,
  lone: boolean,
): { doc: string; cursor: number } {
  const before = doc.slice(0, span.start);
  const after = doc.slice(span.end);
  let insert = text;
  if (lone && !after && !/[./=[]$/.test(text)) insert += ' ';
  return { doc: before + insert + after, cursor: before.length + insert.length };
}

// --- Setup -----------------------------------------------------------------

export function createConsoleInput(
  parent: HTMLElement,
  h: ConsoleInputHandlers,
  placeholder = '',
): ConsoleInput {
  const setDoc = (text: string, cursor = text.length) => {
    view.dispatch({
      changes: { from: 0, to: view.state.doc.length, insert: text },
      selection: { anchor: cursor },
      scrollIntoView: true,
    });
  };

  // Enter: submit, unless the input is an unfinished block.  The input
  // clears at once, so what is typed while the shell answers is the next
  // line (type-ahead); if the block continues, the text comes back with a
  // newline at the cursor and the type-ahead after it.  Presses are
  // answered in order.
  let enterChain: Promise<void> = Promise.resolve();
  const enter: Command = (v) => {
    if (completionStatus(v.state) === 'active') return false; // accept the pick
    const text = v.state.doc.toString();
    if (!text.trim()) return true;
    const cursor = v.state.selection.main.head;
    setDoc('');
    enterChain = enterChain.then(async () => {
      let more = false;
      try {
        more = await h.needsContinuation(text);
      } catch {
        // No answer: run it; the shell reports what is wrong.
      }
      if (more) {
        const typed = v.state.doc.toString();
        const head = `${text.slice(0, cursor)}\n`;
        const doc = head + text.slice(cursor) + typed;
        setDoc(doc, cursor === text.length ? doc.length : head.length);
        return;
      }
      h.history.push(text);
      h.submit(text);
    });
    return true;
  };

  const onFirstLine = (v: EditorView) =>
    v.state.doc.lineAt(v.state.selection.main.head).number === 1;
  const onLastLine = (v: EditorView) =>
    v.state.doc.lineAt(v.state.selection.main.head).number === v.state.doc.lines;

  const historyUp: Command = (v) => {
    if (completionStatus(v.state) === 'active' || !onFirstLine(v)) return false;
    const t = h.history.prev(v.state.doc.toString());
    if (t !== null) setDoc(t);
    return true;
  };
  const historyDown: Command = (v) => {
    if (completionStatus(v.state) === 'active' || !onLastLine(v)) return false;
    const t = h.history.next();
    if (t !== null) setDoc(t);
    return true;
  };

  // Tab: a lone candidate or a longer common prefix is inserted at once;
  // otherwise the popup opens.
  const tab: Command = (v) => {
    // With the popup open, Tab completes afresh from what is typed now.
    if (completionStatus(v.state) !== null) closeCompletion(v);
    const doc = v.state.doc.toString();
    const cursor = v.state.selection.main.head;
    void h.complete(doc, cursor).then((r) => {
      if (!r || !r.candidates.length || v.state.doc.toString() !== doc) return;
      const texts = r.candidates.map((c) => c.text);
      if (texts.length === 1) {
        const a = applyCandidate(doc, r.span, texts[0], true);
        setDoc(a.doc, a.cursor);
        return;
      }
      const common = commonPrefix(texts);
      if (common.length > r.span.end - r.span.start) {
        const a = applyCandidate(doc, r.span, common, false);
        setDoc(a.doc, a.cursor);
        return;
      }
      startCompletion(v);
    });
    return true;
  };

  const source = async (ctx: CompletionContext): Promise<CmCompletionResult | null> => {
    const doc = ctx.state.doc.toString();
    const r = await h.complete(doc, ctx.pos);
    if (!r || !r.candidates.length) return null;
    const options: Completion[] = r.candidates.map((c) => ({
      label: c.text,
      detail: c.doc || undefined,
      type: KIND_CLASS[c.kind] ?? 'object',
    }));
    return { from: r.span.start, to: r.span.end, options, filter: false };
  };

  // Ctrl+C: a selection (input or output) is copied; nothing selected
  // interrupts.  Cmd+C on macOS is the browser's copy and never reaches here.
  const ctrlC: Command = (v) => {
    const sel = v.state.selection.main;
    if (!sel.empty) return false; // the browser copies
    const out = h.outputSelection();
    if (out) {
      void copyText(out);
      return true;
    }
    closeCompletion(v);
    h.interrupt();
    return true;
  };
  const copyCmd: Command = (v) => {
    const sel = v.state.selection.main;
    const text = sel.empty ? h.outputSelection() : v.state.sliceDoc(sel.from, sel.to);
    if (text) void copyText(text);
    return true;
  };

  const keys = Prec.highest(
    keymap.of([
      { key: 'Enter', run: enter },
      { key: 'Shift-Enter', run: insertNewline },
      { key: 'ArrowUp', run: historyUp },
      { key: 'ArrowDown', run: historyDown },
      { key: 'Tab', run: tab },
      { key: 'Ctrl-c', run: ctrlC },
      { key: 'Ctrl-Shift-c', run: copyCmd },
      {
        key: 'Ctrl-l',
        run: () => {
          h.clear();
          return true;
        },
      },
      {
        key: 'Mod-f',
        run: () => {
          h.find();
          return true;
        },
      },
    ]),
  );

  const paste = EditorView.domEventHandlers({
    paste(ev, v) {
      const raw = ev.clipboardData?.getData('text/plain');
      if (raw === undefined) return false;
      ev.preventDefault();
      const text = normalisePaste(raw);
      v.dispatch(v.state.replaceSelection(text));
      return true;
    },
  });

  const view: EditorView = new EditorView({
    parent,
    state: EditorState.create({
      doc: '',
      extensions: [
        keys,
        history(),
        drawSelection(),
        autocompletion({
          override: [source],
          activateOnTyping: false,
          icons: false,
          optionClass: (c) => `gs-cand-${c.type ?? 'object'}`,
        }),
        keymap.of([...defaultKeymap, ...historyKeymap]),
        paste,
        // Editing a recalled entry makes it the new draft.
        EditorView.updateListener.of((u) => {
          if (u.transactions.some((t) => t.isUserEvent('input') || t.isUserEvent('delete')))
            h.history.reset();
        }),
        EditorView.lineWrapping,
        EditorView.contentAttributes.of({ 'aria-label': 'Console input' }),
        placeholder ? cmPlaceholder(placeholder) : [],
        inputTheme,
      ],
    }),
  });

  return {
    view,
    text: () => view.state.doc.toString(),
    setText: (t) => setDoc(t),
    replaceAll: (t) => {
      setDoc(t);
      view.focus();
    },
    focus: () => view.focus(),
    destroy: () => view.destroy(),
  };
}

// Colours come from the page's CSS variables, so a theme switch restyles
// the input without reconfiguring the editor.
const inputTheme = EditorView.theme({
  '&': {
    color: 'var(--gs-terminal-fg)',
    backgroundColor: 'transparent',
    fontFamily: 'var(--gs-font-mono)',
    fontSize: '13px',
    flex: '1',
    minWidth: '0',
  },
  '&.cm-focused': { outline: 'none' },
  '.cm-content': { padding: '0', caretColor: 'var(--gs-terminal-cursor)' },
  '.cm-line': { padding: '0' },
  '.cm-scroller': { fontFamily: 'var(--gs-font-mono)', lineHeight: '1.4' },
  '.cm-cursor': { borderLeftColor: 'var(--gs-terminal-cursor)' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection': {
    backgroundColor: 'var(--gs-terminal-selection)',
  },
  '.cm-placeholder': { color: 'var(--gs-syntax-dim)' },
  '.cm-tooltip': {
    backgroundColor: 'var(--gs-menu-bg)',
    color: 'var(--gs-menu-fg)',
    border: '1px solid var(--gs-border, #454545)',
    fontFamily: 'var(--gs-font-mono)',
    fontSize: '12px',
  },
  '.cm-tooltip-autocomplete > ul > li[aria-selected]': {
    backgroundColor: 'var(--gs-menu-hover-bg)',
    color: 'var(--gs-menu-hover-fg)',
  },
  '.cm-completionDetail': {
    color: 'var(--gs-syntax-dim)',
    fontStyle: 'normal',
    marginLeft: '1.5em',
    fontFamily: 'var(--gs-font-ui)',
  },
  '.gs-cand-method .cm-completionLabel': { color: 'var(--gs-syntax-method)' },
  '.gs-cand-attribute .cm-completionLabel': { color: 'var(--gs-syntax-attribute)' },
  '.gs-cand-alias .cm-completionLabel': { color: 'var(--gs-syntax-alias)' },
  '.gs-cand-keyword .cm-completionLabel': { color: 'var(--gs-syntax-keyword)' },
  '.gs-cand-enum .cm-completionLabel': { color: 'var(--gs-syntax-enum)' },
});
