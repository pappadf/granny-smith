// The console's input: a CodeMirror 6 editor, one line by default, growing
// for blocks and pastes.  It owns the keymap -- Enter
// (submit unless the shell says the input continues), Shift+Enter, history
// on ↑/↓ at the first/last line, Tab completion (common prefix, then a
// popup of `shell.complete` candidates with their kind and doc), Ctrl+C
// (copy with a selection, else interrupt), Ctrl+L, Mod+F -- and the paste
// normalisation.  What those keys *do* to the console is the caller's
// (ConsoleView.svelte), through `ConsoleInputHandlers`.

import { EditorState, Prec, StateEffect, StateField, type Range } from '@codemirror/state';
import {
  Decoration,
  EditorView,
  keymap,
  type DecorationSet,
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
import { onAppearanceChange } from '@/lib/tokens';
import type { CompletionResult } from '@/bus/emulator';
import { normalisePaste } from '@/lib/consoleModel';
import { replaceTokenAt } from '@/lib/pathToken';
import type { HlSpan } from '@/lib/highlight';
import type { ConsoleHistory } from '@/lib/consoleHistory';
import { copyText } from '@/lib/clipboard';

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
  // The text or the cursor changed (the browser follows the input).
  onChange?(text: string, cursor: number): void;
  // Ctrl+Shift+Space: show the signature hint.
  showHint?(): void;
  // Esc with no popup open: close the hint; answers whether it did.
  escape?(): boolean;
}

export interface ConsoleInput {
  readonly view: EditorView;
  text(): string;
  setText(text: string): void;
  // Replace the path token at the cursor (the command browser's write).
  replaceToken(text: string): void;
  // Focus with the cursor at the end.
  focusEnd(): void;
  // Colour the input with `spans` (shell.highlight) if it still holds
  // `text`; an answer for an older text is dropped.
  setHighlight(text: string, spans: readonly HlSpan[]): void;
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

// --- Highlighting ------------------------------------------------------------

// The input's syntax colours: set whole by setHighlight, mapped through
// edits until the next answer replaces them.
const setSpans = StateEffect.define<readonly HlSpan[]>();
const highlightField = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(deco, tr) {
    let next = deco.map(tr.changes);
    for (const e of tr.effects) {
      if (!e.is(setSpans)) continue;
      const len = tr.state.doc.length;
      const marks: Range<Decoration>[] = [];
      for (const sp of e.value)
        if (sp.to <= len && sp.from < sp.to)
          marks.push(Decoration.mark({ class: `hl-${sp.cls}` }).range(sp.from, sp.to));
      next = Decoration.set(marks, true);
    }
    return next;
  },
  provide: (f) => EditorView.decorations.from(f),
});

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
        key: 'Ctrl-Shift-Space',
        run: () => {
          h.showHint?.();
          return true;
        },
      },
      {
        key: 'Escape',
        run: (v) => (completionStatus(v.state) === null ? (h.escape?.() ?? false) : false),
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
          optionClass: (c) => `hl-${c.type ?? 'object'}`,
        }),
        keymap.of([...defaultKeymap, ...historyKeymap]),
        paste,
        highlightField,
        // Editing a recalled entry makes it the new draft.
        EditorView.updateListener.of((u) => {
          if (u.transactions.some((t) => t.isUserEvent('input') || t.isUserEvent('delete')))
            h.history.reset();
          if (u.docChanged || u.selectionSet)
            h.onChange?.(u.state.doc.toString(), u.state.selection.main.head);
        }),
        EditorView.lineWrapping,
        EditorView.contentAttributes.of({ 'aria-label': 'Console input' }),
        placeholder ? cmPlaceholder(placeholder) : [],
        inputTheme,
      ],
    }),
  });
  // A skin or scheme change can change the font metrics: measure again.
  const stopMeasure = onAppearanceChange(() => view.requestMeasure());

  return {
    view,
    text: () => view.state.doc.toString(),
    setText: (t) => setDoc(t),
    replaceToken: (t) => {
      const r = replaceTokenAt(view.state.doc.toString(), view.state.selection.main.head, t);
      setDoc(r.text, r.cursor);
    },
    focusEnd: () => {
      view.dispatch({ selection: { anchor: view.state.doc.length } });
      view.focus();
    },
    setHighlight: (text, spans) => {
      if (view.state.doc.toString() !== text) return;
      view.dispatch({ effects: setSpans.of(spans) });
    },
    focus: () => view.focus(),
    destroy: () => {
      stopMeasure();
      view.destroy();
    },
  };
}

// Colours come from the page's CSS variables, so a theme switch restyles
// the input without reconfiguring the editor.
const inputTheme = EditorView.theme({
  '&': {
    color: 'var(--gs-console-fg)',
    backgroundColor: 'transparent',
    fontFamily: 'var(--gs-console-font)',
    fontSize: 'var(--gs-console-font-size)',
    flex: '1',
    minWidth: '0',
  },
  '&.cm-focused': { outline: 'none' },
  '.cm-content': { padding: '0', caretColor: 'var(--gs-console-cursor)' },
  '.cm-line': { padding: '0' },
  '.cm-scroller': {
    fontFamily: 'var(--gs-console-font)',
    lineHeight: 'var(--gs-console-line-height)',
  },
  '.cm-cursor': { borderLeftColor: 'var(--gs-console-cursor)' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection': {
    backgroundColor: 'var(--gs-console-selection)',
  },
  '.cm-placeholder': { color: 'var(--gs-syntax-dim)' },
  // The completion popup is a menu: the menu's colours, radius and shadow.
  '.cm-tooltip': {
    backgroundColor: 'var(--gs-menu-bg)',
    color: 'var(--gs-menu-fg)',
    border: 'var(--gs-border-width) solid var(--gs-border)',
    borderRadius: 'var(--gs-menu-radius)',
    boxShadow: 'var(--gs-shadow-popup)',
    overflow: 'hidden',
    fontFamily: 'var(--gs-console-font)',
    fontSize: 'var(--gs-console-popup-font-size)',
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
  // Syntax colours: the global hl-* classes (styles/syntax.css) apply here
  // too, to the input's marks and the candidates' labels.
});
