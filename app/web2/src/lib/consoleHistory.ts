// The console's input history: kept out of ConsoleInput.ts so the console's
// view can use it without loading CodeMirror (which is code-split, fetched
// when the Terminal first opens).

export const HISTORY_KEY = 'gs.console.history';
export const HISTORY_MAX = 500;

// The last 500 submissions (a multi-line block is one entry), kept in
// localStorage as a JSON array of strings.  Browsing starts past the end;
// what was being typed is restored when browsing returns there.
export class ConsoleHistory {
  private items: string[] = [];
  private index = 0;
  private draft = '';

  constructor(private readonly storage: Storage | null = safeStorage()) {
    try {
      const raw = this.storage?.getItem(HISTORY_KEY);
      const parsed: unknown = raw ? JSON.parse(raw) : [];
      if (Array.isArray(parsed))
        this.items = parsed.filter((s): s is string => typeof s === 'string').slice(-HISTORY_MAX);
    } catch {
      // A missing or unreadable history only costs history.
    }
    this.index = this.items.length;
  }

  list(): readonly string[] {
    return this.items;
  }

  push(text: string): void {
    if (!text.trim()) return;
    if (this.items[this.items.length - 1] !== text) this.items.push(text);
    if (this.items.length > HISTORY_MAX) this.items.splice(0, this.items.length - HISTORY_MAX);
    this.index = this.items.length;
    this.draft = '';
    try {
      this.storage?.setItem(HISTORY_KEY, JSON.stringify(this.items));
    } catch {
      // Best-effort.
    }
  }

  // The entry before the current one, or null at the oldest.
  prev(current: string): string | null {
    if (this.index === 0) return null;
    if (this.index === this.items.length) this.draft = current;
    this.index--;
    return this.items[this.index];
  }

  // The entry after the current one; past the newest, the draft.
  next(): string | null {
    if (this.index >= this.items.length) return null;
    this.index++;
    return this.index === this.items.length ? this.draft : this.items[this.index];
  }

  reset(): void {
    this.index = this.items.length;
    this.draft = '';
  }
}

function safeStorage(): Storage | null {
  try {
    return typeof localStorage !== 'undefined' ? localStorage : null;
  } catch {
    return null;
  }
}
