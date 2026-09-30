// What the console's input asks the shell while the user types:
//   - highlighting: shell.highlight 30 ms after the last change; an answer
//     for an older text is dropped (the input's setHighlight checks the
//     text), and the last answer is kept for the command entry the text
//     becomes;
//   - following: shell.complete (detail) once typing pauses; the answer
//     goes to the command browser (state/terminalSync) and, with the cursor
//     in a method's arguments, becomes the signature hint;
//   - Tab completion: the same shell.complete, reused when the text and
//     cursor have not changed since it was asked.
// The signature hint is the method's signature (shell.usage) with the
// current argument underlined.  Esc hides it for that method;
// Ctrl+Shift+Space brings it back.

import { tabComplete, type CompletionResult } from '@/bus/emulator';
import { publishCompletion } from '@/state/terminalSync.svelte';
import { loadUsageInfo, type UsageInfo } from '@/lib/commandsTree';
import { loadHighlight, type HlSpan } from '@/lib/highlight';
import { utf8ToUtf16 } from '@/lib/utf8';

const SYNC_DELAY_MS = 40;
const HIGHLIGHT_DELAY_MS = 30;

export interface SignatureHint {
  before: string;
  arg: string;
  after: string;
}

// The part of the input the assist writes to.
export interface HighlightTarget {
  setHighlight(text: string, spans: readonly HlSpan[]): void;
}

export class InputAssist {
  hint = $state<SignatureHint | null>(null);

  #input: () => HighlightTarget | null;
  #disposed = false;

  #syncTimer: ReturnType<typeof setTimeout> | null = null;
  #syncSeq = 0;
  #lastResult: CompletionResult | null = null;
  // The latest shell.complete asked, answered or not.
  #asked: { line: string; cursor: number; result: Promise<CompletionResult | null> } | null = null;

  #hlTimer: ReturnType<typeof setTimeout> | null = null;
  #hlText = '';
  #lastHl: { text: string; spans: readonly HlSpan[] } = { text: '', spans: [] };

  #hintDismissed = '';
  // shell.usage answers by method path.
  #usage: Record<string, UsageInfo | null> = {};

  constructor(input: () => HighlightTarget | null) {
    this.#input = input;
  }

  // The input's text or cursor changed.
  onChange(text: string, cursor: number): void {
    this.#requestHighlight(text);
    if (this.#syncTimer) clearTimeout(this.#syncTimer);
    this.#syncTimer = setTimeout(() => {
      this.#syncTimer = null;
      const seq = ++this.#syncSeq;
      void this.complete(text, cursor).then((r) => {
        if (this.#disposed || seq !== this.#syncSeq) return; // a newer change won
        this.#lastResult = r;
        publishCompletion(text, cursor, r);
        void this.#updateHint(r);
      });
    }, SYNC_DELAY_MS);
  }

  // shell.complete for `line` at `cursor`: the last one asked, when it was
  // for the same text and cursor.
  complete(line: string, cursor: number): Promise<CompletionResult | null> {
    const a = this.#asked;
    if (a && a.line === line && a.cursor === cursor) return a.result;
    const asked = { line, cursor, result: tabComplete(line, cursor) };
    this.#asked = asked;
    // No answer is not kept: the next ask tries again.
    const drop = () => {
      if (this.#asked === asked) this.#asked = null;
    };
    asked.result.then((r) => {
      if (!r) drop();
    }, drop);
    return asked.result;
  }

  // What the shell can complete has changed (the machine did).
  forget(): void {
    this.#asked = null;
  }

  // The highlight spans for `text`, if the last answer was for it.
  spansFor(text: string): readonly HlSpan[] | undefined {
    return this.#lastHl.text === text ? this.#lastHl.spans : undefined;
  }

  // Ctrl+Shift+Space.
  showHint(): void {
    this.#hintDismissed = '';
    void this.#updateHint(this.#lastResult);
  }

  // Esc: hides the hint; answers whether there was one.
  escapeHint(): boolean {
    if (!this.hint) return false;
    this.#hintDismissed = this.#lastResult?.context.method ?? '';
    this.hint = null;
    return true;
  }

  dispose(): void {
    this.#disposed = true;
    if (this.#syncTimer) clearTimeout(this.#syncTimer);
    if (this.#hlTimer) clearTimeout(this.#hlTimer);
    this.#syncTimer = this.#hlTimer = null;
  }

  #requestHighlight(text: string): void {
    if (text === this.#hlText) return;
    this.#hlText = text;
    if (this.#hlTimer) clearTimeout(this.#hlTimer);
    this.#hlTimer = setTimeout(() => {
      this.#hlTimer = null;
      void loadHighlight(text).then((spans) => {
        if (this.#disposed || text !== this.#hlText) return;
        this.#lastHl = { text, spans };
        this.#input()?.setHighlight(text, spans);
      });
    }, HIGHLIGHT_DELAY_MS);
  }

  async #updateHint(r: CompletionResult | null): Promise<void> {
    const method = r?.context.method ?? null;
    if (!method || method === this.#hintDismissed) {
      this.hint = null;
      if (!method) this.#hintDismissed = '';
      return;
    }
    let u = this.#usage[method];
    if (u === undefined) {
      u = await loadUsageInfo(method);
      this.#usage[method] = u;
    }
    if (this.#disposed) return;
    if (!u || !u.signature || this.#lastResult !== r) {
      if (!u?.signature) this.hint = null;
      return;
    }
    const idx = r?.context.argIndex ?? null;
    const span = idx !== null ? u.argSpans[idx] : null;
    if (!span) {
      this.hint = { before: u.signature, arg: '', after: '' };
      return;
    }
    const a = utf8ToUtf16(u.signature, span[0]);
    const b = utf8ToUtf16(u.signature, span[1]);
    this.hint = {
      before: u.signature.slice(0, a),
      arg: u.signature.slice(a, b),
      after: u.signature.slice(b),
    };
  }
}
